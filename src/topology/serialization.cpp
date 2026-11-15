#include "epycbench/topology/serialization.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/invariants.hpp"
#include "topology/topology_json_parser.hpp"
#include "core/json_ownership.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace epycbench::topology {
namespace {

using Json = nlohmann::json;

// Scoped to this supplied serialization boundary: only exclusively owned results.
template <class T> struct ResultCleanup {
    T& value;
    ~ResultCleanup() {
        if (!value) {
            if constexpr (requires { value.error().context; })
                core::detail::retire_json(value.error().context);
        } else if constexpr (std::is_same_v<typename T::value_type, Json>)
            core::detail::retire_json(*value);
    }
};
struct ReportCleanup {
    ValidationReport& value;
    ~ReportCleanup() {
        for (auto& error : value.diagnostics)
            core::detail::retire_json(error.context);
    }
};

constexpr std::string_view schema_token = "epycbench.topology.v1";

auto serialization_error(std::string code, std::string classification, std::string field = {})
    -> core::Error {
    Json context = core::detail::json_object<Json>([&](auto& object_1) {
        core::detail::set_json(object_1, "classification", std::move(classification));
    });
    core::detail::JsonCleanup context_cleanup{context};
    if (!field.empty())
        context["field"] = std::move(field);
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Topology serialization contract failed",
            .operation = "topology_serialization",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto invalid_type(std::string field, std::string classification = "invalid_type") -> core::Error {
    return serialization_error("topology.serialization.invalid_field_type",
                               std::move(classification), std::move(field));
}

auto expect_fields(const Json& value, std::initializer_list<std::string_view> expected,
                   std::string_view path) -> std::expected<void, core::Error> {
    if (!value.is_object())
        return std::unexpected{invalid_type(std::string{path})};
    std::set<std::string, std::less<>> allowed;
    for (const auto name : expected)
        allowed.emplace(name);
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        if (!allowed.contains(name))
            return std::unexpected{serialization_error("topology.serialization.unknown_field",
                                                       "unknown_field",
                                                       std::string{path} + "/" + name)};
    }
    for (const auto name : expected)
        if (!value.contains(name))
            return std::unexpected{
                serialization_error("topology.serialization.missing_field", "missing_field",
                                    std::string{path} + "/" + std::string{name})};
    return {};
}

auto source_token(ProvenanceSource source) -> std::string_view {
    switch (source) {
    case ProvenanceSource::hwloc:
        return "hwloc";
    case ProvenanceSource::linux_sysfs:
        return "linux_sysfs";
    case ProvenanceSource::linux_procfs:
        return "linux_procfs";
    case ProvenanceSource::numa_api:
        return "numa_api";
    case ProvenanceSource::architecture_specific:
        return "architecture_specific";
    case ProvenanceSource::fixture:
        return "fixture";
    case ProvenanceSource::derived:
        return "derived";
    case ProvenanceSource::user:
        return "user";
    }
    return "unknown";
}

auto confidence_token(Confidence confidence) -> std::string_view {
    switch (confidence) {
    case Confidence::reported:
        return "reported";
    case Confidence::derived:
        return "derived";
    case Confidence::inferred:
        return "inferred";
    case Confidence::user_supplied:
        return "user_supplied";
    case Confidence::unknown:
        return "unknown";
    }
    return "unknown";
}

auto parse_source(std::string_view value) -> std::optional<ProvenanceSource> {
    if (value == "hwloc")
        return ProvenanceSource::hwloc;
    if (value == "linux_sysfs")
        return ProvenanceSource::linux_sysfs;
    if (value == "linux_procfs")
        return ProvenanceSource::linux_procfs;
    if (value == "numa_api")
        return ProvenanceSource::numa_api;
    if (value == "architecture_specific")
        return ProvenanceSource::architecture_specific;
    if (value == "fixture")
        return ProvenanceSource::fixture;
    if (value == "derived")
        return ProvenanceSource::derived;
    if (value == "user")
        return ProvenanceSource::user;
    return std::nullopt;
}

auto parse_confidence(std::string_view value) -> std::optional<Confidence> {
    if (value == "reported")
        return Confidence::reported;
    if (value == "derived")
        return Confidence::derived;
    if (value == "inferred")
        return Confidence::inferred;
    if (value == "user_supplied")
        return Confidence::user_supplied;
    if (value == "unknown")
        return Confidence::unknown;
    return std::nullopt;
}

auto locator_safe(std::string_view locator) -> bool {
    if (locator.size() > detail::topology_json_max_locator_bytes)
        return false;
    if (locator.empty())
        return true;
    auto valid = core::canonicalize_json(Json(locator));
    ResultCleanup valid_cleanup{valid};
    if (!valid || locator.front() == '/' || locator.front() == '~' ||
        locator.find('\\') != std::string_view::npos ||
        locator.find("://") != std::string_view::npos)
        return false;
    for (const char raw : locator) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20U || c == 0x7fU)
            return false;
    }
    std::size_t start = 0;
    while (start <= locator.size()) {
        const auto end = locator.find('/', start);
        const auto segment = locator.substr(
            start, end == std::string_view::npos ? locator.size() - start : end - start);
        if (segment == "." || segment == ".." || segment == "home" || segment == "Users" ||
            segment == "tmp" || segment == "private" || segment == "staging")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1U;
    }
    const bool logical = locator.starts_with("hwloc:") || locator.starts_with("sys/") ||
                         locator.starts_with("proc/") || locator.starts_with("cgroup/") ||
                         locator.starts_with("sanitized/") || locator.starts_with("derived/");
    if (!logical)
        return false;
    std::string lowered{locator};
    std::ranges::transform(lowered, lowered.begin(), [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    constexpr std::array prohibited{"hostname", "machine-id", "machine_id",  "boot-id",
                                    "boot_id",  "serial",     "mac_address", "uuid",
                                    "guid",     "wwn",        "ip_address"};
    return std::ranges::none_of(prohibited,
                                [&](std::string_view token) { return lowered.contains(token); });
}

auto evidence_json(const Evidence& evidence) -> std::expected<Json, core::Error> {
    if (!locator_safe(evidence.locator))
        return std::unexpected{serialization_error("topology.serialization.privacy_violation",
                                                   "unsafe_locator", "evidence/locator")};
    return core::detail::json_object<Json>([&](auto& object_2) {
        core::detail::set_json(object_2, "source", source_token(evidence.source));
        core::detail::set_json(object_2, "locator", evidence.locator);
        core::detail::set_json(object_2, "confidence", confidence_token(evidence.confidence));
    });
}

auto evidence_array(std::vector<Evidence> evidence) -> std::expected<Json, core::Error> {
    if (evidence.size() > detail::topology_json_max_evidence_entries)
        return std::unexpected{
            serialization_error("topology.serialization.limit_exceeded", "evidence_entries")};
    std::ranges::sort(evidence);
    auto result = Json::array();
    core::detail::JsonCleanup result_cleanup{result};
    for (const auto& item : evidence) {
        auto encoded = evidence_json(item);
        ResultCleanup encoded_cleanup{encoded};
        if (!encoded)
            return std::unexpected{std::move(encoded.error())};
        result.push_back(std::move(*encoded));
    }
    return decltype(result)(std::move(result));
}

auto parse_evidence(const Json& value, std::string_view path)
    -> std::expected<Evidence, core::Error> {
    if (auto fields = expect_fields(value, {"source", "locator", "confidence"}, path); !fields)
        return std::unexpected{std::move(fields.error())};
    if (!value["source"].is_string() || !value["locator"].is_string() ||
        !value["confidence"].is_string())
        return std::unexpected{invalid_type(std::string{path})};
    const auto source = parse_source(value["source"].get_ref<const std::string&>());
    const auto confidence = parse_confidence(value["confidence"].get_ref<const std::string&>());
    if (!source || !confidence)
        return std::unexpected{serialization_error("topology.serialization.invalid_enum",
                                                   "provenance_enum", std::string{path})};
    const auto& locator = value["locator"].get_ref<const std::string&>();
    if (!locator_safe(locator))
        return std::unexpected{serialization_error("topology.serialization.privacy_violation",
                                                   "unsafe_locator",
                                                   std::string{path} + "/locator")};
    return Evidence{*source, locator, *confidence};
}

auto parse_evidence_array(const Json& value, std::string_view path)
    -> std::expected<std::vector<Evidence>, core::Error> {
    if (!value.is_array())
        return std::unexpected{invalid_type(std::string{path})};
    if (value.size() > detail::topology_json_max_evidence_entries)
        return std::unexpected{serialization_error("topology.serialization.limit_exceeded",
                                                   "evidence_entries", std::string{path})};
    std::vector<Evidence> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto parsed = parse_evidence(value[index], std::string{path} + "/" + std::to_string(index));
        ResultCleanup parsed_cleanup{parsed};
        if (!parsed)
            return std::unexpected{std::move(parsed.error())};
        result.push_back(std::move(*parsed));
    }
    return result;
}

template <typename T, typename Encode>
auto attribute_json(const Attribute<T>& attribute, Encode encode)
    -> std::expected<Json, core::Error> {
    if (attribute.availability() == Availability::conflicted) {
        if (attribute.conflicts().size() > detail::topology_json_max_evidence_entries)
            return std::unexpected{
                serialization_error("topology.serialization.limit_exceeded", "conflict_entries")};
        std::vector<std::tuple<std::string, Evidence, core::detail::OwnedJson<Json>>> alternatives;
        std::set<std::string, std::less<>> distinct_values;
        for (const auto& conflict : attribute.conflicts()) {
            Json encoded = encode(conflict.value);
            core::detail::JsonCleanup encoded_cleanup{encoded};
            auto canonical = core::canonicalize_json(encoded);
            ResultCleanup canonical_cleanup{canonical};
            if (!canonical)
                return std::unexpected{serialization_error(
                    "topology.serialization.canonicalization_failed", "attribute_value")};
            distinct_values.insert(*canonical);
            alternatives.emplace_back(*canonical, conflict.evidence, std::move(encoded));
        }
        if (distinct_values.size() < 2U)
            return std::unexpected{invalid_type("attribute/values", "conflict_not_distinct")};
        std::ranges::sort(alternatives, [](const auto& left, const auto& right) {
            return std::tie(std::get<0>(left), std::get<1>(left)) <
                   std::tie(std::get<0>(right), std::get<1>(right));
        });
        auto values = Json::array();
        core::detail::JsonCleanup values_cleanup{values};
        for (const auto& [unused, evidence, encoded] : alternatives) {
            static_cast<void>(unused);
            auto evidence_value = evidence_json(evidence);
            ResultCleanup evidence_value_cleanup{evidence_value};
            if (!evidence_value)
                return std::unexpected{std::move(evidence_value.error())};
            core::detail::append_object_json(values, [&](auto& object_3) {
                core::detail::set_json(object_3, "value", encoded.value);
                core::detail::set_json(object_3, "evidence", std::move(*evidence_value));
            });
        }
        return core::detail::json_object<Json>([&](auto& object_4) {
            core::detail::set_json(object_4, "state", "conflicted");
            core::detail::set_json(object_4, "values", std::move(values));
        });
    }
    auto evidence = evidence_array(attribute.evidence());
    ResultCleanup evidence_cleanup{evidence};
    if (!evidence)
        return std::unexpected{std::move(evidence.error())};
    if (attribute.availability() == Availability::available) {
        if (!attribute.value())
            return std::unexpected{invalid_type("attribute/value", "available_without_value")};
        return core::detail::json_object<Json>([&](auto& object_5) {
            core::detail::set_json(object_5, "state", "available");
            core::detail::set_json(object_5, "value", encode(*attribute.value()));
            core::detail::set_json(object_5, "evidence", std::move(*evidence));
        });
    }
    if (attribute.value())
        return std::unexpected{invalid_type("attribute/value", "nonavailable_with_value")};
    return core::detail::json_object<Json>([&](auto& object_6) {
        core::detail::set_json(object_6, "state",
                               attribute.availability() == Availability::unavailable ? "unavailable"
                                                                                     : "unknown");
        core::detail::set_json(object_6, "evidence", std::move(*evidence));
    });
}

template <typename T, typename Decode>
auto parse_attribute(const Json& value, Decode decode, std::string_view path)
    -> std::expected<Attribute<T>, core::Error> {
    if (!value.is_object() || !value.contains("state") || !value["state"].is_string())
        return std::unexpected{invalid_type(std::string{path}, "attribute_state")};
    const auto& state = value["state"].get_ref<const std::string&>();
    if (state == "available") {
        if (auto fields = expect_fields(value, {"state", "value", "evidence"}, path); !fields)
            return std::unexpected{std::move(fields.error())};
        auto decoded = decode(value["value"]);
        ResultCleanup decoded_cleanup{decoded};
        auto evidence = parse_evidence_array(value["evidence"], std::string{path} + "/evidence");
        ResultCleanup evidence_cleanup{evidence};
        if (!decoded)
            return std::unexpected{std::move(decoded.error())};
        if (!evidence)
            return std::unexpected{std::move(evidence.error())};
        return Attribute<T>::available(std::move(*decoded), std::move(*evidence));
    }
    if (state == "unavailable" || state == "unknown") {
        if (auto fields = expect_fields(value, {"state", "evidence"}, path); !fields)
            return std::unexpected{std::move(fields.error())};
        auto evidence = parse_evidence_array(value["evidence"], std::string{path} + "/evidence");
        ResultCleanup evidence_cleanup{evidence};
        if (!evidence)
            return std::unexpected{std::move(evidence.error())};
        return state == "unavailable" ? Attribute<T>::unavailable(std::move(*evidence))
                                      : Attribute<T>::unknown(std::move(*evidence));
    }
    if (state != "conflicted")
        return std::unexpected{serialization_error("topology.serialization.invalid_enum",
                                                   "availability", std::string{path} + "/state")};
    if (auto fields = expect_fields(value, {"state", "values"}, path); !fields)
        return std::unexpected{std::move(fields.error())};
    if (!value["values"].is_array() || value["values"].size() < 2U)
        return std::unexpected{invalid_type(std::string{path} + "/values", "conflict_count")};
    if (value["values"].size() > detail::topology_json_max_evidence_entries)
        return std::unexpected{serialization_error("topology.serialization.limit_exceeded",
                                                   "conflict_entries", std::string{path})};
    std::vector<ConflictingValue<T>> conflicts;
    std::set<std::string, std::less<>> distinct;
    for (std::size_t index = 0; index < value["values"].size(); ++index) {
        const auto& alternative = value["values"][index];
        const auto alternative_path = std::string{path} + "/values/" + std::to_string(index);
        if (auto fields = expect_fields(alternative, {"value", "evidence"}, alternative_path);
            !fields)
            return std::unexpected{std::move(fields.error())};
        auto decoded = decode(alternative["value"]);
        ResultCleanup decoded_cleanup{decoded};
        auto evidence = parse_evidence(alternative["evidence"], alternative_path + "/evidence");
        ResultCleanup evidence_cleanup{evidence};
        auto canonical = core::canonicalize_json(alternative["value"]);
        ResultCleanup canonical_cleanup{canonical};
        if (!decoded)
            return std::unexpected{std::move(decoded.error())};
        if (!evidence)
            return std::unexpected{std::move(evidence.error())};
        if (!canonical)
            return std::unexpected{serialization_error(
                "topology.serialization.canonicalization_failed", "conflict_value")};
        distinct.insert(*canonical);
        conflicts.push_back({std::move(*decoded), std::move(*evidence)});
    }
    if (distinct.size() < 2U)
        return std::unexpected{
            invalid_type(std::string{path} + "/values", "conflict_not_distinct")};
    auto first = std::move(conflicts[0]);
    auto second = std::move(conflicts[1]);
    std::vector<ConflictingValue<T>> additional;
    additional.insert(additional.end(), std::make_move_iterator(conflicts.begin() + 2),
                      std::make_move_iterator(conflicts.end()));
    return Attribute<T>::conflicted(std::move(first), std::move(second), std::move(additional));
}

auto byte_json(core::ByteCount value) -> Json { return std::to_string(value.value()); }

auto parse_byte(const Json& value) -> std::expected<core::ByteCount, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type("byte_count")};
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return std::unexpected{invalid_type("byte_count", "noncanonical_decimal")};
    std::uint64_t parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::unexpected{invalid_type("byte_count", "invalid_decimal")};
    return core::ByteCount::from_base_units(parsed);
}

auto parse_u32(const Json& value) -> std::expected<std::uint32_t, core::Error> {
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{invalid_type("unsigned_integer")};
    return static_cast<std::uint32_t>(value.get<std::uint64_t>());
}

auto parse_bool(const Json& value) -> std::expected<bool, core::Error> {
    if (!value.is_boolean())
        return std::unexpected{invalid_type("boolean")};
    return value.get<bool>();
}

auto parse_string(const Json& value) -> std::expected<std::string, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type("string")};
    return value.get<std::string>();
}

template <typename Set> auto parse_range_set(const Json& value) -> std::expected<Set, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type("range_list")};
    const auto& text = value.get_ref<const std::string&>();
    auto parsed = Set::parse(text);
    if (!parsed || parsed->to_range_list() != text)
        return std::unexpected{invalid_type("range_list", "invalid_range_list")};
    return *parsed;
}

auto normalized_pages(const std::vector<core::ByteCount>& input)
    -> std::expected<Json, core::Error> {
    auto pages = input;
    std::ranges::sort(pages);
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    auto result = Json::array();
    core::detail::JsonCleanup result_cleanup{result};
    for (const auto page : pages) {
        if (page.value() == 0U)
            return std::unexpected{invalid_type("page_sizes_bytes", "zero_page_size")};
        result.push_back(byte_json(page));
    }
    return decltype(result)(std::move(result));
}

auto parse_pages(const Json& value, std::string_view path)
    -> std::expected<std::vector<core::ByteCount>, core::Error> {
    if (!value.is_array())
        return std::unexpected{invalid_type(std::string{path})};
    std::vector<core::ByteCount> result;
    for (const auto& item : value) {
        auto page = parse_byte(item);
        ResultCleanup page_cleanup{page};
        if (!page || page->value() == 0U)
            return std::unexpected{invalid_type(std::string{path}, "invalid_page_size")};
        result.push_back(*page);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

template <typename Entry>
auto entries_equal(std::span<const Entry> left, std::span<const Entry> right) -> bool {
    return std::ranges::equal(left, right);
}

auto stable_ids_equal(const StableTopologyIds& left, const StableTopologyIds& right) -> bool {
    return entries_equal(left.machines(), right.machines()) &&
           entries_equal(left.packages(), right.packages()) &&
           entries_equal(left.dies(), right.dies()) &&
           entries_equal(left.groups(), right.groups()) &&
           entries_equal(left.cores(), right.cores()) &&
           entries_equal(left.processing_units(), right.processing_units()) &&
           entries_equal(left.caches(), right.caches()) &&
           entries_equal(left.numa_nodes(), right.numa_nodes()) &&
           entries_equal(left.pci_devices(), right.pci_devices());
}

template <typename SemanticId>
auto id_value(const std::optional<std::reference_wrapper<const SemanticId>>& id)
    -> std::string_view {
    return id->get().value();
}

auto semantic_pus(const ProcessingUnitSet& members, const StableTopologyIds& ids)
    -> std::expected<Json, core::Error> {
    std::vector<std::string> values;
    for (const auto member : members) {
        const auto id = ids.processing_unit_id(member);
        if (!id)
            return std::unexpected{serialization_error("topology.serialization.stable_ids_mismatch",
                                                       "missing_pu_mapping")};
        values.emplace_back(id->get().value());
    }
    std::ranges::sort(values);
    auto result = Json::array();
    core::detail::JsonCleanup cleanup{result};
    for (const auto& value : values)
        result.push_back(value);
    return Json(std::move(result));
}

auto cache_kind_token(CacheKind kind) -> std::string_view {
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

auto parse_cache_kind(std::string_view value) -> std::optional<CacheKind> {
    if (value == "data")
        return CacheKind::data;
    if (value == "instruction")
        return CacheKind::instruction;
    if (value == "unified")
        return CacheKind::unified;
    if (value == "unknown")
        return CacheKind::unknown;
    return std::nullopt;
}

template <typename T>
auto check_attribute_privacy(const Attribute<T>& attribute) -> std::expected<void, core::Error> {
    for (const auto& item : attribute.evidence())
        if (!locator_safe(item.locator))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& item : attribute.conflicts())
        if (!locator_safe(item.evidence.locator))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    return {};
}

auto check_privacy(const TopologySnapshot& topology) -> std::expected<void, core::Error> {
    const auto& machine = topology.machine();
    if (!check_attribute_privacy(machine.cpu_vendor) ||
        !check_attribute_privacy(machine.cpu_model) ||
        !check_attribute_privacy(machine.total_memory) ||
        !check_attribute_privacy(machine.allowed_cpus) ||
        !check_attribute_privacy(machine.effective_cpus) ||
        !check_attribute_privacy(machine.allowed_nodes) ||
        !check_attribute_privacy(machine.effective_nodes))
        return std::unexpected{
            serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.packages())
        if (!check_attribute_privacy(value.os_id))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.dies())
        if (!check_attribute_privacy(value.os_id))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.cores())
        if (!check_attribute_privacy(value.os_id))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.processing_units())
        if (!check_attribute_privacy(value.isolated))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.numa_nodes())
        if (!check_attribute_privacy(value.linux_node_id) ||
            !check_attribute_privacy(value.installed_memory) ||
            !check_attribute_privacy(value.available_memory))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.pci_devices())
        if (!check_attribute_privacy(value.numa_locality) ||
            !check_attribute_privacy(value.local_pus))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.architecture_groups())
        for (const auto& item : value.provenance)
            if (!locator_safe(item.locator))
                return std::unexpected{serialization_error(
                    "topology.serialization.privacy_violation", "unsafe_locator")};
    for (const auto& value : topology.caches()) {
        if (!check_attribute_privacy(value.associativity))
            return std::unexpected{
                serialization_error("topology.serialization.privacy_violation", "unsafe_locator")};
        for (const auto& item : value.provenance)
            if (!locator_safe(item.locator))
                return std::unexpected{serialization_error(
                    "topology.serialization.privacy_violation", "unsafe_locator")};
    }
    return {};
}

} // namespace

namespace {

auto topology_json(const TopologySnapshot& topology, const StableTopologyIds& ids,
                   const TopologyFingerprints& fingerprints) -> std::expected<Json, core::Error>;

template <typename StableId>
auto validate_semantic_id(const Json& value, std::string_view path)
    -> std::expected<std::string, core::Error>;

template <typename Handle>
auto assign_ids(const Json& values, auto parse_id, std::string_view path)
    -> std::expected<std::map<std::string, Handle, std::less<>>, core::Error>;

template <typename Handle>
auto reference(const Json& value, const std::map<std::string, Handle, std::less<>>& ids,
               std::string_view path) -> std::expected<Handle, core::Error>;

auto pu_members(const Json& value,
                const std::map<std::string, core::ProcessingUnitId, std::less<>>& ids,
                std::string_view path) -> std::expected<ProcessingUnitSet, core::Error>;

auto parse_hex(const Json& value, std::size_t width, std::string_view path)
    -> std::expected<std::uint32_t, core::Error>;

struct IdMaps {
    std::map<std::string, core::PackageId, std::less<>> packages;
    std::map<std::string, core::DieId, std::less<>> dies;
    std::map<std::string, core::ArchitectureGroupId, std::less<>> groups;
    std::map<std::string, core::CoreId, std::less<>> cores;
    std::map<std::string, core::ProcessingUnitId, std::less<>> pus;
    std::map<std::string, core::CacheId, std::less<>> caches;
    std::map<std::string, core::NumaNodeId, std::less<>> nodes;
    std::map<std::string, PciAddress, std::less<>> pci;
};

auto build_id_maps(const Json& objects) -> std::expected<IdMaps, core::Error> {
    auto package = [](const auto& value, const auto& path) {
        return validate_semantic_id<StablePackageId>(value, path);
    };
    auto die = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableDieId>(value, path);
    };
    auto group = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableGroupId>(value, path);
    };
    auto core = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableCoreId>(value, path);
    };
    auto pu = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableProcessingUnitId>(value, path);
    };
    auto cache = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableCacheId>(value, path);
    };
    auto node = [](const auto& value, const auto& path) {
        return validate_semantic_id<StableNumaNodeId>(value, path);
    };
    auto packages = assign_ids<core::PackageId>(objects["packages"], package, "/objects/packages");
    ResultCleanup packages_cleanup{packages};
    auto dies = assign_ids<core::DieId>(objects["dies"], die, "/objects/dies");
    ResultCleanup dies_cleanup{dies};
    auto groups =
        assign_ids<core::ArchitectureGroupId>(objects["groups"], group, "/objects/groups");
    ResultCleanup groups_cleanup{groups};
    auto cores = assign_ids<core::CoreId>(objects["cores"], core, "/objects/cores");
    ResultCleanup cores_cleanup{cores};
    auto pus = assign_ids<core::ProcessingUnitId>(objects["processing_units"], pu,
                                                  "/objects/processing_units");
    ResultCleanup pus_cleanup{pus};
    auto caches = assign_ids<core::CacheId>(objects["caches"], cache, "/objects/caches");
    ResultCleanup caches_cleanup{caches};
    auto nodes = assign_ids<core::NumaNodeId>(objects["numa_nodes"], node, "/objects/numa_nodes");
    ResultCleanup nodes_cleanup{nodes};
    if (!packages || !dies || !groups || !cores || !pus || !caches || !nodes)
        return std::unexpected{!packages ? std::move(packages.error())
                               : !dies   ? std::move(dies.error())
                               : !groups ? std::move(groups.error())
                               : !cores  ? std::move(cores.error())
                               : !pus    ? std::move(pus.error())
                               : !caches ? std::move(caches.error())
                                         : std::move(nodes.error())};

    std::map<std::string, PciAddress, std::less<>> pci;
    const auto& pci_values = objects["pci_functions"];
    if (!pci_values.is_array())
        return std::unexpected{invalid_type("/objects/pci_functions")};
    if (pci_values.size() > detail::topology_json_max_category_objects)
        return std::unexpected{serialization_error("topology.serialization.limit_exceeded",
                                                   "category_objects", "/objects/pci_functions")};
    for (std::size_t index = 0; index < pci_values.size(); ++index) {
        if (!pci_values[index].is_object() || !pci_values[index].contains("id"))
            return std::unexpected{serialization_error("topology.serialization.missing_field",
                                                       "missing_id", "/objects/pci_functions")};
        auto id = validate_semantic_id<StablePciDeviceId>(
            pci_values[index]["id"], "/objects/pci_functions/" + std::to_string(index) + "/id");
        ResultCleanup id_cleanup{id};
        if (!id)
            return std::unexpected{std::move(id.error())};
        auto address = PciAddress::parse(std::string_view{*id}.substr(4));
        if (!address)
            return std::unexpected{serialization_error("topology.serialization.invalid_semantic_id",
                                                       "pci_bdf", "/objects/pci_functions")};
        if (!pci.emplace(*id, *address).second)
            return std::unexpected{serialization_error("topology.serialization.duplicate_object_id",
                                                       "duplicate_id", "/objects/pci_functions")};
    }

    const std::size_t total = packages->size() + dies->size() + groups->size() + cores->size() +
                              pus->size() + caches->size() + nodes->size() + pci.size();
    if (total > detail::topology_json_max_total_objects)
        return std::unexpected{
            serialization_error("topology.serialization.limit_exceeded", "total_objects")};
    return IdMaps{std::move(*packages), std::move(*dies),   std::move(*groups), std::move(*cores),
                  std::move(*pus),      std::move(*caches), std::move(*nodes),  std::move(pci)};
}

auto build_snapshot(const Json& root, const IdMaps& ids)
    -> std::expected<TopologySnapshot, core::Error> {
    const auto& machine_json = root["machine"];
    if (auto fields = expect_fields(
            machine_json,
            {"id", "architecture", "cpu_vendor", "cpu_model", "total_memory", "page_sizes_bytes",
             "present_cpus", "online_cpus", "allowed_cpus", "effective_cpus", "present_nodes",
             "online_nodes", "allowed_nodes", "effective_nodes"},
            "/machine");
        !fields)
        return std::unexpected{std::move(fields.error())};
    auto machine_id = validate_semantic_id<StableMachineId>(machine_json["id"], "/machine/id");
    ResultCleanup machine_id_cleanup{machine_id};
    if (!machine_id)
        return std::unexpected{std::move(machine_id.error())};
    auto architecture = parse_string(machine_json["architecture"]);
    ResultCleanup architecture_cleanup{architecture};
    auto vendor = parse_attribute<std::string>(machine_json["cpu_vendor"], parse_string,
                                               "/machine/cpu_vendor");
    ResultCleanup vendor_cleanup{vendor};
    auto model =
        parse_attribute<std::string>(machine_json["cpu_model"], parse_string, "/machine/cpu_model");
    ResultCleanup model_cleanup{model};
    auto memory = parse_attribute<core::ByteCount>(machine_json["total_memory"], parse_byte,
                                                   "/machine/total_memory");
    ResultCleanup memory_cleanup{memory};
    auto pages = parse_pages(machine_json["page_sizes_bytes"], "/machine/page_sizes_bytes");
    ResultCleanup pages_cleanup{pages};
    auto present_cpus = parse_range_set<core::CpuSet>(machine_json["present_cpus"]);
    ResultCleanup present_cpus_cleanup{present_cpus};
    auto online_cpus = parse_range_set<core::CpuSet>(machine_json["online_cpus"]);
    ResultCleanup online_cpus_cleanup{online_cpus};
    auto allowed_cpus = parse_attribute<core::CpuSet>(
        machine_json["allowed_cpus"], parse_range_set<core::CpuSet>, "/machine/allowed_cpus");
    ResultCleanup allowed_cpus_cleanup{allowed_cpus};
    auto effective_cpus = parse_attribute<core::CpuSet>(
        machine_json["effective_cpus"], parse_range_set<core::CpuSet>, "/machine/effective_cpus");
    ResultCleanup effective_cpus_cleanup{effective_cpus};
    auto present_nodes = parse_range_set<core::NodeSet>(machine_json["present_nodes"]);
    ResultCleanup present_nodes_cleanup{present_nodes};
    auto online_nodes = parse_range_set<core::NodeSet>(machine_json["online_nodes"]);
    ResultCleanup online_nodes_cleanup{online_nodes};
    auto allowed_nodes = parse_attribute<core::NodeSet>(
        machine_json["allowed_nodes"], parse_range_set<core::NodeSet>, "/machine/allowed_nodes");
    ResultCleanup allowed_nodes_cleanup{allowed_nodes};
    auto effective_nodes =
        parse_attribute<core::NodeSet>(machine_json["effective_nodes"],
                                       parse_range_set<core::NodeSet>, "/machine/effective_nodes");
    ResultCleanup effective_nodes_cleanup{effective_nodes};
    if (!architecture || !vendor || !model || !memory || !pages || !present_cpus || !online_cpus ||
        !allowed_cpus || !effective_cpus || !present_nodes || !online_nodes || !allowed_nodes ||
        !effective_nodes)
        return std::unexpected{!architecture     ? std::move(architecture.error())
                               : !vendor         ? std::move(vendor.error())
                               : !model          ? std::move(model.error())
                               : !memory         ? std::move(memory.error())
                               : !pages          ? std::move(pages.error())
                               : !present_cpus   ? std::move(present_cpus.error())
                               : !online_cpus    ? std::move(online_cpus.error())
                               : !allowed_cpus   ? std::move(allowed_cpus.error())
                               : !effective_cpus ? std::move(effective_cpus.error())
                               : !present_nodes  ? std::move(present_nodes.error())
                               : !online_nodes   ? std::move(online_nodes.error())
                               : !allowed_nodes  ? std::move(allowed_nodes.error())
                                                 : std::move(effective_nodes.error())};

    TopologySnapshotData data{.machine = {.id = core::TopologyMachineId{0},
                                          .architecture = std::move(*architecture),
                                          .cpu_vendor = std::move(*vendor),
                                          .cpu_model = std::move(*model),
                                          .total_memory = std::move(*memory),
                                          .page_sizes = std::move(*pages),
                                          .present_cpus = std::move(*present_cpus),
                                          .online_cpus = std::move(*online_cpus),
                                          .allowed_cpus = std::move(*allowed_cpus),
                                          .effective_cpus = std::move(*effective_cpus),
                                          .present_nodes = std::move(*present_nodes),
                                          .online_nodes = std::move(*online_nodes),
                                          .allowed_nodes = std::move(*allowed_nodes),
                                          .effective_nodes = std::move(*effective_nodes)},
                              .packages = {},
                              .dies = {},
                              .architecture_groups = {},
                              .cores = {},
                              .processing_units = {},
                              .caches = {},
                              .numa_nodes = {},
                              .numa_distances = {},
                              .pci_devices = {}};

    const auto& objects = root["objects"];
    for (std::size_t index = 0; index < objects["packages"].size(); ++index) {
        const auto& value = objects["packages"][index];
        if (auto fields = expect_fields(value, {"id", "os_id"}, "/objects/packages"); !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.packages.at(value["id"].get<std::string>());
        auto os_id =
            parse_attribute<std::uint32_t>(value["os_id"], parse_u32, "/objects/packages/os_id");
        ResultCleanup os_id_cleanup{os_id};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        data.packages.push_back({id, std::move(*os_id)});
    }
    for (const auto& value : objects["dies"]) {
        if (auto fields =
                expect_fields(value, {"id", "package", "group", "os_id"}, "/objects/dies");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.dies.at(value["id"].get<std::string>());
        auto package = reference(value["package"], ids.packages, "/objects/dies/package");
        ResultCleanup package_cleanup{package};
        std::optional<core::ArchitectureGroupId> group;
        if (!value["group"].is_null()) {
            auto parsed = reference(value["group"], ids.groups, "/objects/dies/group");
            ResultCleanup parsed_cleanup{parsed};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            group = *parsed;
        }
        auto os_id =
            parse_attribute<std::uint32_t>(value["os_id"], parse_u32, "/objects/dies/os_id");
        ResultCleanup os_id_cleanup{os_id};
        if (!package)
            return std::unexpected{std::move(package.error())};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        data.dies.push_back({id, *package, std::move(*os_id), group});
    }
    for (const auto& value : objects["groups"]) {
        if (auto fields = expect_fields(
                value,
                {"id", "package", "parent", "kind", "subtype", "processing_units", "provenance"},
                "/objects/groups");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.groups.at(value["id"].get<std::string>());
        auto package = reference(value["package"], ids.packages, "/objects/groups/package");
        ResultCleanup package_cleanup{package};
        std::optional<core::ArchitectureGroupId> parent;
        if (!value["parent"].is_null()) {
            auto parsed = reference(value["parent"], ids.groups, "/objects/groups/parent");
            ResultCleanup parsed_cleanup{parsed};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            parent = *parsed;
        }
        auto kind = parse_string(value["kind"]);
        ResultCleanup kind_cleanup{kind};
        std::optional<std::string> subtype;
        if (!value["subtype"].is_null()) {
            auto parsed = parse_string(value["subtype"]);
            ResultCleanup parsed_cleanup{parsed};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            subtype = std::move(*parsed);
        }
        auto members =
            pu_members(value["processing_units"], ids.pus, "/objects/groups/processing_units");
        ResultCleanup members_cleanup{members};
        auto provenance = parse_evidence_array(value["provenance"], "/objects/groups/provenance");
        ResultCleanup provenance_cleanup{provenance};
        if (!package)
            return std::unexpected{std::move(package.error())};
        if (!kind)
            return std::unexpected{std::move(kind.error())};
        if (!members)
            return std::unexpected{std::move(members.error())};
        if (!provenance)
            return std::unexpected{std::move(provenance.error())};
        data.architecture_groups.push_back({id, *package, parent, std::move(*kind),
                                            std::move(subtype), std::move(*members),
                                            std::move(*provenance)});
    }
    for (const auto& value : objects["cores"]) {
        if (auto fields = expect_fields(value, {"id", "package", "die", "os_id"}, "/objects/cores");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.cores.at(value["id"].get<std::string>());
        auto package = reference(value["package"], ids.packages, "/objects/cores/package");
        ResultCleanup package_cleanup{package};
        std::optional<core::DieId> die;
        if (!value["die"].is_null()) {
            auto parsed = reference(value["die"], ids.dies, "/objects/cores/die");
            ResultCleanup parsed_cleanup{parsed};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            die = *parsed;
        }
        auto os_id =
            parse_attribute<std::uint32_t>(value["os_id"], parse_u32, "/objects/cores/os_id");
        ResultCleanup os_id_cleanup{os_id};
        if (!package)
            return std::unexpected{std::move(package.error())};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        data.cores.push_back({id, *package, die, std::move(*os_id)});
    }
    for (const auto& value : objects["processing_units"]) {
        if (auto fields = expect_fields(value, {"id", "linux_cpu", "core", "isolated"},
                                        "/objects/processing_units");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.pus.at(value["id"].get<std::string>());
        auto linux_cpu = parse_u32(value["linux_cpu"]);
        ResultCleanup linux_cpu_cleanup{linux_cpu};
        auto core_id = reference(value["core"], ids.cores, "/objects/processing_units/core");
        ResultCleanup core_id_cleanup{core_id};
        auto isolated = parse_attribute<bool>(value["isolated"], parse_bool,
                                              "/objects/processing_units/isolated");
        ResultCleanup isolated_cleanup{isolated};
        if (!linux_cpu)
            return std::unexpected{std::move(linux_cpu.error())};
        if (!core_id)
            return std::unexpected{std::move(core_id.error())};
        if (!isolated)
            return std::unexpected{std::move(isolated.error())};
        data.processing_units.push_back(
            {id, core::LinuxCpuId{*linux_cpu}, *core_id, std::move(*isolated)});
    }
    for (const auto& value : objects["caches"]) {
        if (auto fields = expect_fields(value,
                                        {"id", "level", "kind", "size_bytes", "line_size_bytes",
                                         "associativity", "processing_units", "provenance"},
                                        "/objects/caches");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.caches.at(value["id"].get<std::string>());
        auto level = parse_u32(value["level"]);
        ResultCleanup level_cleanup{level};
        if (!value["kind"].is_string())
            return std::unexpected{invalid_type("/objects/caches/kind")};
        auto kind = parse_cache_kind(value["kind"].get_ref<const std::string&>());
        ResultCleanup kind_cleanup{kind};
        if (!kind)
            return std::unexpected{serialization_error("topology.serialization.invalid_enum",
                                                       "cache_kind", "/objects/caches/kind")};
        auto size = parse_byte(value["size_bytes"]);
        ResultCleanup size_cleanup{size};
        auto line_size = parse_byte(value["line_size_bytes"]);
        ResultCleanup line_size_cleanup{line_size};
        auto associativity = parse_attribute<std::uint32_t>(value["associativity"], parse_u32,
                                                            "/objects/caches/associativity");
        ResultCleanup associativity_cleanup{associativity};
        auto members =
            pu_members(value["processing_units"], ids.pus, "/objects/caches/processing_units");
        ResultCleanup members_cleanup{members};
        auto provenance = parse_evidence_array(value["provenance"], "/objects/caches/provenance");
        ResultCleanup provenance_cleanup{provenance};
        if (!level)
            return std::unexpected{std::move(level.error())};
        if (!size)
            return std::unexpected{std::move(size.error())};
        if (!line_size)
            return std::unexpected{std::move(line_size.error())};
        if (!associativity)
            return std::unexpected{std::move(associativity.error())};
        if (!members)
            return std::unexpected{std::move(members.error())};
        if (!provenance)
            return std::unexpected{std::move(provenance.error())};
        data.caches.push_back({id, *level, *kind, *size, *line_size, std::move(*associativity),
                               std::move(*members), std::move(*provenance)});
    }
    for (const auto& value : objects["numa_nodes"]) {
        if (auto fields = expect_fields(value,
                                        {"id", "linux_node", "installed_memory", "available_memory",
                                         "processing_units", "page_sizes_bytes"},
                                        "/objects/numa_nodes");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto id = ids.nodes.at(value["id"].get<std::string>());
        auto linux_node = parse_attribute<std::uint32_t>(value["linux_node"], parse_u32,
                                                         "/objects/numa_nodes/linux_node");
        ResultCleanup linux_node_cleanup{linux_node};
        auto installed = parse_attribute<core::ByteCount>(value["installed_memory"], parse_byte,
                                                          "/objects/numa_nodes/installed_memory");
        ResultCleanup installed_cleanup{installed};
        auto available = parse_attribute<core::ByteCount>(value["available_memory"], parse_byte,
                                                          "/objects/numa_nodes/available_memory");
        ResultCleanup available_cleanup{available};
        auto members =
            pu_members(value["processing_units"], ids.pus, "/objects/numa_nodes/processing_units");
        ResultCleanup members_cleanup{members};
        auto node_pages =
            parse_pages(value["page_sizes_bytes"], "/objects/numa_nodes/page_sizes_bytes");
        ResultCleanup node_pages_cleanup{node_pages};
        if (!linux_node)
            return std::unexpected{std::move(linux_node.error())};
        if (!installed)
            return std::unexpected{std::move(installed.error())};
        if (!available)
            return std::unexpected{std::move(available.error())};
        if (!members)
            return std::unexpected{std::move(members.error())};
        if (!node_pages)
            return std::unexpected{std::move(node_pages.error())};
        data.numa_nodes.push_back({id, std::move(*linux_node), std::move(*installed),
                                   std::move(*available), std::move(*members),
                                   std::move(*node_pages)});
    }
    for (const auto& value : objects["pci_functions"]) {
        if (auto fields =
                expect_fields(value,
                              {"id", "vendor_id", "device_id", "base_class", "subclass",
                               "programming_interface", "numa_locality", "local_processing_units"},
                              "/objects/pci_functions");
            !fields)
            return std::unexpected{std::move(fields.error())};
        const auto& text_id = value["id"].get_ref<const std::string&>();
        auto pci_vendor = parse_hex(value["vendor_id"], 4U, "/objects/pci_functions/vendor_id");
        ResultCleanup pci_vendor_cleanup{pci_vendor};
        auto device = parse_hex(value["device_id"], 4U, "/objects/pci_functions/device_id");
        ResultCleanup device_cleanup{device};
        auto base = parse_hex(value["base_class"], 2U, "/objects/pci_functions/base_class");
        ResultCleanup base_cleanup{base};
        auto subclass = parse_hex(value["subclass"], 2U, "/objects/pci_functions/subclass");
        ResultCleanup subclass_cleanup{subclass};
        auto interface = parse_hex(value["programming_interface"], 2U,
                                   "/objects/pci_functions/programming_interface");
        ResultCleanup interface_cleanup{interface};
        auto numa = parse_attribute<core::NumaNodeId>(
            value["numa_locality"],
            [&](const Json& item) {
                return reference(item, ids.nodes, "/pci/numa_locality/value");
            },
            "/objects/pci_functions/numa_locality");
        ResultCleanup numa_cleanup{numa};
        auto local = parse_attribute<ProcessingUnitSet>(
            value["local_processing_units"],
            [&](const Json& item) { return pu_members(item, ids.pus, "/pci/local_pus/value"); },
            "/objects/pci_functions/local_processing_units");
        ResultCleanup local_cleanup{local};
        if (!pci_vendor)
            return std::unexpected{std::move(pci_vendor.error())};
        if (!device)
            return std::unexpected{std::move(device.error())};
        if (!base)
            return std::unexpected{std::move(base.error())};
        if (!subclass)
            return std::unexpected{std::move(subclass.error())};
        if (!interface)
            return std::unexpected{std::move(interface.error())};
        if (!numa)
            return std::unexpected{std::move(numa.error())};
        if (!local)
            return std::unexpected{std::move(local.error())};
        const auto class_code = (*base << 16U) | (*subclass << 8U) | *interface;
        data.pci_devices.push_back({ids.pci.at(text_id), static_cast<std::uint16_t>(*pci_vendor),
                                    static_cast<std::uint16_t>(*device), class_code,
                                    std::move(*numa), std::move(*local)});
    }

    if (!root["distances"].is_array())
        return std::unexpected{invalid_type("/distances")};
    if (root["distances"].size() > detail::topology_json_max_distances)
        return std::unexpected{
            serialization_error("topology.serialization.limit_exceeded", "distances")};
    std::set<std::pair<core::NumaNodeId, core::NumaNodeId>> distance_keys;
    for (const auto& value : root["distances"]) {
        if (auto fields =
                expect_fields(value, {"from", "to", "meaning", "unit", "value"}, "/distances");
            !fields)
            return std::unexpected{std::move(fields.error())};
        auto from = reference(value["from"], ids.nodes, "/distances/from");
        ResultCleanup from_cleanup{from};
        auto to = reference(value["to"], ids.nodes, "/distances/to");
        ResultCleanup to_cleanup{to};
        if (!value["meaning"].is_string() ||
            !core::detail::json_string_equal(value["meaning"], "latency") ||
            !value["unit"].is_string() ||
            !core::detail::json_string_equal(value["unit"], "relative"))
            return std::unexpected{serialization_error("topology.serialization.invalid_enum",
                                                       "distance_semantics", "/distances")};
        auto distance = parse_u32(value["value"]);
        ResultCleanup distance_cleanup{distance};
        if (!from)
            return std::unexpected{std::move(from.error())};
        if (!to)
            return std::unexpected{std::move(to.error())};
        if (!distance)
            return std::unexpected{std::move(distance.error())};
        if (!distance_keys.emplace(*from, *to).second)
            return std::unexpected{
                serialization_error("topology.serialization.duplicate_relationship",
                                    "duplicate_distance", "/distances")};
        data.numa_distances.directed_distances.push_back({*from, *to, *distance});
    }
    for (const auto& [unused, id] : ids.nodes) {
        static_cast<void>(unused);
        data.numa_distances.nodes.push_back(id);
    }

    auto created = TopologySnapshot::create(std::move(data));
    if (!created)
        return std::unexpected{
            serialization_error("topology.serialization.model_construction_failed", "owned_model")};
    return std::move(*created);
}

auto validate_root(const Json& root) -> std::expected<void, core::Error> {
    if (auto fields =
            expect_fields(root, {"schema", "fingerprints", "machine", "objects", "distances"}, "");
        !fields)
        return fields;
    if (!root["schema"].is_string())
        return std::unexpected{invalid_type("/schema")};
    if (!core::detail::json_string_equal(root["schema"], schema_token))
        return std::unexpected{
            serialization_error("topology.serialization.unsupported_schema", "schema", "/schema")};
    if (auto fields =
            expect_fields(root["fingerprints"], {"physical", "effective"}, "/fingerprints");
        !fields)
        return fields;
    if (!root["fingerprints"]["physical"].is_string() ||
        !root["fingerprints"]["effective"].is_string())
        return std::unexpected{invalid_type("/fingerprints")};
    if (auto fields = expect_fields(root["objects"],
                                    {"packages", "dies", "groups", "cores", "processing_units",
                                     "caches", "numa_nodes", "pci_functions"},
                                    "/objects");
        !fields)
        return fields;
    return {};
}

template <typename Entries, typename Map>
auto semantic_map_matches(const Entries& entries, const Map& values) -> bool {
    if (entries.size() != values.size())
        return false;
    return std::ranges::all_of(entries, [&](const auto& entry) {
        const auto found = values.find(entry.semantic_id.value());
        return found != values.end() && found->second == entry.handle;
    });
}

auto serialized_ids_match(const IdMaps& serialized, const StableTopologyIds& derived) -> bool {
    return derived.machine_id().value() == "machine" &&
           semantic_map_matches(derived.packages(), serialized.packages) &&
           semantic_map_matches(derived.dies(), serialized.dies) &&
           semantic_map_matches(derived.groups(), serialized.groups) &&
           semantic_map_matches(derived.cores(), serialized.cores) &&
           semantic_map_matches(derived.processing_units(), serialized.pus) &&
           semantic_map_matches(derived.caches(), serialized.caches) &&
           semantic_map_matches(derived.numa_nodes(), serialized.nodes) &&
           semantic_map_matches(derived.pci_devices(), serialized.pci);
}

auto fingerprint_text_valid(std::string_view text) -> bool {
    if (!text.starts_with("sha256:") || text.size() != 71U)
        return false;
    auto parsed_digest = core::Sha256Digest::parse(text.substr(7));
    core::detail::ExpectedErrorCleanup cleanup{parsed_digest};
    return parsed_digest.has_value();
}

} // namespace

auto serialize_topology(const TopologySnapshot& topology, const StableTopologyIds& stable_ids,
                        const TopologyFingerprints& fingerprints)
    -> std::expected<SerializedTopology, core::Error> {
    if (auto privacy = check_privacy(topology); !privacy)
        return std::unexpected{std::move(privacy.error())};
    auto validation = validate_topology(topology);
    ReportCleanup validation_cleanup{validation};
    if (!validation.valid())
        return std::unexpected{
            serialization_error("topology.serialization.invalid_topology", "invariants")};
    auto derived_ids = derive_stable_topology_ids(topology);
    ResultCleanup derived_ids_cleanup{derived_ids};
    if (!derived_ids || !stable_ids_equal(stable_ids, *derived_ids))
        return std::unexpected{
            serialization_error("topology.serialization.stable_ids_mismatch", "mapping")};
    auto derived_fingerprints = derive_topology_fingerprints(topology, *derived_ids);
    ResultCleanup derived_fingerprints_cleanup{derived_fingerprints};
    if (!derived_fingerprints &&
        derived_fingerprints.error().code.value() == "topology.fingerprint.canonicalization_failed")
        return std::unexpected{serialization_error("topology.serialization.canonicalization_failed",
                                                   "fingerprint_projection")};
    if (!derived_fingerprints || *derived_fingerprints != fingerprints)
        return std::unexpected{serialization_error("topology.serialization.fingerprints_mismatch",
                                                   "physical_or_effective")};
    auto value = topology_json(topology, *derived_ids, *derived_fingerprints);
    ResultCleanup value_cleanup{value};
    if (!value)
        return std::unexpected{std::move(value.error())};
    auto canonical = core::canonicalize_json(*value);
    ResultCleanup canonical_cleanup{canonical};
    if (!canonical)
        return std::unexpected{
            serialization_error("topology.serialization.canonicalization_failed", "json")};
    return SerializedTopology{std::move(*canonical), std::move(*derived_fingerprints)};
}

auto deserialize_topology(std::string_view input, TopologyJsonInputPolicy policy)
    -> std::expected<LoadedTopology, core::Error> {
    auto root = detail::parse_topology_json(input);
    ResultCleanup root_cleanup{root};
    if (!root)
        return std::unexpected{std::move(root.error())};
    if (auto validated = validate_root(*root); !validated)
        return std::unexpected{std::move(validated.error())};
    const auto& physical_text = (*root)["fingerprints"]["physical"].get_ref<const std::string&>();
    const auto& effective_text = (*root)["fingerprints"]["effective"].get_ref<const std::string&>();
    if (!fingerprint_text_valid(physical_text) || !fingerprint_text_valid(effective_text))
        return std::unexpected{invalid_type("/fingerprints", "noncanonical_digest")};
    auto ids = build_id_maps((*root)["objects"]);
    ResultCleanup ids_cleanup{ids};
    if (!ids)
        return std::unexpected{std::move(ids.error())};
    auto topology = build_snapshot(*root, *ids);
    ResultCleanup topology_cleanup{topology};
    if (!topology)
        return std::unexpected{std::move(topology.error())};
    auto validation = validate_topology(*topology);
    ReportCleanup validation_cleanup{validation};
    if (!validation.valid())
        return std::unexpected{
            serialization_error("topology.serialization.invariant_failed", "invariants")};
    auto stable_ids = derive_stable_topology_ids(*topology);
    ResultCleanup stable_ids_cleanup{stable_ids};
    if (!stable_ids)
        return std::unexpected{serialization_error("topology.serialization.invalid_semantic_id",
                                                   "rederived_identity")};
    if (!serialized_ids_match(*ids, *stable_ids))
        return std::unexpected{serialization_error("topology.serialization.invalid_semantic_id",
                                                   "semantic_identity_mismatch")};
    auto fingerprints = derive_topology_fingerprints(*topology, *stable_ids);
    ResultCleanup fingerprints_cleanup{fingerprints};
    if (!fingerprints)
        return std::unexpected{
            serialization_error("topology.serialization.invariant_failed", "fingerprint_evidence")};
    if (fingerprints->physical.to_string() != physical_text)
        return std::unexpected{
            serialization_error("topology.serialization.physical_fingerprint_mismatch", "digest")};
    if (fingerprints->effective.to_string() != effective_text)
        return std::unexpected{
            serialization_error("topology.serialization.effective_fingerprint_mismatch", "digest")};
    auto serialized = serialize_topology(*topology, *stable_ids, *fingerprints);
    ResultCleanup serialized_cleanup{serialized};
    if (!serialized)
        return std::unexpected{std::move(serialized.error())};
    if (policy == TopologyJsonInputPolicy::require_canonical && serialized->canonical_json != input)
        return std::unexpected{
            serialization_error("topology.serialization.noncanonical_input", "byte_mismatch")};
    return LoadedTopology{std::move(*topology), std::move(*stable_ids), std::move(*fingerprints),
                          std::move(serialized->canonical_json)};
}

} // namespace epycbench::topology

namespace epycbench::topology {
namespace {

auto topology_json(const TopologySnapshot& topology, const StableTopologyIds& ids,
                   const TopologyFingerprints& fingerprints) -> std::expected<Json, core::Error> {
    const auto& machine = topology.machine();
    auto vendor = attribute_json(machine.cpu_vendor, [](const auto& value) { return Json(value); });
    ResultCleanup vendor_cleanup{vendor};
    auto model = attribute_json(machine.cpu_model, [](const auto& value) { return Json(value); });
    ResultCleanup model_cleanup{model};
    auto memory = attribute_json(machine.total_memory, byte_json);
    ResultCleanup memory_cleanup{memory};
    auto pages = normalized_pages(machine.page_sizes);
    ResultCleanup pages_cleanup{pages};
    auto allowed_cpus = attribute_json(
        machine.allowed_cpus, [](const auto& value) { return Json(value.to_range_list()); });
    ResultCleanup allowed_cpus_cleanup{allowed_cpus};
    auto effective_cpus = attribute_json(
        machine.effective_cpus, [](const auto& value) { return Json(value.to_range_list()); });
    ResultCleanup effective_cpus_cleanup{effective_cpus};
    auto allowed_nodes = attribute_json(
        machine.allowed_nodes, [](const auto& value) { return Json(value.to_range_list()); });
    ResultCleanup allowed_nodes_cleanup{allowed_nodes};
    auto effective_nodes = attribute_json(
        machine.effective_nodes, [](const auto& value) { return Json(value.to_range_list()); });
    ResultCleanup effective_nodes_cleanup{effective_nodes};
    if (!vendor || !model || !memory || !pages || !allowed_cpus || !effective_cpus ||
        !allowed_nodes || !effective_nodes)
        return std::unexpected{!vendor           ? std::move(vendor.error())
                               : !model          ? std::move(model.error())
                               : !memory         ? std::move(memory.error())
                               : !pages          ? std::move(pages.error())
                               : !allowed_cpus   ? std::move(allowed_cpus.error())
                               : !effective_cpus ? std::move(effective_cpus.error())
                               : !allowed_nodes  ? std::move(allowed_nodes.error())
                                                 : std::move(effective_nodes.error())};

    Json objects = core::detail::json_object<Json>([&](auto& object_7) {
        core::detail::set_json(object_7, "packages", Json::array());
        core::detail::set_json(object_7, "dies", Json::array());
        core::detail::set_json(object_7, "groups", Json::array());
        core::detail::set_json(object_7, "cores", Json::array());
        core::detail::set_json(object_7, "processing_units", Json::array());
        core::detail::set_json(object_7, "caches", Json::array());
        core::detail::set_json(object_7, "numa_nodes", Json::array());
        core::detail::set_json(object_7, "pci_functions", Json::array());
    });

    core::detail::JsonCleanup objects_cleanup{objects};

    for (const auto& entry : ids.packages()) {
        const auto package = topology.package(entry.handle);
        auto os_id = attribute_json(package->os_id, [](auto value) { return Json(value); });
        ResultCleanup os_id_cleanup{os_id};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        core::detail::append_object_json(objects["packages"], [&](auto& object_8) {
            core::detail::set_json(object_8, "id", entry.semantic_id.value());
            core::detail::set_json(object_8, "os_id", std::move(*os_id));
        });
    }
    for (const auto& entry : ids.dies()) {
        const auto die = topology.die(entry.handle);
        auto os_id = attribute_json(die->os_id, [](auto value) { return Json(value); });
        ResultCleanup os_id_cleanup{os_id};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        Json group = nullptr;
        core::detail::JsonCleanup group_cleanup{group};
        if (die->group_id)
            group = id_value(ids.group_id(*die->group_id));
        core::detail::append_object_json(objects["dies"], [&](auto& object_9) {
            core::detail::set_json(object_9, "id", entry.semantic_id.value());
            core::detail::set_json(object_9, "package", id_value(ids.package_id(die->package_id)));
            core::detail::set_json(object_9, "group", std::move(group));
            core::detail::set_json(object_9, "os_id", std::move(*os_id));
        });
    }
    for (const auto& entry : ids.groups()) {
        const auto group = topology.architecture_group(entry.handle);
        auto members = semantic_pus(group->member_pus, ids);
        ResultCleanup members_cleanup{members};
        auto provenance = evidence_array(group->provenance);
        ResultCleanup provenance_cleanup{provenance};
        if (!members || !provenance)
            return std::unexpected{!members ? std::move(members.error())
                                            : std::move(provenance.error())};
        Json parent = nullptr;
        core::detail::JsonCleanup parent_cleanup{parent};
        if (group->parent_group_id)
            parent = id_value(ids.group_id(*group->parent_group_id));
        core::detail::append_object_json(objects["groups"], [&](auto& object_10) {
            core::detail::set_json(object_10, "id", entry.semantic_id.value());
            core::detail::set_json(object_10, "package",
                                   id_value(ids.package_id(group->package_id)));
            core::detail::set_json(object_10, "parent", std::move(parent));
            core::detail::set_json(object_10, "kind", group->kind);
            core::detail::set_json(object_10, "subtype",
                                   group->subtype ? Json(*group->subtype) : Json(nullptr));
            core::detail::set_json(object_10, "processing_units", std::move(*members));
            core::detail::set_json(object_10, "provenance", std::move(*provenance));
        });
    }
    for (const auto& entry : ids.cores()) {
        const auto core = topology.core(entry.handle);
        auto os_id = attribute_json(core->os_id, [](auto value) { return Json(value); });
        ResultCleanup os_id_cleanup{os_id};
        if (!os_id)
            return std::unexpected{std::move(os_id.error())};
        Json die = nullptr;
        core::detail::JsonCleanup die_cleanup{die};
        if (core->die_id)
            die = id_value(ids.die_id(*core->die_id));
        core::detail::append_object_json(objects["cores"], [&](auto& object_11) {
            core::detail::set_json(object_11, "id", entry.semantic_id.value());
            core::detail::set_json(object_11, "package",
                                   id_value(ids.package_id(core->package_id)));
            core::detail::set_json(object_11, "die", std::move(die));
            core::detail::set_json(object_11, "os_id", std::move(*os_id));
        });
    }
    for (const auto& entry : ids.processing_units()) {
        const auto pu = topology.processing_unit(entry.handle);
        auto isolated = attribute_json(pu->isolated, [](bool value) { return Json(value); });
        ResultCleanup isolated_cleanup{isolated};
        if (!isolated)
            return std::unexpected{std::move(isolated.error())};
        core::detail::append_object_json(objects["processing_units"], [&](auto& object_12) {
            core::detail::set_json(object_12, "id", entry.semantic_id.value());
            core::detail::set_json(object_12, "linux_cpu", pu->linux_cpu_id.value());
            core::detail::set_json(object_12, "core", id_value(ids.core_id(pu->core_id)));
            core::detail::set_json(object_12, "isolated", std::move(*isolated));
        });
    }
    for (const auto& entry : ids.caches()) {
        const auto cache = topology.cache(entry.handle);
        auto associativity =
            attribute_json(cache->associativity, [](auto value) { return Json(value); });
        ResultCleanup associativity_cleanup{associativity};
        auto members = semantic_pus(cache->member_pus, ids);
        ResultCleanup members_cleanup{members};
        auto provenance = evidence_array(cache->provenance);
        ResultCleanup provenance_cleanup{provenance};
        if (!associativity || !members || !provenance)
            return std::unexpected{!associativity ? std::move(associativity.error())
                                   : !members     ? std::move(members.error())
                                                  : std::move(provenance.error())};
        core::detail::append_object_json(objects["caches"], [&](auto& object_13) {
            core::detail::set_json(object_13, "id", entry.semantic_id.value());
            core::detail::set_json(object_13, "level", cache->level);
            core::detail::set_json(object_13, "kind", cache_kind_token(cache->kind));
            core::detail::set_json(object_13, "size_bytes", byte_json(cache->size));
            core::detail::set_json(object_13, "line_size_bytes", byte_json(cache->line_size));
            core::detail::set_json(object_13, "associativity", std::move(*associativity));
            core::detail::set_json(object_13, "processing_units", std::move(*members));
            core::detail::set_json(object_13, "provenance", std::move(*provenance));
        });
    }
    for (const auto& entry : ids.numa_nodes()) {
        const auto node = topology.numa_node(entry.handle);
        auto linux_node =
            attribute_json(node->linux_node_id, [](auto value) { return Json(value); });
        ResultCleanup linux_node_cleanup{linux_node};
        auto installed = attribute_json(node->installed_memory, byte_json);
        ResultCleanup installed_cleanup{installed};
        auto available = attribute_json(node->available_memory, byte_json);
        ResultCleanup available_cleanup{available};
        auto members = semantic_pus(node->member_pus, ids);
        ResultCleanup members_cleanup{members};
        auto node_pages = normalized_pages(node->page_sizes);
        ResultCleanup node_pages_cleanup{node_pages};
        if (!linux_node || !installed || !available || !members || !node_pages)
            return std::unexpected{!linux_node  ? std::move(linux_node.error())
                                   : !installed ? std::move(installed.error())
                                   : !available ? std::move(available.error())
                                   : !members   ? std::move(members.error())
                                                : std::move(node_pages.error())};
        core::detail::append_object_json(objects["numa_nodes"], [&](auto& object_14) {
            core::detail::set_json(object_14, "id", entry.semantic_id.value());
            core::detail::set_json(object_14, "linux_node", std::move(*linux_node));
            core::detail::set_json(object_14, "installed_memory", std::move(*installed));
            core::detail::set_json(object_14, "available_memory", std::move(*available));
            core::detail::set_json(object_14, "processing_units", std::move(*members));
            core::detail::set_json(object_14, "page_sizes_bytes", std::move(*node_pages));
        });
    }
    for (const auto& entry : ids.pci_devices()) {
        const auto device = topology.pci_device(entry.handle);
        if ((device->class_code & 0xff000000U) != 0U)
            return std::unexpected{invalid_type("pci_functions/class_code", "out_of_range")};
        auto numa = attribute_json(device->numa_locality, [&](auto value) {
            return Json(id_value(ids.numa_node_id(value)));
        });
        ResultCleanup numa_cleanup{numa};
        auto local = attribute_json(
            device->local_pus, [&](const auto& value) { return semantic_pus(value, ids).value(); });
        ResultCleanup local_cleanup{local};
        if (!numa || !local)
            return std::unexpected{!numa ? std::move(numa.error()) : std::move(local.error())};
        core::detail::append_object_json(objects["pci_functions"], [&](auto& object_15) {
            core::detail::set_json(object_15, "id", entry.semantic_id.value());
            core::detail::set_json(object_15, "vendor_id",
                                   std::format("{:04x}", device->vendor_id));
            core::detail::set_json(object_15, "device_id",
                                   std::format("{:04x}", device->device_id));
            core::detail::set_json(object_15, "base_class",
                                   std::format("{:02x}", (device->class_code >> 16U) & 0xffU));
            core::detail::set_json(object_15, "subclass",
                                   std::format("{:02x}", (device->class_code >> 8U) & 0xffU));
            core::detail::set_json(object_15, "programming_interface",
                                   std::format("{:02x}", device->class_code & 0xffU));
            core::detail::set_json(object_15, "numa_locality", std::move(*numa));
            core::detail::set_json(object_15, "local_processing_units", std::move(*local));
        });
    }

    struct DistanceEntry {
        std::string from;
        std::string to;
        std::uint32_t value;
    };
    std::vector<DistanceEntry> ordered_distances;
    for (const auto& distance : topology.numa_distances().directed_distances)
        ordered_distances.push_back({std::string{id_value(ids.numa_node_id(distance.from))},
                                     std::string{id_value(ids.numa_node_id(distance.to))},
                                     distance.value});
    std::ranges::sort(ordered_distances, [](const auto& left, const auto& right) {
        return std::tie(left.from, left.to) < std::tie(right.from, right.to);
    });
    auto distances = Json::array();
    core::detail::JsonCleanup distances_cleanup{distances};
    for (const auto& distance : ordered_distances)
        core::detail::append_object_json(distances, [&](auto& object_16) {
            core::detail::set_json(object_16, "from", distance.from);
            core::detail::set_json(object_16, "to", distance.to);
            core::detail::set_json(object_16, "meaning", "latency");
            core::detail::set_json(object_16, "unit", "relative");
            core::detail::set_json(object_16, "value", distance.value);
        });

    return core::detail::json_object<Json>([&](auto& object_17) {
        core::detail::set_json(object_17, "schema", schema_token);
        core::detail::set_json(
            object_17, "fingerprints", core::detail::json_object<Json>([&](auto& object_18) {
                core::detail::set_json(object_18, "physical", fingerprints.physical.to_string());
                core::detail::set_json(object_18, "effective", fingerprints.effective.to_string());
            }));
        core::detail::set_json(
            object_17, "machine", core::detail::json_object<Json>([&](auto& object_19) {
                core::detail::set_json(object_19, "id", ids.machine_id().value());
                core::detail::set_json(object_19, "architecture", machine.architecture);
                core::detail::set_json(object_19, "cpu_vendor", std::move(*vendor));
                core::detail::set_json(object_19, "cpu_model", std::move(*model));
                core::detail::set_json(object_19, "total_memory", std::move(*memory));
                core::detail::set_json(object_19, "page_sizes_bytes", std::move(*pages));
                core::detail::set_json(object_19, "present_cpus",
                                       machine.present_cpus.to_range_list());
                core::detail::set_json(object_19, "online_cpus",
                                       machine.online_cpus.to_range_list());
                core::detail::set_json(object_19, "allowed_cpus", std::move(*allowed_cpus));
                core::detail::set_json(object_19, "effective_cpus", std::move(*effective_cpus));
                core::detail::set_json(object_19, "present_nodes",
                                       machine.present_nodes.to_range_list());
                core::detail::set_json(object_19, "online_nodes",
                                       machine.online_nodes.to_range_list());
                core::detail::set_json(object_19, "allowed_nodes", std::move(*allowed_nodes));
                core::detail::set_json(object_19, "effective_nodes", std::move(*effective_nodes));
            }));
        core::detail::set_json(object_17, "objects", std::move(objects));
        core::detail::set_json(object_17, "distances", std::move(distances));
    });
}

template <typename StableId>
auto validate_semantic_id(const Json& value, std::string_view path)
    -> std::expected<std::string, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type(std::string{path})};
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > detail::topology_json_max_string_bytes)
        return std::unexpected{serialization_error("topology.serialization.limit_exceeded",
                                                   "semantic_id", std::string{path})};
    auto parsed_id = StableId::parse(text);
    core::detail::ExpectedErrorCleanup parsed_id_cleanup{parsed_id};
    if (!parsed_id)
        return std::unexpected{serialization_error("topology.serialization.invalid_semantic_id",
                                                   "syntax_or_category", std::string{path})};
    return text;
}

template <typename Handle>
auto assign_ids(const Json& values, auto parse_id, std::string_view path)
    -> std::expected<std::map<std::string, Handle, std::less<>>, core::Error> {
    if (!values.is_array())
        return std::unexpected{invalid_type(std::string{path})};
    if (values.size() > detail::topology_json_max_category_objects)
        return std::unexpected{serialization_error("topology.serialization.limit_exceeded",
                                                   "category_objects", std::string{path})};
    std::vector<std::string> ids;
    ids.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!values[index].is_object() || !values[index].contains("id"))
            return std::unexpected{serialization_error("topology.serialization.missing_field",
                                                       "missing_id", std::string{path})};
        auto id =
            parse_id(values[index]["id"], std::string{path} + "/" + std::to_string(index) + "/id");
        ResultCleanup id_cleanup{id};
        if (!id)
            return std::unexpected{std::move(id.error())};
        ids.push_back(std::move(*id));
    }
    std::ranges::sort(ids);
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return std::unexpected{serialization_error("topology.serialization.duplicate_object_id",
                                                   "duplicate_id", std::string{path})};
    std::map<std::string, Handle, std::less<>> result;
    for (std::size_t index = 0; index < ids.size(); ++index)
        result.emplace(ids[index], Handle{static_cast<std::uint32_t>(index)});
    return result;
}

template <typename Handle>
auto reference(const Json& value, const std::map<std::string, Handle, std::less<>>& ids,
               std::string_view path) -> std::expected<Handle, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type(std::string{path})};
    const auto found = ids.find(value.get_ref<const std::string&>());
    if (found == ids.end())
        return std::unexpected{serialization_error("topology.serialization.dangling_reference",
                                                   "missing_or_wrong_category", std::string{path})};
    return found->second;
}

auto pu_members(const Json& value,
                const std::map<std::string, core::ProcessingUnitId, std::less<>>& ids,
                std::string_view path) -> std::expected<ProcessingUnitSet, core::Error> {
    if (!value.is_array())
        return std::unexpected{invalid_type(std::string{path})};
    std::set<std::string, std::less<>> seen;
    ProcessingUnitSet result;
    for (const auto& item : value) {
        if (!item.is_string())
            return std::unexpected{invalid_type(std::string{path})};
        const auto& text = item.get_ref<const std::string&>();
        if (!seen.insert(text).second)
            return std::unexpected{
                serialization_error("topology.serialization.duplicate_relationship",
                                    "duplicate_member", std::string{path})};
        const auto member = ids.find(text);
        if (member == ids.end())
            return std::unexpected{serialization_error("topology.serialization.dangling_reference",
                                                       "missing_or_wrong_category",
                                                       std::string{path})};
        result = result.set_union(ProcessingUnitSet{member->second});
    }
    return result;
}

auto parse_hex(const Json& value, std::size_t width, std::string_view path)
    -> std::expected<std::uint32_t, core::Error> {
    if (!value.is_string())
        return std::unexpected{invalid_type(std::string{path})};
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != width)
        return std::unexpected{invalid_type(std::string{path}, "hex_width")};
    std::uint32_t result{};
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::unexpected{invalid_type(std::string{path}, "noncanonical_hex")};
        result = result * 16U + static_cast<std::uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return result;
}

} // namespace

} // namespace epycbench::topology
