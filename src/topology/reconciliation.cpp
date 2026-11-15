#include "epycbench/topology/reconciliation.hpp"

#include "epycbench/topology/invariants.hpp"
#include "core/json_ownership.hpp"
#include "topology/page_size_projection.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace epycbench::topology {
namespace {

using Json = nlohmann::json;
using JsonOwner = core::detail::OwnedJson<Json>;

// Like invariant validation's pending diagnostics, each construction/sort
// temporary owns retirement of its Core tree even when allocation stays denied.
struct DiagnosticError final : core::Error {
    explicit DiagnosticError(core::Error&& value) noexcept : core::Error(std::move(value)) {}
    DiagnosticError(DiagnosticError&& other) noexcept : core::Error(std::move(other)) {}
    auto operator=(DiagnosticError&& other) noexcept -> DiagnosticError& {
        if (this != &other) {
            core::detail::retire_json(context);
            core::Error::operator=(std::move(other));
        }
        return *this;
    }
    ~DiagnosticError() { core::detail::retire_json(context); }
};
struct PendingDiagnostic {
    DiagnosticError error;
    bool material{};
    std::size_t ordinal{};
    using SortKey = std::tuple<std::string, std::string, std::string, std::string, std::string>;
    std::optional<SortKey> source_sort_key{};
};
static_assert(std::is_nothrow_move_constructible_v<PendingDiagnostic>);
static_assert(std::is_nothrow_move_assignable_v<PendingDiagnostic>);
struct PendingDiagnostics final {
    std::vector<PendingDiagnostic> values;
    std::optional<core::Error>* sink{};
    std::size_t next{};
    explicit PendingDiagnostics(std::optional<core::Error>* raw = nullptr) noexcept : sink{raw} {}
    auto begin() noexcept { return values.begin(); }
    auto end() noexcept { return values.end(); }
    auto begin() const noexcept { return values.begin(); }
    auto end() const noexcept { return values.end(); }
    auto size() const noexcept { return values.size(); }
    void retain(PendingDiagnostic* candidate = nullptr) noexcept {
        if (!sink || *sink) return;
        auto* first = candidate && candidate->material ? candidate : nullptr;
        for (auto& value : values)
            if (value.material && (!first || value.ordinal < first->ordinal)) first = &value;
        if (first) sink->emplace(std::move(static_cast<core::Error&>(first->error)));
    }
    void push_back(PendingDiagnostic&& candidate) {
        candidate.ordinal = next++;
        try {
            if (sink) {
                // Exactly the existing canonical key, constructed while the
                // completed candidate still has a guarded owning lifetime.
                // Sorting cannot lose an allocating-comparator pivot.
                const auto& context = candidate.error.context;
                candidate.source_sort_key.emplace(candidate.error.code.value(),
                    context.value("object_type", std::string{}),
                    context.value("semantic_key", std::string{}),
                    context.value("field", std::string{}), context.dump());
            }
            values.push_back(std::move(candidate));
        } catch (...) { retain(&candidate); throw; }
    }
};
struct PendingReconciliation {
    std::optional<TopologySnapshot> snapshot;
    PendingDiagnostics diagnostics;
    explicit PendingReconciliation(std::optional<core::Error>* raw = nullptr) noexcept : diagnostics{raw} {}
};
struct ReportCleanup {
    ValidationReport& report;
    ~ReportCleanup() {
        for (auto& error : report.diagnostics) core::detail::retire_json(error.context);
    }
};
static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_constructible_v<ReconciliationDiagnostic>);
static_assert(std::is_nothrow_move_constructible_v<ReconciliationResult>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<TopologySnapshot>>);

auto finish(PendingReconciliation&& pending) -> std::expected<ReconciliationResult, core::Error> {
    ReconciliationResult result;
    // All fallible growth precedes transfer out of the owning pending values.
    result.diagnostics.reserve(pending.diagnostics.size());
    for (auto& diagnostic : pending.diagnostics) {
        result.diagnostics.push_back({std::move(static_cast<core::Error&>(diagnostic.error)),
                                      diagnostic.material});
    }
    result.snapshot = std::move(pending.snapshot);
    return std::expected<ReconciliationResult, core::Error>{std::move(result)};
}

template <class T> auto diagnostic_value(T&& value) -> JsonOwner {
    return JsonOwner{Json(std::forward<T>(value))};
}
auto diagnostic_value(JsonOwner&& value) noexcept -> JsonOwner { return std::move(value); }
template <class... T> auto diagnostic_array(T&&... values) -> JsonOwner {
    auto result = Json::array();
    core::detail::JsonCleanup cleanup{result};
    (result.push_back(Json(std::forward<T>(values))), ...);
    return JsonOwner{std::move(result)};
}

auto provenance_source(TopologyEvidenceSource source) -> ProvenanceSource {
    switch (source) {
    case TopologyEvidenceSource::hwloc:
        return ProvenanceSource::hwloc;
    case TopologyEvidenceSource::linux_sysfs:
        return ProvenanceSource::linux_sysfs;
    case TopologyEvidenceSource::linux_procfs:
        return ProvenanceSource::linux_procfs;
    }
    return ProvenanceSource::derived;
}

auto confidence(SourceConfidence value) -> Confidence {
    switch (value) {
    case SourceConfidence::reported:
        return Confidence::reported;
    case SourceConfidence::derived:
        return Confidence::derived;
    case SourceConfidence::inferred:
        return Confidence::inferred;
    case SourceConfidence::unknown:
        return Confidence::unknown;
    }
    return Confidence::unknown;
}

auto normalize(std::vector<SourceProvenance> values) -> std::vector<SourceProvenance> {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

auto model_evidence(std::vector<SourceProvenance> values) -> std::vector<Evidence> {
    values = normalize(std::move(values));
    std::vector<Evidence> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(
            {provenance_source(value.source), value.locator, confidence(value.confidence)});
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

auto provenance_json(std::vector<SourceProvenance> values) -> JsonOwner {
    values = normalize(std::move(values));
    auto result = nlohmann::json::array();
    core::detail::JsonCleanup cleanup{result};
    for (const auto& value : values) {
        result.push_back(value.locator);
    }
    return JsonOwner{std::move(result)};
}

auto internal_error(std::string code, std::string message) -> core::Error {
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::move(code)},
            .message = std::move(message),
            .operation = "reconcile_topology",
            .os_error = std::nullopt,
            .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::fatal,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <class H, class L>
void add_diagnostic(PendingDiagnostics& diagnostics, std::string code,
                    std::string message, bool material, std::string object_type,
                    std::string semantic_key, std::string field, H&& hwloc_value,
                    L&& linux_value, std::vector<SourceProvenance> hwloc_provenance = {},
                    std::vector<SourceProvenance> linux_provenance = {}) {
    auto hwloc_owned = diagnostic_value(std::forward<H>(hwloc_value));
    auto linux_owned = diagnostic_value(std::forward<L>(linux_value));
    auto hwloc_sources = provenance_json(std::move(hwloc_provenance));
    auto linux_sources = provenance_json(std::move(linux_provenance));
    auto context = core::detail::json_object<Json>([&](auto& object) {
        core::detail::set_json(object, "object_type", std::move(object_type));
        core::detail::set_json(object, "semantic_key", std::move(semantic_key));
        core::detail::set_json(object, "field", std::move(field));
        core::detail::set_json(object, "hwloc_value", std::move(hwloc_owned.value));
        core::detail::set_json(object, "linux_value", std::move(linux_owned.value));
        core::detail::set_json(object, "hwloc_provenance", std::move(hwloc_sources.value));
        core::detail::set_json(object, "linux_provenance", std::move(linux_sources.value));
    });
    core::detail::JsonCleanup cleanup{context};
    diagnostics.push_back(
        {.error = DiagnosticError{{.category = core::ErrorCategory::topology,
                   .code = core::StableErrorCode{std::move(code)},
                   .message = std::move(message),
                   .operation = "reconcile_topology",
                   .os_error = std::nullopt,
                   .context = std::move(context),
                   .severity = material ? core::ErrorSeverity::error : core::ErrorSeverity::warning,
                   .continuation = material ? core::ContinuationPolicy::abort_command
                                            : core::ContinuationPolicy::continue_operation}},
         .material = material});
}

auto cpu_key(const core::CpuSet& value) -> std::string { return value.to_range_list(); }

auto object_key(const SourceObjectKey& value) -> std::string {
    return cpu_key(value.member_cpus) + '#' +
           (value.os_id ? std::to_string(*value.os_id) : std::string{"?"});
}

template <typename T>
auto observation_summary(const SourceObservation<T>& observation) -> nlohmann::json {
    if (!observation.value) {
        return observation.availability == SourceAvailability::unknown ? "unknown" : "unavailable";
    }
    if constexpr (std::same_as<T, core::CpuSet> || std::same_as<T, core::NodeSet>) {
        return observation.value->to_range_list();
    } else if constexpr (requires { observation.value->value(); }) {
        return observation.value->value();
    } else {
        return *observation.value;
    }
}

template <typename T>
auto merge_attribute(const SourceObservation<T>& hwloc, const SourceObservation<T>& linux,
                     PendingDiagnostics& diagnostics, std::string code,
                     std::string object_type, std::string semantic_key, std::string field,
                     bool material) -> Attribute<T> {
    if (hwloc.value && linux.value) {
        if (*hwloc.value == *linux.value) {
            auto provenance = hwloc.provenance;
            provenance.insert(provenance.end(), linux.provenance.begin(), linux.provenance.end());
            return Attribute<T>::available(*hwloc.value, model_evidence(std::move(provenance)));
        }
        add_diagnostic(diagnostics, std::move(code), "Available source values disagree", material,
                       std::move(object_type), std::move(semantic_key), std::move(field),
                       observation_summary(hwloc), observation_summary(linux), hwloc.provenance,
                       linux.provenance);
        if (material) {
            return Attribute<T>::unknown();
        }
        const auto hwloc_model = model_evidence(hwloc.provenance);
        const auto linux_model = model_evidence(linux.provenance);
        return Attribute<T>::conflicted(
            {*hwloc.value, hwloc_model.empty() ? Evidence{} : hwloc_model.front()},
            {*linux.value, linux_model.empty() ? Evidence{} : linux_model.front()});
    }
    if (hwloc.value) {
        return Attribute<T>::available(*hwloc.value, model_evidence(hwloc.provenance));
    }
    if (linux.value) {
        return Attribute<T>::available(*linux.value, model_evidence(linux.provenance));
    }
    auto provenance = hwloc.provenance;
    provenance.insert(provenance.end(), linux.provenance.begin(), linux.provenance.end());
    if (hwloc.availability == SourceAvailability::unknown &&
        linux.availability == SourceAvailability::unknown) {
        return Attribute<T>::unknown(model_evidence(std::move(provenance)));
    }
    return Attribute<T>::unavailable(model_evidence(std::move(provenance)));
}

template <typename T> auto single_attribute(const SourceObservation<T>& value) -> Attribute<T> {
    if (value.value) {
        return Attribute<T>::available(*value.value, model_evidence(value.provenance));
    }
    if (value.availability == SourceAvailability::unknown) {
        return Attribute<T>::unknown(model_evidence(value.provenance));
    }
    return Attribute<T>::unavailable(model_evidence(value.provenance));
}

template <typename Object, typename KeyFunction>
auto unique_map(const std::vector<Object>& objects, KeyFunction key,
                PendingDiagnostics& diagnostics, std::string code,
                std::string object_type, TopologyEvidenceSource source)
    -> std::map<std::string, const Object*> {
    std::map<std::string, const Object*> result;
    for (const auto& object : objects) {
        auto semantic_key = key(object);
        const auto [iterator, inserted] = result.emplace(semantic_key, &object);
        if (!inserted) {
            const auto is_hwloc = source == TopologyEvidenceSource::hwloc;
            add_diagnostic(
                diagnostics, code, "Source contains an ambiguous semantic match", true, object_type,
                semantic_key, "semantic_key",
                is_hwloc ? diagnostic_array("duplicate") : diagnostic_array("not_applicable"),
                is_hwloc ? diagnostic_array("not_applicable") : diagnostic_array("duplicate"));
            static_cast<void>(iterator);
        }
    }
    return result;
}

template <typename Map> auto keys(const Map& values) -> std::vector<std::string> {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& [key, value] : values) {
        static_cast<void>(value);
        result.push_back(key);
    }
    return result;
}

auto json_keys(const auto& values) -> JsonOwner {
    auto result = nlohmann::json::array();
    core::detail::JsonCleanup cleanup{result};
    for (const auto& value : values) {
        result.push_back(value);
    }
    return JsonOwner{std::move(result)};
}

void compare_key_sets(const auto& hwloc, const auto& linux,
                      PendingDiagnostics& diagnostics, std::string code,
                      std::string object_type, std::string field) {
    if (keys(hwloc) != keys(linux)) {
        add_diagnostic(diagnostics, std::move(code), "Structural source inventories disagree", true,
                       std::move(object_type), "inventory", std::move(field),
                       json_keys(keys(hwloc)), json_keys(keys(linux)));
    }
}

auto cache_key(const CacheSourceEvidence& cache) -> std::string {
    return std::to_string(cache.level) + ':' + std::to_string(static_cast<int>(cache.kind)) + ':' +
           cpu_key(cache.shared_cpus);
}

auto group_key(const ArchitectureGroupSourceEvidence& group) -> std::string {
    return cpu_key(group.key.member_cpus);
}

auto node_key(const NumaNodeSourceEvidence& node) -> std::string {
    return node.linux_node_id.value ? std::to_string(*node.linux_node_id.value)
                                    : std::string{"members:"} + cpu_key(node.member_cpus);
}

auto distance_key(const NumaDistanceValue& value) -> std::string {
    return std::to_string(value.from_linux_node) + "->" + std::to_string(value.to_linux_node);
}

auto cache_kind(SourceCacheKind kind) -> CacheKind {
    switch (kind) {
    case SourceCacheKind::data:
        return CacheKind::data;
    case SourceCacheKind::instruction:
        return CacheKind::instruction;
    case SourceCacheKind::unified:
        return CacheKind::unified;
    case SourceCacheKind::unknown:
        return CacheKind::unknown;
    }
    return CacheKind::unknown;
}

auto to_pus(const core::CpuSet& cpus, const std::map<std::uint32_t, core::ProcessingUnitId>& pu_ids)
    -> std::optional<ProcessingUnitSet> {
    ProcessingUnitSet result;
    for (const auto cpu : cpus) {
        const auto found = pu_ids.find(cpu.value());
        if (found == pu_ids.end()) {
            return std::nullopt;
        }
        result = result.set_union(ProcessingUnitSet{found->second});
    }
    return result;
}

auto source_attribute(std::optional<std::uint32_t> value,
                      const std::vector<SourceProvenance>& provenance) -> Attribute<std::uint32_t> {
    if (value) {
        return Attribute<std::uint32_t>::available(*value, model_evidence(provenance));
    }
    return Attribute<std::uint32_t>::unavailable(model_evidence(provenance));
}

auto merge_source_id(std::optional<std::uint32_t> hwloc_value,
                     const std::vector<SourceProvenance>& hwloc_provenance,
                     std::optional<std::uint32_t> linux_value,
                     const std::vector<SourceProvenance>& linux_provenance,
                     PendingDiagnostics& diagnostics, std::string code,
                     std::string object_type, std::string semantic_key)
    -> Attribute<std::uint32_t> {
    if (hwloc_value && linux_value) {
        if (*hwloc_value == *linux_value) {
            auto combined = hwloc_provenance;
            combined.insert(combined.end(), linux_provenance.begin(), linux_provenance.end());
            return Attribute<std::uint32_t>::available(*hwloc_value,
                                                       model_evidence(std::move(combined)));
        }
        add_diagnostic(diagnostics, std::move(code), "Available source OS identifiers disagree",
                       false, std::move(object_type), std::move(semantic_key), "os_id",
                       *hwloc_value, *linux_value, hwloc_provenance, linux_provenance);
        const auto hwloc_model = model_evidence(hwloc_provenance);
        const auto linux_model = model_evidence(linux_provenance);
        return Attribute<std::uint32_t>::conflicted(
            {*hwloc_value, hwloc_model.empty() ? Evidence{} : hwloc_model.front()},
            {*linux_value, linux_model.empty() ? Evidence{} : linux_model.front()});
    }
    if (hwloc_value) {
        return Attribute<std::uint32_t>::available(*hwloc_value, model_evidence(hwloc_provenance));
    }
    if (linux_value) {
        return Attribute<std::uint32_t>::available(*linux_value, model_evidence(linux_provenance));
    }
    auto combined = hwloc_provenance;
    combined.insert(combined.end(), linux_provenance.begin(), linux_provenance.end());
    return Attribute<std::uint32_t>::unavailable(model_evidence(std::move(combined)));
}

auto construction_code(TopologyConstructionErrorCode code) -> std::string {
    return std::to_string(static_cast<unsigned>(code));
}

void sort_diagnostics(PendingDiagnostics& diagnostics) {
    if (diagnostics.sink) {
        std::ranges::sort(diagnostics, [](const auto& left, const auto& right) noexcept {
            return *left.source_sort_key < *right.source_sort_key;
        });
        return;
    }
    std::ranges::sort(diagnostics, [](const auto& left, const auto& right) {
        const auto key = [](const auto& diagnostic) {
            const auto& context = diagnostic.error.context;
            return std::tuple{diagnostic.error.code.value(),
                              context.value("object_type", std::string{}),
                              context.value("semantic_key", std::string{}),
                              context.value("field", std::string{}), context.dump()};
        };
        return key(left) < key(right);
    });
}

auto has_material(const PendingDiagnostics& diagnostics) -> bool {
    return std::ranges::any_of(diagnostics, &PendingDiagnostic::material);
}

} // namespace

auto detail::reconcile_topology_with_failure_sink(const TopologySourceEvidence& first,
    const TopologySourceEvidence& second, std::optional<core::Error>* raw_failure)
    -> std::expected<ReconciliationResult, core::Error> {
    const TopologySourceEvidence* hwloc = nullptr;
    const TopologySourceEvidence* linux = nullptr;
    for (const auto* source : {&first, &second}) {
        if (source->source == TopologyEvidenceSource::hwloc) {
            if (hwloc != nullptr) {
                return std::unexpected{
                    internal_error("topology.reconciliation.invalid_sources",
                                   "Reconciliation requires exactly one hwloc evidence source")};
            }
            hwloc = source;
        } else {
            if (linux != nullptr) {
                return std::unexpected{
                    internal_error("topology.reconciliation.invalid_sources",
                                   "Reconciliation requires exactly one Linux evidence source")};
            }
            linux = source;
        }
    }
    if (hwloc == nullptr || linux == nullptr) {
        return std::unexpected{internal_error("topology.reconciliation.invalid_sources",
                                              "Required reconciliation sources are missing")};
    }

    PendingReconciliation result{raw_failure};
    try {

    const auto hwloc_pus = unique_map(
        hwloc->processing_units,
        [](const auto& pu) { return std::to_string(pu.linux_cpu_id.value()); }, result.diagnostics,
        "topology.reconciliation.cpu.ambiguous_match", "processing_unit", hwloc->source);
    const auto linux_pus = unique_map(
        linux->processing_units,
        [](const auto& pu) { return std::to_string(pu.linux_cpu_id.value()); }, result.diagnostics,
        "topology.reconciliation.cpu.ambiguous_match", "processing_unit", linux->source);
    compare_key_sets(hwloc_pus, linux_pus, result.diagnostics,
                     "topology.reconciliation.cpu.inventory_conflict", "processing_unit",
                     "linux_cpu_id");
    for (const auto& [key, hwloc_pu] : hwloc_pus) {
        const auto found = linux_pus.find(key);
        if (found == linux_pus.end()) {
            continue;
        }
        const auto* linux_pu = found->second;
        if (hwloc_pu->online.value && linux_pu->online.value &&
            hwloc_pu->online.value != linux_pu->online.value) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.pu.online_conflict",
                           "Available PU online states disagree", true, "processing_unit", key,
                           "online", *hwloc_pu->online.value, *linux_pu->online.value,
                           hwloc_pu->online.provenance, linux_pu->online.provenance);
        }
        if (hwloc_pu->core.member_cpus != linux_pu->core.member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.core.ownership_conflict",
                           "PU-to-core ownership disagrees", true, "processing_unit", key,
                           "core_members", cpu_key(hwloc_pu->core.member_cpus),
                           cpu_key(linux_pu->core.member_cpus), hwloc_pu->provenance,
                           linux_pu->provenance);
        }
        const auto ownership_conflict = [&](const auto& left, const auto& right, std::string code,
                                            std::string field) {
            if (left.has_value() != right.has_value() ||
                (left && right && left->member_cpus != right->member_cpus)) {
                add_diagnostic(result.diagnostics, std::move(code),
                               "PU structural ownership disagrees", true, "processing_unit", key,
                               std::move(field), left ? cpu_key(left->member_cpus) : "unavailable",
                               right ? cpu_key(right->member_cpus) : "unavailable",
                               hwloc_pu->provenance, linux_pu->provenance);
            }
        };
        ownership_conflict(hwloc_pu->package, linux_pu->package,
                           "topology.reconciliation.package.ownership_conflict", "package");
        ownership_conflict(hwloc_pu->die, linux_pu->die,
                           "topology.reconciliation.die.ownership_conflict", "die");
    }

    const auto hwloc_cores = unique_map(
        hwloc->cores, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.core.ambiguous_match", "core", hwloc->source);
    const auto linux_cores = unique_map(
        linux->cores, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.core.ambiguous_match", "core", linux->source);
    compare_key_sets(hwloc_cores, linux_cores, result.diagnostics,
                     "topology.reconciliation.core.ownership_conflict", "core", "member_cpus");
    for (const auto& [key, hwloc_core] : hwloc_cores) {
        const auto found = linux_cores.find(key);
        if (found == linux_cores.end()) {
            continue;
        }
        const auto* linux_core = found->second;
        if (hwloc_core->package && linux_core->package &&
            hwloc_core->package->member_cpus != linux_core->package->member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.core.package_conflict",
                           "Core-to-package relationships disagree", true, "core", key, "package",
                           cpu_key(hwloc_core->package->member_cpus),
                           cpu_key(linux_core->package->member_cpus), hwloc_core->provenance,
                           linux_core->provenance);
        }
        if (hwloc_core->die && linux_core->die &&
            hwloc_core->die->member_cpus != linux_core->die->member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.core.die_conflict",
                           "Core-to-die relationships disagree", true, "core", key, "die",
                           cpu_key(hwloc_core->die->member_cpus),
                           cpu_key(linux_core->die->member_cpus), hwloc_core->provenance,
                           linux_core->provenance);
        }
    }

    const auto hwloc_packages = unique_map(
        hwloc->packages, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.package.ambiguous_match", "package",
        hwloc->source);
    const auto linux_packages = unique_map(
        linux->packages, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.package.ambiguous_match", "package",
        linux->source);
    compare_key_sets(hwloc_packages, linux_packages, result.diagnostics,
                     "topology.reconciliation.package.ownership_conflict", "package",
                     "member_cpus");

    const auto hwloc_dies = unique_map(
        hwloc->dies, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.die.ambiguous_match", "die", hwloc->source);
    const auto linux_dies = unique_map(
        linux->dies, [](const auto& value) { return cpu_key(value.key.member_cpus); },
        result.diagnostics, "topology.reconciliation.die.ambiguous_match", "die", linux->source);
    compare_key_sets(hwloc_dies, linux_dies, result.diagnostics,
                     "topology.reconciliation.die.ownership_conflict", "die", "member_cpus");
    for (const auto& [key, hwloc_die] : hwloc_dies) {
        const auto found = linux_dies.find(key);
        if (found != linux_dies.end() &&
            hwloc_die->package.member_cpus != found->second->package.member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.die.package_conflict",
                           "Die-to-package relationships disagree", true, "die", key, "package",
                           cpu_key(hwloc_die->package.member_cpus),
                           cpu_key(found->second->package.member_cpus), hwloc_die->provenance,
                           found->second->provenance);
        }
    }

    const auto hwloc_groups = unique_map(hwloc->architecture_groups, group_key, result.diagnostics,
                                         "topology.reconciliation.group.ambiguous_match",
                                         "architecture_group", hwloc->source);
    const auto linux_groups = unique_map(linux->architecture_groups, group_key, result.diagnostics,
                                         "topology.reconciliation.group.ambiguous_match",
                                         "architecture_group", linux->source);
    const auto group_package = [](const ArchitectureGroupSourceEvidence& group,
                                  const auto& packages) -> std::string {
        std::vector<std::string> candidates;
        for (const auto& [key, package] : packages) {
            if (group.key.member_cpus.is_subset_of(package->key.member_cpus)) {
                candidates.push_back(key);
            }
        }
        return candidates.size() == 1 ? candidates.front()
               : candidates.empty()   ? std::string{"unavailable"}
                                      : std::string{"ambiguous"};
    };
    for (const auto& [key, hwloc_group] : hwloc_groups) {
        const auto found = linux_groups.find(key);
        if (found == linux_groups.end()) {
            const auto same_kind =
                std::ranges::find_if(linux->architecture_groups, [&](const auto& candidate) {
                    return candidate.kind == hwloc_group->kind;
                });
            if (same_kind != linux->architecture_groups.end()) {
                add_diagnostic(
                    result.diagnostics, "topology.reconciliation.group.membership_conflict",
                    "Architecture-group memberships disagree", true, "architecture_group",
                    hwloc_group->kind, "member_cpus", cpu_key(hwloc_group->key.member_cpus),
                    cpu_key(same_kind->key.member_cpus), hwloc_group->provenance,
                    same_kind->provenance);
            }
            continue;
        }
        const auto* linux_group = found->second;
        const auto hwloc_package = group_package(*hwloc_group, hwloc_packages);
        const auto linux_package = group_package(*linux_group, linux_packages);
        if (hwloc_package != "unavailable" && linux_package != "unavailable" &&
            hwloc_package != linux_package) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.group.package_conflict",
                           "Architecture-group package contexts disagree", true,
                           "architecture_group", key, "package", hwloc_package, linux_package,
                           hwloc_group->provenance, linux_group->provenance);
        }
        if (hwloc_group->parent && linux_group->parent &&
            hwloc_group->parent->member_cpus != linux_group->parent->member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.group.parent_conflict",
                           "Architecture-group parent relationships disagree", true,
                           "architecture_group", key, "parent",
                           cpu_key(hwloc_group->parent->member_cpus),
                           cpu_key(linux_group->parent->member_cpus), hwloc_group->provenance,
                           linux_group->provenance);
        }
        if (hwloc_group->kind != linux_group->kind) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.group.kind_conflict",
                           "Architecture-group kinds disagree", false, "architecture_group", key,
                           "kind", hwloc_group->kind, linux_group->kind, hwloc_group->provenance,
                           linux_group->provenance);
        }
        static_cast<void>(merge_attribute(hwloc_group->subtype, found->second->subtype,
                                          result.diagnostics,
                                          "topology.reconciliation.group.subtype_conflict",
                                          "architecture_group", key, "subtype", false));
    }

    const auto hwloc_caches =
        unique_map(hwloc->caches, cache_key, result.diagnostics,
                   "topology.reconciliation.cache.ambiguous_match", "cache", hwloc->source);
    const auto linux_caches =
        unique_map(linux->caches, cache_key, result.diagnostics,
                   "topology.reconciliation.cache.ambiguous_match", "cache", linux->source);
    compare_key_sets(hwloc_caches, linux_caches, result.diagnostics,
                     "topology.reconciliation.cache.sharing_conflict", "cache",
                     "level_kind_shared_cpus");
    for (const auto& [key, hwloc_cache] : hwloc_caches) {
        const auto found = linux_caches.find(key);
        if (found == linux_caches.end()) {
            continue;
        }
        const auto* linux_cache = found->second;
        const auto compare_required = [&](const auto& left, const auto& right,
                                          std::string_view field) {
            if (left != right) {
                add_diagnostic(result.diagnostics,
                               "topology.reconciliation.cache.attribute_conflict",
                               "Cache descriptive attributes disagree", false, "cache", key,
                               std::string{field}, left.value(), right.value(),
                               hwloc_cache->provenance, linux_cache->provenance);
            }
        };
        compare_required(hwloc_cache->size, linux_cache->size, "size");
        compare_required(hwloc_cache->line_size, linux_cache->line_size, "line_size");
        for (const auto& [left, right, field] :
             {std::tuple{&hwloc_cache->source_cache_id, &linux_cache->source_cache_id,
                         "source_cache_id"},
              {&hwloc_cache->physical_line_partition, &linux_cache->physical_line_partition,
               "physical_line_partition"}}) {
            static_cast<void>(merge_attribute(*left, *right, result.diagnostics,
                                              "topology.reconciliation.cache.attribute_conflict",
                                              "cache", key, field, false));
        }
        static_cast<void>(merge_attribute(hwloc_cache->number_of_sets, linux_cache->number_of_sets,
                                          result.diagnostics,
                                          "topology.reconciliation.cache.attribute_conflict",
                                          "cache", key, "number_of_sets", false));
        static_cast<void>(merge_attribute(hwloc_cache->allocation_policy,
                                          linux_cache->allocation_policy, result.diagnostics,
                                          "topology.reconciliation.cache.attribute_conflict",
                                          "cache", key, "allocation_policy", false));
        static_cast<void>(merge_attribute(hwloc_cache->write_policy, linux_cache->write_policy,
                                          result.diagnostics,
                                          "topology.reconciliation.cache.attribute_conflict",
                                          "cache", key, "write_policy", false));
    }

    const auto matched_node_key = [](const NumaNodeSourceEvidence& node,
                                     const std::vector<NumaNodeSourceEvidence>& other) {
        if (node.linux_node_id.value) {
            return std::to_string(*node.linux_node_id.value);
        }
        const auto match = std::ranges::find_if(other, [&](const auto& candidate) {
            return candidate.member_cpus == node.member_cpus && candidate.linux_node_id.value;
        });
        return match == other.end() ? node_key(node) : std::to_string(*match->linux_node_id.value);
    };
    const auto hwloc_nodes = unique_map(
        hwloc->numa_nodes,
        [&](const auto& node) { return matched_node_key(node, linux->numa_nodes); },
        result.diagnostics, "topology.reconciliation.numa.ambiguous_match", "numa_node",
        hwloc->source);
    const auto linux_nodes = unique_map(
        linux->numa_nodes,
        [&](const auto& node) { return matched_node_key(node, hwloc->numa_nodes); },
        result.diagnostics, "topology.reconciliation.numa.ambiguous_match", "numa_node",
        linux->source);
    compare_key_sets(hwloc_nodes, linux_nodes, result.diagnostics,
                     "topology.reconciliation.numa.membership_conflict", "numa_node",
                     "linux_node_inventory");
    for (const auto& [key, hwloc_node] : hwloc_nodes) {
        const auto found = linux_nodes.find(key);
        if (found != linux_nodes.end() && hwloc_node->member_cpus != found->second->member_cpus) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.numa.membership_conflict",
                           "NUMA CPU membership disagrees", true, "numa_node", key, "member_cpus",
                           cpu_key(hwloc_node->member_cpus), cpu_key(found->second->member_cpus),
                           hwloc_node->provenance, found->second->provenance);
        }
        if (found != linux_nodes.end()) {
            auto hwloc_pages = hwloc_node->page_sizes;
            auto linux_pages = found->second->page_sizes;
            std::ranges::sort(hwloc_pages);
            std::ranges::sort(linux_pages);
            hwloc_pages.erase(std::unique(hwloc_pages.begin(), hwloc_pages.end()),
                              hwloc_pages.end());
            linux_pages.erase(std::unique(linux_pages.begin(), linux_pages.end()),
                              linux_pages.end());
            if (!hwloc_pages.empty() && !linux_pages.empty() && hwloc_pages != linux_pages) {
                const auto summarize = [](const auto& pages) {
                    auto values = nlohmann::json::array();
                    core::detail::JsonCleanup cleanup{values};
                    for (const auto page : pages) {
                        values.push_back(page.value());
                    }
                    return JsonOwner{std::move(values)};
                };
                add_diagnostic(result.diagnostics,
                               "topology.reconciliation.numa.page_sizes_conflict",
                               "Reported NUMA page-size sets disagree", false, "numa_node", key,
                               "page_sizes", summarize(hwloc_pages), summarize(linux_pages),
                               hwloc_node->provenance, found->second->provenance);
            }
        }
    }

    const auto distance_map = [&](const TopologySourceEvidence& source) {
        std::map<std::string, std::pair<std::uint64_t, const NumaDistanceSourceEvidence*>> values;
        for (const auto& matrix : source.numa_distances) {
            auto nodes = matrix.linux_nodes;
            std::ranges::sort(nodes);
            const auto node_key = [&] {
                std::string key;
                for (const auto node : nodes) {
                    if (!key.empty())
                        key += ',';
                    key += std::to_string(node);
                }
                return key;
            }();
            const auto interpretation = [&] {
                if (matrix.meaning == DistanceMeaning::latency &&
                    (matrix.units == "hwloc-relative-latency" ||
                     matrix.units == "linux-relative-distance"))
                    return std::string{"latency:relative-latency"};
                if (matrix.meaning == DistanceMeaning::unknown)
                    return std::string{"unknown:"} +
                           std::to_string(static_cast<int>(source.source)) + ':' + matrix.units;
                return std::to_string(static_cast<int>(matrix.meaning)) + ':' + matrix.units;
            }();
            for (const auto& value : matrix.directed_values) {
                const auto key = distance_key(value) + ':' + interpretation + ":nodes:" + node_key;
                const auto [existing, inserted] =
                    values.emplace(key, std::pair{value.value, &matrix});
                if (!inserted) {
                    add_diagnostic(
                        result.diagnostics, "topology.reconciliation.distance.ambiguous_match",
                        "Source contains duplicate directed distance evidence", true,
                        "numa_distance", key, "directed_value",
                        source.source == TopologyEvidenceSource::hwloc ? "duplicate"
                                                                       : "not_applicable",
                        source.source == TopologyEvidenceSource::hwloc ? "not_applicable"
                                                                       : "duplicate",
                        source.source == TopologyEvidenceSource::hwloc
                            ? matrix.provenance
                            : std::vector<SourceProvenance>{},
                        source.source == TopologyEvidenceSource::hwloc
                            ? std::vector<SourceProvenance>{}
                            : matrix.provenance);
                    static_cast<void>(existing);
                }
            }
        }
        return values;
    };
    const auto hwloc_distances = distance_map(*hwloc);
    const auto linux_distances = distance_map(*linux);
    compare_key_sets(hwloc_distances, linux_distances, result.diagnostics,
                     "topology.reconciliation.distance.interpretation_conflict", "numa_distance",
                     "pair_meaning_units");
    for (const auto& [key, hwloc_value] : hwloc_distances) {
        const auto found = linux_distances.find(key);
        if (found != linux_distances.end() && hwloc_value.first != found->second.first) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.distance.value_conflict",
                           "Directed NUMA distance values disagree", true, "numa_distance", key,
                           "value", hwloc_value.first, found->second.first,
                           hwloc_value.second->provenance, found->second.second->provenance);
        }
    }

    const auto hwloc_pci = unique_map(
        hwloc->pci_functions, [](const auto& value) { return value.bdf; }, result.diagnostics,
        "topology.reconciliation.pci.ambiguous_match", "pci_function", hwloc->source);
    const auto linux_pci = unique_map(
        linux->pci_functions, [](const auto& value) { return value.bdf; }, result.diagnostics,
        "topology.reconciliation.pci.ambiguous_match", "pci_function", linux->source);
    for (const auto& [key, hwloc_device] : hwloc_pci) {
        const auto found = linux_pci.find(key);
        if (found == linux_pci.end()) {
            continue;
        }
        const auto* linux_device = found->second;
        // hwloc reports only the 16-bit base-class/subclass value. The adapter stores it in
        // the upper 16 bits of class_code; its low byte is unavailable, not a reported zero.
        const auto hwloc_base_and_subclass = hwloc_device->class_code >> 8U;
        const auto linux_base_and_subclass = linux_device->class_code >> 8U;
        if (hwloc_device->vendor_id != linux_device->vendor_id ||
            hwloc_device->device_id != linux_device->device_id ||
            hwloc_base_and_subclass != linux_base_and_subclass) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.pci.identity_conflict",
                           "PCI numeric identity disagrees", true, "pci_function", key,
                           "numeric_identity",
                           diagnostic_array(hwloc_device->vendor_id, hwloc_device->device_id,
                                            hwloc_device->class_code),
                           diagnostic_array(linux_device->vendor_id, linux_device->device_id,
                                            linux_device->class_code),
                           hwloc_device->provenance, linux_device->provenance);
        }
        static_cast<void>(merge_attribute(hwloc_device->subsystem_vendor_id,
                                          linux_device->subsystem_vendor_id, result.diagnostics,
                                          "topology.reconciliation.pci.subsystem_vendor_conflict",
                                          "pci_function", key, "subsystem_vendor_id", false));
        static_cast<void>(merge_attribute(hwloc_device->subsystem_device_id,
                                          linux_device->subsystem_device_id, result.diagnostics,
                                          "topology.reconciliation.pci.subsystem_device_conflict",
                                          "pci_function", key, "subsystem_device_id", false));
        static_cast<void>(merge_attribute(hwloc_device->linux_numa_node,
                                          linux_device->linux_numa_node, result.diagnostics,
                                          "topology.reconciliation.pci.numa_conflict",
                                          "pci_function", key, "linux_numa_node", true));
        static_cast<void>(merge_attribute(hwloc_device->local_cpus, linux_device->local_cpus,
                                          result.diagnostics,
                                          "topology.reconciliation.pci.local_cpus_conflict",
                                          "pci_function", key, "local_cpus", true));
    }

    const auto check_online_set = [&](const auto& observation, const auto& pus, std::string code,
                                      std::string object_type) {
        if (!observation.value) {
            add_diagnostic(result.diagnostics,
                           "topology.reconciliation.resource_set.missing_projection",
                           "Required Linux projection evidence is unavailable", true, object_type,
                           "machine", "online", "not_applicable", observation_summary(observation),
                           {}, observation.provenance);
            return;
        }
        if constexpr (std::same_as<std::remove_cvref_t<decltype(*observation.value)>,
                                   core::CpuSet>) {
            core::CpuSet represented;
            for (const auto& [key, value] : pus) {
                static_cast<void>(value);
                represented = represented.set_union(
                    core::CpuSet{core::LinuxCpuId{static_cast<std::uint32_t>(std::stoul(key))}});
            }
            if (represented != *observation.value) {
                add_diagnostic(result.diagnostics, std::move(code),
                               "Online resource set disagrees with represented inventory", true,
                               object_type, "machine", "online", represented.to_range_list(),
                               observation.value->to_range_list(), {}, observation.provenance);
            }
        }
    };
    check_online_set(linux->machine.cpu_sets.online, linux_pus,
                     "topology.reconciliation.resource_set.online_cpu_conflict", "cpu_set");
    if (!linux->machine.cpu_sets.present.value || !linux->machine.node_sets.possible.value ||
        !linux->machine.node_sets.online.value) {
        add_diagnostic(result.diagnostics,
                       "topology.reconciliation.resource_set.missing_projection",
                       "Required Linux projection evidence is unavailable", true, "resource_set",
                       "machine", "present_or_online", "not_applicable", "unavailable");
    }
    if (linux->machine.cpu_sets.present.value && linux->machine.cpu_sets.online.value &&
        !linux->machine.cpu_sets.online.value->is_subset_of(
            *linux->machine.cpu_sets.present.value)) {
        add_diagnostic(result.diagnostics,
                       "topology.reconciliation.resource_set.physical_cpu_conflict",
                       "Linux online CPUs are outside the present set", true, "cpu_set", "machine",
                       "present_online", "not_applicable",
                       linux->machine.cpu_sets.online.value->to_range_list(), {},
                       linux->machine.cpu_sets.online.provenance);
    }
    if (linux->machine.node_sets.possible.value && linux->machine.node_sets.online.value &&
        !linux->machine.node_sets.online.value->is_subset_of(
            *linux->machine.node_sets.possible.value)) {
        add_diagnostic(result.diagnostics,
                       "topology.reconciliation.resource_set.physical_node_conflict",
                       "Linux online nodes are outside the possible set", true, "node_set",
                       "machine", "possible_online", "not_applicable",
                       linux->machine.node_sets.online.value->to_range_list(), {},
                       linux->machine.node_sets.online.provenance);
    }
    if (linux->machine.node_sets.online.value) {
        core::NodeSet represented_nodes;
        for (const auto& [key, value] : linux_nodes) {
            static_cast<void>(value);
            represented_nodes = represented_nodes.set_union(
                core::NodeSet{core::NumaNodeId{static_cast<std::uint32_t>(std::stoul(key))}});
        }
        if (represented_nodes != *linux->machine.node_sets.online.value) {
            add_diagnostic(result.diagnostics,
                           "topology.reconciliation.resource_set.online_node_conflict",
                           "Online node set disagrees with represented NUMA inventory", true,
                           "node_set", "machine", "online", "not_applicable",
                           linux->machine.node_sets.online.value->to_range_list(), {},
                           linux->machine.node_sets.online.provenance);
        }
    }
    const auto check_allowed = [&](const auto& online, const auto& allowed, std::string code,
                                   std::string object_type) {
        if (online.value && allowed.value && !allowed.value->is_subset_of(*online.value)) {
            add_diagnostic(result.diagnostics, std::move(code),
                           "Process-allowed resources are outside online resources", true,
                           std::move(object_type), "machine", "process_allowed", "not_applicable",
                           allowed.value->to_range_list(), {}, allowed.provenance);
        }
    };
    check_allowed(linux->machine.cpu_sets.online, linux->machine.cpu_sets.process_allowed,
                  "topology.reconciliation.resource_set.allowed_cpu_conflict", "cpu_set");
    check_allowed(linux->machine.node_sets.online, linux->machine.node_sets.process_allowed,
                  "topology.reconciliation.resource_set.allowed_node_conflict", "node_set");
    const auto check_effective = [&](const auto& online, const auto& allowed, const auto& cgroup,
                                     const auto& effective, std::string code,
                                     std::string object_type) {
        if (effective.value && online.value && allowed.value && cgroup.value) {
            const auto expected =
                online.value->intersection(*allowed.value).intersection(*cgroup.value);
            if (expected != *effective.value) {
                add_diagnostic(result.diagnostics, std::move(code),
                               "Derived effective resource set is contradictory", true,
                               std::move(object_type), "machine", "final_effective",
                               "not_applicable", effective.value->to_range_list(), {},
                               effective.provenance);
            }
        }
    };
    check_effective(linux->machine.cpu_sets.online, linux->machine.cpu_sets.process_allowed,
                    linux->machine.cpu_sets.cgroup_effective,
                    linux->machine.cpu_sets.final_effective,
                    "topology.reconciliation.resource_set.effective_cpu_conflict", "cpu_set");
    check_effective(linux->machine.node_sets.online, linux->machine.node_sets.process_allowed,
                    linux->machine.node_sets.cgroup_effective,
                    linux->machine.node_sets.final_effective,
                    "topology.reconciliation.resource_set.effective_node_conflict", "node_set");

    core::CpuSet represented_cpus;
    for (const auto& [key, value] : hwloc_pus) {
        static_cast<void>(value);
        represented_cpus = represented_cpus.set_union(
            core::CpuSet{core::LinuxCpuId{static_cast<std::uint32_t>(std::stoul(key))}});
    }
    const auto check_memberships = [&](const auto& objects, auto membership, std::string code,
                                       std::string object_type) {
        for (const auto& object : objects) {
            const auto& members = membership(object);
            if (!members.is_subset_of(represented_cpus)) {
                add_diagnostic(result.diagnostics, code,
                               "Structural membership references an unrepresented CPU", true,
                               object_type, cpu_key(members), "member_cpus",
                               members.to_range_list(), members.to_range_list());
            }
        }
    };
    check_memberships(
        hwloc->packages.empty() ? linux->packages : hwloc->packages,
        [](const auto& value) -> const auto& { return value.key.member_cpus; },
        "topology.reconciliation.package.ownership_conflict", "package");
    check_memberships(
        hwloc->dies.empty() ? linux->dies : hwloc->dies,
        [](const auto& value) -> const auto& { return value.key.member_cpus; },
        "topology.reconciliation.die.ownership_conflict", "die");
    check_memberships(
        hwloc->cores.empty() ? linux->cores : hwloc->cores,
        [](const auto& value) -> const auto& { return value.key.member_cpus; },
        "topology.reconciliation.core.ownership_conflict", "core");
    check_memberships(
        hwloc->architecture_groups.empty() ? linux->architecture_groups
                                           : hwloc->architecture_groups,
        [](const auto& value) -> const auto& { return value.key.member_cpus; },
        "topology.reconciliation.group.ambiguous_match", "architecture_group");
    check_memberships(
        hwloc->caches.empty() ? linux->caches : hwloc->caches,
        [](const auto& value) -> const auto& { return value.shared_cpus; },
        "topology.reconciliation.cache.sharing_conflict", "cache");
    check_memberships(
        hwloc->numa_nodes.empty() ? linux->numa_nodes : hwloc->numa_nodes,
        [](const auto& value) -> const auto& { return value.member_cpus; },
        "topology.reconciliation.numa.membership_conflict", "numa_node");

    if (has_material(result.diagnostics)) {
        sort_diagnostics(result.diagnostics);
        return finish(std::move(result));
    }

    std::map<std::uint32_t, core::ProcessingUnitId> pu_ids;
    std::uint32_t next_id = 0;
    for (const auto& [key, value] : hwloc_pus) {
        static_cast<void>(value);
        pu_ids.emplace(static_cast<std::uint32_t>(std::stoul(key)),
                       core::ProcessingUnitId{next_id++});
    }

    const auto& package_source = hwloc->packages.empty() ? linux->packages : hwloc->packages;
    std::vector<const PackageSourceEvidence*> sorted_packages;
    for (const auto& package : package_source) {
        sorted_packages.push_back(&package);
    }
    std::ranges::sort(sorted_packages, {},
                      [](const auto* package) { return object_key(package->key); });
    std::map<std::string, core::PackageId> package_ids;
    std::vector<Package> packages;
    for (std::size_t index = 0; index < sorted_packages.size(); ++index) {
        const auto* package = sorted_packages[index];
        const auto id = core::PackageId{static_cast<std::uint32_t>(index)};
        const auto key = cpu_key(package->key.member_cpus);
        package_ids.emplace(key, id);
        const auto other = linux_packages.find(key);
        packages.push_back(
            {id, other == linux_packages.end()
                     ? source_attribute(package->key.os_id, package->provenance)
                     : merge_source_id(
                           package->key.os_id, package->provenance, other->second->key.os_id,
                           other->second->provenance, result.diagnostics,
                           "topology.reconciliation.package.os_id_conflict", "package", key)});
    }

    const auto& die_source = hwloc->dies.empty() ? linux->dies : hwloc->dies;
    std::vector<const DieSourceEvidence*> sorted_dies;
    for (const auto& die : die_source) {
        sorted_dies.push_back(&die);
    }
    std::ranges::sort(sorted_dies, {}, [](const auto* die) { return object_key(die->key); });
    std::map<std::string, core::DieId> die_ids;
    std::vector<Die> dies;
    for (std::size_t index = 0; index < sorted_dies.size(); ++index) {
        const auto* die = sorted_dies[index];
        const auto id = core::DieId{static_cast<std::uint32_t>(index)};
        const auto key = cpu_key(die->key.member_cpus);
        die_ids.emplace(key, id);
        const auto other = linux_dies.find(key);
        dies.push_back(
            {id, package_ids.at(cpu_key(die->package.member_cpus)),
             other == linux_dies.end()
                 ? source_attribute(die->key.os_id, die->provenance)
                 : merge_source_id(die->key.os_id, die->provenance, other->second->key.os_id,
                                   other->second->provenance, result.diagnostics,
                                   "topology.reconciliation.die.os_id_conflict", "die", key),
             std::nullopt});
    }

    const auto& core_source = hwloc->cores.empty() ? linux->cores : hwloc->cores;
    std::vector<const CoreSourceEvidence*> sorted_cores;
    for (const auto& core : core_source) {
        sorted_cores.push_back(&core);
    }
    std::ranges::sort(sorted_cores, {}, [](const auto* core) { return object_key(core->key); });
    std::map<std::string, core::CoreId> core_ids;
    std::vector<Core> cores;
    for (std::size_t index = 0; index < sorted_cores.size(); ++index) {
        const auto* core = sorted_cores[index];
        const auto id = core::CoreId{static_cast<std::uint32_t>(index)};
        const auto key = cpu_key(core->key.member_cpus);
        core_ids.emplace(key, id);
        const auto other = linux_cores.find(key);
        cores.push_back(
            {id, package_ids.at(cpu_key(core->package->member_cpus)),
             core->die ? std::optional<core::DieId>{die_ids.at(cpu_key(core->die->member_cpus))}
                       : std::nullopt,
             other == linux_cores.end()
                 ? source_attribute(core->key.os_id, core->provenance)
                 : merge_source_id(core->key.os_id, core->provenance, other->second->key.os_id,
                                   other->second->provenance, result.diagnostics,
                                   "topology.reconciliation.core.os_id_conflict", "core", key)});
    }

    std::vector<ProcessingUnit> processing_units;
    for (const auto& [key, source_pu] : hwloc_pus) {
        const auto cpu = static_cast<std::uint32_t>(std::stoul(key));
        const auto isolated =
            linux->machine.cpu_sets.isolated.value
                ? Attribute<bool>::available(
                      linux->machine.cpu_sets.isolated.value->contains(core::LinuxCpuId{cpu}),
                      model_evidence(linux->machine.cpu_sets.isolated.provenance))
                : Attribute<bool>::unavailable(
                      model_evidence(linux->machine.cpu_sets.isolated.provenance));
        processing_units.push_back({pu_ids.at(cpu), core::LinuxCpuId{cpu},
                                    core_ids.at(cpu_key(source_pu->core.member_cpus)), isolated});
    }

    const auto& group_source = hwloc->architecture_groups.empty() ? linux->architecture_groups
                                                                  : hwloc->architecture_groups;
    std::vector<const ArchitectureGroupSourceEvidence*> sorted_groups;
    for (const auto& group : group_source) {
        sorted_groups.push_back(&group);
    }
    std::ranges::sort(sorted_groups, {}, [](const auto* group) { return group_key(*group); });
    std::map<std::string, core::ArchitectureGroupId> group_ids;
    for (std::size_t index = 0; index < sorted_groups.size(); ++index) {
        group_ids.emplace(group_key(*sorted_groups[index]),
                          core::ArchitectureGroupId{static_cast<std::uint32_t>(index)});
    }
    std::vector<ArchitectureGroup> groups;
    for (const auto* group : sorted_groups) {
        const auto other = linux_groups.find(group_key(*group));
        const auto members = to_pus(group->key.member_cpus, pu_ids);
        const auto package = std::ranges::find_if(sorted_packages, [&](const auto* candidate) {
            return group->key.member_cpus.is_subset_of(candidate->key.member_cpus);
        });
        if (!members || package == sorted_packages.end()) {
            return std::unexpected{
                internal_error("topology.reconciliation.projection_internal_failure",
                               "Matched architecture group cannot be projected")};
        }
        std::optional<core::ArchitectureGroupId> parent;
        const auto selected_parent = group->parent                 ? group->parent
                                     : other != linux_groups.end() ? other->second->parent
                                                                   : std::nullopt;
        if (selected_parent) {
            const auto found = std::ranges::find_if(sorted_groups, [&](const auto* candidate) {
                return candidate->key.member_cpus == selected_parent->member_cpus;
            });
            if (found != sorted_groups.end()) {
                parent = group_ids.at(group_key(**found));
            }
        }
        auto group_provenance = group->provenance;
        if (other != linux_groups.end()) {
            group_provenance.insert(group_provenance.end(), other->second->provenance.begin(),
                                    other->second->provenance.end());
        }
        const auto subtype = group->subtype.value          ? group->subtype.value
                             : other != linux_groups.end() ? other->second->subtype.value
                                                           : std::nullopt;
        groups.push_back({group_ids.at(group_key(*group)),
                          package_ids.at(cpu_key((*package)->key.member_cpus)), parent, group->kind,
                          subtype, *members, model_evidence(std::move(group_provenance))});
    }

    std::vector<CacheDomain> caches;
    const auto& cache_source = hwloc->caches.empty() ? linux->caches : hwloc->caches;
    std::vector<const CacheSourceEvidence*> sorted_caches;
    for (const auto& cache : cache_source) {
        sorted_caches.push_back(&cache);
    }
    std::ranges::sort(sorted_caches, {}, [](const auto* cache) { return cache_key(*cache); });
    for (std::size_t index = 0; index < sorted_caches.size(); ++index) {
        const auto* cache = sorted_caches[index];
        const auto other = linux_caches.find(cache_key(*cache));
        const auto associativity =
            other == linux_caches.end()
                ? single_attribute(cache->associativity)
                : merge_attribute(cache->associativity, other->second->associativity,
                                  result.diagnostics,
                                  "topology.reconciliation.cache.attribute_conflict", "cache",
                                  cache_key(*cache), "associativity", false);
        auto cache_provenance = cache->provenance;
        if (other != linux_caches.end()) {
            cache_provenance.insert(cache_provenance.end(), other->second->provenance.begin(),
                                    other->second->provenance.end());
        }
        caches.push_back({core::CacheId{static_cast<std::uint32_t>(index)}, cache->level,
                          cache_kind(cache->kind), cache->size, cache->line_size, associativity,
                          *to_pus(cache->shared_cpus, pu_ids),
                          model_evidence(std::move(cache_provenance))});
    }

    std::vector<NumaNode> numa_nodes;
    const auto& node_source = hwloc->numa_nodes.empty() ? linux->numa_nodes : hwloc->numa_nodes;
    std::vector<const NumaNodeSourceEvidence*> sorted_nodes;
    for (const auto& node : node_source) {
        sorted_nodes.push_back(&node);
    }
    std::ranges::sort(sorted_nodes, {},
                      [&](const auto* node) { return matched_node_key(*node, linux->numa_nodes); });
    std::map<std::uint32_t, core::NumaNodeId> node_ids;
    for (std::size_t index = 0; index < sorted_nodes.size(); ++index) {
        const auto* node = sorted_nodes[index];
        const auto id = core::NumaNodeId{static_cast<std::uint32_t>(index)};
        const auto semantic_key = matched_node_key(*node, linux->numa_nodes);
        const auto other = linux_nodes.find(semantic_key);
        const auto linux_id =
            other == linux_nodes.end()
                ? single_attribute(node->linux_node_id)
                : merge_attribute(node->linux_node_id, other->second->linux_node_id,
                                  result.diagnostics,
                                  "topology.reconciliation.numa.membership_conflict", "numa_node",
                                  semantic_key, "linux_node_id", true);
        if (linux_id.value()) {
            node_ids.emplace(*linux_id.value(), id);
        }
        const auto memory = other == linux_nodes.end()
                                ? single_attribute(node->local_memory)
                                : merge_attribute(node->local_memory, other->second->local_memory,
                                                  result.diagnostics,
                                                  "topology.reconciliation.numa.memory_conflict",
                                                  "numa_node", semantic_key, "local_memory", false);
        const auto page_sizes = !node->page_sizes.empty()    ? node->page_sizes
                                : other != linux_nodes.end() ? other->second->page_sizes
                                                             : std::vector<core::ByteCount>{};
        numa_nodes.push_back({id, linux_id, memory, Attribute<core::ByteCount>::unavailable(),
                              *to_pus(node->member_cpus, pu_ids), page_sizes});
    }
    const auto machine_page_sizes = detail::derive_machine_page_sizes(numa_nodes);
    if (!machine_page_sizes) {
        add_diagnostic(result.diagnostics, "topology.reconciliation.numa.invalid_page_size",
                       "NUMA page-size evidence contains an invalid capability", true,
                       "machine", "machine", "page_sizes", machine_page_sizes.error().code.value(),
                       machine_page_sizes.error().code.value());
        sort_diagnostics(result.diagnostics);
        return finish(std::move(result));
    }

    const auto& projected_distances = hwloc_distances.empty() ? linux_distances : hwloc_distances;
    NumaDistanceMatrix distances;
    for (const auto& node : numa_nodes) {
        distances.nodes.push_back(node.id);
    }
    for (const auto& [key, value] : projected_distances) {
        static_cast<void>(key);
        const auto delimiter = key.find("->");
        const auto suffix = key.find(':');
        const auto from = static_cast<std::uint32_t>(std::stoul(key.substr(0, delimiter)));
        const auto to = static_cast<std::uint32_t>(
            std::stoul(key.substr(delimiter + 2, suffix - delimiter - 2)));
        if (value.first > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result.diagnostics, "topology.reconciliation.distance.value_conflict",
                           "Distance value cannot be represented by the owned model", true,
                           "numa_distance", key, "value", value.first, value.first,
                           value.second->provenance, value.second->provenance);
            continue;
        }
        distances.directed_distances.push_back(
            {node_ids.at(from), node_ids.at(to), static_cast<std::uint32_t>(value.first)});
    }
    std::ranges::sort(distances.directed_distances);

    std::map<std::string, const PciFunctionSourceEvidence*> pci_union = linux_pci;
    for (const auto& [key, value] : hwloc_pci) {
        pci_union.try_emplace(key, value);
    }
    std::vector<PciDevice> pci_devices;
    for (const auto& [key, source_device] : pci_union) {
        const auto address = PciAddress::parse(key);
        if (!address) {
            return std::unexpected{
                internal_error("topology.reconciliation.projection_internal_failure",
                               "Validated source PCI address cannot be projected")};
        }
        auto locality_provenance = source_device->linux_numa_node.provenance;
        auto cpu_provenance = source_device->local_cpus.provenance;
        if (const auto shared = hwloc_pci.find(key);
            shared != hwloc_pci.end() && linux_pci.contains(key)) {
            locality_provenance.insert(locality_provenance.end(),
                                       shared->second->linux_numa_node.provenance.begin(),
                                       shared->second->linux_numa_node.provenance.end());
            cpu_provenance.insert(cpu_provenance.end(),
                                  shared->second->local_cpus.provenance.begin(),
                                  shared->second->local_cpus.provenance.end());
        }
        auto numa = source_device->linux_numa_node.value
                        ? Attribute<core::NumaNodeId>::available(
                              node_ids.at(*source_device->linux_numa_node.value),
                              model_evidence(std::move(locality_provenance)))
                        : Attribute<core::NumaNodeId>::unavailable(
                              model_evidence(std::move(locality_provenance)));
        auto local = source_device->local_cpus.value
                         ? Attribute<ProcessingUnitSet>::available(
                               *to_pus(*source_device->local_cpus.value, pu_ids),
                               model_evidence(std::move(cpu_provenance)))
                         : Attribute<ProcessingUnitSet>::unavailable(
                               model_evidence(std::move(cpu_provenance)));
        pci_devices.push_back({*address, source_device->vendor_id, source_device->device_id,
                               source_device->class_code, std::move(numa), std::move(local)});
    }

    auto architecture = hwloc->machine.architecture.value
                            ? *hwloc->machine.architecture.value
                            : linux->machine.architecture.value.value_or(std::string{});
    if (hwloc->machine.architecture.value && linux->machine.architecture.value &&
        hwloc->machine.architecture.value != linux->machine.architecture.value) {
        add_diagnostic(result.diagnostics, "topology.reconciliation.machine.description_conflict",
                       "Machine architecture descriptions disagree", false, "machine", "machine",
                       "architecture", *hwloc->machine.architecture.value,
                       *linux->machine.architecture.value, hwloc->machine.architecture.provenance,
                       linux->machine.architecture.provenance);
    }
    const auto machine = Machine{
        .id = core::TopologyMachineId{0},
        .architecture = std::move(architecture),
        .cpu_vendor = merge_attribute(hwloc->machine.cpu_vendor, linux->machine.cpu_vendor,
                                      result.diagnostics,
                                      "topology.reconciliation.machine.description_conflict",
                                      "machine", "machine", "cpu_vendor", false),
        .cpu_model =
            merge_attribute(hwloc->machine.cpu_model, linux->machine.cpu_model, result.diagnostics,
                            "topology.reconciliation.machine.description_conflict", "machine",
                            "machine", "cpu_model", false),
        .total_memory = merge_attribute(hwloc->machine.total_memory, linux->machine.total_memory,
                                        result.diagnostics,
                                        "topology.reconciliation.machine.description_conflict",
                                        "machine", "machine", "total_memory", false),
        .page_sizes = *machine_page_sizes,
        .present_cpus = *linux->machine.cpu_sets.present.value,
        .online_cpus = *linux->machine.cpu_sets.online.value,
        .allowed_cpus = single_attribute(linux->machine.cpu_sets.process_allowed),
        .effective_cpus = single_attribute(linux->machine.cpu_sets.final_effective),
        .present_nodes = *linux->machine.node_sets.possible.value,
        .online_nodes = *linux->machine.node_sets.online.value,
        .allowed_nodes = single_attribute(linux->machine.node_sets.process_allowed),
        .effective_nodes = single_attribute(linux->machine.node_sets.final_effective)};

    if (has_material(result.diagnostics)) {
        sort_diagnostics(result.diagnostics);
        return finish(std::move(result));
    }
    auto snapshot = TopologySnapshot::create({.machine = machine,
                                              .packages = std::move(packages),
                                              .dies = std::move(dies),
                                              .architecture_groups = std::move(groups),
                                              .cores = std::move(cores),
                                              .processing_units = std::move(processing_units),
                                              .caches = std::move(caches),
                                              .numa_nodes = std::move(numa_nodes),
                                              .numa_distances = std::move(distances),
                                              .pci_devices = std::move(pci_devices)});
    if (!snapshot) {
        add_diagnostic(result.diagnostics, "topology.reconciliation.model_construction_failed",
                       "Owned topology model construction failed", true, "snapshot", "snapshot",
                       "construction", construction_code(snapshot.error().code),
                       construction_code(snapshot.error().code));
        sort_diagnostics(result.diagnostics);
        return finish(std::move(result));
    }
    auto validation = detail::validate_topology_with_failure_sink(*snapshot, raw_failure);
    ReportCleanup validation_cleanup{validation};
    if (!validation.valid()) {
        const core::Error* first_validation = &validation.diagnostics.front();
        if (raw_failure && !*raw_failure) {
            raw_failure->emplace(std::move(validation.diagnostics.front()));
            first_validation = &**raw_failure;
        }
        for (std::size_t index = 0; index < validation.diagnostics.size(); ++index) {
            const auto& diagnostic = index == 0 ? *first_validation : validation.diagnostics[index];
            add_diagnostic(result.diagnostics, "topology.reconciliation.invariant_failed",
                           "Reconciled topology failed invariant validation", true, "snapshot",
                           diagnostic.context.value("object_id", std::string{"snapshot"}),
                           diagnostic.context.value("field_path", std::string{"invariant"}),
                           diagnostic.code.value(), diagnostic.code.value());
        }
        sort_diagnostics(result.diagnostics);
        return finish(std::move(result));
    }
    result.snapshot = std::move(*snapshot);
    sort_diagnostics(result.diagnostics);
    return finish(std::move(result));
    } catch (...) {
        result.diagnostics.retain();
        throw;
    }
}

auto reconcile_topology(const TopologySourceEvidence& first, const TopologySourceEvidence& second)
    -> std::expected<ReconciliationResult, core::Error> {
    return detail::reconcile_topology_with_failure_sink(first, second, nullptr);
}

} // namespace epycbench::topology
