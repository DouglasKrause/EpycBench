#include "epycbench/runtime/production_admission.hpp"
#include "pairwise_v2_contextual_admission_classification.hpp"
#include "runtime/production_admission_policy.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string>

namespace epycbench::runtime {
namespace {
auto production_error(std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        nlohmann::json classified(std::move(classification));
        core::detail::append_object(object, "classification", classified);
        nlohmann::json component("runner_admission");
        core::detail::append_object(object, "component", component);
        if (underlying != nullptr) {
            nlohmann::json code(underlying->code.value());
            core::detail::append_object(object, "underlying_code", code);
        }
    });
    core::detail::JsonCleanup context_cleanup{context};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.runner_state.admission_failed"},
            .message = "Runner placement admission failed",
            .operation = "runner_admission",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto pair_error() -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.pairwise_admission.incompatible_requests"},
            .message = "Admitted placement requests are not pairwise compatible",
            .operation = "pairwise_admission",
            .os_error = std::nullopt,
            .context = {{"classification", "monitoring_policy_mismatch"}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

} // namespace

auto detail::project_production_monitoring_policy(const ipc::PlacementRequest& request,
                                                  std::optional<core::Error>* raw_failure)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error> {
    const auto& value = request.monitoring_inputs();
    if (!value)
        return std::unexpected{production_error("production_profile_requires_v3")};
    return detail::project_production_monitoring_policy(
        detail::ProductionMonitoringPolicyView{
            value->required(), value->optional(), value->before_observation(),
            value->after_observation(), value->during_observation(), value->during_interval(),
            value->unavailable_optional_source()},
        raw_failure);
}

auto detail::project_production_monitoring_policy(ProductionMonitoringPolicyView value,
                                                  std::optional<core::Error>* raw_failure)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error> {
    const auto disposition = [&](ipc::PlacementMonitoringChannel channel) {
        if (std::ranges::find(value.required, channel) != value.required.end())
            return monitoring::MonitoringChannelDisposition::required;
        if (std::ranges::find(value.optional, channel) != value.optional.end())
            return monitoring::MonitoringChannelDisposition::optional;
        return monitoring::MonitoringChannelDisposition::disabled;
    };
    monitoring::OptionalUnavailableSourcePolicy optional;
    switch (value.unavailable) {
    case ipc::PlacementOptionalUnavailableSourcePolicy::fail:
        optional = monitoring::OptionalUnavailableSourcePolicy::fail;
        break;
    case ipc::PlacementOptionalUnavailableSourcePolicy::warn:
        optional = monitoring::OptionalUnavailableSourcePolicy::warn;
        break;
    case ipc::PlacementOptionalUnavailableSourcePolicy::disable:
        optional = monitoring::OptionalUnavailableSourcePolicy::disable;
        break;
    default:
        return std::unexpected{production_error("monitoring_policy_invalid")};
    }
    return detail::wrap_production_monitoring_policy(
        monitoring::MonitoringExecutionPolicy::create(
            disposition(ipc::PlacementMonitoringChannel::page_faults),
            disposition(ipc::PlacementMonitoringChannel::context_switches),
            disposition(ipc::PlacementMonitoringChannel::temperature),
            disposition(ipc::PlacementMonitoringChannel::frequency), value.before, value.after,
            value.during, value.interval, optional),
        raw_failure);
}

auto detail::wrap_production_monitoring_policy(
    std::expected<monitoring::MonitoringExecutionPolicy, core::Error> policy,
    std::optional<core::Error>* raw_failure)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error> {
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy) {
        const core::Error* underlying = &policy.error();
        if (raw_failure) {
            if (!*raw_failure)
                raw_failure->emplace(std::move(policy.error()));
            underlying = &**raw_failure;
        }
        return std::unexpected{production_error("monitoring_policy_invalid", underlying)};
    }
    return policy;
}

auto detail::production_monitoring_needs_service(
    const monitoring::MonitoringExecutionPolicy& policy) noexcept -> bool {
    using monitoring::MonitoringChannel;
    using monitoring::MonitoringChannelDisposition;
    return policy.disposition(MonitoringChannel::context_switches) !=
               MonitoringChannelDisposition::disabled ||
           policy.disposition(MonitoringChannel::temperature) !=
               MonitoringChannelDisposition::disabled ||
           policy.disposition(MonitoringChannel::frequency) !=
               MonitoringChannelDisposition::disabled;
}

auto detail::PairwiseV2AdmissionClassificationAccess::create_inputs(
    AdmittedPlacementRequest first, AdmittedPlacementRequest second,
    std::uint64_t resolved_comparison_order_seed,
    StatisticsComparisonPolicySource statistics_source) noexcept
    -> std::expected<AdmittedPairwiseProductionInputs,
                     detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    try {
        auto pair = detail::PairwiseV2AdmissionClassificationAccess::admit_pair(
                               std::move(first), std::move(second),
                               resolved_comparison_order_seed);
        if (!pair)
            return std::unexpected{std::move(pair.error())};
        auto first_policy = detail::project_production_monitoring_policy(pair->first().request());
        if (!first_policy)
            return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
                Branch::first_monitoring_policy_mismatch, first_policy.error()}};
        auto second_policy = detail::project_production_monitoring_policy(pair->second().request());
        if (!second_policy)
            return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
                Branch::second_monitoring_policy_mismatch, second_policy.error()}};
        if (*first_policy != *second_policy)
            return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
                Branch::monitoring_policy_mismatch, pair_error()}};
        auto placement = detail::PairwiseV2AdmissionClassificationAccess::create_service_placement(
            pair->first(), detail::production_monitoring_needs_service(*first_policy));
        if (!placement)
            return std::unexpected{std::move(placement.error())};
        auto statistics_policy = detail::PairwiseV2AdmissionClassificationAccess::create_statistics_policy(
            statistics_source, pair->first().execution_inputs().measurement());
        if (!statistics_policy)
            return std::unexpected{std::move(statistics_policy.error())};
        return AdmittedPairwiseProductionInputs{std::move(*pair), std::move(*first_policy),
                                                std::move(*placement),
                                                std::move(statistics_source),
                                                std::move(*statistics_policy)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::allocation, production_error("contained_exception")}};
    } catch (const std::exception&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::production_contained_exception,
            production_error("contained_exception")}};
    } catch (...) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::production_contained_unknown_failure,
            production_error("contained_unknown_failure")}};
    }
}

auto AdmittedPairwiseProductionInputs::create(
    AdmittedPlacementRequest first, AdmittedPlacementRequest second,
    std::uint64_t resolved_comparison_order_seed,
    StatisticsComparisonPolicySource statistics_source) noexcept
    -> std::expected<AdmittedPairwiseProductionInputs, core::Error> {
    auto result = detail::PairwiseV2AdmissionClassificationAccess::create_inputs(
        std::move(first), std::move(second), resolved_comparison_order_seed,
        std::move(statistics_source));
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

auto AdmittedPairwiseProductionInputs::measurement_monitor_required() const noexcept -> bool {
    return detail::production_monitoring_needs_service(policy_);
}

} // namespace epycbench::runtime
