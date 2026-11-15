#include "epycbench/topology/fingerprint.hpp"
#include "core/json_ownership.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/invariants.hpp"
#include "topology/fingerprint_projection.hpp"
#include "topology/prepared_stable_id_access.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace epycbench::topology {
namespace {

using Json = nlohmann::json;
using core::detail::append_json;
using core::detail::append_object_json;
using core::detail::JsonCleanup;
using core::detail::JsonPointerCleanup;
using core::detail::set_json;
using JsonOwner = core::detail::OwnedJson<Json>;
template <class F> auto json_object(F&& fill) -> Json {
    return core::detail::json_object<Json>(std::forward<F>(fill));
}
constexpr std::string_view profile = "epycbench.topology-fingerprint.v1";

struct ReportCleanup {
    ValidationReport& report;
    ~ReportCleanup() {
        for (auto& error : report.diagnostics) core::detail::retire_json(error.context);
    }
};

auto parse_error(std::string_view kind, std::string classification,
                 std::string underlying = {}) -> core::Error {
    Json context = json_object([&](Json& json_target_0) {
        set_json(json_target_0, "fingerprint_kind", kind);
        set_json(json_target_0, "failure_classification", std::move(classification));
    });
    JsonCleanup context_cleanup{context};
    if (!underlying.empty())
        context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{"topology.fingerprint.invalid_text"},
            .message = "Topology fingerprint text is invalid or noncanonical",
            .operation = "parse_topology_fingerprint",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto parse_digest(std::string_view text, std::string_view kind)
    -> std::expected<core::Sha256Digest, core::Error> {
    try {
        if (text.find('\0') != std::string_view::npos)
            return std::unexpected{parse_error(kind, "invalid_encoding")};
        constexpr std::string_view prefix{"sha256:"};
        if (!text.starts_with(prefix)) {
            const auto classification = text.find(':') == std::string_view::npos
                                            ? "missing_prefix"
                                            : "unsupported_algorithm";
            return std::unexpected{parse_error(kind, classification)};
        }
        const auto hexadecimal = text.substr(prefix.size());
        if (hexadecimal.size() != 64U)
            return std::unexpected{parse_error(kind, "invalid_length")};
        auto parsed = core::Sha256Digest::parse(hexadecimal);
        core::detail::ExpectedErrorCleanup parsed_error_cleanup{parsed};
        if (!parsed) {
            const auto& underlying = parsed.error().code.value();
            const auto classification =
                underlying == "core.digest.noncanonical_case" ? "noncanonical_hex"
                                                               : "invalid_hex";
            return std::unexpected{parse_error(kind, classification, underlying)};
        }
        return *parsed;
    } catch (const std::exception&) {
        return std::unexpected{parse_error(kind, "invalid_encoding")};
    } catch (...) {
        return std::unexpected{parse_error(kind, "invalid_encoding")};
    }
}

auto error(std::string code, std::string message, std::string object_type, std::string object_id,
           std::string field, std::string classification, std::string underlying = {})
    -> core::Error {
    Json context = json_object([&](Json& json_target_1) {
        set_json(json_target_1, "object_type", std::move(object_type));
        set_json(json_target_1, "object_id", std::move(object_id));
        set_json(json_target_1, "field", std::move(field));
        set_json(json_target_1, "classification", std::move(classification));
    });
    JsonCleanup context_cleanup{context};
    if (!underlying.empty())
        context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::move(code)},
            .message = std::move(message),
            .operation = "derive_topology_fingerprints",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto handle(auto id) -> std::string { return std::to_string(id.value()); }
auto byte_string(core::ByteCount value) -> std::string { return std::to_string(value.value()); }

template <typename T>
auto require(const Attribute<T>& attribute, std::string object_type, std::string object_id,
             std::string field) -> std::expected<void, core::Error> {
    if (attribute.availability() == Availability::conflicted)
        return std::unexpected{error("topology.fingerprint.conflicted_required_evidence",
                                     "Required fingerprint evidence is conflicted",
                                     std::move(object_type), std::move(object_id), std::move(field),
                                     "conflicted")};
    if (attribute.availability() != Availability::available || !attribute.value())
        return std::unexpected{
            error("topology.fingerprint.missing_required_evidence",
                  "Required fingerprint evidence is unavailable", std::move(object_type),
                  std::move(object_id), std::move(field),
                  attribute.availability() == Availability::unknown ? "unknown" : "unavailable")};
    return {};
}

auto check_required(const TopologySnapshot& topology) -> std::expected<void, core::Error> {
    const auto& machine = topology.machine();
    if (machine.architecture.empty())
        return std::unexpected{error("topology.fingerprint.missing_required_evidence",
                                     "Machine architecture is empty", "machine", handle(machine.id),
                                     "architecture", "empty")};
    if (auto checked = require(machine.total_memory, "machine", handle(machine.id), "total_memory");
        !checked)
        return checked;
    for (const auto& node : topology.numa_nodes())
        if (auto checked =
                require(node.installed_memory, "numa_node", handle(node.id), "installed_memory");
            !checked)
            return checked;
    if (auto checked = require(machine.allowed_cpus, "machine", handle(machine.id), "allowed_cpus");
        !checked)
        return checked;
    if (auto checked =
            require(machine.effective_cpus, "machine", handle(machine.id), "effective_cpus");
        !checked)
        return checked;
    if (auto checked =
            require(machine.allowed_nodes, "machine", handle(machine.id), "allowed_nodes");
        !checked)
        return checked;
    if (auto checked =
            require(machine.effective_nodes, "machine", handle(machine.id), "effective_nodes");
        !checked)
        return checked;
    for (const auto& device : topology.pci_devices()) {
        if (device.numa_locality.availability() == Availability::conflicted)
            return std::unexpected{error("topology.fingerprint.conflicted_required_evidence",
                                         "PCI NUMA locality is conflicted", "pci_device",
                                         device.address.to_string(), "numa_locality",
                                         "conflicted")};
        if (device.local_pus.availability() == Availability::conflicted)
            return std::unexpected{error("topology.fingerprint.conflicted_required_evidence",
                                         "PCI local PU evidence is conflicted", "pci_device",
                                         device.address.to_string(), "local_pus", "conflicted")};
    }
    return {};
}

template <typename Entry, typename LeftForward, typename RightForward, typename LeftReverse,
          typename RightReverse>
auto equal_index(std::span<const Entry> left, std::span<const Entry> right,
                 LeftForward left_forward, RightForward right_forward, LeftReverse left_reverse,
                 RightReverse right_reverse) -> bool {
    if (!std::ranges::equal(left, right))
        return false;
    return std::ranges::all_of(left, [&](const auto& entry) {
        const auto left_id = left_forward(entry.handle);
        const auto right_id = right_forward(entry.handle);
        const auto left_handle = left_reverse(entry.semantic_id);
        const auto right_handle = right_reverse(entry.semantic_id);
        return left_id && right_id && left_handle && right_handle &&
               left_id->get() == entry.semantic_id && right_id->get() == entry.semantic_id &&
               *left_handle == entry.handle && *right_handle == entry.handle;
    });
}

auto equal_ids(const StableTopologyIds& left, const StableTopologyIds& right) -> bool {
    const auto machines_equal =
        std::ranges::equal(left.machines(), right.machines()) &&
        left.machine_id() == right.machine_id() &&
        left.machine(left.machine_id()) == right.machine(right.machine_id());
    return machines_equal &&
           equal_index(
               left.packages(), right.packages(), [&](auto id) { return left.package_id(id); },
               [&](auto id) { return right.package_id(id); },
               [&](const auto& id) { return left.package(id); },
               [&](const auto& id) { return right.package(id); }) &&
           equal_index(
               left.dies(), right.dies(), [&](auto id) { return left.die_id(id); },
               [&](auto id) { return right.die_id(id); },
               [&](const auto& id) { return left.die(id); },
               [&](const auto& id) { return right.die(id); }) &&
           equal_index(
               left.groups(), right.groups(), [&](auto id) { return left.group_id(id); },
               [&](auto id) { return right.group_id(id); },
               [&](const auto& id) { return left.group(id); },
               [&](const auto& id) { return right.group(id); }) &&
           equal_index(
               left.cores(), right.cores(), [&](auto id) { return left.core_id(id); },
               [&](auto id) { return right.core_id(id); },
               [&](const auto& id) { return left.core(id); },
               [&](const auto& id) { return right.core(id); }) &&
           equal_index(
               left.processing_units(), right.processing_units(),
               [&](auto id) { return left.processing_unit_id(id); },
               [&](auto id) { return right.processing_unit_id(id); },
               [&](const auto& id) { return left.processing_unit(id); },
               [&](const auto& id) { return right.processing_unit(id); }) &&
           equal_index(
               left.caches(), right.caches(), [&](auto id) { return left.cache_id(id); },
               [&](auto id) { return right.cache_id(id); },
               [&](const auto& id) { return left.cache(id); },
               [&](const auto& id) { return right.cache(id); }) &&
           equal_index(
               left.numa_nodes(), right.numa_nodes(),
               [&](auto id) { return left.numa_node_id(id); },
               [&](auto id) { return right.numa_node_id(id); },
               [&](const auto& id) { return left.numa_node(id); },
               [&](const auto& id) { return right.numa_node(id); }) &&
           equal_index(
               left.pci_devices(), right.pci_devices(),
               [&](auto id) { return left.pci_device_id(id); },
               [&](auto id) { return right.pci_device_id(id); },
               [&](const auto& id) { return left.pci_device(id); },
               [&](const auto& id) { return right.pci_device(id); });
}

auto canonical(const Json& value) -> std::expected<std::string, core::Error> {
    auto result = core::canonicalize_json(value);
    core::detail::ExpectedErrorCleanup result_error_cleanup{result};
    if (!result)
        return std::unexpected{error("topology.fingerprint.canonicalization_failed",
                                     "Fingerprint projection cannot be canonicalized", "projection",
                                     "safe", "json", "canonicalization",
                                     result.error().code.value())};
    return result;
}

template <typename T, typename Encode>
auto attribute_json(const Attribute<T>& attribute, Encode encode)
    -> std::expected<Json, core::Error> {
    switch (attribute.availability()) {
    case Availability::available:
        if (!attribute.value())
            return std::unexpected{error("topology.fingerprint.unsupported_attribute_state",
                                         "Available attribute has no value", "attribute", "safe",
                                         "availability", "inconsistent")};
        return json_object([&](Json& json_target_2) {
            set_json(json_target_2, "state", "available");
            set_json(json_target_2, "value", encode(*attribute.value()));
        });
    case Availability::unavailable:
        return json_object(
            [&](Json& json_target_3) { set_json(json_target_3, "state", "unavailable"); });
    case Availability::unknown:
        return json_object(
            [&](Json& json_target_4) { set_json(json_target_4, "state", "unknown"); });
    case Availability::conflicted: {
        std::vector<std::pair<std::string, JsonOwner>> alternatives;
        for (const auto& conflict : attribute.conflicts()) {
            auto value = encode(conflict.value);
            JsonCleanup value_cleanup{value};
            auto bytes = canonical(value);
            if (!bytes)
                return std::unexpected{std::move(bytes.error())};
            alternatives.emplace_back(std::move(*bytes), JsonOwner{std::move(value)});
        }
        std::ranges::sort(alternatives, {}, &std::pair<std::string, JsonOwner>::first);
        alternatives.erase(
            std::unique(alternatives.begin(), alternatives.end(),
                        [](const auto& a, const auto& b) { return a.first == b.first; }),
            alternatives.end());
        if (alternatives.size() < 2U)
            return std::unexpected{error("topology.fingerprint.invalid_topology",
                                         "Conflict has fewer than two distinct values", "attribute",
                                         "safe", "conflicts", "not_distinct")};
        auto values = Json::array();
        JsonCleanup values_cleanup{values};
        for (auto& [unused, value] : alternatives) {
            static_cast<void>(unused);
            values.push_back(std::move(value.value));
        }
        return json_object([&](Json& json_target_5) {
            set_json(json_target_5, "state", "conflicted");
            set_json(json_target_5, "values", std::move(values));
        });
    }
    }
    return std::unexpected{error("topology.fingerprint.unsupported_attribute_state",
                                 "Attribute state is inconsistent", "attribute", "safe",
                                 "availability", "inconsistent")};
}

auto pages(const std::vector<core::ByteCount>& input, std::string type, std::string id)
    -> std::expected<Json, core::Error> {
    auto values = input;
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    auto result = Json::array();
    JsonCleanup result_cleanup{result};
    for (const auto value : values) {
        if (value.value() == 0U)
            return std::unexpected{error("topology.fingerprint.unsupported_attribute_state",
                                         "Page-size capability is zero", std::move(type),
                                         std::move(id), "page_sizes", "zero")};
        result.push_back(byte_string(value));
    }
    return Json(std::move(result));
}

auto pu_ids(const ProcessingUnitSet& set, const StableTopologyIds& ids)
    -> std::expected<Json, core::Error> {
    std::vector<std::string> result;
    for (const auto member : set) {
        const auto id = ids.processing_unit_id(member);
        if (!id)
            return std::unexpected{error("topology.fingerprint.stable_ids_mismatch",
                                         "PU relationship lacks stable identity", "processing_unit",
                                         handle(member), "semantic_id", "missing")};
        result.emplace_back(id->get().value());
    }
    std::ranges::sort(result);
    auto array = Json::array();
    JsonCleanup cleanup{array};
    array.get_ref<Json::array_t&>().reserve(result.size());
    for (const auto& value : result)
        array.push_back(value);
    return Json(std::move(array));
}

template <typename Entries, typename Predicate>
auto related(const Entries& entries, Predicate predicate) -> Json {
    auto result = Json::array();
    JsonCleanup result_cleanup{result};
    for (const auto& entry : entries)
        if (predicate(entry.handle))
            result.push_back(entry.semantic_id.value());
    return Json(std::move(result));
}

auto cache_kind(CacheKind kind) -> std::string_view {
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

auto physical_json(const TopologySnapshot& topology, const StableTopologyIds& ids)
    -> std::expected<Json, core::Error> {
    const auto& machine = topology.machine();
    auto machine_pages = pages(machine.page_sizes, "machine", "machine");
    JsonPointerCleanup machine_pages_cleanup{machine_pages ? &*machine_pages : nullptr};
    core::detail::ExpectedErrorCleanup machine_pages_error_cleanup{machine_pages};
    auto vendor = attribute_json(machine.cpu_vendor, [](const std::string& v) { return Json(v); });
    JsonPointerCleanup vendor_cleanup{vendor ? &*vendor : nullptr};
    core::detail::ExpectedErrorCleanup vendor_error_cleanup{vendor};
    auto model = attribute_json(machine.cpu_model, [](const std::string& v) { return Json(v); });
    JsonPointerCleanup model_cleanup{model ? &*model : nullptr};
    core::detail::ExpectedErrorCleanup model_error_cleanup{model};
    auto memory = attribute_json(machine.total_memory,
                                 [](core::ByteCount v) { return Json(byte_string(v)); });
    JsonPointerCleanup memory_cleanup{memory ? &*memory : nullptr};
    core::detail::ExpectedErrorCleanup memory_error_cleanup{memory};
    if (!machine_pages || !vendor || !model || !memory)
        return std::unexpected{!machine_pages ? std::move(machine_pages.error())
                               : !vendor      ? std::move(vendor.error())
                               : !model       ? std::move(model.error())
                                              : std::move(memory.error())};

    Json objects = json_object([&](Json& json_target_6) {
        set_json(json_target_6, "packages", Json::array());
        set_json(json_target_6, "dies", Json::array());
        set_json(json_target_6, "groups", Json::array());
        set_json(json_target_6, "cores", Json::array());
        set_json(json_target_6, "processing_units", Json::array());
        set_json(json_target_6, "caches", Json::array());
        set_json(json_target_6, "numa_nodes", Json::array());
        set_json(json_target_6, "pci_functions", Json::array());
    });
    JsonCleanup objects_cleanup{objects};

    for (const auto& entry : ids.packages()) {
        ProcessingUnitSet members;
        for (const auto& pu : topology.processing_units())
            if (topology.core(pu.core_id)->package_id == entry.handle)
                members = members.set_union(ProcessingUnitSet{pu.id});
        auto pus = pu_ids(members, ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        if (!pus)
            return std::unexpected{std::move(pus.error())};
        append_object_json(objects["packages"], [&](Json& json_target_7) {
            set_json(json_target_7, "id", entry.semantic_id.value());
            set_json(json_target_7, "processing_units", std::move(*pus));
            set_json(json_target_7, "cores", related(ids.cores(), [&](auto id) {
                         return topology.core(id)->package_id == entry.handle;
                     }));
            set_json(json_target_7, "dies", related(ids.dies(), [&](auto id) {
                         return topology.die(id)->package_id == entry.handle;
                     }));
            set_json(json_target_7, "groups", related(ids.groups(), [&](auto id) {
                         return topology.architecture_group(id)->package_id == entry.handle;
                     }));
        });
    }
    for (const auto& entry : ids.dies()) {
        const auto die = *topology.die(entry.handle);
        ProcessingUnitSet members;
        for (const auto& pu : topology.processing_units())
            if (topology.core(pu.core_id)->die_id == entry.handle)
                members = members.set_union(ProcessingUnitSet{pu.id});
        auto pus = pu_ids(members, ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        const auto package = ids.package_id(die.package_id);
        if (!pus || !package)
            return std::unexpected{error("topology.fingerprint.stable_ids_mismatch",
                                         "Die relation lacks stable identity", "die",
                                         handle(entry.handle), "relationship", "missing")};
        Json group = nullptr;
        if (die.group_id)
            group = ids.group_id(*die.group_id)->get().value();
        append_object_json(objects["dies"], [&](Json& json_target_8) {
            set_json(json_target_8, "id", entry.semantic_id.value());
            set_json(json_target_8, "package", package->get().value());
            set_json(json_target_8, "group", std::move(group));
            set_json(json_target_8, "processing_units", std::move(*pus));
            set_json(json_target_8, "cores", related(ids.cores(), [&](auto id) {
                         return topology.core(id)->die_id == entry.handle;
                     }));
        });
    }
    for (const auto& entry : ids.groups()) {
        const auto group = *topology.architecture_group(entry.handle);
        auto pus = pu_ids(group.member_pus, ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        Json parent = nullptr;
        if (group.parent_group_id)
            parent = ids.group_id(*group.parent_group_id)->get().value();
        append_object_json(objects["groups"], [&](Json& json_target_9) {
            set_json(json_target_9, "id", entry.semantic_id.value());
            set_json(json_target_9, "package", ids.package_id(group.package_id)->get().value());
            set_json(json_target_9, "parent", std::move(parent));
            set_json(json_target_9, "kind", group.kind);
            set_json(json_target_9, "subtype",
                     group.subtype ? Json(*group.subtype) : Json(nullptr));
            set_json(json_target_9, "processing_units", std::move(*pus));
        });
    }
    for (const auto& entry : ids.cores()) {
        const auto core = *topology.core(entry.handle);
        auto pus = pu_ids(topology.core_members(entry.handle), ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        Json die = nullptr;
        if (core.die_id)
            die = ids.die_id(*core.die_id)->get().value();
        append_object_json(objects["cores"], [&](Json& json_target_10) {
            set_json(json_target_10, "id", entry.semantic_id.value());
            set_json(json_target_10, "package", ids.package_id(core.package_id)->get().value());
            set_json(json_target_10, "die", std::move(die));
            set_json(json_target_10, "processing_units", std::move(*pus));
        });
    }
    for (const auto& entry : ids.processing_units()) {
        const auto pu = *topology.processing_unit(entry.handle);
        append_object_json(objects["processing_units"], [&](Json& json_target_11) {
            set_json(json_target_11, "id", entry.semantic_id.value());
            set_json(json_target_11, "linux_cpu", pu.linux_cpu_id.value());
            set_json(json_target_11, "core", ids.core_id(pu.core_id)->get().value());
        });
    }
    for (const auto& entry : ids.caches()) {
        const auto cache = *topology.cache(entry.handle);
        auto associativity =
            attribute_json(cache.associativity, [](std::uint32_t v) { return Json(v); });
        JsonPointerCleanup associativity_cleanup{associativity ? &*associativity : nullptr};
        core::detail::ExpectedErrorCleanup associativity_error_cleanup{associativity};
        auto pus = pu_ids(cache.member_pus, ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        if (!associativity || !pus)
            return std::unexpected{!associativity ? std::move(associativity.error())
                                                  : std::move(pus.error())};
        append_object_json(objects["caches"], [&](Json& json_target_12) {
            set_json(json_target_12, "id", entry.semantic_id.value());
            set_json(json_target_12, "level", cache.level);
            set_json(json_target_12, "kind", cache_kind(cache.kind));
            set_json(json_target_12, "size_bytes", byte_string(cache.size));
            set_json(json_target_12, "line_size_bytes", byte_string(cache.line_size));
            set_json(json_target_12, "associativity", std::move(*associativity));
            set_json(json_target_12, "processing_units", std::move(*pus));
        });
    }
    for (const auto& entry : ids.numa_nodes()) {
        const auto node = *topology.numa_node(entry.handle);
        auto node_pages =
            pages(node.page_sizes, "numa_node", std::string{entry.semantic_id.value()});
        JsonPointerCleanup node_pages_cleanup{node_pages ? &*node_pages : nullptr};
        core::detail::ExpectedErrorCleanup node_pages_error_cleanup{node_pages};
        auto node_memory = attribute_json(node.installed_memory,
                                          [](core::ByteCount v) { return Json(byte_string(v)); });
        JsonPointerCleanup node_memory_cleanup{node_memory ? &*node_memory : nullptr};
        core::detail::ExpectedErrorCleanup node_memory_error_cleanup{node_memory};
        auto pus = pu_ids(node.member_pus, ids);
        JsonPointerCleanup pus_cleanup{pus ? &*pus : nullptr};
        core::detail::ExpectedErrorCleanup pus_error_cleanup{pus};
        if (!node_pages || !node_memory || !pus)
            return std::unexpected{!node_pages    ? std::move(node_pages.error())
                                   : !node_memory ? std::move(node_memory.error())
                                                  : std::move(pus.error())};
        append_object_json(objects["numa_nodes"], [&](Json& json_target_13) {
            set_json(json_target_13, "id", entry.semantic_id.value());
            set_json(json_target_13, "linux_node", *node.linux_node_id.value());
            set_json(json_target_13, "installed_memory_bytes", std::move(*node_memory));
            set_json(json_target_13, "processing_units", std::move(*pus));
            set_json(json_target_13, "page_sizes_bytes", std::move(*node_pages));
        });
    }
    for (const auto& entry : ids.pci_devices()) {
        const auto device = *topology.pci_device(entry.handle);
        if ((device.class_code & 0xff000000U) != 0U)
            return std::unexpected{error("topology.fingerprint.unsupported_attribute_state",
                                         "PCI class exceeds 24 bits", "pci_device",
                                         std::string{entry.semantic_id.value()}, "class_code",
                                         "out_of_range")};
        auto numa = attribute_json(device.numa_locality, [&](core::NumaNodeId id) {
            return Json(std::string{ids.numa_node_id(id)->get().value()});
        });
        JsonPointerCleanup numa_cleanup{numa ? &*numa : nullptr};
        core::detail::ExpectedErrorCleanup numa_error_cleanup{numa};
        auto local = attribute_json(device.local_pus, [&](const ProcessingUnitSet& set) {
            auto projected = pu_ids(set, ids);
            JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
            core::detail::ExpectedErrorCleanup projected_error_cleanup{projected};
            return Json(std::move(*projected));
        });
        JsonPointerCleanup local_cleanup{local ? &*local : nullptr};
        core::detail::ExpectedErrorCleanup local_error_cleanup{local};
        if (!numa || !local)
            return std::unexpected{!numa ? std::move(numa.error()) : std::move(local.error())};
        append_object_json(objects["pci_functions"], [&](Json& json_target_14) {
            set_json(json_target_14, "id", entry.semantic_id.value());
            set_json(json_target_14, "vendor_id", std::format("{:04x}", device.vendor_id));
            set_json(json_target_14, "device_id", std::format("{:04x}", device.device_id));
            set_json(json_target_14, "base_class",
                     std::format("{:02x}", (device.class_code >> 16U) & 0xffU));
            set_json(json_target_14, "subclass",
                     std::format("{:02x}", (device.class_code >> 8U) & 0xffU));
            set_json(json_target_14, "programming_interface",
                     std::format("{:02x}", device.class_code & 0xffU));
            set_json(json_target_14, "numa_locality", std::move(*numa));
            set_json(json_target_14, "local_processing_units", std::move(*local));
        });
    }

    struct Distance {
        std::string from;
        std::string to;
        std::uint32_t value;
    };
    std::vector<Distance> ordered;
    for (const auto& distance : topology.numa_distances().directed_distances)
        ordered.push_back({std::string{ids.numa_node_id(distance.from)->get().value()},
                           std::string{ids.numa_node_id(distance.to)->get().value()},
                           distance.value});
    std::ranges::sort(ordered, [](const auto& a, const auto& b) {
        return std::tie(a.from, a.to) < std::tie(b.from, b.to);
    });
    auto distances = Json::array();
    JsonCleanup distances_cleanup{distances};
    for (const auto& distance : ordered)
        append_object_json(distances, [&](Json& json_target_15) {
            set_json(json_target_15, "from", distance.from);
            set_json(json_target_15, "to", distance.to);
            set_json(json_target_15, "meaning", "latency");
            set_json(json_target_15, "unit", "relative");
            set_json(json_target_15, "value", distance.value);
        });

    return json_object([&](Json& json_target_16) {
        set_json(json_target_16, "profile", profile);
        set_json(json_target_16, "projection", "physical");
        set_json(json_target_16, "machine", json_object([&](Json& json_target_17) {
                     set_json(json_target_17, "id", ids.machine_id().value());
                     set_json(json_target_17, "architecture", machine.architecture);
                     set_json(json_target_17, "cpu_vendor", std::move(*vendor));
                     set_json(json_target_17, "cpu_model", std::move(*model));
                     set_json(json_target_17, "installed_memory_bytes", std::move(*memory));
                     set_json(json_target_17, "page_sizes_bytes", std::move(*machine_pages));
                     set_json(json_target_17, "present_cpus", machine.present_cpus.to_range_list());
                     set_json(json_target_17, "present_nodes",
                              machine.present_nodes.to_range_list());
                 }));
        set_json(json_target_16, "objects", std::move(objects));
        set_json(json_target_16, "distances", std::move(distances));
    });
}

auto effective_json(const TopologySnapshot& topology, const StableTopologyIds& ids,
                    std::string_view physical) -> std::expected<Json, core::Error> {
    const auto& machine = topology.machine();
    auto cpu_set = [](const core::CpuSet& v) { return Json(v.to_range_list()); };
    auto node_set = [](const core::NodeSet& v) { return Json(v.to_range_list()); };
    auto allowed_cpus = attribute_json(machine.allowed_cpus, cpu_set);
    JsonPointerCleanup allowed_cpus_cleanup{allowed_cpus ? &*allowed_cpus : nullptr};
    core::detail::ExpectedErrorCleanup allowed_cpus_error_cleanup{allowed_cpus};
    auto effective_cpus = attribute_json(machine.effective_cpus, cpu_set);
    JsonPointerCleanup effective_cpus_cleanup{effective_cpus ? &*effective_cpus : nullptr};
    core::detail::ExpectedErrorCleanup effective_cpus_error_cleanup{effective_cpus};
    auto allowed_nodes = attribute_json(machine.allowed_nodes, node_set);
    JsonPointerCleanup allowed_nodes_cleanup{allowed_nodes ? &*allowed_nodes : nullptr};
    core::detail::ExpectedErrorCleanup allowed_nodes_error_cleanup{allowed_nodes};
    auto effective_nodes = attribute_json(machine.effective_nodes, node_set);
    JsonPointerCleanup effective_nodes_cleanup{effective_nodes ? &*effective_nodes : nullptr};
    core::detail::ExpectedErrorCleanup effective_nodes_error_cleanup{effective_nodes};
    auto pus = Json::array();
    JsonCleanup pus_cleanup{pus};
    for (const auto& entry : ids.processing_units()) {
        auto isolated = attribute_json(topology.processing_unit(entry.handle)->isolated,
                                       [](bool v) { return Json(v); });
        JsonPointerCleanup isolated_cleanup{isolated ? &*isolated : nullptr};
        core::detail::ExpectedErrorCleanup isolated_error_cleanup{isolated};
        if (!isolated)
            return std::unexpected{std::move(isolated.error())};
        append_object_json(pus, [&](Json& json_target_18) {
            set_json(json_target_18, "id", entry.semantic_id.value());
            set_json(json_target_18, "isolated", std::move(*isolated));
        });
    }
    return json_object([&](Json& json_target_19) {
        set_json(json_target_19, "profile", profile);
        set_json(json_target_19, "projection", "effective");
        set_json(json_target_19, "physical", physical);
        set_json(json_target_19, "resources", json_object([&](Json& json_target_20) {
                     set_json(json_target_20, "online_cpus", machine.online_cpus.to_range_list());
                     set_json(json_target_20, "process_allowed_cpus", std::move(*allowed_cpus));
                     set_json(json_target_20, "effective_cpus", std::move(*effective_cpus));
                     set_json(json_target_20, "online_nodes", machine.online_nodes.to_range_list());
                     set_json(json_target_20, "process_allowed_nodes", std::move(*allowed_nodes));
                     set_json(json_target_20, "effective_nodes", std::move(*effective_nodes));
                 }));
        set_json(json_target_19, "processing_units", std::move(pus));
    });
}

} // namespace

auto PhysicalTopologyFingerprint::parse(std::string_view text)
    -> std::expected<PhysicalTopologyFingerprint, core::Error> {
    auto parsed = parse_digest(text, "physical");
    if (!parsed)
        return std::unexpected{std::move(parsed.error())};
    return PhysicalTopologyFingerprint{*parsed};
}

auto EffectiveTopologyFingerprint::parse(std::string_view text)
    -> std::expected<EffectiveTopologyFingerprint, core::Error> {
    auto parsed = parse_digest(text, "effective");
    if (!parsed)
        return std::unexpected{std::move(parsed.error())};
    return EffectiveTopologyFingerprint{*parsed};
}

class FingerprintFactory {
  public:
    static auto physical(core::Sha256Digest digest) { return PhysicalTopologyFingerprint{digest}; }
    static auto effective(core::Sha256Digest digest) {
        return EffectiveTopologyFingerprint{digest};
    }
};

auto PhysicalTopologyFingerprint::to_string() const -> std::string {
    return "sha256:" + digest_.to_hex();
}
auto EffectiveTopologyFingerprint::to_string() const -> std::string {
    return "sha256:" + digest_.to_hex();
}

namespace detail {
auto canonical_physical_fingerprint_projection(const TopologySnapshot& topology,
                                               const StableTopologyIds& ids)
    -> std::expected<std::string, core::Error> {
    auto value = physical_json(topology, ids);
    JsonPointerCleanup value_cleanup{value ? &*value : nullptr};
    core::detail::ExpectedErrorCleanup value_error_cleanup{value};
    return value ? canonical(*value) : std::unexpected{std::move(value.error())};
}
auto canonical_effective_fingerprint_projection(const TopologySnapshot& topology,
                                                const StableTopologyIds& ids,
                                                std::string_view physical)
    -> std::expected<std::string, core::Error> {
    auto value = effective_json(topology, ids, physical);
    JsonPointerCleanup value_cleanup{value ? &*value : nullptr};
    core::detail::ExpectedErrorCleanup value_error_cleanup{value};
    return value ? canonical(*value) : std::unexpected{std::move(value.error())};
}
} // namespace detail

auto detail::derive_topology_fingerprints_with_failure_sink(const TopologySnapshot& topology,
    const StableTopologyIds& ids, std::optional<core::Error>* raw_failure)
    -> std::expected<TopologyFingerprints, core::Error> {
    auto required = check_required(topology);
    core::detail::ExpectedErrorCleanup required_error_cleanup{required};
    if (!required)
        return std::unexpected{std::move(required.error())};
    auto validation = detail::validate_topology_with_failure_sink(topology, raw_failure);
    ReportCleanup validation_cleanup{validation};
    if (!validation.valid()) {
        const core::Error* actual = &validation.diagnostics.front();
        if (raw_failure && !*raw_failure) {
            raw_failure->emplace(std::move(validation.diagnostics.front()));
            actual = &**raw_failure;
        }
        return std::unexpected{error("topology.fingerprint.invalid_topology",
                                     "Fingerprinting requires a valid topology", "snapshot",
                                     handle(topology.machine().id), "invariants", "invalid",
                                     actual->code.value())};
    }
    auto derived = detail::PreparedStableTopologyIdAccess::derive_with_failure_sink(topology, raw_failure);
    core::detail::ExpectedErrorCleanup derived_error_cleanup{derived};
    if (!derived) {
        const core::Error* actual = &derived.error();
        if (raw_failure && !*raw_failure) {
            raw_failure->emplace(std::move(derived.error()));
            actual = &**raw_failure;
        }
        return std::unexpected{error("topology.fingerprint.stable_ids_mismatch",
                                     "Stable IDs do not match the topology", "snapshot",
                                     handle(topology.machine().id), "stable_ids", "mismatch",
                                     actual->code.value())};
    }
    if (!equal_ids(ids, *derived))
        return std::unexpected{error("topology.fingerprint.stable_ids_mismatch",
                                     "Stable IDs do not match the topology", "snapshot",
                                     handle(topology.machine().id), "stable_ids", "mismatch", "")};
    auto physical_bytes = detail::canonical_physical_fingerprint_projection(topology, ids);
    if (!physical_bytes)
        return std::unexpected{std::move(physical_bytes.error())};
    auto physical = FingerprintFactory::physical(core::sha256(*physical_bytes));
    auto effective_bytes =
        detail::canonical_effective_fingerprint_projection(topology, ids, physical.to_string());
    if (!effective_bytes)
        return std::unexpected{std::move(effective_bytes.error())};
    return TopologyFingerprints{std::move(physical),
                                FingerprintFactory::effective(core::sha256(*effective_bytes))};
}

auto derive_topology_fingerprints(const TopologySnapshot& topology, const StableTopologyIds& ids)
    -> std::expected<TopologyFingerprints, core::Error> {
    return detail::derive_topology_fingerprints_with_failure_sink(topology, ids, nullptr);
}

} // namespace epycbench::topology
