#include "epycbench/topology/checked_topology.hpp"

#include "epycbench/topology/hwloc_source.hpp"
#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/linux_source.hpp"
#include "epycbench/topology/reconciliation.hpp"
#include "topology/checked_topology_access.hpp"
#include "topology/linux_source_access.hpp"
#include "core/json_ownership.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <algorithm>
#include <utility>

namespace epycbench::topology {
namespace {

struct ReportCleanup {
    std::vector<core::Error>* diagnostics;
    ~ReportCleanup() noexcept {
        if (diagnostics)
            for (auto& diagnostic : *diagnostics)
                core::detail::retire_json(diagnostic.context);
    }
};
struct ReconciliationCleanup {
    ReconciliationResult* value;
    ~ReconciliationCleanup() noexcept {
        if (value)
            for (auto& diagnostic : value->diagnostics)
                core::detail::retire_json(diagnostic.error.context);
    }
};
void retain_raw_failure(std::optional<core::Error>* sink, core::Error& actual) noexcept {
    if (sink && !*sink) sink->emplace(std::move(actual));
}

auto error(std::string code, std::string classification) -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        object["failure_classification"] = std::move(classification);
    });
    core::detail::JsonCleanup cleanup{context};
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Checked topology acquisition failed",
            .operation = "discover_checked_topology",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto production_hwloc(void*) noexcept -> std::expected<TopologySourceEvidence, core::Error> {
    try {
        return discover_hwloc_topology();
    } catch (...) {
        return std::unexpected{
            error("topology.checked_acquisition.contained_exception", "hwloc_discovery_exception")};
    }
}
auto production_linux(void*) noexcept -> std::expected<TopologySourceEvidence, core::Error> {
    try {
        return discover_linux_topology();
    } catch (...) {
        return std::unexpected{
            error("topology.checked_acquisition.contained_exception", "linux_discovery_exception")};
    }
}
auto production_reconcile(void*, const TopologySourceEvidence& hwloc,
                          const TopologySourceEvidence& linux) noexcept
    -> std::expected<ReconciliationResult, core::Error> {
    try {
        return reconcile_topology(hwloc, linux);
    } catch (...) {
        return std::unexpected{
            error("topology.checked_acquisition.contained_exception", "reconciliation_exception")};
    }
}
auto production_validate(void*, const TopologySnapshot& topology) -> ValidationReport {
    return validate_topology(topology);
}
struct PreparedLinuxInvocation {
    platform_linux::detail::PreparedTopologySourceIO& prepared;
    std::uint64_t absolute_end;
    std::optional<core::Error>* raw_failure;
    detail::CheckedTopologyOperations operations;
};
auto prepared_hwloc(void* opaque) -> std::expected<TopologySourceEvidence, core::Error> {
    const auto& operations = static_cast<PreparedLinuxInvocation*>(opaque)->operations;
    return operations.discover_hwloc(operations.context);
}
auto prepared_reconcile(void* opaque, const TopologySourceEvidence& hwloc, const TopologySourceEvidence& linux)
    -> std::expected<ReconciliationResult, core::Error> {
    const auto& operations = static_cast<PreparedLinuxInvocation*>(opaque)->operations;
    return operations.reconcile(operations.context, hwloc, linux);
}
auto prepared_validate(void* opaque, const TopologySnapshot& value) -> ValidationReport {
    const auto& operations = static_cast<PreparedLinuxInvocation*>(opaque)->operations;
    return operations.validate(operations.context, value);
}
auto fallible_linux(void* opaque) -> std::expected<TopologySourceEvidence, core::Error> {
    auto& invocation = *static_cast<PreparedLinuxInvocation*>(opaque);
    return detail::discover_linux_topology_with_prepared(invocation.prepared, invocation.absolute_end, {}, invocation.raw_failure);
}
auto source_reconcile(void* opaque, const TopologySourceEvidence& hwloc,
    const TopologySourceEvidence& linux) -> std::expected<ReconciliationResult, core::Error> {
    return detail::reconcile_topology_with_failure_sink(hwloc, linux,
        static_cast<std::optional<core::Error>*>(opaque));
}
auto source_validate(void* opaque, const TopologySnapshot& value) -> ValidationReport {
    return detail::validate_topology_with_failure_sink(value,
        static_cast<std::optional<core::Error>*>(opaque));
}

} // namespace

namespace detail {

auto discover_checked_topology_with_fallible(CheckedTopologyOperations operations,
                                             std::optional<core::Error>* raw_failure)
    -> std::expected<TopologySnapshot, CheckedTopologyFailure> {
    if (operations.discover_hwloc == nullptr || operations.discover_linux == nullptr ||
        operations.reconcile == nullptr || operations.validate == nullptr) {
        return std::unexpected{CheckedTopologyFailure{
            CheckedTopologyStage::validation,
            error("topology.checked_acquisition.invalid_operations", "invalid_operations")}};
    }
    auto hwloc = operations.discover_hwloc(operations.context);
    core::detail::ExpectedErrorCleanup hwloc_error_cleanup{hwloc};
    ReportCleanup hwloc_cleanup{hwloc ? &hwloc->diagnostics : nullptr};
    if (!hwloc)
        return std::unexpected{CheckedTopologyFailure{CheckedTopologyStage::hwloc_discovery,
                                                      std::move(hwloc.error())}};
    auto linux = operations.discover_linux(operations.context);
    core::detail::ExpectedErrorCleanup linux_error_cleanup{linux};
    ReportCleanup linux_cleanup{linux ? &linux->diagnostics : nullptr};
    if (!linux)
        return std::unexpected{CheckedTopologyFailure{CheckedTopologyStage::linux_discovery,
                                                      std::move(linux.error())}};
    auto reconciled = operations.reconcile(operations.context, *hwloc, *linux);
    core::detail::ExpectedErrorCleanup reconciliation_error_cleanup{reconciled};
    ReconciliationCleanup reconciliation_cleanup{reconciled ? &*reconciled : nullptr};
    if (!reconciled)
        return std::unexpected{CheckedTopologyFailure{CheckedTopologyStage::reconciliation,
                                                      std::move(reconciled.error())}};
    if (!reconciled->snapshot ||
        std::ranges::any_of(reconciled->diagnostics, &ReconciliationDiagnostic::material)) {
        const auto material = std::ranges::find_if(reconciled->diagnostics,
                                                   &ReconciliationDiagnostic::material);
        if (material != reconciled->diagnostics.end())
            retain_raw_failure(raw_failure, material->error);
        return std::unexpected{CheckedTopologyFailure{
            CheckedTopologyStage::reconciliation,
            error("topology.reconciliation.material_conflict", "material_conflict")}};
    }
    auto report = operations.validate(operations.context, *reconciled->snapshot);
    ReportCleanup report_cleanup{&report.diagnostics};
    if (!report.valid()) {
        retain_raw_failure(raw_failure, report.diagnostics.front());
        return std::unexpected{CheckedTopologyFailure{
            CheckedTopologyStage::validation,
            error("topology.fingerprint.invalid_topology", "topology_validation_failed")}};
    }
    return std::move(*reconciled->snapshot);
}

auto discover_checked_topology_with(CheckedTopologyOperations operations) noexcept
    -> std::expected<TopologySnapshot, CheckedTopologyFailure> {
    try {
        return discover_checked_topology_with_fallible(operations);
    } catch (...) {
        return std::unexpected{CheckedTopologyFailure{
            CheckedTopologyStage::validation,
            error("topology.checked_acquisition.contained_exception", "contained_exception")}};
    }
}

auto discover_checked_topology_with_prepared_linux_fallible(CheckedTopologyOperations operations,
    platform_linux::detail::PreparedTopologySourceIO* prepared, std::uint64_t end,
    std::optional<core::Error>* raw_failure) -> std::expected<TopologySnapshot, CheckedTopologyFailure> {
    if (!prepared || !end || !operations.discover_hwloc || !operations.reconcile || !operations.validate)
        return std::unexpected{CheckedTopologyFailure{CheckedTopologyStage::linux_discovery,
            error("topology.checked_acquisition.invalid_operations", "missing_prepared_linux_source")}};
    if (!prepared->invocation_ready())
        return std::unexpected{CheckedTopologyFailure{CheckedTopologyStage::linux_discovery,
            error("topology.checked_acquisition.invalid_operations", "unready_prepared_linux_source")}};
    PreparedLinuxInvocation invocation{*prepared, end, raw_failure, operations};
    return discover_checked_topology_with_fallible(
        {&invocation, prepared_hwloc, fallible_linux, prepared_reconcile, prepared_validate}, raw_failure);
}
auto discover_checked_topology_fallible(std::optional<core::Error>* raw_failure,
    platform_linux::detail::PreparedTopologySourceIO* prepared, std::uint64_t absolute_end)
    -> std::expected<TopologySnapshot, core::Error> {
    if (!prepared || !absolute_end)
        return std::unexpected{error("topology.checked_acquisition.invalid_operations", "missing_prepared_linux_source")};
    if (!prepared->invocation_ready())
        return std::unexpected{error("topology.checked_acquisition.invalid_operations", "unready_prepared_linux_source")};
    // Untimed configured dependency preparation under ADR-0017. The enclosing
    // original-main owner checks inherited placement and enforces containment;
    // independent Linux acquisition keeps its actual prepared IO owner/end.
    auto result = discover_checked_topology_with_prepared_linux_fallible(
        {raw_failure, production_hwloc, nullptr, source_reconcile, source_validate},
        prepared, absolute_end, raw_failure);
    core::detail::JsonPointerCleanup cleanup{!result ? &result.error().error.context : nullptr};
    if (!result) return std::unexpected{std::move(result.error().error)};
    return std::move(*result);
}

} // namespace detail

auto discover_checked_topology() noexcept -> std::expected<TopologySnapshot, core::Error> {
    auto result = detail::discover_checked_topology_with(
        {nullptr, production_hwloc, production_linux, production_reconcile, production_validate});
    if (!result)
        return std::unexpected{std::move(result.error().error)};
    return std::move(*result);
}

} // namespace epycbench::topology
