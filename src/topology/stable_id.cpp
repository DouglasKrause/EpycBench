#include "epycbench/topology/stable_id.hpp"

#include "epycbench/topology/invariants.hpp"
#include "core/json_ownership.hpp"
#include "topology/prepared_stable_id_access.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

namespace epycbench::topology {
namespace {
using Json = nlohmann::json;
struct ReportCleanup {
    ValidationReport& report;
    ~ReportCleanup() {
        for (auto& error : report.diagnostics)
            core::detail::retire_json(error.context);
    }
};

auto stable_error(std::string code, std::string message, std::string object_type,
                  std::string object_handle, std::string field) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::topology,
        .code = core::StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = "derive_stable_topology_ids",
        .os_error = std::nullopt,
        .context = core::detail::json_object<Json>([&](auto& object_10) {
            core::detail::set_json(object_10, "object_type", std::move(object_type));
            core::detail::set_json(object_10, "object_handle", std::move(object_handle));
            core::detail::set_json(object_10, "field", std::move(field));
        }),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

auto kind_name(detail::StableIdKind kind) -> std::string_view {
    switch (kind) {
    case detail::StableIdKind::machine:
        return "machine";
    case detail::StableIdKind::package:
        return "package";
    case detail::StableIdKind::die:
        return "die";
    case detail::StableIdKind::group:
        return "architecture_group";
    case detail::StableIdKind::core:
        return "core";
    case detail::StableIdKind::processing_unit:
        return "processing_unit";
    case detail::StableIdKind::cache:
        return "cache";
    case detail::StableIdKind::numa:
        return "numa_node";
    case detail::StableIdKind::pci:
        return "pci_device";
    }
    return "unknown";
}

auto parse_error(detail::StableIdKind kind, std::string classification) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::topology,
        .code = core::StableErrorCode{"topology.stable_id.invalid_canonical_component"},
        .message = "Stable topology identity text is malformed or noncanonical",
        .operation = "parse_stable_topology_id",
        .os_error = std::nullopt,
        .context = core::detail::json_object<Json>([&](auto& object_11) {
            core::detail::set_json(object_11, "object_type", kind_name(kind));
            core::detail::set_json(object_11, "classification", std::move(classification));
        }),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

auto decimal(std::string_view text) -> bool {
    if (text.empty() || (text.size() > 1U && text.front() == '0')) {
        return false;
    }
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

auto range_list(std::string_view text) -> bool {
    const auto parsed = core::CpuSet::parse(text);
    return parsed && parsed->to_range_list() == text;
}

auto package_component(std::string_view text) -> bool {
    return decimal(text) || (text.starts_with("set") && range_list(text.substr(3)));
}

auto valid_utf8(std::string_view text) -> bool {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::uint32_t scalar{};
        std::size_t length{};
        std::uint32_t minimum{};
        if (first <= 0x7fU) {
            scalar = first;
            length = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            scalar = first & 0x1fU;
            length = 2;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            scalar = first & 0x0fU;
            length = 3;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            scalar = first & 0x07U;
            length = 4;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (length > text.size() - index) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<std::uint8_t>(text[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            scalar = (scalar << 6U) | (byte & 0x3fU);
        }
        if (scalar < minimum || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

auto free_character(std::uint8_t byte) -> bool {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
}

auto encode_component(std::string_view text) -> std::expected<std::string, core::Error> {
    if (!valid_utf8(text)) {
        return std::unexpected{
            stable_error("topology.stable_id.invalid_canonical_component",
                         "Stable identity free-form component is not valid UTF-8",
                         "architecture_group", "unavailable", "free_form_component")};
    }
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string result;
    for (const auto character : text) {
        const auto byte = static_cast<std::uint8_t>(character);
        if (free_character(byte)) {
            result.push_back(character);
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 0x0fU]);
        }
    }
    return result;
}

auto decode_component(std::string_view text) -> std::optional<std::string> {
    std::string result;
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<std::uint8_t>(text[index]);
        if (free_character(byte)) {
            result.push_back(text[index++]);
            continue;
        }
        if (text[index] != '%' || index + 2U >= text.size()) {
            return std::nullopt;
        }
        const auto high = nibble(text[index + 1U]);
        const auto low = nibble(text[index + 2U]);
        if (!high || !low) {
            return std::nullopt;
        }
        const auto decoded = static_cast<std::uint8_t>((*high << 4U) | *low);
        if (free_character(decoded)) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(decoded));
        index += 3U;
    }
    if (!valid_utf8(result)) {
        return std::nullopt;
    }
    return result;
}

auto valid_group(std::string_view text) -> bool;

auto parse_group_component(std::string_view text, std::string_view prefix, std::string_view next,
                           std::size_t& position) -> std::optional<std::string_view> {
    if (!text.substr(position).starts_with(prefix)) {
        return std::nullopt;
    }
    position += prefix.size();
    const auto end = text.find(next, position);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value = text.substr(position, end - position);
    position = end;
    return value;
}

auto valid_group(std::string_view text) -> bool {
    constexpr std::string_view prefix{"group/pkg"};
    if (!text.starts_with(prefix)) {
        return false;
    }
    auto position = prefix.size();
    const auto package_end = text.find("/kind/", position);
    if (package_end == std::string_view::npos ||
        !package_component(text.substr(position, package_end - position))) {
        return false;
    }
    position = package_end;
    const auto kind = parse_group_component(text, "/kind/", "/subtype/", position);
    const auto subtype = parse_group_component(text, "/subtype/", "/set", position);
    const auto members = parse_group_component(text, "/set", "/parent/", position);
    if (!kind || kind->empty() || !decode_component(*kind) || !subtype || !members ||
        !range_list(*members)) {
        return false;
    }
    if (*subtype != "none") {
        if (!subtype->starts_with("value-") || !decode_component(subtype->substr(6))) {
            return false;
        }
    }
    position += std::string_view{"/parent/"}.size();
    if (position > text.size()) {
        return false;
    }
    const auto parent = text.substr(position);
    if (parent == "root") {
        return true;
    }
    const auto decoded_parent = decode_component(parent);
    return decoded_parent && valid_group(*decoded_parent);
}

auto valid_stable_id(detail::StableIdKind kind, std::string_view text) -> bool {
    switch (kind) {
    case detail::StableIdKind::machine:
        return text == "machine";
    case detail::StableIdKind::processing_unit:
        return text.starts_with("pu/") && decimal(text.substr(3));
    case detail::StableIdKind::package:
        return text.starts_with("package/") && package_component(text.substr(8));
    case detail::StableIdKind::die: {
        if (!text.starts_with("die/pkg")) {
            return false;
        }
        const auto separator = text.find('/', 7);
        if (separator == std::string_view::npos ||
            !package_component(text.substr(7, separator - 7))) {
            return false;
        }
        const auto suffix = text.substr(separator + 1U);
        return (suffix.starts_with("id") && decimal(suffix.substr(2))) ||
               (suffix.starts_with("set") && range_list(suffix.substr(3)));
    }
    case detail::StableIdKind::core: {
        if (!text.starts_with("core/pkg")) {
            return false;
        }
        const auto separator = text.rfind("-set");
        return separator != std::string_view::npos && separator >= 8U &&
               package_component(text.substr(8, separator - 8)) &&
               range_list(text.substr(separator + 4U));
    }
    case detail::StableIdKind::cache: {
        if (!text.starts_with("cache/L")) {
            return false;
        }
        const auto kind_separator = text.find('-', 7);
        const auto set_separator = text.rfind("-set");
        if (kind_separator == std::string_view::npos || set_separator == std::string_view::npos ||
            kind_separator >= set_separator || !decimal(text.substr(7, kind_separator - 7)) ||
            text.substr(7, kind_separator - 7) == "0" ||
            !range_list(text.substr(set_separator + 4U))) {
            return false;
        }
        const auto cache_kind =
            text.substr(kind_separator + 1U, set_separator - kind_separator - 1U);
        return cache_kind == "data" || cache_kind == "instruction" || cache_kind == "unified" ||
               cache_kind == "unknown";
    }
    case detail::StableIdKind::numa:
        return text.starts_with("numa/") && decimal(text.substr(5));
    case detail::StableIdKind::pci:
        return text.starts_with("pci/") && PciAddress::parse(text.substr(4)).has_value() &&
               "pci/" + PciAddress::parse(text.substr(4))->to_string() == text;
    case detail::StableIdKind::group:
        return valid_group(text);
    }
    return false;
}

auto cache_kind_text(CacheKind kind) -> std::string_view {
    switch (kind) {
    case CacheKind::data:
        return "data";
    case CacheKind::instruction:
        return "instruction";
    case CacheKind::unified:
        return "unified";
    case CacheKind::unknown:
        return "unknown";
    }
    return "unknown";
}

auto diagnostic_handle(auto id) -> std::string { return std::to_string(id.value()); }
auto diagnostic_handle(PciAddress address) -> std::string { return address.to_string(); }

auto physical_members(const TopologySnapshot& topology, const ProcessingUnitSet& members)
    -> std::expected<core::CpuSet, core::Error> {
    core::CpuSet result;
    for (const auto handle : members) {
        const auto pu = topology.processing_unit(handle);
        if (!pu) {
            return std::unexpected{
                stable_error("topology.stable_id.invalid_topology",
                             "Stable identity membership references a missing processing unit",
                             "membership", diagnostic_handle(handle), "member_pus")};
        }
        result = result.set_union(core::CpuSet{pu->linux_cpu_id});
    }
    return result;
}

auto package_members(const TopologySnapshot& topology, core::PackageId package)
    -> std::expected<core::CpuSet, core::Error> {
    ProcessingUnitSet handles;
    for (const auto& pu : topology.processing_units()) {
        const auto core = topology.core(pu.core_id);
        if (core && core->package_id == package) {
            handles = handles.set_union(ProcessingUnitSet{pu.id});
        }
    }
    return physical_members(topology, handles);
}

auto die_members(const TopologySnapshot& topology, core::DieId die)
    -> std::expected<core::CpuSet, core::Error> {
    ProcessingUnitSet handles;
    for (const auto& pu : topology.processing_units()) {
        const auto core = topology.core(pu.core_id);
        if (core && core->die_id == die) {
            handles = handles.set_union(ProcessingUnitSet{pu.id});
        }
    }
    return physical_members(topology, handles);
}

template <typename AttributeType>
auto available_value(const Attribute<AttributeType>& attribute) -> std::optional<AttributeType> {
    return attribute.availability() == Availability::available ? attribute.value() : std::nullopt;
}

auto package_suffix(std::string_view package_id) -> std::string {
    return std::string{package_id.substr(std::string_view{"package/"}.size())};
}

template <typename Handle, typename SemanticId>
auto lookup_forward(const std::vector<StableTopologyIdEntry<Handle, SemanticId>>& entries,
                    const std::map<Handle, std::size_t>& forward, Handle handle)
    -> std::optional<std::reference_wrapper<const SemanticId>> {
    const auto found = forward.find(handle);
    if (found == forward.end()) {
        return std::nullopt;
    }
    return std::cref(entries[found->second].semantic_id);
}

template <typename Handle, typename SemanticId>
auto lookup_reverse(const std::map<SemanticId, Handle>& reverse, const SemanticId& id)
    -> std::optional<Handle> {
    const auto found = reverse.find(id);
    return found == reverse.end() ? std::nullopt : std::optional<Handle>{found->second};
}

} // namespace

namespace detail {

auto parse_stable_topology_id(StableIdKind kind, std::string_view text)
    -> std::expected<std::string, core::Error> {
    if (!valid_stable_id(kind, text)) {
        return std::unexpected{parse_error(kind, "malformed_or_noncanonical")};
    }
    return std::string{text};
}

} // namespace detail

auto detail::PreparedStableTopologyIdAccess::derive_with_failure_sink(
    const TopologySnapshot& topology, std::optional<core::Error>* raw_failure)
    -> std::expected<StableTopologyIds, core::Error> {
    for (const auto& node : topology.numa_nodes()) {
        if (node.linux_node_id.availability() == Availability::conflicted) {
            return std::unexpected{stable_error("topology.stable_id.conflicted_identity_evidence",
                                                "NUMA node Linux identifier is conflicted",
                                                "numa_node", diagnostic_handle(node.id),
                                                "linux_node_id")};
        }
        if (node.linux_node_id.availability() != Availability::available) {
            return std::unexpected{
                stable_error("topology.stable_id.missing_required_identity_evidence",
                             "NUMA node Linux identifier is unavailable", "numa_node",
                             diagnostic_handle(node.id), "linux_node_id")};
        }
    }

    auto validation = detail::validate_topology_with_failure_sink(topology, raw_failure,
        detail::SourceValidationEligibility::stable_identity);
    ReportCleanup validation_cleanup{validation};
    const auto permitted_optional_conflict = [](const core::Error& error) noexcept {
        const auto& context = error.context;
        const auto object = context.find("object_type"), field = context.find("field_path");
        if (object == context.end() || field == context.end() || !object->is_string() || !field->is_string()) return false;
        const auto& object_type = object->get_ref<const std::string&>();
        return error.code.value() == "topology.attribute.conflict_without_difference" &&
               (object_type == "package" || object_type == "die") &&
               field->get_ref<const std::string&>() == "os_id";
    };
    const auto invalid =
        std::ranges::find_if_not(validation.diagnostics, permitted_optional_conflict);
    if (invalid != validation.diagnostics.end()) {
        const core::Error* actual = &*invalid;
        if (raw_failure && !*raw_failure) {
            raw_failure->emplace(std::move(*invalid));
            actual = &**raw_failure;
        }
        return std::unexpected{stable_error(
            "topology.stable_id.invalid_topology",
            "Stable identities require an invariant-valid topology snapshot", "snapshot",
            diagnostic_handle(topology.machine().id), actual->code.value())};
    }

    StableTopologyIds result;
    const auto add = [&]<typename Handle, typename SemanticId>(
                         StableTopologyIds::Index<Handle, SemanticId>& index, Handle handle,
                         std::string text,
                         std::string_view object_type) -> std::expected<void, core::Error> {
        auto id = StableTopologyIds::make_id<SemanticId>(std::move(text));
        if (std::ranges::any_of(index.entries,
                                [&](const auto& entry) { return entry.semantic_id == id; })) {
            return std::unexpected{stable_error(
                "topology.stable_id.collision", "Stable topology identity is not unique",
                std::string{object_type}, diagnostic_handle(handle), "semantic_id")};
        }
        index.entries.push_back({handle, std::move(id)});
        return {};
    };
    const auto finalize = []<typename Handle, typename SemanticId>(
                              StableTopologyIds::Index<Handle, SemanticId>& index) {
        std::ranges::sort(index.entries, {},
                          [](const auto& entry) { return entry.semantic_id.value(); });
        for (std::size_t position = 0; position < index.entries.size(); ++position) {
            const auto& entry = index.entries[position];
            index.forward.emplace(entry.handle, position);
            index.reverse.emplace(entry.semantic_id, entry.handle);
        }
    };

    if (auto added = add(result.machines_, topology.machine().id, "machine", "machine"); !added) {
        return std::unexpected{std::move(added.error())};
    }

    std::map<core::PackageId, core::CpuSet> package_cpu_sets;
    std::map<std::uint32_t, std::size_t> package_os_counts;
    std::set<std::string> package_memberships;
    for (const auto& package : topology.packages()) {
        auto members = package_members(topology, package.id);
        if (!members || members->empty()) {
            return std::unexpected{
                stable_error("topology.stable_id.unsupported_object_state",
                             "Package stable identity requires nonempty physical membership",
                             "package", diagnostic_handle(package.id), "member_pus")};
        }
        package_cpu_sets.emplace(package.id, *members);
        if (!package_memberships.emplace(members->to_range_list()).second) {
            return std::unexpected{
                stable_error("topology.stable_id.collision",
                             "Packages have duplicate physical processing-unit membership",
                             "package", diagnostic_handle(package.id), "member_pus")};
        }
        if (const auto os_id = available_value(package.os_id)) {
            ++package_os_counts[*os_id];
        }
    }
    for (const auto& package : topology.packages()) {
        const auto os_id = available_value(package.os_id);
        const auto text = os_id && package_os_counts[*os_id] == 1U
                              ? "package/" + std::to_string(*os_id)
                              : "package/set" + package_cpu_sets.at(package.id).to_range_list();
        if (auto added = add(result.packages_, package.id, text, "package"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }
    finalize(result.packages_);

    std::map<std::pair<core::PackageId, std::uint32_t>, std::size_t> die_os_counts;
    std::map<core::DieId, core::CpuSet> die_cpu_sets;
    std::set<std::pair<core::PackageId, std::string>> die_memberships;
    for (const auto& die : topology.dies()) {
        auto members = die_members(topology, die.id);
        if (!members || members->empty()) {
            return std::unexpected{
                stable_error("topology.stable_id.unsupported_object_state",
                             "Die stable identity requires nonempty physical membership", "die",
                             diagnostic_handle(die.id), "member_pus")};
        }
        die_cpu_sets.emplace(die.id, *members);
        if (!die_memberships.emplace(die.package_id, members->to_range_list()).second) {
            return std::unexpected{stable_error(
                "topology.stable_id.collision",
                "Dies have duplicate physical processing-unit membership within a package", "die",
                diagnostic_handle(die.id), "member_pus")};
        }
        if (const auto os_id = available_value(die.os_id)) {
            ++die_os_counts[{die.package_id, *os_id}];
        }
    }
    for (const auto& die : topology.dies()) {
        const auto package_id = result.package_id(die.package_id);
        if (!package_id) {
            return std::unexpected{stable_error("topology.stable_id.invalid_topology",
                                                "Die parent package has no stable identity", "die",
                                                diagnostic_handle(die.id), "package_id")};
        }
        const auto os_id = available_value(die.os_id);
        const auto suffix = os_id && die_os_counts[{die.package_id, *os_id}] == 1U
                                ? "id" + std::to_string(*os_id)
                                : "set" + die_cpu_sets.at(die.id).to_range_list();
        const auto text = "die/pkg" + package_suffix(package_id->get().value()) + '/' + suffix;
        if (auto added = add(result.dies_, die.id, text, "die"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    for (const auto& pu : topology.processing_units()) {
        if (auto added = add(result.processing_units_, pu.id,
                             "pu/" + std::to_string(pu.linux_cpu_id.value()), "processing_unit");
            !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    for (const auto& core_object : topology.cores()) {
        const auto package_id = result.package_id(core_object.package_id);
        auto members = physical_members(topology, topology.core_members(core_object.id));
        core::detail::ExpectedErrorCleanup members_cleanup{members};
        if (!package_id || !members || members->empty()) {
            return std::unexpected{stable_error(
                "topology.stable_id.unsupported_object_state",
                "Core stable identity requires package context and nonempty membership", "core",
                diagnostic_handle(core_object.id), "member_pus")};
        }
        const auto text = "core/pkg" + package_suffix(package_id->get().value()) + "-set" +
                          members->to_range_list();
        if (auto added = add(result.cores_, core_object.id, text, "core"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    std::map<core::ArchitectureGroupId, std::string> group_text;
    std::map<core::ArchitectureGroupId, int> group_state;
    const auto derive_group =
        [&](const auto& self,
            core::ArchitectureGroupId handle) -> std::expected<std::string, core::Error> {
        if (group_state[handle] == 2) {
            return group_text.at(handle);
        }
        if (group_state[handle] == 1) {
            return std::unexpected{stable_error("topology.stable_id.invalid_topology",
                                                "Architecture-group hierarchy contains a cycle",
                                                "architecture_group", diagnostic_handle(handle),
                                                "parent_group_id")};
        }
        const auto group = topology.architecture_group(handle);
        if (!group) {
            return std::unexpected{
                stable_error("topology.stable_id.invalid_topology", "Architecture group is missing",
                             "architecture_group", diagnostic_handle(handle), "id")};
        }
        group_state[handle] = 1;
        const auto package_id = result.package_id(group->package_id);
        auto members = physical_members(topology, group->member_pus);
        core::detail::ExpectedErrorCleanup members_cleanup{members};
        auto kind = encode_component(group->kind);
        core::detail::ExpectedErrorCleanup kind_cleanup{kind};
        auto subtype = group->subtype ? encode_component(*group->subtype)
                                      : std::expected<std::string, core::Error>{std::string{}};
        core::detail::ExpectedErrorCleanup subtype_cleanup{subtype};
        if (!package_id || !members || members->empty() || !kind || kind->empty() || !subtype) {
            return std::unexpected{stable_error(
                kind && subtype ? "topology.stable_id.unsupported_object_state"
                                : "topology.stable_id.invalid_canonical_component",
                "Architecture group has insufficient canonical identity facts",
                "architecture_group", diagnostic_handle(handle), "semantic_components")};
        }
        std::string parent{"root"};
        if (group->parent_group_id) {
            auto parent_id = self(self, *group->parent_group_id);
            core::detail::ExpectedErrorCleanup parent_id_cleanup{parent_id};
            if (!parent_id) {
                return std::unexpected{std::move(parent_id.error())};
            }
            auto encoded_parent = encode_component(*parent_id);
            core::detail::ExpectedErrorCleanup encoded_parent_cleanup{encoded_parent};
            if (!encoded_parent) {
                return std::unexpected{std::move(encoded_parent.error())};
            }
            parent = *encoded_parent;
        }
        auto text = "group/pkg" + package_suffix(package_id->get().value()) + "/kind/" + *kind +
                    "/subtype/" + (group->subtype ? "value-" + *subtype : std::string{"none"}) +
                    "/set" + members->to_range_list() + "/parent/" + parent;
        group_state[handle] = 2;
        group_text.emplace(handle, text);
        return text;
    };
    for (const auto& group : topology.architecture_groups()) {
        auto text = derive_group(derive_group, group.id);
        core::detail::ExpectedErrorCleanup text_cleanup{text};
        if (!text) {
            return std::unexpected{std::move(text.error())};
        }
        if (auto added = add(result.groups_, group.id, *text, "architecture_group"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    for (const auto& cache : topology.caches()) {
        auto members = physical_members(topology, cache.member_pus);
        core::detail::ExpectedErrorCleanup members_cleanup{members};
        if (!members || members->empty()) {
            return std::unexpected{
                stable_error("topology.stable_id.unsupported_object_state",
                             "Cache stable identity requires nonempty shared membership", "cache",
                             diagnostic_handle(cache.id), "member_pus")};
        }
        const auto text = "cache/L" + std::to_string(cache.level) + '-' +
                          std::string{cache_kind_text(cache.kind)} + "-set" +
                          members->to_range_list();
        if (auto added = add(result.caches_, cache.id, text, "cache"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    for (const auto& node : topology.numa_nodes()) {
        const auto text = "numa/" + std::to_string(*node.linux_node_id.value());
        if (auto added = add(result.numa_nodes_, node.id, text, "numa_node"); !added) {
            return std::unexpected{std::move(added.error())};
        }
    }
    for (const auto& device : topology.pci_devices()) {
        if (auto added = add(result.pci_devices_, device.address,
                             "pci/" + device.address.to_string(), "pci_device");
            !added) {
            return std::unexpected{std::move(added.error())};
        }
    }

    finalize(result.machines_);
    finalize(result.dies_);
    finalize(result.groups_);
    finalize(result.cores_);
    finalize(result.processing_units_);
    finalize(result.caches_);
    finalize(result.numa_nodes_);
    finalize(result.pci_devices_);
    const auto complete = []<typename Handle, typename SemanticId>(
                              const StableTopologyIds::Index<Handle, SemanticId>& index) {
        return index.entries.size() == index.forward.size() &&
               index.entries.size() == index.reverse.size();
    };
    if (!complete(result.machines_) || !complete(result.packages_) || !complete(result.dies_) ||
        !complete(result.groups_) || !complete(result.cores_) ||
        !complete(result.processing_units_) || !complete(result.caches_) ||
        !complete(result.numa_nodes_) || !complete(result.pci_devices_)) {
        return std::unexpected{stable_error("topology.stable_id.unsupported_object_state",
                                            "Stable identity index could not be completed",
                                            "snapshot", diagnostic_handle(topology.machine().id),
                                            "forward_reverse_index")};
    }
    return result;
}

auto derive_stable_topology_ids(const TopologySnapshot& topology)
    -> std::expected<StableTopologyIds, core::Error> {
    return detail::PreparedStableTopologyIdAccess::derive_with_failure_sink(topology, nullptr);
}

auto StableTopologyIds::machine_id() const noexcept -> const StableMachineId& {
    return machines_.entries.front().semantic_id;
}

#define EPYCBENCH_FORWARD_LOOKUP(method, member, handle_type, id_type)                             \
    auto StableTopologyIds::method(handle_type handle) const                                       \
        -> std::optional<std::reference_wrapper<const id_type>> {                                  \
        return lookup_forward(member.entries, member.forward, handle);                             \
    }

EPYCBENCH_FORWARD_LOOKUP(package_id, packages_, core::PackageId, StablePackageId)
EPYCBENCH_FORWARD_LOOKUP(die_id, dies_, core::DieId, StableDieId)
EPYCBENCH_FORWARD_LOOKUP(group_id, groups_, core::ArchitectureGroupId, StableGroupId)
EPYCBENCH_FORWARD_LOOKUP(core_id, cores_, core::CoreId, StableCoreId)
EPYCBENCH_FORWARD_LOOKUP(processing_unit_id, processing_units_, core::ProcessingUnitId,
                         StableProcessingUnitId)
EPYCBENCH_FORWARD_LOOKUP(cache_id, caches_, core::CacheId, StableCacheId)
EPYCBENCH_FORWARD_LOOKUP(numa_node_id, numa_nodes_, core::NumaNodeId, StableNumaNodeId)
EPYCBENCH_FORWARD_LOOKUP(pci_device_id, pci_devices_, PciAddress, StablePciDeviceId)

#undef EPYCBENCH_FORWARD_LOOKUP

#define EPYCBENCH_REVERSE_LOOKUP(method, member, handle_type, id_type)                             \
    auto StableTopologyIds::method(const id_type& id) const -> std::optional<handle_type> {        \
        return lookup_reverse(member.reverse, id);                                                 \
    }

EPYCBENCH_REVERSE_LOOKUP(machine, machines_, core::TopologyMachineId, StableMachineId)
EPYCBENCH_REVERSE_LOOKUP(package, packages_, core::PackageId, StablePackageId)
EPYCBENCH_REVERSE_LOOKUP(die, dies_, core::DieId, StableDieId)
EPYCBENCH_REVERSE_LOOKUP(group, groups_, core::ArchitectureGroupId, StableGroupId)
EPYCBENCH_REVERSE_LOOKUP(core, cores_, core::CoreId, StableCoreId)
EPYCBENCH_REVERSE_LOOKUP(processing_unit, processing_units_, core::ProcessingUnitId,
                         StableProcessingUnitId)
EPYCBENCH_REVERSE_LOOKUP(cache, caches_, core::CacheId, StableCacheId)
EPYCBENCH_REVERSE_LOOKUP(numa_node, numa_nodes_, core::NumaNodeId, StableNumaNodeId)
EPYCBENCH_REVERSE_LOOKUP(pci_device, pci_devices_, PciAddress, StablePciDeviceId)

#undef EPYCBENCH_REVERSE_LOOKUP

#define EPYCBENCH_ITERATION(method, member, entry_type)                                            \
    auto StableTopologyIds::method() const noexcept -> std::span<const entry_type> {               \
        return member.entries;                                                                     \
    }

EPYCBENCH_ITERATION(machines, machines_, StableMachineEntry)
EPYCBENCH_ITERATION(packages, packages_, StablePackageEntry)
EPYCBENCH_ITERATION(dies, dies_, StableDieEntry)
EPYCBENCH_ITERATION(groups, groups_, StableGroupEntry)
EPYCBENCH_ITERATION(cores, cores_, StableCoreEntry)
EPYCBENCH_ITERATION(processing_units, processing_units_, StableProcessingUnitEntry)
EPYCBENCH_ITERATION(caches, caches_, StableCacheEntry)
EPYCBENCH_ITERATION(numa_nodes, numa_nodes_, StableNumaNodeEntry)
EPYCBENCH_ITERATION(pci_devices, pci_devices_, StablePciDeviceEntry)

#undef EPYCBENCH_ITERATION

} // namespace epycbench::topology
