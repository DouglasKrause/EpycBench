#include "epycbench/monitoring/q0b_family_characterization.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string_view>

namespace epycbench::monitoring::qualification {
namespace {

auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b_characterization.invalid_input"},
            .message = "MON-007 Q0b characterization input is inconsistent",
            .operation = "monitoring.project_q0b_family_calibration",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto interval_json(const measurement::MeasurementInterval& interval)
    -> nlohmann::ordered_json {
    return {{"start_ns", interval.start().nanoseconds()},
            {"end_ns", interval.end().nanoseconds()},
            {"elapsed_ns", interval.elapsed().value()}};
}

auto policy_json(const measurement::CalibrationDurationPolicy& policy)
    -> nlohmann::ordered_json {
    return {{"minimum_duration_ns", policy.minimum_duration().value()},
            {"target_duration_ns", policy.target_duration().value()},
            {"maximum_duration_ns", policy.maximum_duration().value()}};
}

template <typename Outcome, typename CandidateValue>
auto calibration_json(const Outcome& outcome, CandidateValue candidate_value)
    -> nlohmann::ordered_json {
    auto empty_trials = nlohmann::ordered_json::array();
    for (const auto& trial : outcome.empty_harness_trials())
        empty_trials.push_back(interval_json(trial.interval()));

    auto candidate_trials = nlohmann::ordered_json::array();
    for (const auto& trial : outcome.candidate_trials()) {
        auto value = interval_json(trial.interval());
        value["candidate"] = candidate_value(trial);
        candidate_trials.push_back(std::move(value));
    }

    return {{"path", "RT-009 production first-variant calibration"},
            {"duration_policy", policy_json(outcome.duration_policy())},
            {"empty_harness_trials", std::move(empty_trials)},
            {"timer_overhead_estimate_ns",
             outcome.timer_overhead_estimate().duration().value()},
            {"candidate_trials", std::move(candidate_trials)},
            {"disposition", "selected"},
            {"selected_candidate", outcome.selected_candidate()->value()}};
}

template <typename Outcome, typename CandidateValue>
auto project(std::string_view family,
             const MonitoringProductionAdmissionProjection& admission,
             const MonitoringQ0bBaseEnvironmentProjection& base, const Outcome& outcome,
             CandidateValue candidate_value) -> std::expected<nlohmann::ordered_json, core::Error> {
    if (admission.variant() != runtime::PairwiseVariant::first ||
        base.variant() != runtime::PairwiseVariant::first)
        return std::unexpected{error("variant_not_first")};
    const auto derived_base = project_monitoring_q0b_base_environment(admission);
    if (!derived_base)
        return std::unexpected{derived_base.error()};
    if (derived_base->digest() != base.digest() ||
        derived_base->canonical_json() != base.canonical_json())
        return std::unexpected{error("base_environment_mismatch")};
    if (outcome.disposition() != measurement::CalibrationDisposition::selected ||
        !outcome.selected_candidate())
        return std::unexpected{error("calibration_not_selected")};
    if (outcome.empty_harness_trials().empty() || outcome.candidate_trials().empty() ||
        std::ranges::find(outcome.candidate_trials(), *outcome.selected_candidate(),
                          [](const auto& trial) { return trial.candidate(); }) ==
            outcome.candidate_trials().end())
        return std::unexpected{error("calibration_evidence_inconsistent")};

    return nlohmann::ordered_json{
        {"family", family},
        {"production_admission",
         {{"schema", monitoring_admitted_production_facts_schema},
          {"sha256", admission.digest().to_hex()},
          {"variant_role", "first"},
          {"scenario_digest", admission.scenario_digest()},
          {"resolved_scenario_plan_digest", admission.resolved_scenario_plan_digest()},
          {"base_environment_sha256", base.digest().to_hex()}}},
        {"selected_work", outcome.selected_candidate()->value()},
        {"calibration", calibration_json(outcome, candidate_value)}};
}

} // namespace

auto project_q0b_family_calibration(
    const MonitoringProductionAdmissionProjection& admission,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const measurement::SequentialReadBandwidthCalibrationOutcome& outcome) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        return project("sequential-read-bandwidth", admission, base, outcome,
                       [](const auto& trial) { return trial.candidate().value(); });
    } catch (const std::exception&) {
        return std::unexpected{error("contained_exception")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure")};
    }
}

auto project_q0b_family_calibration(
    const MonitoringProductionAdmissionProjection& admission,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const measurement::DependentLoadLatencyCalibrationOutcome& outcome) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        return project("dependent-load-latency", admission, base, outcome,
                       [](const auto& trial) { return trial.candidate().value(); });
    } catch (const std::exception&) {
        return std::unexpected{error("contained_exception")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure")};
    }
}

} // namespace epycbench::monitoring::qualification
