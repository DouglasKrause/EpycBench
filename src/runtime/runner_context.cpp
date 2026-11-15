#include "epycbench/runtime/runner_context.hpp"

#include "epycbench/topology/checked_topology.hpp"
#include "epycbench/topology/invariants.hpp"
#include "runtime/runner_context_access.hpp"
#include "topology/checked_topology_access.hpp"
#include "core/json_ownership.hpp"
#include "topology/prepared_stable_id_access.hpp"
#include "topology/source_diagnostic_access.hpp"

#include <utility>

namespace epycbench::runtime {
namespace {

struct ReportCleanup {
    topology::ValidationReport& value;
    ~ReportCleanup() noexcept {
        for (auto& diagnostic : value.diagnostics)
            core::detail::retire_json(diagnostic.context);
    }
};
auto retain_raw_failure(std::optional<core::Error>* sink, core::Error& actual) noexcept
    -> const core::Error* {
    if (sink && !*sink) { sink->emplace(std::move(actual)); return &**sink; }
    return &actual;
}

auto error(std::string code, std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        object["failure_classification"] = std::move(classification);
    });
    core::detail::JsonCleanup cleanup{context};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Runner topology context construction failed",
            .operation = "runner_context",
            .os_error = underlying == nullptr ? std::nullopt : underlying->os_error,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto validate(void*, const topology::TopologySnapshot& value) -> topology::ValidationReport {
    return topology::validate_topology(value);
}
auto derive_ids(void*, const topology::TopologySnapshot& value) noexcept
    -> std::expected<topology::StableTopologyIds, core::Error> {
    try {
        return topology::derive_stable_topology_ids(value);
    } catch (...) {
        return std::unexpected{
            error("runtime.runner_context.contained_exception", "stable_identity_exception")};
    }
}
auto derive_fingerprints(void*, const topology::TopologySnapshot& value,
                         const topology::StableTopologyIds& ids) noexcept
    -> std::expected<topology::TopologyFingerprints, core::Error> {
    try {
        return topology::derive_topology_fingerprints(value, ids);
    } catch (...) {
        return std::unexpected{
            error("runtime.runner_context.contained_exception", "fingerprint_exception")};
    }
}
auto acquire_topology(void*) noexcept -> std::expected<topology::TopologySnapshot, core::Error> {
    return topology::discover_checked_topology();
}

auto production_acquire(void*) noexcept -> std::expected<RunnerContext, core::Error> {
    return detail::acquire_runner_context_with(
        {nullptr, acquire_topology, {nullptr, validate, derive_ids, derive_fingerprints}});
}
struct PreparedContextInvocation {
    std::optional<core::Error>* raw_failure;
    platform_linux::detail::PreparedTopologySourceIO* prepared;
    std::uint64_t absolute_end;
};
auto fallible_acquire_topology(void* opaque)
    -> std::expected<topology::TopologySnapshot, core::Error> {
    auto& invocation = *static_cast<PreparedContextInvocation*>(opaque);
    return topology::detail::discover_checked_topology_fallible(
        invocation.raw_failure, invocation.prepared, invocation.absolute_end);
}

auto source_validate(void* opaque, const topology::TopologySnapshot& value) -> topology::ValidationReport {
    auto& source = *static_cast<PreparedContextInvocation*>(opaque);
    return topology::detail::validate_topology_with_failure_sink(value, source.raw_failure);
}
auto source_derive_ids(void* opaque, const topology::TopologySnapshot& value)
    -> std::expected<topology::StableTopologyIds, core::Error> {
    auto& source = *static_cast<PreparedContextInvocation*>(opaque);
    return topology::detail::PreparedStableTopologyIdAccess::derive_with_failure_sink(value, source.raw_failure);
}
auto source_derive_fingerprints(void* opaque, const topology::TopologySnapshot& value,
    const topology::StableTopologyIds& ids) -> std::expected<topology::TopologyFingerprints, core::Error> {
    auto& source = *static_cast<PreparedContextInvocation*>(opaque);
    return topology::detail::derive_topology_fingerprints_with_failure_sink(value, ids, source.raw_failure);
}

} // namespace

namespace detail {

auto RunnerContextFactory::create(topology::TopologySnapshot topology,
                                  RunnerContextOperations operations) noexcept
    -> std::expected<RunnerContext, core::Error> {
    try {
        return create_fallible(std::move(topology), operations);
    } catch (...) {
        return std::unexpected{
            error("runtime.runner_context.contained_exception", "contained_exception")};
    }
}

auto RunnerContextFactory::create_fallible(topology::TopologySnapshot topology,
    RunnerContextOperations operations, std::optional<core::Error>* raw_failure)
    -> std::expected<RunnerContext, core::Error> {
    if (operations.validate == nullptr || operations.derive_ids == nullptr ||
        operations.derive_fingerprints == nullptr)
        return std::unexpected{
            error("runtime.runner_context.invalid_context", "invalid_operations")};
    auto report = operations.validate(operations.context, topology);
    ReportCleanup report_cleanup{report};
    if (!report.valid()) {
        static_cast<void>(retain_raw_failure(raw_failure, report.diagnostics.front()));
        return std::unexpected{
            error("runtime.runner_context.invalid_context", "topology_validation_failed")};
    }
    auto ids = operations.derive_ids(operations.context, topology);
    core::detail::ExpectedErrorCleanup ids_cleanup{ids};
    if (!ids) {
        const auto* underlying = retain_raw_failure(raw_failure, ids.error());
        return std::unexpected{error("runtime.runner_context.invalid_context",
                                     "stable_identity_derivation_failed", underlying)};
    }
    auto fingerprints = operations.derive_fingerprints(operations.context, topology, *ids);
    core::detail::ExpectedErrorCleanup fingerprints_cleanup{fingerprints};
    if (!fingerprints) {
        const auto* underlying = retain_raw_failure(raw_failure, fingerprints.error());
        return std::unexpected{error("runtime.runner_context.invalid_context",
                                     "fingerprint_derivation_failed", underlying)};
    }
    return RunnerContext{std::move(topology), std::move(*ids), std::move(*fingerprints)};
}

namespace {
auto acquire_context_impl(RunnerContextAcquireOperations operations,
                          std::optional<core::Error>* raw_failure, bool legacy)
    -> std::expected<RunnerContext, core::Error> {
    if (operations.acquire_topology == nullptr)
        return std::unexpected{error("runtime.runner_context.acquire_failed", "invalid_provider")};
    auto topology = operations.acquire_topology(operations.context);
    core::detail::ExpectedErrorCleanup cleanup{topology};
    if (!topology) {
        const auto* underlying = retain_raw_failure(raw_failure, topology.error());
        return std::unexpected{error("runtime.runner_context.acquire_failed",
                                     "topology_acquisition_failed", underlying)};
    }
    // Ordinary providers retain their original construction containment.
    if (legacy)
        return RunnerContextFactory::create(std::move(*topology), operations.context_operations);
    return RunnerContextFactory::create_fallible(
        std::move(*topology), operations.context_operations, raw_failure);
}
} // namespace

auto acquire_runner_context_with(RunnerContextAcquireOperations operations) noexcept
    -> std::expected<RunnerContext, core::Error> {
    try {
        return acquire_context_impl(operations, nullptr, true);
    } catch (...) {
        return std::unexpected{
            error("runtime.runner_context.contained_exception", "contained_exception")};
    }
}

auto acquire_runner_context_with_fallible(RunnerContextAcquireOperations operations,
                                         std::optional<core::Error>* raw_failure)
    -> std::expected<RunnerContext, core::Error> {
    return acquire_context_impl(operations, raw_failure, false);
}

auto acquire_runner_context_fallible(std::optional<core::Error>* raw_failure,
    platform_linux::detail::PreparedTopologySourceIO* prepared, std::uint64_t absolute_end)
    -> std::expected<RunnerContext, core::Error> {
    PreparedContextInvocation invocation{raw_failure, prepared, absolute_end};
    return acquire_runner_context_with_fallible(
        {&invocation, fallible_acquire_topology,
         {&invocation, source_validate, source_derive_ids, source_derive_fingerprints}}, raw_failure);
}

} // namespace detail

auto RunnerContext::create(topology::TopologySnapshot topology) noexcept
    -> std::expected<RunnerContext, core::Error> {
    return detail::RunnerContextFactory::create(
        std::move(topology), {nullptr, validate, derive_ids, derive_fingerprints});
}

auto production_runner_context_provider() noexcept -> RunnerContextProvider {
    return {nullptr, production_acquire};
}

} // namespace epycbench::runtime
