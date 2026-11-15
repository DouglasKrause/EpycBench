#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
#include "core/json_ownership.hpp"
#include "runtime/finalized_evidence_wire_memory_projection.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"
#include "runtime/finalized_evidence_wire_typed_stock.hpp"
#include "runtime/finalized_evidence_wire_monitoring_projection.hpp"
#include "runtime/finalized_evidence_wire_schema.generated.hpp"

#include "core/seed_derivation_access.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "kernel/kernel_leaf_access.hpp"
#include "measurement/measurement_leaf_access.hpp"
#include "monitoring/pairwise_prepared_capture_storage.hpp"
#include "monitoring/page_fault_classifier_kernel.hpp"
#include "monitoring/context_switch_classifier_kernel.hpp"
#include "monitoring/temperature_classifier_kernel.hpp"
#include "monitoring/frequency_classifier_kernel.hpp"
#include "memory/pairwise_memory_access.hpp"
#include "statistics/adaptive_stopping_access.hpp"
#include "statistics/bootstrap_access.hpp"
#include "statistics/log_ratio_access.hpp"

#include <bit>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <exception>
#include <optional>
#include <limits>
#include <string_view>
#include <vector>
#include <array>

#include <nlohmann/json.hpp>

namespace epycbench::runtime::detail {

struct FinalizedEvidenceWirePolicyAccess final {
    [[nodiscard]] static auto policy(
        statistics::BootstrapPolicy comparison,
        statistics::BootstrapPolicy stopping, double fraction,
        statistics::PracticalEffectThreshold threshold,
        statistics::RelativeHalfWidthTarget target) noexcept
        -> StatisticsProductionPolicy {
        return StatisticsProductionPolicy{comparison, stopping, fraction,
                                          threshold, target};
    }
};

} // namespace epycbench::runtime::detail

namespace epycbench::runtime::wire_v1::detail {
auto fixed_reconstruct_failed_component_header(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, ReconstructionPath path)
    -> std::expected<ReconstructedFailedComponentHeader, FixedTypedReconstructionError>;
auto fixed_reconstruct_failed_component_workers(
    const nlohmann::json&, ReconstructedFailedComponentHeader, ReconstructionPath path)
    -> std::expected<FailedObservationEvidence, FixedTypedReconstructionError>;
auto fixed_reconstruct_block_plan_component(const nlohmann::json&)
    -> std::expected<measurement::MeasurementBlockPlan, FixedTypedReconstructionError>;
auto fixed_reconstruct_factual_component(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, std::size_t expected_slot,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FactualObservationEvidence, FixedTypedReconstructionError>;
auto fixed_reconstruct_evidence_header(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, FixedTypedReconstructionError>;
auto fixed_reconstruct_evidence_header(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, FixedTypedReconstructionError>;
auto fixed_reconstruct_calibration_warmup(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, FixedTypedReconstructionError>;
auto fixed_reconstruct_calibration_warmup(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, FixedTypedReconstructionError>;
auto fixed_reconstruct_observation_blocks(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError>;
auto fixed_reconstruct_observation_blocks(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError>;
auto fixed_reconstruct_observation_blocks(
    const nlohmann::json& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError>;
auto fixed_reconstruct_stopping_and_failures(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, FixedTypedReconstructionError>;
auto fixed_reconstruct_stopping_and_failures(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, FixedTypedReconstructionError>;
auto fixed_reconstruct_finalized_error_value(const nlohmann::json& bounded_error)
    -> std::expected<FinalizedEvidenceError, FixedTypedReconstructionError>;
auto fixed_reconstruct_terminal_and_finalization(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, FixedTypedReconstructionError>;
auto fixed_reconstruct_terminal_and_finalization(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, FixedTypedReconstructionError>;
auto fixed_reconstruct_observation_blocks(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError>;
auto fixed_reconstruct_finalized_evidence(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FinalizedPairwiseRunEvidence, FixedTypedReconstructionError>;
auto placement_evaluator_domain(const memory::RegionPlacementObservation& value) noexcept -> bool {
    const auto total = value.total_pages();
    if (total == 0U || value.resident_pages() > total ||
        value.unresolved_pages() != total - value.resident_pages()) return false;
    std::uint64_t sum{};
    std::optional<core::LinuxNumaNodeId> prior;
    for (const auto& node : value.resident_page_counts()) {
        if (node.page_count() == 0U || (prior && node.linux_node() <= *prior) ||
            node.page_count() > std::numeric_limits<std::uint64_t>::max() - sum) return false;
        sum += node.page_count(); prior = node.linux_node();
    }
    return sum == value.resident_pages();
}

namespace {

auto error(TypedReconstructionErrorCode code, ReconstructionPath path)
    -> std::unexpected<FixedTypedReconstructionError> {
    return std::unexpected{FixedTypedReconstructionError{code, std::move(path)}};
}

auto u64(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<std::uint64_t, FixedTypedReconstructionError> {
    if (path.overflowed())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    if (!value.is_string())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() != 1U && text.front() == '0'))
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    std::uint64_t result{};
    const auto [end, conversion] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (conversion != std::errc{} || end != text.data() + text.size())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    return result;
}

auto binary64(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<double, FixedTypedReconstructionError> {
    if (!value.is_string())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 16U)
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    std::uint64_t bits{};
    for (const char character : text) {
        const std::uint64_t digit = character >= '0' && character <= '9'
            ? static_cast<std::uint64_t>(character - '0')
            : character >= 'a' && character <= 'f'
                ? static_cast<std::uint64_t>(character - 'a' + 10)
                : 16U;
        if (digit == 16U)
            return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
        bits = (bits << 4U) | digit;
    }
    const auto result = std::bit_cast<double>(bits);
    if (!std::isfinite(result))
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    return result;
}

auto bootstrap(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<statistics::BootstrapPolicy, FixedTypedReconstructionError> {
    const auto count = u64(value.at("replicate_count"), ReconstructionPath{path} + ".replicate_count");
    const auto confidence = binary64(value.at("confidence"), ReconstructionPath{path} + ".confidence");
    const auto seed = u64(value.at("seed"), ReconstructionPath{path} + ".seed");
    if (!count) return std::unexpected{count.error()};
    if (!confidence) return std::unexpected{confidence.error()};
    if (!seed) return std::unexpected{seed.error()};
    if (*count == 0U || *count > std::numeric_limits<std::size_t>::max() ||
        *confidence <= 0.0 || *confidence >= 1.0)
        return error(TypedReconstructionErrorCode::typed_invariant, path);
    auto result = statistics::BootstrapPolicy::create(
        *count, *confidence, core::detail::SeedDerivationAccess::seed(*seed));
    core::detail::ExpectedErrorCleanup result_cleanup{result};
    if (!result)
        return error(TypedReconstructionErrorCode::typed_invariant, ReconstructionPath{path});
    return *result;
}

auto disposition(std::string_view token, ReconstructionPath path)
    -> std::expected<monitoring::MonitoringChannelDisposition, FixedTypedReconstructionError> {
    if (token == "disabled") return monitoring::MonitoringChannelDisposition::disabled;
    if (token == "optional") return monitoring::MonitoringChannelDisposition::optional;
    if (token == "required") return monitoring::MonitoringChannelDisposition::required;
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto interval(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<measurement::MeasurementInterval, FixedTypedReconstructionError> {
    auto start = u64(value.at("start"), ReconstructionPath{path} + ".start");
    auto end = u64(value.at("end"), ReconstructionPath{path} + ".end");
    auto elapsed = u64(value.at("elapsed_nanoseconds"),
                       ReconstructionPath{path} + ".elapsed_nanoseconds");
    if (!start) return std::unexpected{start.error()};
    if (!end) return std::unexpected{end.error()};
    if (!elapsed) return std::unexpected{elapsed.error()};
    if (*end < *start)
        return error(TypedReconstructionErrorCode::typed_invariant, ReconstructionPath{path});
    auto result = measurement::MeasurementInterval::create(
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*start),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*end));
    core::detail::ExpectedErrorCleanup result_cleanup{result};
    if (!result)
        return error(TypedReconstructionErrorCode::typed_invariant, ReconstructionPath{path});
    if (result->elapsed().value() != *elapsed)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     ReconstructionPath{path} + ".elapsed_nanoseconds");
    return *result;
}

auto calibration_disposition(std::string_view token, ReconstructionPath path)
    -> std::expected<measurement::CalibrationDisposition, FixedTypedReconstructionError> {
    if (token == "selected") return measurement::CalibrationDisposition::selected;
    if (token == "minimum_candidate_above_maximum_duration")
        return measurement::CalibrationDisposition::minimum_candidate_above_maximum_duration;
    if (token == "maximum_candidate_below_minimum_duration")
        return measurement::CalibrationDisposition::maximum_candidate_below_minimum_duration;
    if (token == "no_candidate_in_acceptable_band")
        return measurement::CalibrationDisposition::no_candidate_in_acceptable_band;
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto warmup_disposition(std::string_view token, ReconstructionPath path)
    -> std::expected<measurement::WarmupDisposition, FixedTypedReconstructionError> {
    if (token == "skipped") return measurement::WarmupDisposition::skipped;
    if (token == "stabilized") return measurement::WarmupDisposition::stabilized;
    if (token == "maximum_trials_reached_without_stability")
        return measurement::WarmupDisposition::maximum_trials_reached_without_stability;
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto rational_less_equal(std::uint64_t left_numerator, std::uint64_t left_denominator,
                         std::uint64_t right_numerator,
                         std::uint64_t right_denominator) noexcept -> bool {
    bool reversed = false;
    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient)
            return reversed ? left_quotient > right_quotient : left_quotient < right_quotient;
        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            if (left_remainder == 0U && right_remainder == 0U) return true;
            return reversed ? right_remainder == 0U : left_remainder == 0U;
        }
        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        reversed = !reversed;
    }
}

auto duration_policy(const nlohmann::json& value)
    -> std::expected<measurement::CalibrationDurationPolicy, FixedTypedReconstructionError> {
    auto minimum = u64(value.at("minimum_duration"), "calibration.duration_policy.minimum_duration");
    auto target = u64(value.at("target_duration"), "calibration.duration_policy.target_duration");
    auto maximum = u64(value.at("maximum_duration"), "calibration.duration_policy.maximum_duration");
    if (!minimum) return std::unexpected{minimum.error()};
    if (!target) return std::unexpected{target.error()};
    if (!maximum) return std::unexpected{maximum.error()};
    if (*minimum > *target || *target > *maximum)
        return error(TypedReconstructionErrorCode::typed_invariant, "calibration.duration_policy");
    auto result = measurement::CalibrationDurationPolicy::create(
        core::Duration::from_base_units(*minimum), core::Duration::from_base_units(*target),
        core::Duration::from_base_units(*maximum));
    core::detail::ExpectedErrorCleanup result_cleanup{result};
    if (!result) return error(TypedReconstructionErrorCode::typed_invariant,
                              "calibration.duration_policy");
    return *result;
}

template <class Count, class Trial, class MakeTrial, class MakeOutcome>
auto calibration(const nlohmann::json& value, std::uint64_t quantum,
                 std::uint64_t maximum_units, MakeTrial make_trial, MakeOutcome make_outcome)
    -> std::expected<PairwiseCalibrationEvidence, FixedTypedReconstructionError> {
    static_assert(sizeof(MakeTrial)==sizeof(std::array<std::byte,1>));
    static_assert(sizeof(MakeOutcome)==sizeof(std::array<std::byte,1>));
    auto policy = duration_policy(value.at("duration_policy"));
    if (!policy) return std::unexpected{policy.error()};
    const auto& empty_json = value.at("empty_harness_trials");
    if (empty_json.size() != 5U)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "calibration.empty_harness_trials");
    std::vector<measurement::EmptyHarnessTrial> empty;
    empty.reserve(5U);
    std::uint64_t minimum_overhead = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index{}; index < empty_json.size(); ++index) {
        auto measured = interval(empty_json[index],
                                 "calibration.empty_harness_trials[" + ReconstructionPath::ordinal(index) + "]");
        if (!measured) return std::unexpected{measured.error()};
        minimum_overhead = std::min(minimum_overhead, measured->elapsed().value());
        empty.push_back(measurement::EmptyHarnessTrial::create(std::move(*measured)));
    }
    auto overhead = u64(value.at("timer_overhead_estimate"),
                        "calibration.timer_overhead_estimate");
    if (!overhead) return std::unexpected{overhead.error()};
    if (*overhead != minimum_overhead)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "calibration.timer_overhead_estimate");

    const auto& candidate_json = value.at("candidate_trials");
    if (candidate_json.empty() || candidate_json.size() > 128U)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "calibration.candidate_trials");
    std::vector<Trial> trials;
    trials.reserve(candidate_json.size());
    std::vector<std::uint64_t> units;
    units.reserve(candidate_json.size());
    for (std::size_t index{}; index < candidate_json.size(); ++index) {
        auto candidate = u64(candidate_json[index].at("candidate"),
                             "calibration.candidate_trials[" + ReconstructionPath::ordinal(index) + "].candidate");
        auto measured = interval(candidate_json[index].at("interval"),
                                 "calibration.candidate_trials[" + ReconstructionPath::ordinal(index) + "].interval");
        if (!candidate) return std::unexpected{candidate.error()};
        if (!measured) return std::unexpected{measured.error()};
        if (*candidate == 0U || *candidate % quantum != 0U)
            return error(TypedReconstructionErrorCode::typed_invariant,
                         "calibration.candidate_trials[" + ReconstructionPath::ordinal(index) + "].candidate");
        auto count = Count::create(*candidate);
        core::detail::ExpectedErrorCleanup count_cleanup{count};
        if (!count) return error(TypedReconstructionErrorCode::typed_invariant,
                                 "calibration.candidate_trials");
        units.push_back(*candidate / quantum);
        trials.push_back(make_trial(*count, std::move(*measured)));
    }
    // Reproduce the production exponential/binary search route exactly.
    std::optional<std::uint64_t> lower, upper;
    std::uint64_t expected = 1U;
    for (std::size_t index{}; index < trials.size(); ++index) {
        if (units[index] != expected)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "calibration.candidate_trials[" + ReconstructionPath::ordinal(index) + "].candidate");
        const auto elapsed = trials[index].interval().elapsed();
        if (elapsed == policy->target_duration()) {
            if (index + 1U != trials.size())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "calibration.candidate_trials");
            continue;
        }
        if (elapsed < policy->target_duration()) lower = expected; else upper = expected;
        if (index + 1U == trials.size()) continue;
        if (elapsed < policy->target_duration() && expected < maximum_units && !upper) {
            expected = expected > maximum_units - expected ? maximum_units : expected * 2U;
        } else if (lower && upper && *upper - *lower > 1U) {
            expected = *lower + ((*upper - *lower) / 2U);
        } else {
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "calibration.candidate_trials");
        }
    }
    const auto terminal_elapsed = trials.back().interval().elapsed();
    if (terminal_elapsed != policy->target_duration() &&
        !(lower && upper && *upper - *lower <= 1U) &&
        !(trials.size() == 1U && terminal_elapsed > policy->target_duration()) &&
        !(terminal_elapsed < policy->target_duration() && units.back() == maximum_units))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "calibration.candidate_trials");

    std::optional<Count> derived_selected;
    std::uint64_t best_distance{};
    for (const auto& trial : trials) {
        const auto elapsed = trial.interval().elapsed();
        if (elapsed < policy->minimum_duration() || elapsed > policy->maximum_duration()) continue;
        const auto distance = elapsed.value() < policy->target_duration().value()
            ? policy->target_duration().value() - elapsed.value()
            : elapsed.value() - policy->target_duration().value();
        if (!derived_selected || distance < best_distance ||
            (distance == best_distance && trial.candidate().value() < derived_selected->value())) {
            derived_selected.reset();
            derived_selected.emplace(trial.candidate());
            best_distance = distance;
        }
    }
    auto derived_disposition = measurement::CalibrationDisposition::no_candidate_in_acceptable_band;
    if (derived_selected) derived_disposition = measurement::CalibrationDisposition::selected;
    else if (trials.front().interval().elapsed() > policy->maximum_duration())
        derived_disposition = measurement::CalibrationDisposition::minimum_candidate_above_maximum_duration;
    else if (units.back() == maximum_units &&
             trials.back().interval().elapsed() < policy->minimum_duration())
        derived_disposition = measurement::CalibrationDisposition::maximum_candidate_below_minimum_duration;
    auto stated_disposition = calibration_disposition(value.at("disposition").get<std::string_view>(),
                                                      "calibration.disposition");
    if (!stated_disposition) return std::unexpected{stated_disposition.error()};
    if (*stated_disposition != derived_disposition)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "calibration.disposition");
    std::optional<Count> stated_selected;
    if (!value.at("selected_candidate").is_null()) {
        auto raw = u64(value.at("selected_candidate"), "calibration.selected_candidate");
        if (!raw) return std::unexpected{raw.error()};
        if (*raw == 0U)
            return error(TypedReconstructionErrorCode::typed_invariant, "calibration.selected_candidate");
        auto made = Count::create(*raw);
        core::detail::ExpectedErrorCleanup made_cleanup{made};
        if (!made) return error(TypedReconstructionErrorCode::typed_invariant,
                                "calibration.selected_candidate");
        stated_selected.emplace(*made);
    }
    if (stated_selected != derived_selected)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "calibration.selected_candidate");
    return PairwiseCalibrationEvidence{make_outcome(
        std::move(*policy), std::move(empty),
        measurement::TimerOverheadEstimate::create(core::Duration::from_base_units(*overhead)),
        std::move(trials), *stated_disposition, stated_selected)};
}

auto evaluate_warmup(std::span<const measurement::WarmupTrial> trials, std::size_t window,
                     measurement::WarmupStabilityThreshold threshold)
    -> measurement::WarmupStabilityEvaluation {
    std::vector<std::uint64_t> values;
    values.reserve(window);
    for (const auto& trial : trials.subspan(trials.size() - window))
        values.push_back(trial.interval().elapsed().value());
    std::sort(values.begin(), values.end());
    const auto median = values[(values.size() - 1U) / 2U];
    std::uint64_t maximum_deviation{};
    bool stable = true;
    for (const auto sample : values) {
        const auto deviation = sample < median ? median - sample : sample - median;
        maximum_deviation = std::max(maximum_deviation, deviation);
        if (median == 0U) stable = stable && sample == 0U;
        else {
            // Avoid multiplication overflow while retaining exact rational comparison.
            stable = stable && rational_less_equal(
                deviation, median, threshold.numerator(), threshold.denominator());
        }
    }
    return measurement::WarmupStabilityEvaluation::create(
        core::Duration::from_base_units(median),
        core::Duration::from_base_units(maximum_deviation), stable);
}

template <class Count, class MakeOutcome>
auto warmup(const nlohmann::json& value, MakeOutcome make_outcome)
    -> std::expected<PairwiseWarmupEvidence, FixedTypedReconstructionError> {
    static_assert(sizeof(MakeOutcome)==sizeof(std::array<std::byte,1>));
    const auto& policy_json = value.at("policy");
    auto minimum = u64(policy_json.at("minimum_trials"), "warmup.policy.minimum_trials");
    auto maximum = u64(policy_json.at("maximum_trials"), "warmup.policy.maximum_trials");
    auto window = u64(policy_json.at("stability_window"), "warmup.policy.stability_window");
    auto numerator = u64(policy_json.at("threshold_numerator"), "warmup.policy.threshold_numerator");
    auto denominator = u64(policy_json.at("threshold_denominator"), "warmup.policy.threshold_denominator");
    if (!minimum) return std::unexpected{minimum.error()};
    if (!maximum) return std::unexpected{maximum.error()};
    if (!window) return std::unexpected{window.error()};
    if (!numerator) return std::unexpected{numerator.error()};
    if (!denominator) return std::unexpected{denominator.error()};
    if (*denominator == 0U)
        return error(TypedReconstructionErrorCode::typed_invariant, "warmup.policy.stability_threshold");
    auto threshold = measurement::WarmupStabilityThreshold::create(*numerator, *denominator);
    core::detail::ExpectedErrorCleanup threshold_cleanup{threshold};
    if (!threshold) return error(TypedReconstructionErrorCode::typed_invariant,
                                 "warmup.policy.stability_threshold");
    if (*minimum > *maximum || (*maximum == 0U && *minimum != 0U) ||
        (*maximum > 0U && (*window == 0U || *window > *maximum)))
        return error(TypedReconstructionErrorCode::typed_invariant, "warmup.policy");
    auto policy = measurement::WarmupPolicy::create(*minimum, *maximum, *window, *threshold);
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy) return error(TypedReconstructionErrorCode::typed_invariant, "warmup.policy");
    auto selected_raw = u64(value.at("selected_work"), "warmup.selected_work");
    if (!selected_raw) return std::unexpected{selected_raw.error()};
    if (*selected_raw == 0U)
        return error(TypedReconstructionErrorCode::typed_invariant, "warmup.selected_work");
    auto selected = Count::create(*selected_raw);
    core::detail::ExpectedErrorCleanup selected_cleanup{selected};
    if (!selected) return error(TypedReconstructionErrorCode::typed_invariant,
                                "warmup.selected_work");
    std::vector<measurement::WarmupTrial> trials;
    const auto& trial_json = value.at("trials");
    if (trial_json.size() > *maximum)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "warmup.trials");
    trials.reserve(trial_json.size());
    for (std::size_t index{}; index < trial_json.size(); ++index) {
        auto measured = interval(trial_json[index],
                                 "warmup.trials[" + ReconstructionPath::ordinal(index) + "]");
        if (!measured) return std::unexpected{measured.error()};
        trials.push_back(measurement::WarmupTrial::create(std::move(*measured)));
    }
    auto stated_disposition = warmup_disposition(value.at("disposition").get<std::string_view>(),
                                                 "warmup.disposition");
    if (!stated_disposition) return std::unexpected{stated_disposition.error()};
    std::optional<measurement::WarmupStabilityEvaluation> derived;
    for (std::size_t count = 1U; count <= trials.size(); ++count) {
        if (count < *minimum || count < *window) continue;
        auto current = evaluate_warmup(std::span<const measurement::WarmupTrial>{trials}.first(count),
                                       static_cast<std::size_t>(*window), *threshold);
        if (current.stable() && count != trials.size())
            return error(TypedReconstructionErrorCode::inconsistent_projection, "warmup.trials");
        if (count == trials.size()) derived.emplace(std::move(current));
    }
    measurement::WarmupDisposition expected;
    if (*maximum == 0U) expected = measurement::WarmupDisposition::skipped;
    else if (derived && derived->stable()) expected = measurement::WarmupDisposition::stabilized;
    else expected = measurement::WarmupDisposition::maximum_trials_reached_without_stability;
    if (*stated_disposition != expected ||
        (expected == measurement::WarmupDisposition::skipped && !trials.empty()) ||
        (expected == measurement::WarmupDisposition::stabilized &&
         (trials.size() < *minimum || trials.size() < *window)) ||
        (expected == measurement::WarmupDisposition::maximum_trials_reached_without_stability &&
         trials.size() != *maximum))
        return error(TypedReconstructionErrorCode::inconsistent_projection, "warmup.disposition");
    const auto& evaluation_json = value.at("final_stability_evaluation");
    if (evaluation_json.is_null() != !derived)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "warmup.final_stability_evaluation");
    if (derived) {
        auto median = u64(evaluation_json.at("median_duration"),
                          "warmup.final_stability_evaluation.median_duration");
        auto deviation = u64(evaluation_json.at("maximum_absolute_deviation"),
                             "warmup.final_stability_evaluation.maximum_absolute_deviation");
        if (!median) return std::unexpected{median.error()};
        if (!deviation) return std::unexpected{deviation.error()};
        if (*median != derived->median_duration().value() ||
            *deviation != derived->maximum_absolute_deviation().value() ||
            evaluation_json.at("stable").get<bool>() != derived->stable())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "warmup.final_stability_evaluation");
    }
    return PairwiseWarmupEvidence{make_outcome(std::move(*policy), *selected,
        std::move(trials), *stated_disposition, derived)};
}

} // namespace

auto fixed_reconstruct_evidence_header(const nlohmann::json& body)
    -> std::expected<ReconstructedEvidenceHeader, FixedTypedReconstructionError> {
    try {
        const auto& provenance = body.at("provenance");
        const auto experiment = u64(provenance.at("experiment_id"), "provenance.experiment_id");
        if (!experiment) return std::unexpected{experiment.error()};
        PairwiseRunProvenance provenance_value{
            core::ExperimentId{*experiment}, provenance.at("scenario_profile").get<std::string>(),
            provenance.at("scenario_digest").get<std::string>(),
            provenance.at("resolved_plan_digest").get<std::string>(),
            provenance.at("workload_id").get<std::string>(),
            provenance.at("first_variant_id").get<std::string>(),
            provenance.at("second_variant_id").get<std::string>()};

        const auto& statistics_value = body.at("statistics_policy");
        auto comparison = bootstrap(statistics_value.at("comparison_bootstrap_policy"),
                                    "statistics_policy.comparison_bootstrap_policy");
        auto stopping = bootstrap(statistics_value.at("stopping_bootstrap_policy"),
                                  "statistics_policy.stopping_bootstrap_policy");
        auto fraction = binary64(statistics_value.at("effective_practical_effect_fraction"),
                                 "statistics_policy.effective_practical_effect_fraction");
        auto delta = binary64(statistics_value.at("practical_effect_delta"),
                              "statistics_policy.practical_effect_delta");
        auto target_value = binary64(statistics_value.at("relative_half_width_target"),
                                     "statistics_policy.relative_half_width_target");
        if (!comparison) return std::unexpected{comparison.error()};
        if (!stopping) return std::unexpected{stopping.error()};
        if (!fraction) return std::unexpected{fraction.error()};
        if (!delta) return std::unexpected{delta.error()};
        if (!target_value) return std::unexpected{target_value.error()};
        if (*fraction < 0.0 || *fraction > 1.0 || *target_value < 0.0 || *target_value > 1.0 ||
            !std::isfinite(std::log1p(*fraction)) || std::log1p(*fraction) < 0.0)
            return error(TypedReconstructionErrorCode::typed_invariant, "statistics_policy");
        auto threshold = statistics::PracticalEffectThreshold::create(*fraction);
        core::detail::ExpectedErrorCleanup threshold_cleanup{threshold};
        auto target = statistics::RelativeHalfWidthTarget::create(*target_value);
        core::detail::ExpectedErrorCleanup target_cleanup{target};
        if (!threshold || !target)
            return error(TypedReconstructionErrorCode::typed_invariant, "statistics_policy");
        if (std::bit_cast<std::uint64_t>(threshold->delta()) !=
            std::bit_cast<std::uint64_t>(*delta))
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "statistics_policy.practical_effect_delta");
        auto statistics_policy = runtime::detail::FinalizedEvidenceWirePolicyAccess::policy(
            *comparison, *stopping, *fraction, *threshold, *target);

        const auto& monitoring_value = body.at("monitoring_policy");
        auto page = disposition(monitoring_value.at("page_faults").get<std::string_view>(),
                                "monitoring_policy.page_faults");
        auto context = disposition(monitoring_value.at("context_switches").get<std::string_view>(),
                                   "monitoring_policy.context_switches");
        auto temperature = disposition(monitoring_value.at("temperature").get<std::string_view>(),
                                       "monitoring_policy.temperature");
        auto frequency = disposition(monitoring_value.at("frequency").get<std::string_view>(),
                                     "monitoring_policy.frequency");
        if (!page) return std::unexpected{page.error()};
        if (!context) return std::unexpected{context.error()};
        if (!temperature) return std::unexpected{temperature.error()};
        if (!frequency) return std::unexpected{frequency.error()};
        std::optional<core::Duration> interval;
        if (!monitoring_value.at("during_interval").is_null()) {
            auto amount = u64(monitoring_value.at("during_interval"),
                              "monitoring_policy.during_interval");
            if (!amount) return std::unexpected{amount.error()};
            interval = core::Duration::from_base_units(*amount);
        }
        const auto unsupported_token =
            monitoring_value.at("optional_unsupported").get<std::string_view>();
        std::optional<monitoring::OptionalUnavailableSourcePolicy> unsupported;
        if (unsupported_token == "fail") unsupported = monitoring::OptionalUnavailableSourcePolicy::fail;
        else if (unsupported_token == "warn") unsupported = monitoring::OptionalUnavailableSourcePolicy::warn;
        else if (unsupported_token == "disable") unsupported = monitoring::OptionalUnavailableSourcePolicy::disable;
        else return error(TypedReconstructionErrorCode::invalid_scalar,
                          "monitoring_policy.optional_unsupported");
        const bool before = monitoring_value.at("before_observation").get<bool>();
        const bool after = monitoring_value.at("after_observation").get<bool>();
        const bool during = monitoring_value.at("during_observation").get<bool>();
        const auto disabled = monitoring::MonitoringChannelDisposition::disabled;
        if (during != interval.has_value() || (interval && interval->value() == 0U) ||
            (*page != disabled && (!before || !after)) ||
            ((*temperature != disabled || *frequency != disabled) && !during))
            return error(TypedReconstructionErrorCode::typed_invariant, "monitoring_policy");
        auto monitoring_policy = monitoring::MonitoringExecutionPolicy::create(
            *page, *context, *temperature, *frequency,
            monitoring_value.at("before_observation").get<bool>(),
            monitoring_value.at("after_observation").get<bool>(),
            monitoring_value.at("during_observation").get<bool>(), interval, *unsupported);
        core::detail::ExpectedErrorCleanup monitoring_policy_cleanup{monitoring_policy};
        if (!monitoring_policy)
            return error(TypedReconstructionErrorCode::typed_invariant, "monitoring_policy");

        const auto& repetition_value = body.at("repetition_policy");
        auto minimum = u64(repetition_value.at("minimum_blocks"), "repetition_policy.minimum_blocks");
        auto maximum = u64(repetition_value.at("maximum_blocks"), "repetition_policy.maximum_blocks");
        auto valid = u64(repetition_value.at("minimum_valid_observations_per_variant"),
                         "repetition_policy.minimum_valid_observations_per_variant");
        if (!minimum) return std::unexpected{minimum.error()};
        if (!maximum) return std::unexpected{maximum.error()};
        if (!valid) return std::unexpected{valid.error()};
        if (*minimum > *maximum || *maximum > std::numeric_limits<std::uint64_t>::max() / 4U + 1U)
            return error(TypedReconstructionErrorCode::typed_invariant, "repetition_policy");
        auto repetition_policy = measurement::RepetitionPolicy::create(
            *minimum, *maximum, *valid, repetition_value.at("stopping_enabled").get<bool>());
        core::detail::ExpectedErrorCleanup repetition_policy_cleanup{repetition_policy};
        if (!repetition_policy)
            return error(TypedReconstructionErrorCode::typed_invariant, "repetition_policy");

        const auto& order_value = body.at("comparison_order");
        const auto strategy_token = order_value.at("strategy").get<std::string_view>();
        std::optional<measurement::ComparisonOrderStrategy> strategy;
        if (strategy_token == "randomized_abba") strategy = measurement::ComparisonOrderStrategy::randomized_abba;
        else if (strategy_token == "randomized_baab") strategy = measurement::ComparisonOrderStrategy::randomized_baab;
        else if (strategy_token == "randomized_balanced") strategy = measurement::ComparisonOrderStrategy::randomized_balanced;
        else return error(TypedReconstructionErrorCode::invalid_scalar, "comparison_order.strategy");
        auto seed = u64(order_value.at("seed"), "comparison_order.seed");
        if (!seed) return std::unexpected{seed.error()};

        return ReconstructedEvidenceHeader{
            std::move(provenance_value), std::move(statistics_policy),
            std::move(*monitoring_policy), std::move(*repetition_policy),
            measurement::ComparisonOrderPolicy::create(*strategy, *seed)};
    } catch (const nlohmann::json::out_of_range&) {
        return error(TypedReconstructionErrorCode::missing_member, "root");
    } catch (const nlohmann::json::exception&) {
        return error(TypedReconstructionErrorCode::invalid_scalar, "root");
    }
}

auto fixed_reconstruct_evidence_header(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, FixedTypedReconstructionError> {
    return fixed_reconstruct_evidence_header(bounded_body.value());
}

auto fixed_reconstruct_calibration_component(const nlohmann::json& calibration_json)
    -> std::expected<PairwiseCalibrationEvidence, FixedTypedReconstructionError> {
    try {
        const auto kind = calibration_json.at("kind").get<std::string_view>();
        if (kind == "bandwidth") {
            return calibration<kernel::SequentialReadBandwidthPassCount,
                measurement::SequentialReadBandwidthCalibrationTrial>(
                calibration_json, 1U, std::numeric_limits<std::uint64_t>::max(),
                [](auto count, auto measured) {
                    return measurement::SequentialReadBandwidthCalibrationTrial::create(
                        count, std::move(measured));
                },
                [](auto&&... arguments) {
                    return measurement::detail::CalibrationAccess::make_outcome(
                        std::forward<decltype(arguments)>(arguments)...);
                });
        } else if (kind == "latency") {
            return calibration<kernel::DependentLoadLatencyStepCount,
                measurement::DependentLoadLatencyCalibrationTrial>(
                calibration_json, 4096U,
                std::numeric_limits<std::uint64_t>::max() / 4096U,
                [](auto count, auto measured) {
                    return measurement::DependentLoadLatencyCalibrationTrial::create(
                        count, std::move(measured));
                },
                [](auto&&... arguments) {
                    return measurement::detail::CalibrationAccess::make_outcome(
                        std::forward<decltype(arguments)>(arguments)...);
                });
        } else return error(TypedReconstructionErrorCode::invalid_scalar,
                            "calibration.kind");
    } catch (const nlohmann::json::out_of_range&) {
        return error(TypedReconstructionErrorCode::missing_member, "root");
    } catch (const nlohmann::json::exception&) {
        return error(TypedReconstructionErrorCode::invalid_scalar, "root");
    }
}

auto fixed_reconstruct_warmup_component(const nlohmann::json& warmup_json)
    -> std::expected<PairwiseWarmupEvidence, FixedTypedReconstructionError> {
    try {
        const auto kind = warmup_json.at("kind").get<std::string_view>();
        if (kind == "bandwidth") {
            return warmup<kernel::SequentialReadBandwidthPassCount>(
                warmup_json, [](auto&&... arguments) {
                    return measurement::detail::WarmupAccess::make_outcome(
                        std::forward<decltype(arguments)>(arguments)...);
                });
        } else if (kind == "latency") {
            return warmup<kernel::DependentLoadLatencyStepCount>(
                warmup_json, [](auto&&... arguments) {
                    return measurement::detail::WarmupAccess::make_outcome(
                        std::forward<decltype(arguments)>(arguments)...);
                });
        } else return error(TypedReconstructionErrorCode::invalid_scalar, "warmup.kind");
    } catch (const nlohmann::json::out_of_range&) {
        return error(TypedReconstructionErrorCode::missing_member, "root");
    } catch (const nlohmann::json::exception&) {
        return error(TypedReconstructionErrorCode::invalid_scalar, "root");
    }
}

auto fixed_reconstruct_calibration_warmup(const nlohmann::json& body)
    -> std::expected<ReconstructedCalibrationWarmup, FixedTypedReconstructionError> {
    try {
        ReconstructedCalibrationWarmup result;
        const auto& calibration_json = body.at("calibration");
        if (!calibration_json.is_null()) {
            auto value = fixed_reconstruct_calibration_component(calibration_json);
            if (!value) return std::unexpected{std::move(value.error())};
            result.calibration.emplace(std::move(*value));
        }
        const auto& warmup_json = body.at("warmup");
        if (!warmup_json.is_null()) {
            auto value = fixed_reconstruct_warmup_component(warmup_json);
            if (!value) return std::unexpected{std::move(value.error())};
            result.warmup.emplace(std::move(*value));
        }
        return result;
    } catch (const nlohmann::json::out_of_range&) {
        return error(TypedReconstructionErrorCode::missing_member, "root");
    } catch (const nlohmann::json::exception&) {
        return error(TypedReconstructionErrorCode::invalid_scalar, "root");
    }
}

auto fixed_reconstruct_calibration_warmup(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, FixedTypedReconstructionError> {
    return fixed_reconstruct_calibration_warmup(bounded_body.value());
}

namespace {

template <class Enum>
auto closed_token(std::string_view token, ReconstructionPath path,
                  std::initializer_list<std::pair<std::string_view, Enum>> values)
    -> std::expected<Enum, FixedTypedReconstructionError> {
    for (const auto& [candidate, value] : values)
        if (candidate == token) return value;
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto implementation(std::string_view token, ReconstructionPath path)
    -> std::expected<kernel::KernelImplementationId, FixedTypedReconstructionError> {
    return closed_token<kernel::KernelImplementationId>(token, std::move(path), {
        {"scalar_u64_sequential_v1", kernel::KernelImplementationId::scalar_u64_sequential_v1},
        {"scalar_u64_random_cycle_v1", kernel::KernelImplementationId::scalar_u64_random_cycle_v1}});
}

auto reconstruct_validity(const nlohmann::json& value)
    -> std::expected<measurement::ObservationValidityEvidence, FixedTypedReconstructionError> {
    auto observation = u64(value.at("observation_id"), "validity.observation_id");
    auto numerator = u64(value.at("policy").at("maximum_harness_overhead").at("numerator"),
                         "validity.policy.maximum_harness_overhead.numerator");
    auto denominator = u64(value.at("policy").at("maximum_harness_overhead").at("denominator"),
                           "validity.policy.maximum_harness_overhead.denominator");
    if (!observation) return std::unexpected{observation.error()};
    if (!numerator) return std::unexpected{numerator.error()};
    if (!denominator) return std::unexpected{denominator.error()};
    if (*denominator == 0U)
        return error(TypedReconstructionErrorCode::typed_invariant, "validity.policy");
    auto maximum = measurement::MaximumHarnessOverhead::create(*numerator, *denominator);
    core::detail::ExpectedErrorCleanup maximum_cleanup{maximum};
    if (!maximum) return error(TypedReconstructionErrorCode::typed_invariant, "validity.policy");
    const auto& timer = value.at("timer_overhead");
    auto estimate = u64(timer.at("estimate"), "validity.timer_overhead.estimate");
    auto threshold_n = u64(timer.at("threshold").at("numerator"),
                           "validity.timer_overhead.threshold.numerator");
    auto threshold_d = u64(timer.at("threshold").at("denominator"),
                           "validity.timer_overhead.threshold.denominator");
    if (!estimate) return std::unexpected{estimate.error()};
    if (!threshold_n) return std::unexpected{threshold_n.error()};
    if (!threshold_d) return std::unexpected{threshold_d.error()};
    if (*threshold_d == 0U)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "validity.timer_overhead.threshold");
    auto timer_threshold = measurement::MaximumHarnessOverhead::create(*threshold_n, *threshold_d);
    core::detail::ExpectedErrorCleanup threshold_cleanup{timer_threshold};
    if (!timer_threshold || timer_threshold->numerator() != maximum->numerator() ||
        timer_threshold->denominator() != maximum->denominator())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "validity.timer_overhead.threshold");
    std::optional<measurement::TimerOverheadFraction> fraction;
    if (!timer.at("fraction").is_null()) {
        auto n = u64(timer.at("fraction").at("numerator_nanoseconds"),
                     "validity.timer_overhead.fraction.numerator_nanoseconds");
        auto d = u64(timer.at("fraction").at("denominator_nanoseconds"),
                     "validity.timer_overhead.fraction.denominator_nanoseconds");
        if (!n) return std::unexpected{n.error()};
        if (!d) return std::unexpected{d.error()};
        if (*d == 0U) return error(TypedReconstructionErrorCode::typed_invariant,
                                  "validity.timer_overhead.fraction");
        fraction.emplace(measurement::TimerOverheadFraction::create(*n, *d));
    }
    const auto& checks_json = value.at("checks");
    if (checks_json.size() != measurement::observation_validity_check_count)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "validity.checks");
    std::array<measurement::ObservationValidityCheckOutcome,
               measurement::observation_validity_check_count> outcomes{};
    for (std::size_t index{}; index < outcomes.size(); ++index) {
        auto id = u64(checks_json[index].at("id"), "validity.checks.id");
        if (!id) return std::unexpected{id.error()};
        if (*id != index) return error(TypedReconstructionErrorCode::inconsistent_projection,
                                       "validity.checks.id");
        auto outcome = closed_token<measurement::ObservationValidityCheckOutcome>(checks_json[index].at("outcome").get<std::string_view>(),
            "validity.checks.outcome", {
                {"pass", measurement::ObservationValidityCheckOutcome::pass},
                {"fail", measurement::ObservationValidityCheckOutcome::fail},
                {"warning", measurement::ObservationValidityCheckOutcome::warning},
                {"unavailable", measurement::ObservationValidityCheckOutcome::unavailable},
                {"not_applicable", measurement::ObservationValidityCheckOutcome::not_applicable}});
        if (!outcome) return std::unexpected{outcome.error()};
        outcomes[index] = *outcome;
    }
    measurement::ObservationValidityChecks checks{
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::kernel_correctness, outcomes[0]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::worker_interval_containment, outcomes[1]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::timer_overhead, outcomes[2]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::placement, outcomes[3]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::migration, outcomes[4]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::major_page_faults, outcomes[5]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::throttling, outcomes[6]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::context_switching, outcomes[7]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::frequency_variation, outcomes[8]),
        measurement::ObservationValidityCheck::create(measurement::ObservationValidityCheckId::thermal_variation, outcomes[9])};
    auto stated = closed_token<measurement::ObservationValidityDisposition>(value.at("disposition").get<std::string_view>(),
        "validity.disposition", {{"valid", measurement::ObservationValidityDisposition::valid},
                                  {"invalid", measurement::ObservationValidityDisposition::invalid}});
    if (!stated) return std::unexpected{stated.error()};
    const bool any_fail = std::ranges::any_of(checks, [](const auto& check) {
        return check.outcome() == measurement::ObservationValidityCheckOutcome::fail;
    });
    if ((*stated == measurement::ObservationValidityDisposition::invalid) != any_fail)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "validity.disposition");
    return measurement::ObservationValidityEvidence::create(
        core::ObservationId{*observation}, *stated, std::move(checks),
        measurement::ObservationValidityPolicy::create(*maximum),
        measurement::TimerOverheadValidityEvidence::create(
            measurement::TimerOverheadEstimate::create(core::Duration::from_base_units(*estimate)),
            *timer_threshold, fraction));
}

auto placement_state(std::string_view token)
    -> std::expected<measurement::WorkerPlacementValidityState, FixedTypedReconstructionError> {
    return closed_token<measurement::WorkerPlacementValidityState>(token, "placements.state", {
        {"qualified", measurement::WorkerPlacementValidityState::qualified},
        {"not_qualified", measurement::WorkerPlacementValidityState::not_qualified},
        {"unavailable", measurement::WorkerPlacementValidityState::unavailable},
        {"not_applicable", measurement::WorkerPlacementValidityState::not_applicable}});
}
auto migration_state(std::string_view token)
    -> std::expected<measurement::WorkerMigrationValidityState, FixedTypedReconstructionError> {
    return closed_token<measurement::WorkerMigrationValidityState>(token, "migrations.state", {
        {"not_migrated", measurement::WorkerMigrationValidityState::not_migrated},
        {"migrated", measurement::WorkerMigrationValidityState::migrated},
        {"unavailable", measurement::WorkerMigrationValidityState::unavailable},
        {"not_applicable", measurement::WorkerMigrationValidityState::not_applicable}});
}

auto phase(std::string_view token)
    -> std::expected<monitoring::MonitoringSamplePhase, FixedTypedReconstructionError> {
    return closed_token<monitoring::MonitoringSamplePhase>(token, "monitoring.raw.phase", {
        {"before_observation", monitoring::MonitoringSamplePhase::before_observation},
        {"during_observation", monitoring::MonitoringSamplePhase::during_observation},
        {"after_observation", monitoring::MonitoringSamplePhase::after_observation}});
}

auto monitor_signal(std::string_view token, ReconstructionPath path)
    -> std::expected<monitoring::MonitoringSignalState, FixedTypedReconstructionError> {
    return closed_token<monitoring::MonitoringSignalState>(token, std::move(path), {
        {"clear", monitoring::MonitoringSignalState::clear},
        {"detected", monitoring::MonitoringSignalState::detected},
        {"unavailable", monitoring::MonitoringSignalState::unavailable},
        {"not_applicable", monitoring::MonitoringSignalState::not_applicable}});
}

auto frequency_kind(std::string_view token)
    -> std::expected<monitoring::FrequencySourceKind, FixedTypedReconstructionError> {
    return closed_token<monitoring::FrequencySourceKind>(token, "monitoring.frequency.source_kind", {
        {"hardware_current", monitoring::FrequencySourceKind::hardware_current},
        {"scaling_current", monitoring::FrequencySourceKind::scaling_current}});
}

auto frequency_domain(const nlohmann::json& value)
    -> std::expected<monitoring::FrequencyDomainId, FixedTypedReconstructionError> {
    std::vector<core::LinuxCpuId> cpus;
    cpus.reserve(value.at("cpus").size());
    for (const auto& item : value.at("cpus")) {
        auto cpu = u64(item, "monitoring.frequency.domain_id.cpus");
        if (!cpu) return std::unexpected{cpu.error()};
        if (*cpu > std::numeric_limits<std::uint32_t>::max())
            return error(TypedReconstructionErrorCode::typed_invariant,
                         "monitoring.frequency.domain_id.cpus");
        cpus.emplace_back(static_cast<std::uint32_t>(*cpu));
    }
    if (cpus.empty() || !std::ranges::is_sorted(cpus) ||
        std::adjacent_find(cpus.begin(), cpus.end()) != cpus.end())
        return error(TypedReconstructionErrorCode::typed_invariant,
                     "monitoring.frequency.domain_id");
    auto domain = monitoring::FrequencyDomainId::create(std::move(cpus));
    core::detail::ExpectedErrorCleanup domain_cleanup{domain};
    if (!domain) return error(TypedReconstructionErrorCode::typed_invariant,
                              "monitoring.frequency.domain_id");
    return std::move(*domain);
}

template <class Payload, class MakePayload>
auto acquisition(const nlohmann::json& value, monitoring::MonitoringChannel expected_channel,
                 std::string_view channel_token, MakePayload make_payload)
    -> std::expected<monitoring::TimestampedMonitoringAcquisition<Payload>,
                     FixedTypedReconstructionError> {
    if (value.at("channel").get<std::string_view>() != channel_token)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.channel");
    if (expected_channel == monitoring::MonitoringChannel::temperature &&
        !core::detail::json_string_equal(value.at("implementation_id"),
                                         "linux-hwmon-k10temp-tctl"))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.temperature.implementation");
    auto sample_phase = phase(value.at("phase").get<std::string_view>());
    auto start = u64(value.at("collection_start"), "monitoring.raw.collection_start");
    auto end = u64(value.at("collection_end"), "monitoring.raw.collection_end");
    auto duration = u64(value.at("collection_duration"), "monitoring.raw.collection_duration");
    if (!sample_phase) return std::unexpected{sample_phase.error()};
    if (!start) return std::unexpected{start.error()};
    if (!end) return std::unexpected{end.error()};
    if (!duration) return std::unexpected{duration.error()};
    if (*end < *start || *duration != *end - *start)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.collection_duration");
    const auto status = value.at("status").get<std::string_view>();
    std::optional<Payload> payload;
    if (!value.at("payload").is_null()) {
        auto made = make_payload(value.at("payload"));
        if (!made) return std::unexpected{made.error()};
        payload.emplace(std::move(*made));
    }
    if ((status == "available") != payload.has_value() ||
        (status != "available" && status != "unavailable"))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.status");
    auto payload_result = payload
        ? monitoring::MonitoringPayloadResult<Payload>::available(std::move(*payload))
        : monitoring::MonitoringPayloadResult<Payload>::unavailable();
    return monitoring::detail::PairwisePreparedCaptureStorageAccess::acquisition(
        expected_channel,
        monitoring::MonitoringImplementationId::create(
            value.at("implementation_id").get<std::string>()),
        *sample_phase, platform_linux::RawMonotonicTimestamp::from_nanoseconds(*start),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(*end),
        std::move(payload_result));
}

auto reconstruct_monitoring(const nlohmann::json& value,
                            std::span<const core::WorkerId> authenticated_workers,
                            std::span<const core::LinuxCpuId> authenticated_cpus,
                            platform_linux::RawMonotonicTimestamp observation_start,
                            platform_linux::RawMonotonicTimestamp observation_end)
    -> std::expected<monitoring::MonitoringCaptureResult, FixedTypedReconstructionError> {
    monitoring::MonitoringCaptureResult result;
    const auto& evidence = value.at("evidence");
    auto page_state = monitor_signal(evidence.at("page_faults_state").get<std::string_view>(), "monitoring.evidence.page_faults_state");
    auto context_state = monitor_signal(evidence.at("context_switches_state").get<std::string_view>(), "monitoring.evidence.context_switches_state");
    auto temperature_state = monitor_signal(evidence.at("temperature_state").get<std::string_view>(), "monitoring.evidence.temperature_state");
    auto frequency_state = monitor_signal(evidence.at("frequency_state").get<std::string_view>(), "monitoring.evidence.frequency_state");
    auto throttling = monitor_signal(evidence.at("throttling_state").get<std::string_view>(), "monitoring.evidence.throttling_state");
    if (!page_state) return std::unexpected{page_state.error()};
    if (!context_state) return std::unexpected{context_state.error()};
    if (!temperature_state) return std::unexpected{temperature_state.error()};
    if (!frequency_state) return std::unexpected{frequency_state.error()};
    if (!throttling) return std::unexpected{throttling.error()};
    if (*throttling != monitoring::MonitoringSignalState::not_applicable)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.evidence.throttling_state");
    result.evidence_.states_ = {*page_state, *context_state, *temperature_state, *frequency_state};
    const auto coherent_presence = [&](std::string_view key,
                                       monitoring::MonitoringSignalState state) {
        return evidence.at(key).is_null()
            ? state == monitoring::MonitoringSignalState::not_applicable ||
                state == monitoring::MonitoringSignalState::unavailable
            : state != monitoring::MonitoringSignalState::not_applicable;
    };
    if (!coherent_presence("page_faults", *page_state) ||
        !coherent_presence("context_switches", *context_state) ||
        !coherent_presence("temperature", *temperature_state) ||
        !coherent_presence("frequency", *frequency_state))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.evidence.channel_presence");
    if (!evidence.at("page_faults").is_null()) {
        const auto& classified = evidence.at("page_faults"); auto state = monitor_signal(classified.at("state").get<std::string_view>(), "monitoring.evidence.page_faults.state"); if (!state || *state != *page_state) return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.evidence.page_faults.state");
        std::vector<monitoring::WorkerPageFaultEvidence> workers;
        workers.reserve(classified.at("workers").size());
        for (const auto& item : classified.at("workers")) { auto worker = u64(item.at("worker_id"), "monitoring.evidence.page_faults.worker_id"); auto worker_state = monitor_signal(item.at("state").get<std::string_view>(), "monitoring.evidence.page_faults.worker.state"); if (!worker || !worker_state || *worker > std::numeric_limits<std::uint32_t>::max()) return error(TypedReconstructionErrorCode::typed_invariant, "monitoring.evidence.page_faults.worker"); std::optional<monitoring::PageFaultCount> minor, major; if (!item.at("minor_delta").is_null()) { auto raw=u64(item.at("minor_delta"), "monitoring.evidence.page_faults.minor_delta"); if(!raw)return std::unexpected{raw.error()}; minor=monitoring::PageFaultCount::from_events(*raw);} if (!item.at("major_delta").is_null()) { auto raw=u64(item.at("major_delta"), "monitoring.evidence.page_faults.major_delta"); if(!raw)return std::unexpected{raw.error()}; major=monitoring::PageFaultCount::from_events(*raw);} workers.push_back(monitoring::detail::PairwisePreparedCaptureStorageAccess::page_worker(core::WorkerId{static_cast<std::uint32_t>(*worker)}, *worker_state, minor, major)); }
        result.evidence_.page_.emplace(monitoring::detail::PairwisePreparedCaptureStorageAccess::page_evidence(*state, std::move(workers)));
    }
    if (!evidence.at("context_switches").is_null()) {
        const auto& classified=evidence.at("context_switches"); auto state=monitor_signal(classified.at("state").get<std::string_view>(), "monitoring.evidence.context_switches.state"); if(!state || *state != *context_state)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.evidence.context_switches.state"); std::vector<monitoring::WorkerContextSwitchEvidence> workers; workers.reserve(classified.at("workers").size());
        for(const auto& item:classified.at("workers")){auto worker=u64(item.at("worker_id"),"monitoring.evidence.context.worker_id");auto ws=monitor_signal(item.at("state").get<std::string_view>(),"monitoring.evidence.context.worker.state");if(!worker||!ws||*worker>std::numeric_limits<std::uint32_t>::max())return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.evidence.context.worker");std::optional<monitoring::ContextSwitchCount> voluntary,involuntary;if(!item.at("voluntary_count").is_null()){auto raw=u64(item.at("voluntary_count"),"monitoring.evidence.context.voluntary");if(!raw)return std::unexpected{raw.error()};voluntary=monitoring::ContextSwitchCount::from_events(*raw);}if(!item.at("involuntary_count").is_null()){auto raw=u64(item.at("involuntary_count"),"monitoring.evidence.context.involuntary");if(!raw)return std::unexpected{raw.error()};involuntary=monitoring::ContextSwitchCount::from_events(*raw);}workers.push_back(monitoring::detail::PairwisePreparedCaptureStorageAccess::context_worker(core::WorkerId{static_cast<std::uint32_t>(*worker)},*ws,voluntary,involuntary));} result.evidence_.context_.emplace(monitoring::detail::PairwisePreparedCaptureStorageAccess::context_evidence(*state,std::move(workers)));
    }
    const auto worker_order_matches = [&](const auto& workers) {
        if (workers.size() != authenticated_workers.size()) return false;
        for (std::size_t index{}; index < workers.size(); ++index)
            if (!(workers[index].worker_id() == authenticated_workers[index])) return false;
        return true;
    };
    if ((result.evidence_.page_ &&
         !worker_order_matches(result.evidence_.page_->workers())) ||
        (result.evidence_.context_ &&
         !worker_order_matches(result.evidence_.context_->workers())))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.evidence.worker_association");
    auto signed_value=[](const nlohmann::json& item,ReconstructionPath path)->std::expected<std::int64_t,FixedTypedReconstructionError>{if(!item.is_string())return error(TypedReconstructionErrorCode::invalid_scalar,std::move(path));const auto& text=item.get_ref<const std::string&>();std::int64_t amount{};auto [end,code]=std::from_chars(text.data(),text.data()+text.size(),amount);if(code!=std::errc{}||end!=text.data()+text.size()||text.empty()||(text.size()!=1U&&text.front()=='0')||(text.size()>1U&&text.front()=='-'&&text[1]=='0'))return error(TypedReconstructionErrorCode::invalid_scalar,std::move(path));return amount;};
    if(!evidence.at("temperature").is_null()){
        const auto& classified=evidence.at("temperature");if(classified.at("sensor_kind").get<std::string_view>()!="cpu_control")return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.evidence.temperature.sensor_kind");auto state=monitor_signal(classified.at("state").get<std::string_view>(),"monitoring.evidence.temperature.state");auto count=u64(classified.at("sample_count"),"monitoring.evidence.temperature.sample_count");auto sample_interval=u64(classified.at("sampling_interval"),"monitoring.evidence.temperature.sampling_interval");auto completeness=closed_token<monitoring::TemperatureCaptureCompleteness>(classified.at("capture_completeness").get<std::string_view>(),"monitoring.evidence.temperature.capture_completeness",{{"complete",monitoring::TemperatureCaptureCompleteness::complete},{"incomplete",monitoring::TemperatureCaptureCompleteness::incomplete}});if(!state||!count||!sample_interval||!completeness||*state!=*temperature_state)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.evidence.temperature");std::optional<monitoring::TemperatureMilliCelsius> minimum,maximum;std::optional<monitoring::TemperatureRangeMilliCelsius> range;if(!classified.at("observed_minimum").is_null()){auto raw=signed_value(classified.at("observed_minimum"),"monitoring.evidence.temperature.minimum");if(!raw)return std::unexpected{raw.error()};minimum=monitoring::TemperatureMilliCelsius::from_millicelsius(*raw);}if(!classified.at("observed_maximum").is_null()){auto raw=signed_value(classified.at("observed_maximum"),"monitoring.evidence.temperature.maximum");if(!raw)return std::unexpected{raw.error()};maximum=monitoring::TemperatureMilliCelsius::from_millicelsius(*raw);}if(!classified.at("observed_range").is_null()){auto raw=u64(classified.at("observed_range"),"monitoring.evidence.temperature.range");if(!raw)return std::unexpected{raw.error()};range=monitoring::TemperatureRangeMilliCelsius::from_millicelsius(*raw);}if((minimum.has_value()!=maximum.has_value())||(minimum&&(*maximum<*minimum||!range||range->value()!=(static_cast<std::uint64_t>(maximum->value())-static_cast<std::uint64_t>(minimum->value()))))||((*count==0U)!=!minimum))return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.evidence.temperature.derived");result.evidence_.temperature_.emplace(monitoring::detail::PairwisePreparedCaptureStorageAccess::temperature_evidence(monitoring::TemperatureSensorKind::cpu_control,*state,minimum,maximum,range,static_cast<std::size_t>(*count),core::Duration::from_base_units(*sample_interval),*completeness));
    }
    if(!evidence.at("frequency").is_null()){
        const auto& classified=evidence.at("frequency");auto state=monitor_signal(classified.at("state").get<std::string_view>(),"monitoring.evidence.frequency.state");auto sample_interval=u64(classified.at("sampling_interval"),"monitoring.evidence.frequency.sampling_interval");auto completeness=closed_token<monitoring::FrequencyCaptureCompleteness>(classified.at("capture_completeness").get<std::string_view>(),"monitoring.evidence.frequency.capture_completeness",{{"complete",monitoring::FrequencyCaptureCompleteness::complete},{"incomplete",monitoring::FrequencyCaptureCompleteness::incomplete}});if(!state||!sample_interval||!completeness||*state!=*frequency_state)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.evidence.frequency");std::vector<monitoring::FrequencyDomainEvidence> domains; domains.reserve(classified.at("domains").size());for(const auto& item:classified.at("domains")){auto domain=frequency_domain(item.at("domain_id"));auto kind=frequency_kind(item.at("source_kind").get<std::string_view>());auto ds=monitor_signal(item.at("state").get<std::string_view>(),"monitoring.evidence.frequency.domain.state");auto count=u64(item.at("sample_count"),"monitoring.evidence.frequency.domain.sample_count");if(!domain||!kind||!ds||!count)return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.evidence.frequency.domain");std::optional<monitoring::FrequencyHz> minimum,maximum;std::optional<monitoring::FrequencyRangeHz> range;std::optional<monitoring::FrequencyVariationRatio> variation;if(!item.at("observed_minimum").is_null()){auto raw=u64(item.at("observed_minimum"),"monitoring.evidence.frequency.minimum");if(!raw)return std::unexpected{raw.error()};minimum=monitoring::FrequencyHz::from_hz(*raw);}if(!item.at("observed_maximum").is_null()){auto raw=u64(item.at("observed_maximum"),"monitoring.evidence.frequency.maximum");if(!raw)return std::unexpected{raw.error()};maximum=monitoring::FrequencyHz::from_hz(*raw);}if(!item.at("observed_range").is_null()){auto raw=u64(item.at("observed_range"),"monitoring.evidence.frequency.range");if(!raw)return std::unexpected{raw.error()};range=monitoring::FrequencyRangeHz::from_hz(*raw);}if(!item.at("observed_variation").is_null()){auto n=u64(item.at("observed_variation").at("numerator"),"monitoring.evidence.frequency.variation.numerator");auto d=u64(item.at("observed_variation").at("denominator"),"monitoring.evidence.frequency.variation.denominator");if(!n||!d)return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.evidence.frequency.variation");if(*d==0U||*n>*d)return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.evidence.frequency.variation");auto made=monitoring::FrequencyVariationRatio::create(*n,*d);core::detail::ExpectedErrorCleanup made_cleanup{made};if(!made)return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.evidence.frequency.variation");variation=*made;}if((minimum.has_value()!=maximum.has_value())||(minimum&&(*maximum<*minimum||!range||range->value()!=maximum->value()-minimum->value()))||((*count==0U)!=!minimum))return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.evidence.frequency.domain.derived");domains.push_back(monitoring::FrequencyDomainEvidence::create(std::move(*domain),*kind,*ds,minimum,maximum,range,variation,static_cast<std::size_t>(*count)));}result.evidence_.frequency_.emplace(monitoring::detail::PairwisePreparedCaptureStorageAccess::frequency_evidence(*state,std::move(domains),core::Duration::from_base_units(*sample_interval),*completeness));
    }
    const auto& raw=value.at("raw");
    auto page_payload=[](const nlohmann::json& item)->std::expected<monitoring::PageFaultSnapshot,FixedTypedReconstructionError>{auto worker=u64(item.at("worker_id"),"monitoring.raw.page.worker_id");auto minor=u64(item.at("minor_faults"),"monitoring.raw.page.minor_faults");auto major=u64(item.at("major_faults"),"monitoring.raw.page.major_faults");if(!worker)return std::unexpected{worker.error()};if(!minor)return std::unexpected{minor.error()};if(!major)return std::unexpected{major.error()};if(*worker>std::numeric_limits<std::uint32_t>::max())return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.raw.page.worker_id");return monitoring::PageFaultSnapshot::create(core::WorkerId{static_cast<std::uint32_t>(*worker)},monitoring::PageFaultCount::from_events(*minor),monitoring::PageFaultCount::from_events(*major));};
    result.raw_.page_before_.reserve(raw.at("page_fault_before").size());
    result.raw_.page_after_.reserve(raw.at("page_fault_after").size());
    result.raw_.context_.reserve(raw.at("context_switches").size());
    result.raw_.temperature_.reserve(raw.at("temperature").size());
    result.raw_.frequency_metadata_.reserve(raw.at("frequency_metadata").size());
    for(const auto& item:raw.at("page_fault_before")){auto made=acquisition<monitoring::PageFaultSnapshot>(item,monitoring::MonitoringChannel::page_faults,"page_faults",page_payload);if(!made)return std::unexpected{made.error()};if(made->phase()!=monitoring::MonitoringSamplePhase::before_observation)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.page_fault_before.phase");result.raw_.page_before_.push_back(std::move(*made));}
    for(const auto& item:raw.at("page_fault_after")){auto made=acquisition<monitoring::PageFaultSnapshot>(item,monitoring::MonitoringChannel::page_faults,"page_faults",page_payload);if(!made)return std::unexpected{made.error()};if(made->phase()!=monitoring::MonitoringSamplePhase::after_observation)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.page_fault_after.phase");result.raw_.page_after_.push_back(std::move(*made));}
    const auto page_order_matches = [&](const auto& samples) {
        if (samples.empty()) return true;
        if (samples.size() != authenticated_workers.size()) return false;
        for (std::size_t index{}; index < samples.size(); ++index)
            if (samples[index].payload() &&
                !(samples[index].payload()->worker_id() == authenticated_workers[index]))
                return false;
        return true;
    };
    if (!page_order_matches(result.raw_.page_before_) ||
        !page_order_matches(result.raw_.page_after_) ||
        result.raw_.page_before_.size() != result.raw_.page_after_.size())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.page_faults.worker_association");
    auto context_payload=[](const nlohmann::json& item)->std::expected<monitoring::ContextSwitchEventBatch,FixedTypedReconstructionError>{auto worker=u64(item.at("worker_id"),"monitoring.raw.context.worker_id");if(!worker)return std::unexpected{worker.error()};if(*worker>std::numeric_limits<std::uint32_t>::max())return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.raw.context.worker_id");auto completeness=closed_token<monitoring::ContextSwitchCaptureCompleteness>(item.at("completeness").get<std::string_view>(),"monitoring.raw.context.completeness",{{"complete",monitoring::ContextSwitchCaptureCompleteness::complete},{"incomplete",monitoring::ContextSwitchCaptureCompleteness::incomplete}});if(!completeness)return std::unexpected{completeness.error()};std::vector<monitoring::ContextSwitchEvent> events; events.reserve(item.at("events").size());for(const auto& event:item.at("events")){auto event_worker=u64(event.at("worker_id"),"monitoring.raw.context.event.worker_id");auto timestamp=u64(event.at("timestamp"),"monitoring.raw.context.event.timestamp");auto kind=closed_token<monitoring::ContextSwitchKind>(event.at("kind").get<std::string_view>(),"monitoring.raw.context.event.kind",{{"voluntary",monitoring::ContextSwitchKind::voluntary},{"involuntary",monitoring::ContextSwitchKind::involuntary}});if(!event_worker||!timestamp||!kind||*event_worker!=*worker)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.context.event");events.push_back(monitoring::ContextSwitchEvent::create(core::WorkerId{static_cast<std::uint32_t>(*worker)},platform_linux::RawMonotonicTimestamp::from_nanoseconds(*timestamp),*kind));}return monitoring::ContextSwitchEventBatch::create(core::WorkerId{static_cast<std::uint32_t>(*worker)},*completeness,std::move(events));};
    for(const auto& item:raw.at("context_switches")){auto made=acquisition<monitoring::ContextSwitchEventBatch>(item,monitoring::MonitoringChannel::context_switches,"context_switches",context_payload);if(!made)return std::unexpected{made.error()};if(made->phase()!=monitoring::MonitoringSamplePhase::during_observation)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.context.phase");/* MON-003: a drain can retain earlier, nonmonotonic events in ring order. */if(made->payload())for(const auto& event:made->payload()->events())if(made->collection_end()<event.timestamp())return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.context.event.timestamp");result.raw_.context_.push_back(std::move(*made));} result.raw_.context_count_=result.raw_.context_.size();
    if (!result.raw_.context_.empty() && !result.evidence_.context_)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.evidence.context_switches.presence");
    if (!result.raw_.context_.empty()) {
        if (authenticated_workers.empty() ||
            result.raw_.context_.size() % authenticated_workers.size() != 0U)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.raw.context.worker_cardinality");
        const auto stride = result.raw_.context_.size() / authenticated_workers.size();
        for (std::size_t worker{}; worker < authenticated_workers.size(); ++worker)
            for (std::size_t ordinal{}; ordinal < stride; ++ordinal) {
                const auto& sample = result.raw_.context_[worker * stride + ordinal];
                if (sample.payload() &&
                    !(sample.payload()->worker_id() == authenticated_workers[worker]))
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "monitoring.raw.context.worker_association");
            }
    }
    auto temperature_payload=[](const nlohmann::json& item)->std::expected<monitoring::TemperatureSnapshot,FixedTypedReconstructionError>{if(item.at("sensor_kind").get<std::string_view>()!="cpu_control")return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.raw.temperature.sensor_kind");const auto& text=item.at("temperature").get_ref<const std::string&>();std::int64_t amount{};auto [end,code]=std::from_chars(text.data(),text.data()+text.size(),amount);if(code!=std::errc{}||end!=text.data()+text.size())return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.raw.temperature.temperature");return monitoring::TemperatureSnapshot::create(monitoring::TemperatureSensorKind::cpu_control,monitoring::TemperatureMilliCelsius::from_millicelsius(amount));};
    for(const auto& item:raw.at("temperature")){auto made=acquisition<monitoring::TemperatureSnapshot>(item,monitoring::MonitoringChannel::temperature,"temperature",temperature_payload);if(!made)return std::unexpected{made.error()};if(made->phase()!=monitoring::MonitoringSamplePhase::during_observation)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.temperature.phase");result.raw_.temperature_.push_back(std::move(*made));}
    if ((*page_state == monitoring::MonitoringSignalState::not_applicable &&
         (!result.raw_.page_before_.empty() || !result.raw_.page_after_.empty())) ||
        (*context_state == monitoring::MonitoringSignalState::not_applicable &&
         !result.raw_.context_.empty()) ||
        (*temperature_state == monitoring::MonitoringSignalState::not_applicable &&
         !result.raw_.temperature_.empty()))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.disabled_channel");
    auto frequency_payload=[](const nlohmann::json& item)->std::expected<monitoring::FrequencySnapshot,FixedTypedReconstructionError>{auto domain=frequency_domain(item.at("domain_id"));auto kind=frequency_kind(item.at("source_kind").get<std::string_view>());auto hz=u64(item.at("frequency"),"monitoring.raw.frequency.frequency");if(!domain)return std::unexpected{domain.error()};if(!kind)return std::unexpected{kind.error()};if(!hz)return std::unexpected{hz.error()};return monitoring::FrequencySnapshot::create(std::move(*domain),*kind,monitoring::FrequencyHz::from_hz(*hz));};
    result.raw_.frequency_storage_.reserve(raw.at("frequency").size());
    result.raw_.frequency_views_.reserve(raw.at("frequency").size());
    for(const auto& group:raw.at("frequency")){auto domain=frequency_domain(group.at("domain_id"));if(!domain)return std::unexpected{domain.error()};result.raw_.frequency_storage_.emplace_back();auto& storage=result.raw_.frequency_storage_.back();storage.reserve(group.at("acquisitions").size());for(const auto& item:group.at("acquisitions")){auto made=acquisition<monitoring::FrequencySnapshot>(item,monitoring::MonitoringChannel::frequency,"frequency",frequency_payload);if(!made)return std::unexpected{made.error()};if(made->phase()!=monitoring::MonitoringSamplePhase::during_observation)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.frequency.phase");if(made->payload()&&(!(made->payload()->domain_id()==*domain)))return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.frequency.domain_id");storage.push_back(std::move(*made));}result.raw_.frequency_views_.push_back(monitoring::FrequencyDomainAcquisitions{std::move(*domain),std::span<const monitoring::FrequencyAcquisition>{storage}});}
    for(const auto& item:raw.at("frequency_metadata")){auto domain=frequency_domain(item.at("domain_id"));auto kind=frequency_kind(item.at("source_kind").get<std::string_view>());auto minimum=u64(item.at("scaling_minimum"),"monitoring.raw.frequency_metadata.minimum");auto maximum=u64(item.at("scaling_maximum"),"monitoring.raw.frequency_metadata.maximum");if(!domain)return std::unexpected{domain.error()};if(!kind)return std::unexpected{kind.error()};if(!minimum)return std::unexpected{minimum.error()};if(!maximum)return std::unexpected{maximum.error()};if(*minimum>*maximum)return error(TypedReconstructionErrorCode::typed_invariant,"monitoring.raw.frequency_metadata");result.raw_.frequency_metadata_.push_back(monitoring::detail::PairwisePreparedCaptureStorageAccess::frequency_metadata(std::move(*domain),*kind,monitoring::FrequencyDriverId::create(item.at("scaling_driver").get<std::string>()),monitoring::FrequencyGovernorId::create(item.at("scaling_governor").get<std::string>()),monitoring::FrequencyHz::from_hz(*minimum),monitoring::FrequencyHz::from_hz(*maximum)));}
    if(result.raw_.frequency_views_.size()!=result.raw_.frequency_metadata_.size())return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.raw.frequency_metadata.cardinality");
    std::vector<core::LinuxCpuId> frequency_cpus;
    std::size_t frequency_memberships{};
    for (const auto& domain : result.raw_.frequency_views_) {
        const auto sum = add_wire_json_count(frequency_memberships, domain.domain_id.cpus().size());
        if (!sum) return error(TypedReconstructionErrorCode::typed_invariant, "monitoring.raw.frequency.domain_partition");
        frequency_memberships = *sum;
    }
    frequency_cpus.reserve(frequency_memberships);
    std::optional<core::LinuxCpuId> previous_domain_first;
    for (std::size_t index{}; index < result.raw_.frequency_views_.size(); ++index) {
        const auto& domain = result.raw_.frequency_views_[index].domain_id;
        if (!(domain == result.raw_.frequency_metadata_[index].domain_id()))
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.raw.frequency_metadata.domain_id");
        if (previous_domain_first && domain.cpus().front() <= *previous_domain_first)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.raw.frequency.domain_partition");
        previous_domain_first = domain.cpus().front();
        for (const auto cpu : domain.cpus()) {
            if (std::ranges::find(authenticated_cpus, cpu) == authenticated_cpus.end() ||
                std::ranges::find(frequency_cpus, cpu) != frequency_cpus.end())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "monitoring.raw.frequency.domain_partition");
            frequency_cpus.push_back(cpu);
        }
    }
    // This legacy interface authenticates the present universe and worker IDs,
    // not a worker-to-CPU map. The request-owning prepared path additionally
    // checks coverage of the actual selected worker CPUs.
    if (result.evidence_.frequency_ &&
        (result.raw_.frequency_views_.empty() ||
         result.raw_.frequency_views_.size() >
             std::min(authenticated_workers.size(), authenticated_cpus.size()) ||
         frequency_cpus.size() > authenticated_cpus.size()))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.raw.frequency.authenticated_cpu_partition");
    if (result.evidence_.frequency_) {
        const auto classified = result.evidence_.frequency_->domains();
        if (classified.size() != result.raw_.frequency_views_.size())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.frequency.domain_cardinality");
        for (std::size_t index{}; index < classified.size(); ++index) {
            if (!(classified[index].domain_id() == result.raw_.frequency_views_[index].domain_id) ||
                classified[index].source_kind() !=
                    result.raw_.frequency_metadata_[index].source_kind())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "monitoring.evidence.frequency.domain_association");
            const auto expected_id = classified[index].source_kind() ==
                    monitoring::FrequencySourceKind::hardware_current
                ? "linux-cpufreq-cpuinfo-cur-freq" : "linux-cpufreq-scaling-cur-freq";
            for (const auto& sample : result.raw_.frequency_views_[index].acquisitions)
                if (sample.implementation_id().value() != expected_id ||
                    (sample.payload() &&
                     sample.payload()->source_kind() != classified[index].source_kind()))
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "monitoring.raw.frequency.source_kind");
        }
    } else if (!result.raw_.frequency_views_.empty()) {
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "monitoring.evidence.frequency.presence");
    }
    if (result.evidence_.page_) {
        if (result.raw_.page_before_.size() != authenticated_workers.size() ||
            result.raw_.page_after_.size() != authenticated_workers.size())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.page_faults.raw_cardinality");
        std::vector<monitoring::PageFaultAcquisitionPair> pairs;
        pairs.reserve(authenticated_workers.size());
        for (std::size_t index{}; index < authenticated_workers.size(); ++index)
            pairs.emplace_back(authenticated_workers[index], result.raw_.page_before_[index],
                               result.raw_.page_after_[index]);
        ClassifierWorkerSink<monitoring::PageFaultCount> sink{pairs.size()};
        auto classified = monitoring::detail::classify_page_faults_bounded(
            observation_start, observation_end, pairs, {&sink, decltype(sink)::append});
        const auto retained = result.evidence_.page_->workers();
        bool same = classified && *classified == result.evidence_.page_->state() &&
                    !sink.overflow && sink.count == retained.size();
        for (std::size_t i{}; same && i < sink.count; ++i) {
            const auto& item = sink.values[i];
            same = item.worker == retained[i].worker_id().value() &&
                   item.state == retained[i].state() && item.first == retained[i].minor_delta() &&
                   item.second == retained[i].major_delta();
        }
        if (!same)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.page_faults.raw_projection");
    }
    if (result.evidence_.context_) {
        if (result.raw_.context_.size() % authenticated_workers.size() != 0U)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.context_switches.raw_projection");
        const auto stride = result.raw_.context_.size() / authenticated_workers.size();
        std::vector<monitoring::ContextSwitchWorkerAcquisitionView> views;
        views.reserve(authenticated_workers.size());
        for (std::size_t index{}; index < authenticated_workers.size(); ++index)
            views.emplace_back(authenticated_workers[index],
                monitoring::ContextSwitchEventSource::implementation_id(), observation_start,
                observation_end, monitoring::ContextSwitchCaptureCompleteness::complete,
                std::span<const monitoring::TimestampedMonitoringAcquisition<
                    monitoring::ContextSwitchEventBatch>>{result.raw_.context_}
                    .subspan(index * stride, stride));
        ClassifierWorkerSink<monitoring::ContextSwitchCount> sink{views.size()};
        auto classified = monitoring::detail::classify_context_switches_bounded(
            observation_start, observation_end, authenticated_workers, views,
            {&sink, decltype(sink)::append});
        const auto retained = result.evidence_.context_->workers();
        bool same = classified && *classified == result.evidence_.context_->state() &&
                    !sink.overflow && sink.count == retained.size();
        for (std::size_t i{}; same && i < sink.count; ++i) {
            const auto& item = sink.values[i];
            same = item.worker == retained[i].worker_id().value() &&
                   item.state == retained[i].state() && item.first == retained[i].voluntary_count() &&
                   item.second == retained[i].involuntary_count();
        }
        if (!same)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.context_switches.raw_projection");
    }
    const auto inside_points = [&](auto acquisitions)
        -> std::expected<decltype(acquisitions), FixedTypedReconstructionError> {
        std::size_t first{}, count{};
        std::optional<platform_linux::RawMonotonicTimestamp> previous;
        for (std::size_t index{}; index < acquisitions.size(); ++index) {
            const auto& item = acquisitions[index];
            if (previous && item.collection_start() < *previous)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "monitoring.raw.points.ordering");
            previous = item.collection_start();
            if (item.collection_start() >= observation_start &&
                item.collection_end() <= observation_end) {
                if (count == 0U) first = index;
                else if (index - first != count)
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "monitoring.raw.points.ordering");
                ++count;
            }
        }
        return acquisitions.subspan(first, count);
    };
    if (result.evidence_.temperature_) {
        auto selected = inside_points(std::span<const monitoring::MonitoringRawCaptureEvidence::TemperatureAcquisition>{
            result.raw_.temperature_});
        if (!selected) return std::unexpected{selected.error()};
        std::vector<const monitoring::TimestampedMonitoringAcquisition<
            monitoring::TemperatureSnapshot>*> available(result.raw_.temperature_.size());
        auto classified = monitoring::detail::classify_temperature_bounded(
            observation_start, observation_end, result.evidence_.temperature_->sampling_interval(),
            result.evidence_.temperature_->capture_completeness(), *selected, available);
        const auto& retained = *result.evidence_.temperature_;
        if (!classified || classified->state != retained.state() ||
            classified->minimum != retained.observed_minimum() ||
            classified->maximum != retained.observed_maximum() ||
            classified->range != retained.observed_range() ||
            classified->count != retained.sample_count())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.temperature.raw_projection");
    }
    if (result.evidence_.frequency_) {
        std::vector<monitoring::FrequencyDomainAcquisitions> classifier_domains;
        classifier_domains.reserve(result.raw_.frequency_views_.size());
        for (const auto& domain : result.raw_.frequency_views_) {
            auto selected = inside_points(domain.acquisitions);
            if (!selected) return std::unexpected{selected.error()};
            classifier_domains.push_back({domain.domain_id, *selected});
        }
        std::size_t maximum_samples{};
        for (const auto& domain : classifier_domains) {
            maximum_samples = std::max(maximum_samples, domain.acquisitions.size());
            std::size_t available_count{};
            std::uint64_t maximum_hz{};
            for (const auto& sample : domain.acquisitions) {
                if (sample.payload()) {
                    ++available_count;
                    maximum_hz = std::max(maximum_hz, sample.payload()->frequency().value());
                }
            }
            // The noexcept kernel constructs a ratio for >=2 samples. Avoid its
            // allocating zero-denominator error branch, preserving wrapper failure.
            if (available_count >= 2U && maximum_hz == 0U)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "monitoring.evidence.frequency.raw_projection");
        }
        std::vector<const monitoring::FrequencyAcquisition*> available(maximum_samples);
        std::vector<std::optional<monitoring::detail::FrequencyDomainClassification>>
            classified_domains(result.raw_.frequency_views_.size());
        struct Sink final {
            std::size_t capacity{}, count{};
            bool overflow{};
            static auto append(void* context,
                               const monitoring::detail::FrequencyDomainClassification&) noexcept
                -> bool {
                auto& self = *static_cast<Sink*>(context);
                if (self.count == self.capacity) self.overflow = true;
                else ++self.count;
                return true;
            }
        } sink{classified_domains.size()};
        auto classified = monitoring::detail::classify_frequency_bounded(
            observation_start, observation_end, result.evidence_.frequency_->sampling_interval(),
            result.evidence_.frequency_->capture_completeness(), classifier_domains,
            available, classified_domains, {&sink, Sink::append});
        const auto retained = result.evidence_.frequency_->domains();
        bool same = classified && *classified == result.evidence_.frequency_->state() &&
                    !sink.overflow && sink.count == retained.size();
        for (std::size_t i{}; same && i < sink.count; ++i) {
            const auto& item = *classified_domains[i];
            same = item.domain && *item.domain == retained[i].domain_id() &&
                   item.kind == retained[i].source_kind() && item.state == retained[i].state() &&
                   item.minimum == retained[i].observed_minimum() &&
                   item.maximum == retained[i].observed_maximum() &&
                   item.range == retained[i].observed_range() &&
                   item.variation == retained[i].observed_variation() &&
                   item.count == retained[i].sample_count();
        }
        if (!same)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "monitoring.evidence.frequency.raw_projection");
    }
    const auto& activity=value.at("activity"); auto set=[&](const char* key,std::uint64_t& destination)->std::expected<void,FixedTypedReconstructionError>{auto raw_value=u64(activity.at(key),ReconstructionPath{"monitoring.activity."}+key);if(!raw_value)return std::unexpected{raw_value.error()};destination=*raw_value;return{};}; if(!set("scheduled_rounds",result.activity_.scheduled_)||!set("completed_rounds",result.activity_.completed_)||!set("missed_rounds",result.activity_.missed_)||!set("service_wakeups",result.activity_.wakeups_)||!set("context_drains",result.activity_.drains_)||!set("temperature_attempts",result.activity_.temperature_)||!set("frequency_attempts",result.activity_.frequency_))return error(TypedReconstructionErrorCode::invalid_scalar,"monitoring.activity"); if(result.activity_.completed_>result.activity_.scheduled_||result.activity_.missed_>result.activity_.scheduled_-result.activity_.completed_)return error(TypedReconstructionErrorCode::inconsistent_projection,"monitoring.activity");
    if ((!result.evidence_.page_ && (!result.raw_.page_before_.empty() || !result.raw_.page_after_.empty())) ||
        (!result.evidence_.context_ && !result.raw_.context_.empty()) ||
        (!result.evidence_.temperature_ && !result.raw_.temperature_.empty()) ||
        (!result.evidence_.frequency_ && (!result.raw_.frequency_views_.empty() || !result.raw_.frequency_metadata_.empty())))
        return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.raw.disabled_channel");
    if ((!result.evidence_.context_ && result.activity_.drains_ != 0U) ||
        (!result.evidence_.temperature_ && result.activity_.temperature_ != 0U) ||
        (!result.evidence_.frequency_ && result.activity_.frequency_ != 0U))
        return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.activity");
    // Reset drains can precede retained context acquisitions. Point attempts
    // each append one acquisition, including an unavailable payload.
    if (result.raw_.context_.size() > result.activity_.drains_ ||
        result.raw_.temperature_.size() != result.activity_.temperature_)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.activity.raw_counts");
    std::uint64_t frequency_attempts{};
    std::optional<std::size_t> frequency_rounds;
    for (const auto& domain : result.raw_.frequency_views_) {
        const auto size = domain.acquisitions.size();
        if ((frequency_rounds && *frequency_rounds != size) || size > UINT64_MAX - frequency_attempts)
            return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.activity.raw_counts");
        frequency_rounds = size;
        frequency_attempts += size;
    }
    if (frequency_attempts != result.activity_.frequency_)
        return error(TypedReconstructionErrorCode::inconsistent_projection, "monitoring.activity.raw_counts");
    return result;
}

auto reconstruct_measurement(const nlohmann::json& value)
    -> std::expected<measurement::MeasurementObservation, FixedTypedReconstructionError> {
    auto observation = u64(value.at("observation_id"), "measurement.observation_id");
    auto experiment = u64(value.at("experiment_id"), "measurement.experiment_id");
    auto block = u64(value.at("block_id"), "measurement.block_id");
    auto impl = implementation(value.at("implementation_id").get<std::string_view>(),
                               "measurement.implementation_id");
    auto enclosing = interval(value.at("enclosing_interval"), "measurement.enclosing_interval");
    if (!observation) return std::unexpected{observation.error()};
    if (!experiment) return std::unexpected{experiment.error()};
    if (!block) return std::unexpected{block.error()};
    if (!impl) return std::unexpected{impl.error()};
    if (!enclosing) return std::unexpected{enclosing.error()};
    const auto kind = value.at("kind").get<std::string_view>();
    if (kind == "bandwidth") {
        auto bytes = u64(value.at("configuration").at("working_set_per_worker"), "measurement.configuration.working_set_per_worker");
        auto passes = u64(value.at("pass_count"), "measurement.pass_count");
        auto aggregate = u64(value.at("aggregate_work"), "measurement.aggregate_work");
        if (!bytes) return std::unexpected{bytes.error()};
        if (!passes) return std::unexpected{passes.error()};
        if (!aggregate) return std::unexpected{aggregate.error()};
        if (*bytes == 0U || *bytes % 64U != 0U || *passes == 0U)
            return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        auto config = kernel::SequentialReadBandwidthConfig::create(core::ByteCount::from_base_units(*bytes));
        core::detail::ExpectedErrorCleanup config_cleanup{config};
        auto pass = kernel::SequentialReadBandwidthPassCount::create(*passes);
        core::detail::ExpectedErrorCleanup pass_cleanup{pass};
        if (!config || !pass) return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        std::vector<measurement::SequentialReadBandwidthWorkerObservation> workers;
        workers.reserve(value.at("workers").size());
        for (const auto& worker : value.at("workers")) {
            auto id = u64(worker.at("worker_id"), "measurement.workers.worker_id");
            auto measured = interval(worker.at("interval"), "measurement.workers.interval");
            auto work = u64(worker.at("work").at("logical_bytes_read"), "measurement.workers.work.logical_bytes_read");
            auto expected = u64(worker.at("verification").at("expected"), "measurement.workers.verification.expected");
            auto actual = u64(worker.at("verification").at("actual"), "measurement.workers.verification.actual");
            if (!id) return std::unexpected{id.error()};
            if (!measured) return std::unexpected{measured.error()};
            if (!work) return std::unexpected{work.error()};
            if (!expected) return std::unexpected{expected.error()};
            if (!actual) return std::unexpected{actual.error()};
            if (*id > std::numeric_limits<std::uint32_t>::max()) return error(TypedReconstructionErrorCode::typed_invariant, "measurement.workers.worker_id");
            if (*bytes > UINT64_MAX / *passes)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "measurement.workers.work.logical_bytes_read");
            auto expected_work = config->working_set_per_worker().checked_multiply(*passes);
            if (!expected_work || expected_work->value() != *work)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "measurement.workers.work.logical_bytes_read");
            if (worker.at("verification").at("matched").get<bool>() != (*expected == *actual))
                return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.workers.verification.matched");
            workers.push_back(measurement::SequentialReadBandwidthWorkerObservation::create(
                core::WorkerId{static_cast<std::uint32_t>(*id)}, std::move(*measured), kernel::detail::KernelLeafAccess::bandwidth_work(core::ByteCount::from_base_units(*work)),
                kernel::detail::KernelLeafAccess::bandwidth_verification(*expected, *actual)));
        }
        if (*impl != kernel::KernelImplementationId::scalar_u64_sequential_v1 || workers.empty())
            return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        std::uint64_t total{};
        for (const auto& worker : workers) {
            const auto amount = worker.work().logical_bytes_read().value();
            if (amount > UINT64_MAX - total)
                return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
            total += amount;
        }
        auto made = measurement::SequentialReadBandwidthObservation::create(core::ObservationId{*observation}, core::ExperimentId{*experiment}, kernel::DependentLoadLatencyBlockId{*block}, *impl, *config, *pass, std::move(workers), std::move(*enclosing));
        core::detail::ExpectedErrorCleanup made_cleanup{made};
        if (!made) return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        if (made->aggregate_work().value() != *aggregate) return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.aggregate_work");
        return measurement::MeasurementObservation{std::move(*made)};
    }
    if (kind == "latency") {
        auto bytes = u64(value.at("configuration").at("working_set"), "measurement.configuration.working_set");
        if (!bytes) return std::unexpected{bytes.error()};
        const auto& seed = value.at("configuration").at("pointer_cycle_seed");
        std::optional<kernel::PointerCycleSeedIntent> seed_value;
        const auto mode = seed.at("mode").get<std::string_view>();
        if (mode == "explicit_value" && !seed.at("value").is_null()) { auto raw = u64(seed.at("value"), "measurement.configuration.pointer_cycle_seed.value"); if (!raw) return std::unexpected{raw.error()}; seed_value = kernel::PointerCycleSeedIntent::explicit_value(*raw); }
        else if (mode == "derive_per_experiment" && seed.at("value").is_null()) seed_value = kernel::PointerCycleSeedIntent::derive_per_experiment();
        else return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.configuration.pointer_cycle_seed");
        if (*bytes < 128U || *bytes % 64U != 0U)
            return error(TypedReconstructionErrorCode::typed_invariant, "measurement.configuration");
        auto config = kernel::DependentLoadLatencyConfig::create(core::ByteCount::from_base_units(*bytes), *seed_value);
        core::detail::ExpectedErrorCleanup config_cleanup{config};
        if (!config) return error(TypedReconstructionErrorCode::typed_invariant, "measurement.configuration");
        std::vector<measurement::DependentLoadLatencyWorkerObservation> workers;
        workers.reserve(value.at("workers").size());
        for (const auto& worker : value.at("workers")) {
            auto id = u64(worker.at("worker_id"), "measurement.workers.worker_id"); auto measured = interval(worker.at("interval"), "measurement.workers.interval"); auto start = u64(worker.at("schedule").at("start_index"), "measurement.workers.schedule.start_index"); auto steps = u64(worker.at("schedule").at("step_count"), "measurement.workers.schedule.step_count"); auto completed = u64(worker.at("completed_work"), "measurement.workers.completed_work"); auto expected = u64(worker.at("verification").at("expected"), "measurement.workers.verification.expected"); auto actual = u64(worker.at("verification").at("actual"), "measurement.workers.verification.actual");
            if (!id) return std::unexpected{id.error()};
            if (!measured) return std::unexpected{measured.error()};
            if (!start) return std::unexpected{start.error()};
            if (!steps) return std::unexpected{steps.error()};
            if (!completed) return std::unexpected{completed.error()};
            if (!expected) return std::unexpected{expected.error()};
            if (!actual) return std::unexpected{actual.error()};
            if (*id > std::numeric_limits<std::uint32_t>::max()) return error(TypedReconstructionErrorCode::typed_invariant, "measurement.workers.worker_id");
            if (*steps == 0U) return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.workers.completed_work");
            auto count = kernel::DependentLoadLatencyStepCount::create(*steps); core::detail::ExpectedErrorCleanup count_cleanup{count}; if (!count || *completed != *steps) return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.workers.completed_work");
            if (worker.at("verification").at("matched").get<bool>() != (*expected == *actual)) return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.workers.verification.matched");
            workers.push_back(measurement::DependentLoadLatencyWorkerObservation::create(core::WorkerId{static_cast<std::uint32_t>(*id)}, std::move(*measured), kernel::detail::KernelLeafAccess::schedule(static_cast<std::size_t>(*start), *count), kernel::detail::KernelLeafAccess::latency_verification(static_cast<std::size_t>(*expected), static_cast<std::size_t>(*actual))));
        }
        auto aggregate = u64(value.at("aggregate_work"), "measurement.aggregate_work"); if (!aggregate) return std::unexpected{aggregate.error()};
        if (*impl != kernel::KernelImplementationId::scalar_u64_random_cycle_v1 || workers.empty())
            return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        std::uint64_t total{};
        for (const auto& worker : workers) {
            const auto amount = worker.completed_work().value();
            if (amount > UINT64_MAX - total)
                return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
            total += amount;
        }
        auto made = measurement::DependentLoadLatencyObservation::create(core::ObservationId{*observation}, core::ExperimentId{*experiment}, kernel::DependentLoadLatencyBlockId{*block}, *impl, *config, std::move(workers), std::move(*enclosing));
        core::detail::ExpectedErrorCleanup made_cleanup{made};
        if (!made) return error(TypedReconstructionErrorCode::typed_invariant, "measurement");
        if (made->aggregate_work().value() != *aggregate) return error(TypedReconstructionErrorCode::inconsistent_projection, "measurement.aggregate_work");
        return measurement::MeasurementObservation{std::move(*made)};
    }
    return error(TypedReconstructionErrorCode::invalid_scalar, "measurement.kind");
}

} // namespace

auto fixed_reconstruct_block_plan_component(const nlohmann::json& plan_json)
    -> std::expected<measurement::MeasurementBlockPlan, FixedTypedReconstructionError> {
    auto block_id = u64(plan_json.at("block_id"), "blocks.plan.block_id");
    if (!block_id) return std::unexpected{block_id.error()};
    const auto& order = plan_json.at("comparison_order");
    auto strategy = closed_token<measurement::ComparisonOrderStrategy>(order.at("strategy").get<std::string_view>(), "blocks.plan.comparison_order.strategy", {
        {"randomized_abba", measurement::ComparisonOrderStrategy::randomized_abba}, {"randomized_baab", measurement::ComparisonOrderStrategy::randomized_baab}, {"randomized_balanced", measurement::ComparisonOrderStrategy::randomized_balanced}});
    auto seed = u64(order.at("seed"), "blocks.plan.comparison_order.seed"); if (!strategy) return std::unexpected{strategy.error()}; if (!seed) return std::unexpected{seed.error()};
    auto plan = measurement::MeasurementBlockPlan::create(kernel::DependentLoadLatencyBlockId{*block_id}, measurement::ComparisonOrderPolicy::create(*strategy, *seed), measurement::BlockPreparationPlan::create(plan_json.at("preparation").at("calibration_required").get<bool>(), plan_json.at("preparation").at("warmup_required").get<bool>()));
    if (plan_json.at("observations").size() != plan.observations().size()) return error(TypedReconstructionErrorCode::inconsistent_projection, "blocks.plan.observations");
    for (std::size_t index{}; index < plan.observations().size(); ++index) {
        const auto& stated = plan_json.at("observations")[index]; auto stated_block = u64(stated.at("block_id"), "blocks.plan.observations.block_id"); auto stated_index = u64(stated.at("slot_index"), "blocks.plan.observations.slot_index"); if (!stated_block) return std::unexpected{stated_block.error()}; if (!stated_index) return std::unexpected{stated_index.error()};
        const auto variant = stated.at("variant_slot").get<std::string_view>(); const auto expected_variant = plan.observations()[index].comparison_variant() == measurement::ComparisonVariantSlot::first ? "first" : "second";
        if (*stated_block != *block_id || *stated_index != index || variant != expected_variant) return error(TypedReconstructionErrorCode::inconsistent_projection, "blocks.plan.observations");
    }
    return plan;
}

auto fixed_reconstruct_factual_component(const nlohmann::json& observation_json,
                                   const measurement::MeasurementBlockPlan& plan,
                                   std::size_t expected_slot,
                                   std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FactualObservationEvidence, FixedTypedReconstructionError> {
    auto observation_id = u64(observation_json.at("observation_id"), "blocks.observations.observation_id"); if (!observation_id) return std::unexpected{observation_id.error()};
    const auto& slot_json = observation_json.at("slot"); auto slot_index = u64(slot_json.at("slot_index"), "blocks.observations.slot.slot_index"); if (!slot_index) return std::unexpected{slot_index.error()}; if (*slot_index >= plan.observations().size()) return error(TypedReconstructionErrorCode::inconsistent_projection, "blocks.observations.slot");
    const auto& slot = plan.observations()[static_cast<std::size_t>(*slot_index)]; auto measurement = reconstruct_measurement(observation_json.at("measurement")); auto validity = reconstruct_validity(observation_json.at("validity")); if (!measurement) return std::unexpected{measurement.error()}; if (!validity) return std::unexpected{validity.error()};
    auto slot_block = u64(slot_json.at("block_id"), "blocks.observations.slot.block_id");
    if (!slot_block) return std::unexpected{slot_block.error()};
    const auto expected_variant = slot.comparison_variant() == measurement::ComparisonVariantSlot::first ? "first" : "second";
    if (*slot_block != slot.block_id().value() ||
        slot_json.at("variant_slot").get<std::string_view>() != expected_variant ||
        *slot_index != expected_slot)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "blocks.observations.slot");
    if (!(validity->observation_id() == core::ObservationId{*observation_id}) || std::visit([&](const auto& item){ return !(item.observation_id() == core::ObservationId{*observation_id}) || !(item.block_id() == plan.block_id()); }, *measurement)) return error(TypedReconstructionErrorCode::inconsistent_projection, "blocks.observations.identity");
    std::vector<measurement::WorkerPlacementValidityEvidence> placements; placements.reserve(observation_json.at("placements").size()); for (const auto& item : observation_json.at("placements")) { auto worker = u64(item.at("worker_id"), "placements.worker_id"); auto state = placement_state(item.at("state").get<std::string_view>()); if (!worker) return std::unexpected{worker.error()}; if (!state) return std::unexpected{state.error()}; if (*worker > std::numeric_limits<std::uint32_t>::max()) return error(TypedReconstructionErrorCode::typed_invariant, "placements.worker_id"); placements.push_back(measurement::WorkerPlacementValidityEvidence::create(core::WorkerId{static_cast<std::uint32_t>(*worker)}, *state)); }
    std::vector<measurement::WorkerMigrationValidityEvidence> migrations; migrations.reserve(observation_json.at("migrations").size()); for (const auto& item : observation_json.at("migrations")) { auto worker = u64(item.at("worker_id"), "migrations.worker_id"); auto state = migration_state(item.at("state").get<std::string_view>()); if (!worker) return std::unexpected{worker.error()}; if (!state) return std::unexpected{state.error()}; if (*worker > std::numeric_limits<std::uint32_t>::max()) return error(TypedReconstructionErrorCode::typed_invariant, "migrations.worker_id"); migrations.push_back(measurement::WorkerMigrationValidityEvidence::create(core::WorkerId{static_cast<std::uint32_t>(*worker)}, *state)); }
    const auto worker_associations_match = std::visit([&](const auto& measured) {
        const auto workers = measured.workers();
        if (workers.size() != placements.size() || workers.size() != migrations.size())
            return false;
        for (std::size_t index{}; index < workers.size(); ++index)
            if (!(workers[index].worker_id() == placements[index].worker_id()) ||
                !(workers[index].worker_id() == migrations[index].worker_id()))
                return false;
        return true;
    }, *measurement);
    if (!worker_associations_match)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "blocks.observations.worker_associations");
    const auto placement_outcome = [&] {
        if (std::ranges::any_of(placements, [](const auto& item){ return item.state() == measurement::WorkerPlacementValidityState::not_qualified; })) return measurement::ObservationValidityCheckOutcome::fail;
        if (std::ranges::any_of(placements, [](const auto& item){ return item.state() == measurement::WorkerPlacementValidityState::unavailable; })) return measurement::ObservationValidityCheckOutcome::unavailable;
        if (std::ranges::all_of(placements, [](const auto& item){ return item.state() == measurement::WorkerPlacementValidityState::not_applicable; })) return measurement::ObservationValidityCheckOutcome::not_applicable;
        return measurement::ObservationValidityCheckOutcome::pass;
    }();
    const auto migration_outcome = [&] {
        if (std::ranges::any_of(migrations, [](const auto& item){ return item.state() == measurement::WorkerMigrationValidityState::migrated; })) return measurement::ObservationValidityCheckOutcome::fail;
        if (std::ranges::any_of(migrations, [](const auto& item){ return item.state() == measurement::WorkerMigrationValidityState::unavailable; })) return measurement::ObservationValidityCheckOutcome::unavailable;
        if (std::ranges::all_of(migrations, [](const auto& item){ return item.state() == measurement::WorkerMigrationValidityState::not_applicable; })) return measurement::ObservationValidityCheckOutcome::not_applicable;
        return measurement::ObservationValidityCheckOutcome::pass;
    }();
    if (validity->checks()[3].outcome() != placement_outcome ||
        validity->checks()[4].outcome() != migration_outcome)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "blocks.observations.validity.worker_evidence");
    std::optional<monitoring::MonitoringCaptureResult> monitoring_result;
    if (!observation_json.at("monitoring").is_null()) {
        std::vector<core::WorkerId> monitoring_workers;
        std::visit([&](const auto& measured) {
            monitoring_workers.reserve(measured.workers().size());
            for (const auto& worker : measured.workers())
                monitoring_workers.push_back(worker.worker_id());
        }, *measurement);
        auto reconstructed = reconstruct_monitoring(
            observation_json.at("monitoring"), monitoring_workers,
            authenticated_cpus,
            std::visit([](const auto& measured) {
                return measured.enclosing_interval().start();
            }, *measurement),
            std::visit([](const auto& measured) {
                return measured.enclosing_interval().end();
            }, *measurement));
        if (!reconstructed) return std::unexpected{reconstructed.error()};
        monitoring_result.emplace(std::move(*reconstructed));
    }
    const auto validity_projection_matches = std::visit([&](const auto& measured) {
        std::array<measurement::ObservationValidityCheckOutcome, 10U> expected{};
        expected[0] = std::ranges::all_of(measured.workers(), [](const auto& worker) {
            return worker.verification().matched();
        }) ? measurement::ObservationValidityCheckOutcome::pass
           : measurement::ObservationValidityCheckOutcome::fail;
        expected[1] = std::ranges::all_of(measured.workers(), [&](const auto& worker) {
            return measured.enclosing_interval().start() <= worker.interval().start() &&
                   worker.interval().end() <= measured.enclosing_interval().end();
        }) ? measurement::ObservationValidityCheckOutcome::pass
           : measurement::ObservationValidityCheckOutcome::fail;
        const auto elapsed = measured.enclosing_interval().elapsed().value();
        const auto estimate = validity->timer_overhead().estimate().duration().value();
        const auto threshold = validity->policy().maximum_harness_overhead();
        expected[2] = elapsed != 0U && rational_less_equal(
            estimate, elapsed, threshold.numerator(), threshold.denominator())
            ? measurement::ObservationValidityCheckOutcome::pass
            : measurement::ObservationValidityCheckOutcome::fail;
        expected[3] = placement_outcome;
        expected[4] = migration_outcome;
        const auto monitor_outcome = [](monitoring::MonitoringSignalState state,
                                        bool detected_failure) {
            switch (state) {
            case monitoring::MonitoringSignalState::clear:
                return measurement::ObservationValidityCheckOutcome::pass;
            case monitoring::MonitoringSignalState::detected:
                return detected_failure
                    ? measurement::ObservationValidityCheckOutcome::fail
                    : measurement::ObservationValidityCheckOutcome::warning;
            case monitoring::MonitoringSignalState::unavailable:
                return measurement::ObservationValidityCheckOutcome::unavailable;
            case monitoring::MonitoringSignalState::not_applicable:
                return measurement::ObservationValidityCheckOutcome::not_applicable;
            }
            return measurement::ObservationValidityCheckOutcome::unavailable;
        };
        const auto page = monitoring_result
            ? monitoring_result->evidence().page_faults_state()
            : monitoring::MonitoringSignalState::not_applicable;
        const auto context = monitoring_result
            ? monitoring_result->evidence().context_switches_state()
            : monitoring::MonitoringSignalState::not_applicable;
        const auto frequency = monitoring_result
            ? monitoring_result->evidence().frequency_state()
            : monitoring::MonitoringSignalState::not_applicable;
        const auto temperature = monitoring_result
            ? monitoring_result->evidence().temperature_state()
            : monitoring::MonitoringSignalState::not_applicable;
        expected[5] = monitor_outcome(page, true);
        expected[6] = measurement::ObservationValidityCheckOutcome::not_applicable;
        expected[7] = monitor_outcome(context, false);
        expected[8] = monitor_outcome(frequency, false);
        expected[9] = monitor_outcome(temperature, false);
        for (std::size_t index{}; index < expected.size(); ++index)
            if (validity->checks()[index].outcome() != expected[index]) return false;
        const auto fraction = validity->timer_overhead().fraction();
        if (elapsed == 0U) return !fraction;
        return fraction && fraction->numerator_nanoseconds() == estimate &&
               fraction->denominator_nanoseconds() == elapsed;
    }, *measurement);
    if (!validity_projection_matches)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "blocks.observations.validity.derived");
    return FactualObservationEvidence{slot, core::ObservationId{*observation_id}, std::move(*measurement), std::move(*validity), std::move(monitoring_result), std::move(placements), std::move(migrations)};
}

auto fixed_reconstruct_observation_blocks(
    const nlohmann::json& body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError> {
    try {
        ReconstructedObservationBlocks result;
        result.blocks.reserve(body.at("blocks").size());
        std::optional<std::uint64_t> previous_block;
        for (const auto& block_json : body.at("blocks")) {
            const auto& plan_json = block_json.at("plan");
            auto block_id = u64(plan_json.at("block_id"), "blocks.plan.block_id");
            if (!block_id) return std::unexpected{block_id.error()};
            if (previous_block && *block_id <= *previous_block)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "blocks.plan.block_id");
            previous_block = *block_id;
            auto reconstructed_plan = fixed_reconstruct_block_plan_component(plan_json);
            if (!reconstructed_plan)
                return std::unexpected{std::move(reconstructed_plan.error())};
            auto plan = std::move(*reconstructed_plan);
            std::vector<FactualObservationEvidence> observations;
            observations.reserve(block_json.at("observations").size());
            for (const auto& observation_json : block_json.at("observations")) {
                auto observation = fixed_reconstruct_factual_component(
                    observation_json, plan, observations.size(), authenticated_cpus);
                if (!observation) return std::unexpected{std::move(observation.error())};
                observations.push_back(std::move(*observation));
            }
            auto completion = closed_token<BlockAttemptCompletion>(block_json.at("completion").get<std::string_view>(), "blocks.completion", {{"complete_valid", BlockAttemptCompletion::complete_valid}, {"complete_invalid", BlockAttemptCompletion::complete_invalid}, {"operationally_incomplete", BlockAttemptCompletion::operationally_incomplete}}); if (!completion) return std::unexpected{completion.error()};
            const bool failed_present = !block_json.at("failed_observation").is_null();
            if (*completion == BlockAttemptCompletion::operationally_incomplete) {
                if (!failed_present || observations.size() >= plan.observations().size())
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "blocks.completion");
            } else {
                if (failed_present || observations.size() != plan.observations().size())
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "blocks.completion");
                const bool all_valid = std::ranges::all_of(observations, [](const auto& item) {
                    return item.validity.disposition() == measurement::ObservationValidityDisposition::valid;
                });
                if ((*completion == BlockAttemptCompletion::complete_valid) != all_valid)
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "blocks.completion");
            }
            result.blocks.push_back(ReconstructedObservationBlock{
                std::move(plan), std::move(observations), failed_present, *completion});
        }
        return result;
    } catch (const nlohmann::json::out_of_range&) { return error(TypedReconstructionErrorCode::missing_member, "root"); }
      catch (const nlohmann::json::exception&) { return error(TypedReconstructionErrorCode::invalid_scalar, "root"); }
}

namespace {

template <class Enum, std::size_t N>
auto closed_token(std::string_view token, const std::array<std::string_view, N>& tokens,
                  ReconstructionPath path) -> std::expected<Enum, FixedTypedReconstructionError> {
    const auto found = std::find(tokens.begin(), tokens.end(), token);
    if (found == tokens.end())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    return static_cast<Enum>(std::distance(tokens.begin(), found));
}

constexpr std::array operation_tokens{
#define OPERATION(name, token) std::string_view{token},
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
};
constexpr std::array reason_tokens{
#define OPERATION(...)
#define REASON(name, token) std::string_view{token},
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
};
constexpr auto phase_tokens = std::to_array<std::string_view>({"preparation", "active", "finalization",
    "cleanup", "create", "service_exception", "begin_capture", "abort_capture",
    "complete_capture", "stop_and_join", "prepare", "page_read", "context_read",
    "temperature_read", "frequency_read", "retained_region_release_mapping",
    "retained_region_release_bound", "retained_region_release_initialized",
    "retained_region_release_placement_observed", "execution_input_validation",
    "execution_validation", "start_state_schedule", "terminal_verification_preparation",
    "worker_launch", "worker_startup", "worker_callback", "worker_command",
    "worker_execute", "worker_generation", "worker_projection",
    "worker_result_publication", "worker_join"});

auto parse_operation(std::string_view value, ReconstructionPath path) {
    return closed_token<PairwiseFailureOperation>(value, operation_tokens, std::move(path));
}
auto parse_reason(std::string_view value, ReconstructionPath path) {
    return closed_token<PairwiseFailureReason>(value, reason_tokens, std::move(path));
}
auto parse_phase(std::string_view value, ReconstructionPath path) {
    return closed_token<PairwiseLifecyclePhase>(value, phase_tokens, std::move(path));
}

auto parse_flat_context(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<PairwiseFlatFailureContext, FixedTypedReconstructionError> {
    const auto kind = value.at("kind").get<std::string_view>();
    if (kind == "none") return NoPairwiseFailureContext{};
    if (kind == "lifecycle_phase") {
        auto phase = parse_phase(value.at("phase").get<std::string_view>(), path + ".phase");
        if (!phase) return std::unexpected{phase.error()};
        return LifecyclePhaseContext{*phase};
    }
    if (kind == "expected_observed_count" || kind == "expected_observed_ordinal") {
        auto expected = u64(value.at("expected"), path + ".expected");
        auto observed = u64(value.at("observed"), path + ".observed");
        if (!expected) return std::unexpected{expected.error()};
        if (!observed) return std::unexpected{observed.error()};
        if (kind == "expected_observed_count")
            return ExpectedObservedCountContext{{*expected}, {*observed}};
        return ExpectedObservedOrdinalContext{{*expected}, {*observed}};
    }
    if (kind == "worker_association") {
        const auto role = value.at("role").get<std::string_view>();
        auto ordinal = u64(value.at("worker_ordinal"), path + ".worker_ordinal");
        if (!ordinal) return std::unexpected{ordinal.error()};
        if (role != "first" && role != "second")
            return error(TypedReconstructionErrorCode::invalid_scalar, path + ".role");
        return WorkerAssociationContext{role == "first" ? PairwiseRole::first : PairwiseRole::second,
                                        {*ordinal}};
    }
    if (kind == "kernel_identity") {
        const auto family = value.at("family").get<std::string_view>();
        const auto implementation = value.at("implementation").get<std::string_view>();
        constexpr auto kernel_phases = std::to_array<std::string_view>({"none", "pass_count_preparation",
            "work_accounting", "execution_input_validation", "execution_validation",
            "start_state_schedule", "terminal_verification_preparation"});
        auto phase = closed_token<PairwiseKernelPhase>(value.at("kernel_phase").get<std::string_view>(),
                                                       kernel_phases, path + ".kernel_phase");
        if (!phase) return std::unexpected{phase.error()};
        if ((family != "bandwidth" && family != "latency") ||
            (implementation != "scalar_u64_sequential_v1" &&
             implementation != "scalar_u64_random_cycle_v1"))
            return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
        return KernelIdentityContext{
            family == "bandwidth" ? PairwiseKernelFamily::bandwidth : PairwiseKernelFamily::latency,
            implementation == "scalar_u64_sequential_v1"
                ? PairwiseKernelImplementation::scalar_u64_sequential_v1
                : PairwiseKernelImplementation::scalar_u64_random_cycle_v1,
            *phase};
    }
    if (kind == "verification_phase") {
        const auto token = value.at("verification_phase").get<std::string_view>();
        if (token != "before" && token != "after")
            return error(TypedReconstructionErrorCode::invalid_scalar, path + ".verification_phase");
        return VerificationPhaseContext{token == "before" ? PairwiseVerificationPhase::before
                                                           : PairwiseVerificationPhase::after};
    }
    if (kind == "monitoring_channel") {
        constexpr auto tokens = std::to_array<std::string_view>(
            {"page_faults", "context_switches", "temperature", "frequency"});
        auto channel = closed_token<PairwiseMonitoringChannel>(
            value.at("channel").get<std::string_view>(), tokens, path + ".channel");
        if (!channel) return std::unexpected{channel.error()};
        return MonitoringChannelContext{*channel};
    }
    if (kind == "missing_operation") {
        constexpr auto tokens = std::to_array<std::string_view>(
            {"now", "bind_service_cpu", "support", "read", "clock_now"});
        auto operation = closed_token<PairwiseMissingOperation>(
            value.at("missing_operation").get<std::string_view>(), tokens,
            path + ".missing_operation");
        if (!operation) return std::unexpected{operation.error()};
        return MissingOperationContext{*operation};
    }
    if (kind == "memory_protection_state") {
        const auto protection = value.at("protection").get<std::string_view>();
        const auto range = value.at("range").get<std::string_view>();
        const auto qualification = value.at("qualification").get<std::string_view>();
        auto reason = parse_reason(value.at("reason").get<std::string_view>(), path + ".reason");
        if (!reason) return std::unexpected{reason.error()};
        if (protection != "read_only" || range != "full_mapping")
            return error(TypedReconstructionErrorCode::invalid_scalar, path);
        constexpr auto tokens = std::to_array<std::string_view>(
            {"qualified", "skipped", "not_qualified"});
        auto qualified = closed_token<PairwiseQualificationState>(qualification, tokens,
                                                                  path + ".qualification");
        if (!qualified) return std::unexpected{qualified.error()};
        return MemoryProtectionStateContext{PairwiseProtectionKind::read_only,
                                             PairwiseRangeKind::full_mapping, *qualified, *reason};
    }
    if (kind == "requested_protection_advice") {
        constexpr auto protections = std::to_array<std::string_view>(
            {"unspecified", "read_write", "read_only"});
        constexpr auto advice = std::to_array<std::string_view>(
            {"none", "base_pages", "huge_pages", "no_huge_pages"});
        auto protection = closed_token<PairwiseMappingProtection>(
            value.at("protection").get<std::string_view>(), protections, path + ".protection");
        auto requested = closed_token<PairwiseMappingAdvice>(
            value.at("advice").get<std::string_view>(), advice, path + ".advice");
        if (!protection) return std::unexpected{protection.error()};
        if (!requested) return std::unexpected{requested.error()};
        return RequestedProtectionAdviceContext{*protection, *requested};
    }
    if (kind == "capacity_acquisition") {
        const auto leg = value.at("leg").get<std::string_view>();
        if (leg != "first_controller" && leg != "second_runner")
            return error(TypedReconstructionErrorCode::invalid_scalar, path + ".leg");
        return PairwiseMonitoringCapacityAcquisitionContext{
            leg == "first_controller" ? PairwiseMonitoringCapacityAcquisitionLeg::first_controller
                                      : PairwiseMonitoringCapacityAcquisitionLeg::second_runner,
            value.at("uncertain").get<bool>()};
    }
    if (kind == "perf_loss") {
        const auto& record = value.at("record");
        const auto& ring = value.at("ring");
        const auto& drain = value.at("drain");
        constexpr auto types = std::to_array<std::string_view>({"lost", "lost_samples"});
        constexpr auto roles = std::to_array<std::string_view>({"unscoped", "initial", "scheduled", "final"});
        auto type = closed_token<PairwisePerfLossType>(record.at("loss_type").get<std::string_view>(), types, path);
        auto role = closed_token<PairwisePerfDrainRole>(drain.at("role").get<std::string_view>(), roles, path);
        auto header = u64(record.at("header_size"), path);
        auto head = u64(ring.at("captured_head"), path);
        auto tail = u64(ring.at("captured_tail"), path);
        auto bytes = u64(ring.at("ring_bytes"), path);
        const auto optional = [&](const nlohmann::json& item)
            -> std::expected<std::optional<PairwiseCount>, FixedTypedReconstructionError> {
            if (item.is_null()) return std::optional<PairwiseCount>{};
            auto count = u64(item, path);
            if (!count) return std::unexpected{count.error()};
            return std::optional<PairwiseCount>{{*count}};
        };
        auto lost = optional(record.at("lost_count"));
        auto ordinal = optional(drain.at("ordinal"));
        auto started = optional(drain.at("started_ns"));
        auto previous = optional(value.at("last_successful_drain_completed_ns"));
        if (!type || !role || !header || !head || !tail || !bytes ||
            !lost || !ordinal || !started || !previous)
            return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
        return PairwisePerfLossContext{
            {*type, {*header}, *lost}, {{*head}, {*tail}, {*bytes}},
            {*role, *ordinal ? std::optional<PairwiseOrdinal>{{(**ordinal).value}} : std::nullopt,
             *started}, *previous};
    }
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto parse_code(std::string_view token, ReconstructionPath path)
    -> std::expected<PairwiseFailureCode, FixedTypedReconstructionError> {
    // The wire code is the enum identifier, not the stable dotted spelling.
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, stable, renderer) if (token == #code) return PairwiseFailureCode::code;
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

auto parse_native(const nlohmann::json& value, ReconstructionPath path, bool outer)
    -> std::expected<std::optional<int>, FixedTypedReconstructionError> {
    if (value.is_null()) return std::optional<int>{};
    if (outer && !value.is_object())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    if (!outer && !value.is_string())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    if (outer && !core::detail::json_string_equal(value.at("domain"), "posix_errno"))
        return error(TypedReconstructionErrorCode::invalid_scalar, path + ".domain");
    const auto& encoded = outer ? value.at("value") : value;
    if (!encoded.is_string())
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    std::int32_t parsed{};
    const auto& text = encoded.get_ref<const std::string&>();
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed <= 0)
        return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
    return std::optional<int>{parsed};
}

auto parse_nested_value(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<PairwiseNestedFailureValue, FixedTypedReconstructionError> {
    auto code = parse_code(value.at("code").get<std::string_view>(), path + ".code");
    auto operation = parse_operation(value.at("operation").get<std::string_view>(), path + ".operation");
    auto reason = parse_reason(value.at("reason").get<std::string_view>(), path + ".reason");
    auto context = parse_flat_context(value.at("context"), path + ".context");
    auto native = parse_native(value.at("posix_errno"), path + ".posix_errno", false);
    if (!code) return std::unexpected{code.error()};
    if (!operation) return std::unexpected{operation.error()};
    if (!reason) return std::unexpected{reason.error()};
    if (!context) return std::unexpected{context.error()};
    if (!native) return std::unexpected{native.error()};
    return PairwiseNestedFailureValue{*code, *operation, *reason, *context, *native};
}

auto parse_context(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<PairwiseFailureContext, FixedTypedReconstructionError> {
    if (!core::detail::json_string_equal(value.at("kind"), "nested_failure")) {
        auto flat = parse_flat_context(value, path);
        if (!flat) return std::unexpected{flat.error()};
        return std::visit([](const auto& item) -> PairwiseFailureContext { return item; }, *flat);
    }
    auto failure = parse_nested_value(value.at("failure"), path + ".failure");
    if (!failure) return std::unexpected{failure.error()};
    std::optional<PairwiseLifecyclePhase> phase;
    if (!value.at("nested_phase").is_null()) {
        auto parsed = parse_phase(value.at("nested_phase").get<std::string_view>(),
                                  path + ".nested_phase");
        if (!parsed) return std::unexpected{parsed.error()};
        phase = *parsed;
    }
    std::optional<PairwiseNestedFailureValue> platform;
    if (!value.at("nested_platform_failure").is_null()) {
        auto parsed = parse_nested_value(value.at("nested_platform_failure"),
                                         path + ".nested_platform_failure");
        if (!parsed) return std::unexpected{parsed.error()};
        platform = *parsed;
    }
    return NestedFailureContext{*failure, phase, platform};
}

auto reconstruct_finalized_error(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<FinalizedEvidenceError, FixedTypedReconstructionError> {
    auto code = parse_code(value.at("code").get<std::string_view>(), path + ".code");
    auto operation = parse_operation(value.at("operation").get<std::string_view>(), path + ".operation");
    auto reason = parse_reason(value.at("reason").get<std::string_view>(), path + ".reason");
    auto context = parse_context(value.at("context"), path + ".context");
    auto native = parse_native(value.at("os_error"), path + ".os_error", true);
    if (!code) return std::unexpected{code.error()};
    if (!operation) return std::unexpected{operation.error()};
    if (!reason) return std::unexpected{reason.error()};
    if (!context) return std::unexpected{context.error()};
    if (!native) return std::unexpected{native.error()};
    const auto subsystem = pairwise_failure_subsystem(*code);
    auto make = [&]<PairwiseFailureSubsystem S>() -> std::optional<PairwiseEvidenceFailure> {
        auto made = BasicPairwiseFailure<S>::create(*code, *operation, *reason, *context, *native);
        if (!made) return std::nullopt;
        return PairwiseEvidenceFailure{*made};
    };
    std::optional<PairwiseEvidenceFailure> failure;
    switch (subsystem) {
    case PairwiseFailureSubsystem::platform: failure = make.template operator()<PairwiseFailureSubsystem::platform>(); break;
    case PairwiseFailureSubsystem::memory: failure = make.template operator()<PairwiseFailureSubsystem::memory>(); break;
    case PairwiseFailureSubsystem::monitoring: failure = make.template operator()<PairwiseFailureSubsystem::monitoring>(); break;
    case PairwiseFailureSubsystem::kernel: failure = make.template operator()<PairwiseFailureSubsystem::kernel>(); break;
    case PairwiseFailureSubsystem::measurement: failure = make.template operator()<PairwiseFailureSubsystem::measurement>(); break;
    case PairwiseFailureSubsystem::statistics: failure = make.template operator()<PairwiseFailureSubsystem::statistics>(); break;
    case PairwiseFailureSubsystem::worker: failure = make.template operator()<PairwiseFailureSubsystem::worker>(); break;
    case PairwiseFailureSubsystem::runtime: failure = make.template operator()<PairwiseFailureSubsystem::runtime>(); break;
    case PairwiseFailureSubsystem::count: break;
    }
    if (!failure)
        return error(TypedReconstructionErrorCode::typed_invariant, std::move(path));
    auto result = FinalizedEvidenceError::from_failure(std::move(*failure));
    const auto profile = std::find_if(error_failure_rows.begin(), error_failure_rows.end(),
        [&](const auto& row) { return row.code == value.at("code").get<std::string_view>(); });
    if (profile == error_failure_rows.end() || !core::detail::json_string_equal(value.at("subsystem"), profile->subsystem) ||
        !core::detail::json_string_equal(value.at("category"), profile->category) || !core::detail::json_string_equal(value.at("stable_code"), result.stable_code()) ||
        !core::detail::json_string_equal(value.at("severity"), profile->severity) || !core::detail::json_string_equal(value.at("continuation"), profile->continuation) ||
        !core::detail::json_string_equal(value.at("renderer"), profile->renderer) ||
        !core::detail::json_string_equal(value.at("operation"), result.operation_token()) || !core::detail::json_string_equal(value.at("reason"), result.reason_token()))
        return error(TypedReconstructionErrorCode::inconsistent_projection, std::move(path));
    return result;
}

auto scheduled_slot(const nlohmann::json& value, const measurement::MeasurementBlockPlan& plan,
                    ReconstructionPath path)
    -> std::expected<measurement::ScheduledObservationSlot, FixedTypedReconstructionError> {
    auto block = u64(value.at("block_id"), path + ".block_id");
    auto index = u64(value.at("slot_index"), path + ".slot_index");
    if (!block) return std::unexpected{block.error()};
    if (!index) return std::unexpected{index.error()};
    const auto token = value.at("variant_slot").get<std::string_view>();
    if (*index >= 4U || (token != "first" && token != "second"))
        return error(TypedReconstructionErrorCode::typed_invariant, std::move(path));
    const auto& result = plan.observations()[static_cast<std::size_t>(*index)];
    const auto stated = token == "first" ? measurement::ComparisonVariantSlot::first
                                         : measurement::ComparisonVariantSlot::second;
    if (result.block_id() != kernel::DependentLoadLatencyBlockId{*block} ||
        result.comparison_variant() != stated)
        return error(TypedReconstructionErrorCode::inconsistent_projection, std::move(path));
    return result;
}

} // namespace

auto fixed_reconstruct_failed_component_header(const nlohmann::json& failed,
                                         const measurement::MeasurementBlockPlan& plan,
                                         ReconstructionPath path)
    -> std::expected<ReconstructedFailedComponentHeader, FixedTypedReconstructionError> {
    auto slot = scheduled_slot(failed.at("slot"), plan, ReconstructionPath{path} + ".slot");
    auto observation = u64(failed.at("observation_id"), ReconstructionPath{path} + ".observation_id");
    auto primary = reconstruct_finalized_error(failed.at("primary_error"),
                                               ReconstructionPath{path} + ".primary_error");
    if (!slot) return std::unexpected{std::move(slot.error())};
    if (!observation) return std::unexpected{std::move(observation.error())};
    if (!primary) return std::unexpected{std::move(primary.error())};
    return ReconstructedFailedComponentHeader{*slot, core::ObservationId{*observation}, *primary};
}
auto fixed_reconstruct_failed_component_workers(const nlohmann::json& failed,
                                           ReconstructedFailedComponentHeader header,
                                           ReconstructionPath path)
    -> std::expected<FailedObservationEvidence, FixedTypedReconstructionError> {
    std::vector<FinalizedEvidenceError> workers;
    const auto& worker_json = failed.at("worker_errors");
    workers.reserve(worker_json.size());
    for (std::size_t worker{}; worker < worker_json.size(); ++worker) {
        auto parsed = reconstruct_finalized_error(worker_json[worker],
            ReconstructionPath{path} + ".worker_errors[" + ReconstructionPath::ordinal(worker) + "]");
        if (!parsed) return std::unexpected{std::move(parsed.error())};
        workers.push_back(std::move(*parsed));
    }
    return FailedObservationEvidence{header.slot, header.observation_id,
                                    std::move(header.primary_error), std::move(workers)};
}

auto fixed_reconstruct_finalized_error_value(const nlohmann::json& bounded_error)
    -> std::expected<FinalizedEvidenceError, FixedTypedReconstructionError> {
    try {
        return reconstruct_finalized_error(bounded_error, "finalized_error");
    } catch (const nlohmann::json::out_of_range& exception) {
        return error(TypedReconstructionErrorCode::missing_member, ReconstructionPath::reference(exception.what()));
    } catch (const nlohmann::json::type_error& exception) {
        return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath::reference(exception.what()));
    }
}

template <class Prefix>
auto reconstruct_stopping_record(const nlohmann::json& value,
    const statistics::BootstrapPolicy& root_stopping_policy, double target,
    std::uint64_t maximum_blocks, std::optional<std::uint64_t> previous_boundary,
    Prefix prefix, ReconstructionPath path)
    -> std::expected<RunnerStoppingEvaluationEvidence, FixedTypedReconstructionError> {
    auto boundary = u64(value.at("boundary_block_id"), path + ".boundary_block_id");
    auto completed = u64(value.at("completed_blocks"), path + ".completed_blocks");
    auto valid_blocks = u64(value.at("complete_valid_blocks"), path + ".complete_valid_blocks");
    auto first = u64(value.at("valid_first_observations"), path + ".valid_first_observations");
    auto second = u64(value.at("valid_second_observations"), path + ".valid_second_observations");
    if (!boundary) return std::unexpected{boundary.error()};
    if (!completed) return std::unexpected{completed.error()};
    if (!valid_blocks) return std::unexpected{valid_blocks.error()};
    if (!first) return std::unexpected{first.error()};
    if (!second) return std::unexpected{second.error()};
    if (*boundary == std::numeric_limits<std::uint64_t>::max() ||
        *valid_blocks > std::numeric_limits<std::uint64_t>::max() / 2U ||
        *completed != *boundary + 1U || *valid_blocks > *completed ||
        *first != 2U * *valid_blocks || *second != 2U * *valid_blocks ||
        (previous_boundary && *boundary <= *previous_boundary))
        return error(TypedReconstructionErrorCode::inconsistent_projection, path);
    const auto counts = prefix(*boundary);
    if (!counts.boundary_found || counts.completed != *completed || counts.valid != *valid_blocks)
        return error(TypedReconstructionErrorCode::inconsistent_projection, path);
    const auto& boot = value.at("bootstrap");
    auto policy = bootstrap(boot.at("policy"), path + ".bootstrap.policy");
    auto log_lower = binary64(boot.at("log_lower"), path + ".bootstrap.log_lower");
    auto log_upper = binary64(boot.at("log_upper"), path + ".bootstrap.log_upper");
    auto ratio_lower = binary64(boot.at("ratio_lower"), path + ".bootstrap.ratio_lower");
    auto ratio_upper = binary64(boot.at("ratio_upper"), path + ".bootstrap.ratio_upper");
    if (!policy) return std::unexpected{policy.error()};
    if (!log_lower) return std::unexpected{log_lower.error()};
    if (!log_upper) return std::unexpected{log_upper.error()};
    if (!ratio_lower) return std::unexpected{ratio_lower.error()};
    if (!ratio_upper) return std::unexpected{ratio_upper.error()};
    if (*policy != root_stopping_policy || *log_lower > *log_upper ||
        *ratio_lower <= 0.0 || *ratio_lower > *ratio_upper ||
        std::exp(*log_lower) != *ratio_lower || std::exp(*log_upper) != *ratio_upper)
        return error(TypedReconstructionErrorCode::inconsistent_projection, path + ".bootstrap");
    const auto log_interval = statistics::detail::BootstrapAccess::log_interval(
        statistics::detail::LogRatioAccess::paired(*log_lower),
        statistics::detail::LogRatioAccess::paired(*log_upper));
    const auto ratio_interval = statistics::detail::BootstrapAccess::ratio_interval(
        statistics::detail::LogRatioAccess::ratio(*ratio_lower),
        statistics::detail::LogRatioAccess::ratio(*ratio_upper));
    statistics::BlockBootstrapResult bootstrap_result{*policy, log_interval, ratio_interval};
    const auto& evaluation = value.at("evaluation");
    auto observed = binary64(evaluation.at("observed_relative_half_width"),
                             path + ".evaluation.observed_relative_half_width");
    if (!observed || *observed < 0.0) {
        if (!observed) return std::unexpected{observed.error()};
        return error(TypedReconstructionErrorCode::typed_invariant, path + ".evaluation");
    }
    const bool met = *observed <= target;
    const auto attainment = evaluation.at("attainment").get<std::string_view>();
    const auto decision = value.at("decision").get<std::string_view>();
    const auto subsequent = value.at("subsequent_disposition").get<std::string_view>();
    if ((attainment != (met ? "target_met" : "target_not_met")) ||
        (decision != (met ? "target_met" : "target_not_met")))
        return error(TypedReconstructionErrorCode::inconsistent_projection, path + ".evaluation");
    const auto expected_subsequent = met ? std::string_view{"precision_target_reached"}
        : (*completed >= maximum_blocks ? std::string_view{"maximum_blocks_reached"}
                                          : std::string_view{"continued"});
    if (subsequent != expected_subsequent)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     path + ".subsequent_disposition");
    return RunnerStoppingEvaluationEvidence{
        kernel::DependentLoadLatencyBlockId{*boundary}, *completed, *valid_blocks, *first,
        *second, std::move(bootstrap_result),
        statistics::AdaptiveStoppingEvaluation{
            statistics::detail::AdaptiveStoppingAccess::observed(*observed),
            met ? statistics::PrecisionTargetAttainment::target_met
                : statistics::PrecisionTargetAttainment::target_not_met},
        met ? measurement::PrecisionStoppingDecision::target_met
            : measurement::PrecisionStoppingDecision::target_not_met,
        met ? RepetitionBoundaryDisposition::precision_target_reached
            : (*completed >= maximum_blocks
                   ? RepetitionBoundaryDisposition::maximum_blocks_reached
                   : RepetitionBoundaryDisposition::continued)};
}

auto fixed_reconstruct_stopping_component(const nlohmann::json& value,
    const StatisticsProductionPolicy& statistics_policy, const measurement::RepetitionPolicy& repetition,
    StoppingPrefixCounts prefix, std::optional<std::uint64_t> previous, ReconstructionPath path)
    -> std::expected<RunnerStoppingEvaluationEvidence, FixedTypedReconstructionError> {
    return reconstruct_stopping_record(value, statistics_policy.stopping_bootstrap_policy(),
        statistics_policy.relative_half_width_target().value(), repetition.maximum_blocks(), previous,
        [&](std::uint64_t boundary) {
            auto counts = prefix;
            counts.boundary_found = counts.boundary_found && counts.boundary == boundary;
            return counts;
        }, std::move(path));
}

auto reconstruct_stopping_and_failures_impl(
    const nlohmann::json& body,
    std::span<const core::LinuxCpuId> authenticated_cpus,
    const ReconstructedObservationBlocks* borrowed_blocks = nullptr)
    -> std::expected<ReconstructedStoppingAndFailures, FixedTypedReconstructionError> {
    try {
        ReconstructedStoppingAndFailures result;
        const auto target = binary64(body.at("statistics_policy").at("relative_half_width_target"),
                                     "statistics_policy.relative_half_width_target");
        if (!target) return std::unexpected{target.error()};
        const auto maximum_blocks = u64(body.at("repetition_policy").at("maximum_blocks"),
                                        "repetition_policy.maximum_blocks");
        if (!maximum_blocks) return std::unexpected{maximum_blocks.error()};
        auto root_stopping_policy = bootstrap(
            body.at("statistics_policy").at("stopping_bootstrap_policy"),
            "statistics_policy.stopping_bootstrap_policy");
        if (!root_stopping_policy) return std::unexpected{root_stopping_policy.error()};
        std::optional<ReconstructedObservationBlocks> owned_blocks;
        if (!borrowed_blocks) {
            auto made = fixed_reconstruct_observation_blocks(body, authenticated_cpus);
            if (!made) return std::unexpected{std::move(made.error())};
            owned_blocks.emplace(std::move(*made));
            borrowed_blocks = &*owned_blocks;
        }
        const auto* reconstructed_blocks = borrowed_blocks;
        const auto& stopping = body.at("stopping_evaluations");
        result.stopping_evaluations.reserve(stopping.size());
        std::uint64_t previous_boundary{};
        for (std::size_t index{}; index < stopping.size(); ++index) {
            const auto path = "stopping_evaluations[" + ReconstructionPath::ordinal(index) + "]";
            const auto& value = stopping[index];
            auto item = reconstruct_stopping_record(value, *root_stopping_policy, *target,
                *maximum_blocks, index ? std::optional{previous_boundary} : std::nullopt,
                [&](std::uint64_t boundary) {
                    StoppingPrefixCounts counts{};
                    for (const auto& block : reconstructed_blocks->blocks) {
                        if (block.plan.block_id().value() > boundary) break;
                        if (block.stated_completion != BlockAttemptCompletion::operationally_incomplete)
                            ++counts.completed;
                        if (block.stated_completion == BlockAttemptCompletion::complete_valid)
                            ++counts.valid;
                        counts.boundary_found = block.plan.block_id().value() == boundary &&
                            block.stated_completion != BlockAttemptCompletion::operationally_incomplete;
                    }
                    return counts;
                }, path);
            if (!item) return std::unexpected{std::move(item.error())};
            previous_boundary = item->boundary_block_id.value();
            result.stopping_evaluations.push_back(std::move(*item));
        }
        const auto& blocks = body.at("blocks");
        result.failed_observations.reserve(static_cast<std::size_t>(std::ranges::count_if(blocks, [](const auto& block) {
            return !block.at("failed_observation").is_null();
        })));
        std::uint64_t next_observation_id{};
        bool observation_ids_exhausted = false;
        for (std::size_t block_index{}; block_index < blocks.size(); ++block_index) {
            for (std::size_t factual_index{};
                 factual_index < blocks[block_index].at("observations").size(); ++factual_index) {
                auto factual_id = u64(
                    blocks[block_index].at("observations")[factual_index].at("observation_id"),
                    "blocks[" + ReconstructionPath::ordinal(block_index) + "].observations[" +
                        ReconstructionPath::ordinal(factual_index) + "].observation_id");
                if (!factual_id) return std::unexpected{factual_id.error()};
                if (observation_ids_exhausted || *factual_id != next_observation_id)
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "blocks[" + ReconstructionPath::ordinal(block_index) + "].observations[" +
                                     ReconstructionPath::ordinal(factual_index) + "].observation_id");
                if (next_observation_id == std::numeric_limits<std::uint64_t>::max())
                    observation_ids_exhausted = true;
                else ++next_observation_id;
            }
            const auto& failed = blocks[block_index].at("failed_observation");
            if (failed.is_null()) continue;
            const auto path = "blocks[" + ReconstructionPath::ordinal(block_index) + "].failed_observation";
            auto header = fixed_reconstruct_failed_component_header(
                failed, reconstructed_blocks->blocks[block_index].plan, path);
            if (!header) return std::unexpected{std::move(header.error())};
            const auto* slot = &header->slot;
            const auto observation = header->observation_id.value();
            if (slot->block_id() != reconstructed_blocks->blocks[block_index].plan.block_id() ||
                !core::detail::json_string_equal(blocks[block_index].at("completion"), "operationally_incomplete") ||
                slot->slot_index() != blocks[block_index].at("observations").size())
                return error(TypedReconstructionErrorCode::inconsistent_projection, path + ".slot");
            if (observation_ids_exhausted || observation != next_observation_id)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             path + ".observation_id");
            if (next_observation_id == std::numeric_limits<std::uint64_t>::max())
                observation_ids_exhausted = true;
            else ++next_observation_id;
            auto reconstructed = fixed_reconstruct_failed_component_workers(failed, std::move(*header), path);
            if (!reconstructed) return std::unexpected{std::move(reconstructed.error())};
            result.failed_observations.push_back(ReconstructedFailedObservation{
                block_index, std::move(*reconstructed)});
        }
        return result;
    } catch (const nlohmann::json::out_of_range& exception) {
        return error(TypedReconstructionErrorCode::missing_member, ReconstructionPath::reference(exception.what()));
    } catch (const nlohmann::json::type_error& exception) {
        return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath::reference(exception.what()));
    }
}

auto fixed_reconstruct_stopping_and_failures(const nlohmann::json& body)
    -> std::expected<ReconstructedStoppingAndFailures, FixedTypedReconstructionError> {
    return reconstruct_stopping_and_failures_impl(body, {});
}

auto fixed_reconstruct_stopping_and_failures(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, FixedTypedReconstructionError> {
    return reconstruct_stopping_and_failures_impl(bounded_body.value(), {});
}

auto fixed_reconstruct_observation_blocks(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError> {
    return fixed_reconstruct_observation_blocks(bounded_body.value(), {});
}

auto fixed_reconstruct_observation_blocks(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError> {
    return fixed_reconstruct_observation_blocks(bounded_body, {});
}

auto fixed_reconstruct_observation_blocks(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, FixedTypedReconstructionError> {
    return fixed_reconstruct_observation_blocks(bounded_body.value(), authenticated_cpus);
}

namespace {

auto ratio_value(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<memory::ExactRatio, FixedTypedReconstructionError> {
    auto numerator = u64(value.at("numerator"), ReconstructionPath{path} + ".numerator");
    auto denominator = u64(value.at("denominator"), ReconstructionPath{path} + ".denominator");
    if (!numerator) return std::unexpected{numerator.error()};
    if (!denominator) return std::unexpected{denominator.error()};
    if (*denominator == 0U || *numerator > *denominator)
        return error(TypedReconstructionErrorCode::typed_invariant, path);
    auto made = memory::ExactRatio::create(*numerator, *denominator);
    core::detail::ExpectedErrorCleanup made_cleanup{made};
    if (!made) return error(TypedReconstructionErrorCode::typed_invariant, ReconstructionPath{path});
    return *made;
}

auto placement_observation(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<memory::RegionPlacementObservation, FixedTypedReconstructionError> {
    auto total = u64(value.at("total_pages"), ReconstructionPath{path} + ".total_pages");
    auto resident = u64(value.at("resident_pages"), ReconstructionPath{path} + ".resident_pages");
    auto unresolved = u64(value.at("unresolved_pages"), ReconstructionPath{path} + ".unresolved_pages");
    if (!total) return std::unexpected{total.error()};
    if (!resident) return std::unexpected{resident.error()};
    if (!unresolved) return std::unexpected{unresolved.error()};
    if (*resident > *total || *unresolved > *total - *resident)
        return error(TypedReconstructionErrorCode::typed_invariant, ReconstructionPath{path});
    std::vector<memory::LinuxNodePageCount> counts;
    counts.reserve(value.at("resident_page_counts").size());
    std::uint64_t sum{};
    for (std::size_t index{}; index < value.at("resident_page_counts").size(); ++index) {
        const auto item_path = ReconstructionPath{path} + ".resident_page_counts[" +
                               ReconstructionPath::ordinal(index) + "]";
        const auto& item = value.at("resident_page_counts")[index];
        auto node = u64(item.at("linux_node"), item_path + ".linux_node");
        auto pages = u64(item.at("page_count"), item_path + ".page_count");
        if (!node) return std::unexpected{node.error()};
        if (!pages) return std::unexpected{pages.error()};
        if (*node > std::numeric_limits<std::uint32_t>::max() ||
            sum > std::numeric_limits<std::uint64_t>::max() - *pages)
            return error(TypedReconstructionErrorCode::typed_invariant, item_path);
        if (!counts.empty() && counts.back().linux_node().value() >= *node)
            return error(TypedReconstructionErrorCode::inconsistent_projection, item_path);
        sum += *pages;
        counts.push_back(memory::detail::PairwiseMemoryAccess::node_count(
            core::LinuxNumaNodeId{static_cast<std::uint32_t>(*node)}, *pages));
    }
    if (sum != *resident)
        return error(TypedReconstructionErrorCode::inconsistent_projection, ReconstructionPath{path});
    return memory::detail::PairwiseMemoryAccess::observation(
        *total, *resident, *unresolved, std::move(counts));
}

auto placement_evaluation(const nlohmann::json& value, ReconstructionPath path)
    -> std::expected<memory::RegionPlacementEvaluation, FixedTypedReconstructionError> {
    auto requested = u64(value.at("requested_node_pages"),
                         ReconstructionPath{path} + ".requested_node_pages");
    auto unexpected = u64(value.at("unexpected_node_pages"),
                          ReconstructionPath{path} + ".unexpected_node_pages");
    auto unresolved = u64(value.at("unresolved_pages"),
                          ReconstructionPath{path} + ".unresolved_pages");
    auto ratio = ratio_value(value.at("observed_ratio"),
                             ReconstructionPath{path} + ".observed_ratio");
    if (!requested) return std::unexpected{requested.error()};
    if (!unexpected) return std::unexpected{unexpected.error()};
    if (!unresolved) return std::unexpected{unresolved.error()};
    if (!ratio) return std::unexpected{ratio.error()};
    return memory::detail::PairwiseMemoryAccess::placement_evaluation(
        *requested, *unexpected, *unresolved, *ratio);
}

auto qualification(std::string_view token, ReconstructionPath path)
    -> std::expected<memory::PlacementQualificationState, FixedTypedReconstructionError> {
    if (token == "skipped") return memory::PlacementQualificationState::skipped;
    if (token == "qualified") return memory::PlacementQualificationState::qualified;
    if (token == "not_qualified") return memory::PlacementQualificationState::not_qualified;
    return error(TypedReconstructionErrorCode::invalid_scalar, std::move(path));
}

template <class T, class Parse>
auto optional_object(const nlohmann::json& value, ReconstructionPath path, Parse parse)
    -> std::expected<std::optional<T>, FixedTypedReconstructionError> {
    if (value.is_null()) return std::optional<T>{};
    auto made = parse(value, path);
    if (!made) return std::unexpected{made.error()};
    return std::optional<T>{std::move(*made)};
}

auto region_diagnostic(const nlohmann::json& value, ReconstructionPath path,
                       core::WorkerId expected_worker)
    -> std::expected<memory::RegionFinalizationDiagnostic, FixedTypedReconstructionError> {
    const auto kind = value.at("kind").get<std::string_view>();
    if (kind == "unavailable") {
        auto reason = u64(value.at("unavailable_reason"),
                          ReconstructionPath{path} + ".unavailable_reason");
        if (!reason) return std::unexpected{reason.error()};
        if (*reason > 1U)
            return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath{path});
        return memory::detail::PairwiseMemoryAccess::unavailable_diagnostic(
            *reason == 0U ? memory::RegionFinalizationUnavailableReason::contained_exception
                          : memory::RegionFinalizationUnavailableReason::contained_unknown_failure);
    }
    if (kind != "available")
        return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath{path} + ".kind");
    const auto& request_json = value.at("request");
    auto worker = u64(request_json.at("worker_id"), ReconstructionPath{path} + ".request.worker_id");
    auto requested = u64(request_json.at("requested_bytes"),
                         ReconstructionPath{path} + ".request.requested_bytes");
    auto page = u64(request_json.at("page_size"), ReconstructionPath{path} + ".request.page_size");
    auto threshold = ratio_value(request_json.at("placement_threshold"),
                                 ReconstructionPath{path} + ".request.placement_threshold");
    if (!worker) return std::unexpected{worker.error()};
    if (!requested) return std::unexpected{requested.error()};
    if (!page) return std::unexpected{page.error()};
    if (!threshold) return std::unexpected{threshold.error()};
    if (*worker != expected_worker.value() || !core::detail::json_string_equal(request_json.at("page_mode"), "base") ||
        !core::detail::json_string_equal(request_json.at("placement_policy"), "bind"))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     ReconstructionPath{path} + ".request");
    std::vector<memory::RegionNumaTarget> targets;
    targets.reserve(request_json.at("numa_targets").size());
    for (std::size_t index{}; index < request_json.at("numa_targets").size(); ++index) {
        const auto& item = request_json.at("numa_targets")[index];
        const auto& stable_value = item.at("stable_node");
        // Preserve the historical reference diagnostic on wrong-kind input.
        // Bounded shape admission excludes this branch before construction.
        if (!stable_value.is_string()) (void)stable_value.get<std::string>();
        const auto& stable_text = stable_value.get_ref<const std::string&>();
        const auto decimal = std::string_view{stable_text}.substr(std::min(stable_text.size(), std::size_t{5}));
        std::uint32_t stable_number{};
        const auto parsed = std::from_chars(decimal.data(), decimal.data() + decimal.size(), stable_number);
        if (!stable_text.starts_with("numa/") || decimal.empty() ||
            (decimal.size() > 1U && decimal.front() == '0') || parsed.ec != std::errc{} ||
            parsed.ptr != decimal.data() + decimal.size())
            return error(TypedReconstructionErrorCode::typed_invariant, path + ".request.numa_targets");
        auto stable = topology::StableNumaNodeId::parse(stable_text);
        core::detail::ExpectedErrorCleanup stable_cleanup{stable};
        auto linux = u64(item.at("linux_node"), ReconstructionPath{path} + ".request.numa_targets.linux_node");
        auto topology_node = u64(item.at("topology_node"),
                                 ReconstructionPath{path} + ".request.numa_targets.topology_node");
        if (!stable || !linux || !topology_node ||
            *linux > std::numeric_limits<std::uint32_t>::max() ||
            *topology_node > std::numeric_limits<std::uint32_t>::max())
            return error(TypedReconstructionErrorCode::typed_invariant,
                         ReconstructionPath{path} + ".request.numa_targets");
        targets.push_back({std::move(*stable), core::LinuxNumaNodeId{static_cast<std::uint32_t>(*linux)},
                           core::NumaNodeId{static_cast<std::uint32_t>(*topology_node)}});
    }
    if (*requested == 0U || *page == 0U || *requested % *page != 0U || targets.empty())
        return error(TypedReconstructionErrorCode::typed_invariant, path + ".request");
    for (std::size_t i{}; i < targets.size(); ++i)
        for (std::size_t j = i + 1U; j < targets.size(); ++j)
            if (targets[i].stable_node == targets[j].stable_node ||
                targets[i].linux_node == targets[j].linux_node ||
                targets[i].topology_node == targets[j].topology_node)
                return error(TypedReconstructionErrorCode::typed_invariant, path + ".request");
    auto request = memory::RegionRequest::create({expected_worker,
        core::ByteCount::from_base_units(*requested), core::ByteCount::from_base_units(*page),
        memory::PlacementPolicy::bind, memory::PageMode::base, std::move(targets), *threshold,
        request_json.at("prefault").get<bool>(), request_json.at("verify_before").get<bool>(),
        request_json.at("verify_after").get<bool>()});
    core::detail::ExpectedErrorCleanup request_cleanup{request};
    if (!request) return error(TypedReconstructionErrorCode::typed_invariant,
                               ReconstructionPath{path} + ".request");
    const auto& binding = value.at("binding");
    if (!core::detail::json_string_equal(binding.at("policy"), "bind") || !core::detail::json_string_equal(binding.at("range"), "full_mapping") ||
        !binding.at("policy_call_accepted").get<bool>() ||
        binding.at("migration_requested").get<bool>() ||
        binding.at("strict_verification_requested").get<bool>())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     ReconstructionPath{path} + ".binding");
    const auto& initialization = value.at("initialization");
    const bool prefault = initialization.at("prefault_requested").get<bool>();
    if (!core::detail::json_string_equal(initialization.at("range"), "logical_requested_region") ||
        initialization.at("prefault_pass_performed").get<bool>() != prefault ||
        !initialization.at("initialization_completed").get<bool>() ||
        initialization.at("padding_touched").get<bool>() || prefault != request->prefault())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     ReconstructionPath{path} + ".initialization");
    auto parse_pre = [&](const nlohmann::json& item)
        -> std::expected<memory::PreRunPlacementDiagnostic, FixedTypedReconstructionError> {
        const auto state_token = item.at("state").get<std::string_view>();
        auto observation = optional_object<memory::RegionPlacementObservation>(
            item.at("observation"), ReconstructionPath{path} + ".pre_run.observation", placement_observation);
        auto evaluation = optional_object<memory::RegionPlacementEvaluation>(
            item.at("evaluation"), ReconstructionPath{path} + ".pre_run.evaluation", placement_evaluation);
        if (!observation) return std::unexpected{observation.error()};
        if (!evaluation) return std::unexpected{evaluation.error()};
        std::optional<memory::PlacementQualificationState> qualified;
        if (!item.at("qualification_state").is_null()) {
            auto q = qualification(item.at("qualification_state").get<std::string_view>(),
                                   ReconstructionPath{path} + ".pre_run.qualification_state");
            if (!q) return std::unexpected{q.error()};
            qualified = *q;
        }
        const bool skipped = state_token == "skipped";
        if ((!skipped && state_token != "observed") ||
            (skipped && (*observation || *evaluation || qualified)) ||
            (!skipped && (!*observation || !*evaluation || !qualified)))
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         ReconstructionPath{path} + ".pre_run");
        if (skipped && request->verify_before())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         ReconstructionPath{path} + ".pre_run");
        if (!skipped) {
            if (!request->verify_before() ||
                (*observation)->total_pages() != request->requested_bytes().value() /
                                                   request->page_size().value())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             ReconstructionPath{path} + ".pre_run");
            if (!placement_evaluator_domain(**observation))
                return error(TypedReconstructionErrorCode::inconsistent_projection, path + ".pre_run");
            auto derived = memory::detail::PlacementEvaluatedRegionAccess::
                evaluate_region_placement(*request, **observation);
            core::detail::ExpectedErrorCleanup derived_cleanup{derived};
            if (!derived || derived->qualification_state != *qualified ||
                derived->evaluation.requested_node_pages() != (*evaluation)->requested_node_pages() ||
                derived->evaluation.unexpected_node_pages() != (*evaluation)->unexpected_node_pages() ||
                derived->evaluation.unresolved_pages() != (*evaluation)->unresolved_pages() ||
                derived->evaluation.observed_ratio() != (*evaluation)->observed_ratio())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             ReconstructionPath{path} + ".pre_run");
        }
        return memory::detail::PairwiseMemoryAccess::pre_run(
            skipped ? memory::PreRunDiagnosticState::skipped
                    : memory::PreRunDiagnosticState::observed,
            std::move(*observation), std::move(*evaluation), qualified);
    };
    auto pre = parse_pre(value.at("pre_run"));
    if (!pre) return std::unexpected{pre.error()};
    std::optional<memory::RegionProtectionEvidence> protection;
    if (!value.at("protection").is_null()) {
        const auto& p = value.at("protection");
        if (!core::detail::json_string_equal(p.at("protection_kind"), "read_only") || !core::detail::json_string_equal(p.at("range"), "full_mapping") ||
            !p.at("protection_accepted").get<bool>() || p.at("writable").get<bool>() ||
            p.at("executable").get<bool>())
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         ReconstructionPath{path} + ".protection");
        protection = memory::detail::PairwiseMemoryAccess::protection_evidence();
    }
    std::optional<memory::PostRunPlacementEvidence> post;
    if (!value.at("post_run").is_null()) {
        const auto& item = value.at("post_run");
        auto observation = optional_object<memory::RegionPlacementObservation>(
            item.at("observation"), ReconstructionPath{path} + ".post_run.observation", placement_observation);
        auto evaluation = optional_object<memory::RegionPlacementEvaluation>(
            item.at("evaluation"), ReconstructionPath{path} + ".post_run.evaluation", placement_evaluation);
        if (!observation) return std::unexpected{observation.error()};
        if (!evaluation) return std::unexpected{evaluation.error()};
        std::optional<memory::PlacementQualificationState> qualified;
        if (!item.at("qualification_state").is_null()) {
            auto q = qualification(item.at("qualification_state").get<std::string_view>(),
                                   ReconstructionPath{path} + ".post_run.qualification_state");
            if (!q) return std::unexpected{q.error()};
            qualified = *q;
        }
        const auto state_token = item.at("state").get<std::string_view>();
        const bool observed = state_token == "observed";
        if ((!observed && state_token != "skipped" && state_token != "not_applicable") ||
            (observed && (!*observation || !*evaluation || !qualified)) ||
            (!observed && (*observation || *evaluation || qualified)))
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         ReconstructionPath{path} + ".post_run");
        if (observed) {
            if (!request->verify_after() ||
                (*observation)->total_pages() != request->requested_bytes().value() /
                                                   request->page_size().value())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             ReconstructionPath{path} + ".post_run");
            if (!placement_evaluator_domain(**observation))
                return error(TypedReconstructionErrorCode::inconsistent_projection, path + ".post_run");
            auto derived = memory::detail::PlacementEvaluatedRegionAccess::
                evaluate_region_placement(*request, **observation);
            core::detail::ExpectedErrorCleanup derived_cleanup{derived};
            if (!derived || derived->qualification_state != *qualified ||
                derived->evaluation.requested_node_pages() != (*evaluation)->requested_node_pages() ||
                derived->evaluation.unexpected_node_pages() != (*evaluation)->unexpected_node_pages() ||
                derived->evaluation.unresolved_pages() != (*evaluation)->unresolved_pages() ||
                derived->evaluation.observed_ratio() != (*evaluation)->observed_ratio())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             ReconstructionPath{path} + ".post_run");
        }
        post = memory::detail::PairwiseMemoryAccess::post_run(
            observed ? memory::PostRunVerificationState::observed
                     : state_token == "skipped" ? memory::PostRunVerificationState::skipped
                                                 : memory::PostRunVerificationState::not_applicable,
            std::move(*observation), std::move(*evaluation), qualified);
    }
    const auto& cleanup_json = value.at("cleanup");
    const bool attempted = cleanup_json.at("release_attempted").get<bool>();
    const bool succeeded = cleanup_json.at("release_succeeded").get<bool>();
    const bool active = cleanup_json.at("mapping_active_after").get<bool>();
    if (!core::detail::json_string_equal(cleanup_json.at("range"), "full_mapping") ||
        (attempted ? (succeeded == active) : (succeeded || !active)))
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     ReconstructionPath{path} + ".cleanup");
    const auto path_token = value.at("finalization_path").get<std::string_view>();
    memory::RegionFinalizationPath final_path;
    if (path_token == "protected_execution") final_path = memory::RegionFinalizationPath::protected_execution;
    else if (path_token == "unprotected_execution_ineligible") final_path = memory::RegionFinalizationPath::unprotected_execution_ineligible;
    else if (path_token == "unprotected_protection_failed") final_path = memory::RegionFinalizationPath::unprotected_protection_failed;
    else return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath{path} + ".finalization_path");
    const auto disposition_token = value.at("disposition").get<std::string_view>();
    if (disposition_token != "succeeded" && disposition_token != "failed")
        return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath{path} + ".disposition");
    if ((final_path == memory::RegionFinalizationPath::protected_execution) != protection.has_value() ||
        (final_path == memory::RegionFinalizationPath::protected_execution &&
         ((disposition_token == "succeeded" && !post) ||
          (post && (request->verify_after()
                        ? post->state() != memory::PostRunVerificationState::observed
                        : post->state() != memory::PostRunVerificationState::skipped)))) ||
        (final_path != memory::RegionFinalizationPath::protected_execution &&
         (!post || post->state() != memory::PostRunVerificationState::not_applicable)))
        return error(TypedReconstructionErrorCode::inconsistent_projection, ReconstructionPath{path});
    if (disposition_token == "succeeded" && (!attempted || !succeeded || active))
        return error(TypedReconstructionErrorCode::inconsistent_projection, ReconstructionPath{path});
    return memory::detail::PairwiseMemoryAccess::diagnostic(
        std::move(*request), memory::detail::PairwiseMemoryAccess::binding_evidence(),
        memory::detail::PairwiseMemoryAccess::initialization_evidence(prefault), std::move(*pre),
        std::move(protection), std::move(post),
        memory::detail::PairwiseMemoryAccess::cleanup(attempted, succeeded, active), final_path,
        disposition_token == "succeeded" ? memory::RegionFinalizationDisposition::succeeded
                                          : memory::RegionFinalizationDisposition::failed);
}

} // namespace

auto fixed_reconstruct_finalization_component(const nlohmann::json& final_json,
    std::optional<std::uint64_t> operational_error_count)
    -> std::expected<PairwiseFinalizationEvidence, FixedTypedReconstructionError> {
    auto parse_variant = [&](const nlohmann::json& value,
                             measurement::ComparisonVariantSlot expected,
                             ReconstructionPath path)
        -> std::expected<FinalizedVariantEvidence, FixedTypedReconstructionError> {
        const auto token = value.at("slot").get<std::string_view>();
        if (token != (expected == measurement::ComparisonVariantSlot::first ? "first" : "second"))
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         ReconstructionPath{path} + ".slot");
        std::vector<FinalizedWorkerEvidence> workers;
        workers.reserve(value.at("workers").size());
        for (std::size_t index{}; index < value.at("workers").size(); ++index) {
            const auto worker_path = ReconstructionPath{path} + ".workers[" + ReconstructionPath::ordinal(index) + "]";
            const auto& worker_json = value.at("workers")[index];
            auto id = u64(worker_json.at("worker_id"), worker_path + ".worker_id");
            if (!id) return std::unexpected{id.error()};
            if (*id > std::numeric_limits<std::uint32_t>::max() || *id != index)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             worker_path + ".worker_id");
            auto region = region_diagnostic(worker_json.at("region"),
                                            worker_path + ".region", core::WorkerId{static_cast<std::uint32_t>(*id)});
            if (!region) return std::unexpected{region.error()};
            auto ordinal = [&](std::string_view key)
                -> std::expected<std::optional<std::uint64_t>, FixedTypedReconstructionError> {
                if (worker_json.at(key).is_null()) return std::optional<std::uint64_t>{};
                auto parsed = u64(worker_json.at(key), worker_path + "." + key);
                if (!parsed) return std::unexpected{parsed.error()};
                return std::optional<std::uint64_t>{*parsed};
            };
            auto primary = ordinal("primary_error_ordinal");
            auto cleanup = ordinal("cleanup_error_ordinal");
            if (!primary) return std::unexpected{primary.error()};
            if (!cleanup) return std::unexpected{cleanup.error()};
            if ((operational_error_count && *primary && **primary >= *operational_error_count) ||
                (operational_error_count && *cleanup && **cleanup >= *operational_error_count) ||
                (*cleanup && !*primary) || (*primary && *cleanup && **primary >= **cleanup))
                return error(TypedReconstructionErrorCode::inconsistent_projection, worker_path);
            if (const auto* available = region->available()) {
                if ((available->disposition() == memory::RegionFinalizationDisposition::succeeded &&
                     (*primary || *cleanup)) ||
                    (available->disposition() == memory::RegionFinalizationDisposition::failed &&
                     !*primary))
                    return error(TypedReconstructionErrorCode::inconsistent_projection, worker_path);
            } else if (!*primary) {
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             worker_path);
            }
            workers.push_back({core::WorkerId{static_cast<std::uint32_t>(*id)},
                               std::move(*region), *primary, *cleanup});
        }
        return FinalizedVariantEvidence{expected, std::move(workers)};
    };
    auto first = parse_variant(final_json.at("first"), measurement::ComparisonVariantSlot::first,
                               "finalization.first");
    auto second = parse_variant(final_json.at("second"), measurement::ComparisonVariantSlot::second,
                                "finalization.second");
    if (!first) return std::unexpected{first.error()};
    if (!second) return std::unexpected{second.error()};
    if (first->workers.size() != second->workers.size())
        return error(TypedReconstructionErrorCode::inconsistent_projection, "finalization");
    if (first->workers.empty())
        return error(TypedReconstructionErrorCode::inconsistent_projection, "finalization.workers");
    std::vector<std::uint8_t> used(operational_error_count.value_or(0U), 0U);
    std::vector<std::uint64_t> ordinal_order;
    const auto workers = add_wire_json_count(first->workers.size(), second->workers.size());
    const auto ordinals = workers ? multiply_component_stock(*workers, 2U)
        : std::expected<std::size_t, WireJsonError>{std::unexpected{workers.error()}};
    if (!ordinals) throw std::bad_alloc{};
    ordinal_order.reserve(*ordinals);
    for (const auto* variant : {&*first, &*second})
        for (const auto& worker : variant->workers)
            for (const auto ordinal : {worker.primary_error_ordinal,
                                       worker.cleanup_error_ordinal}) {
                if (!ordinal) continue;
                if (operational_error_count && used[*ordinal])
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "finalization.error_ordinals");
                if (operational_error_count) used[*ordinal] = true;
                ordinal_order.push_back(*ordinal);
            }
    for (std::size_t index = 1U; index < ordinal_order.size(); ++index)
        if (ordinal_order[index - 1U] == UINT64_MAX ||
            ordinal_order[index] != ordinal_order[index - 1U] + 1U)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "finalization.error_ordinals");
    return PairwiseFinalizationEvidence{std::move(*first), std::move(*second)};
}

auto validate_normal_finalization(const PairwiseFinalizationEvidence& finalization)
    -> std::expected<void, FixedTypedReconstructionError> {
    for (const auto* variant : {&finalization.first, &finalization.second})
        for (const auto& worker : variant->workers) {
            const auto* available = worker.region.available();
            if (!available || worker.primary_error_ordinal || worker.cleanup_error_ordinal ||
                available->disposition() != memory::RegionFinalizationDisposition::succeeded ||
                !available->cleanup_evidence().release_attempted() ||
                !available->cleanup_evidence().release_succeeded() ||
                available->cleanup_evidence().mapping_active_after() ||
                !available->post_run())
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "finalization.normal_worker");
            if (available->request().verify_after()) {
                if (available->post_run()->state() !=
                        memory::PostRunVerificationState::observed ||
                    available->post_run()->qualification_state() !=
                        memory::PlacementQualificationState::qualified)
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "finalization.normal_worker.post_run");
            } else if (available->post_run()->state() !=
                       memory::PostRunVerificationState::skipped) {
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "finalization.normal_worker.post_run");
            }
        }
    return {};
}

auto reconstruct_terminal_and_finalization_impl(
    const nlohmann::json& body,
    std::span<const core::LinuxCpuId> authenticated_cpus,
    const ReconstructedEvidenceHeader* header = nullptr,
    const ReconstructedObservationBlocks* blocks = nullptr,
    const ReconstructedStoppingAndFailures* stopping = nullptr)
    -> std::expected<ReconstructedTerminalAndFinalization, FixedTypedReconstructionError> {
    try {
        std::optional<ReconstructedEvidenceHeader> owned_header;
        std::optional<ReconstructedObservationBlocks> owned_blocks;
        std::optional<ReconstructedStoppingAndFailures> owned_stopping;
        if (!header) {
            auto made = fixed_reconstruct_evidence_header(body);
            if (!made) return std::unexpected{std::move(made.error())};
            owned_header.emplace(std::move(*made)); header = &*owned_header;
        }
        if (!blocks) {
            auto made = fixed_reconstruct_observation_blocks(body, authenticated_cpus);
            if (!made) return std::unexpected{std::move(made.error())};
            owned_blocks.emplace(std::move(*made)); blocks = &*owned_blocks;
        }
        if (!stopping) {
            auto made = reconstruct_stopping_and_failures_impl(body, authenticated_cpus, blocks);
            if (!made) return std::unexpected{std::move(made.error())};
            owned_stopping.emplace(std::move(*made)); stopping = &*owned_stopping;
        }
        const auto& terminal_json = body.at("terminal");
        const auto kind = terminal_json.at("kind").get<std::string_view>();
        std::optional<PairwiseRunTerminalEvidence> terminal;
        std::uint64_t operational_error_count{};
        if (kind == "normal") {
            const auto& outcome_json = terminal_json.at("repetition_outcome");
            auto experiment = u64(outcome_json.at("experiment_id"), "terminal.repetition_outcome.experiment_id");
            const auto& policy_json = outcome_json.at("policy");
            auto minimum = u64(policy_json.at("minimum_blocks"), "terminal.repetition_outcome.policy.minimum_blocks");
            auto maximum = u64(policy_json.at("maximum_blocks"), "terminal.repetition_outcome.policy.maximum_blocks");
            auto minimum_valid = u64(policy_json.at("minimum_valid_observations_per_variant"), "terminal.repetition_outcome.policy.minimum_valid_observations_per_variant");
            auto completed = u64(outcome_json.at("completed_blocks"), "terminal.repetition_outcome.completed_blocks");
            auto valid_blocks = u64(outcome_json.at("complete_valid_blocks"), "terminal.repetition_outcome.complete_valid_blocks");
            auto first = u64(outcome_json.at("valid_first_variant_observations"), "terminal.repetition_outcome.valid_first_variant_observations");
            auto second = u64(outcome_json.at("valid_second_variant_observations"), "terminal.repetition_outcome.valid_second_variant_observations");
            if (!experiment) return std::unexpected{experiment.error()};
            if (!minimum) return std::unexpected{minimum.error()};
            if (!maximum) return std::unexpected{maximum.error()};
            if (!minimum_valid) return std::unexpected{minimum_valid.error()};
            if (!completed) return std::unexpected{completed.error()};
            if (!valid_blocks) return std::unexpected{valid_blocks.error()};
            if (!first) return std::unexpected{first.error()};
            if (!second) return std::unexpected{second.error()};
            std::uint64_t derived_completed{}, derived_valid{};
            for (const auto& block : blocks->blocks) {
                derived_completed += block.stated_completion != BlockAttemptCompletion::operationally_incomplete;
                derived_valid += block.stated_completion == BlockAttemptCompletion::complete_valid;
            }
            if (*experiment != header->provenance.experiment_id.value() ||
                *minimum != header->repetition_policy.minimum_blocks() ||
                *maximum != header->repetition_policy.maximum_blocks() ||
                *minimum_valid != header->repetition_policy.minimum_valid_observations_per_variant() ||
                policy_json.at("stopping_enabled").get<bool>() != header->repetition_policy.stopping_enabled() ||
                *completed != derived_completed ||
                *valid_blocks != derived_valid || *first != 2U * derived_valid ||
                *second != 2U * derived_valid)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "terminal.repetition_outcome");
            const auto disposition_token = outcome_json.at("disposition").get<std::string_view>();
            measurement::RepetitionDisposition disposition;
            if (disposition_token == "precision_target_reached") disposition = measurement::RepetitionDisposition::precision_target_reached;
            else if (disposition_token == "maximum_blocks_reached") disposition = measurement::RepetitionDisposition::maximum_blocks_reached;
            else if (disposition_token == "selected_work_unavailable") disposition = measurement::RepetitionDisposition::selected_work_unavailable;
            else return error(TypedReconstructionErrorCode::invalid_scalar, "terminal.repetition_outcome.disposition");
            if ((disposition == measurement::RepetitionDisposition::precision_target_reached &&
                 (stopping->stopping_evaluations.empty() ||
                  stopping->stopping_evaluations.back().subsequent_disposition != RepetitionBoundaryDisposition::precision_target_reached)) ||
                (disposition == measurement::RepetitionDisposition::maximum_blocks_reached &&
                 *completed != header->repetition_policy.maximum_blocks()) ||
                (disposition == measurement::RepetitionDisposition::selected_work_unavailable &&
                 (!blocks->blocks.empty() || *completed != 0U)))
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "terminal.repetition_outcome.disposition");
            terminal.emplace(NormalRunTerminalEvidence{
                measurement::detail::MeasurementLeafAccess::repetition_outcome(
                    core::ExperimentId{*experiment}, header->repetition_policy, disposition,
                    *completed, *valid_blocks, *first, *second)});
        } else if (kind == "operational") {
            auto primary = reconstruct_finalized_error(terminal_json.at("primary_error"),
                                                       "terminal.primary_error");
            if (!primary) return std::unexpected{primary.error()};
            std::vector<FinalizedEvidenceError> secondary;
            secondary.reserve(terminal_json.at("secondary_errors").size());
            for (std::size_t index{}; index < terminal_json.at("secondary_errors").size(); ++index) {
                auto current = reconstruct_finalized_error(
                    terminal_json.at("secondary_errors")[index],
                    "terminal.secondary_errors[" + ReconstructionPath::ordinal(index) + "]");
                if (!current) return std::unexpected{current.error()};
                secondary.push_back(std::move(*current));
            }
            std::optional<kernel::DependentLoadLatencyBlockId> active;
            if (!terminal_json.at("active_block_id").is_null()) {
                auto id = u64(terminal_json.at("active_block_id"), "terminal.active_block_id");
                if (!id) return std::unexpected{id.error()};
                if (blocks->blocks.empty() ||
                    blocks->blocks.back().stated_completion != BlockAttemptCompletion::operationally_incomplete ||
                    blocks->blocks.back().plan.block_id().value() != *id)
                    return error(TypedReconstructionErrorCode::inconsistent_projection,
                                 "terminal.active_block_id");
                active.emplace(*id);
            }
            operational_error_count = 1U + secondary.size();
            terminal.emplace(OperationalRunTerminalEvidence{
                std::move(*primary), std::move(secondary), std::move(active)});
        } else return error(TypedReconstructionErrorCode::invalid_scalar, "terminal.kind");

        std::optional<PairwiseFinalizationEvidence> finalization;
        if (!body.at("finalization").is_null()) {
            const auto& final_json = body.at("finalization");
            auto made = fixed_reconstruct_finalization_component(final_json, operational_error_count);
            if (!made) return std::unexpected{std::move(made.error())};
            finalization.emplace(std::move(*made));
        }
        if (!finalization &&
            (!body.at("calibration").is_null() || !body.at("warmup").is_null() ||
             !blocks->blocks.empty() || !stopping->stopping_evaluations.empty()))
            return error(TypedReconstructionErrorCode::inconsistent_projection, "finalization");
        if (kind == "normal" && !finalization)
            return error(TypedReconstructionErrorCode::inconsistent_projection, "finalization");
        if (kind == "normal") {
            auto checked = validate_normal_finalization(*finalization);
            if (!checked) return std::unexpected{checked.error()};
        }
        if (finalization) {
            const std::array<std::span<const FinalizedWorkerEvidence>, 2U> admitted{
                finalization->first.workers, finalization->second.workers};
            for (const auto& block : blocks->blocks)
                for (const auto& factual : block.observations) {
                    const auto variant = factual.slot.comparison_variant() ==
                                                 measurement::ComparisonVariantSlot::first
                                             ? 0U : 1U;
                    const bool associated = std::visit([&](const auto& observation) {
                        const auto workers = observation.workers();
                        if (workers.size() != admitted[variant].size() ||
                            workers.size() != factual.placements.size() ||
                            workers.size() != factual.migrations.size()) return false;
                        for (std::size_t index{}; index < workers.size(); ++index)
                            if (workers[index].worker_id() != admitted[variant][index].worker_id ||
                                factual.placements[index].worker_id() != admitted[variant][index].worker_id ||
                                factual.migrations[index].worker_id() != admitted[variant][index].worker_id)
                                return false;
                        return true;
                    }, factual.observation);
                    if (!associated)
                        return error(TypedReconstructionErrorCode::inconsistent_projection,
                                     "finalization.observation_workers");
                }
            for (std::size_t variant{}; variant < admitted.size(); ++variant)
                for (std::size_t worker_index{}; worker_index < admitted[variant].size(); ++worker_index) {
                    const auto* available = admitted[variant][worker_index].region.available();
                    if (!available) continue;
                    const auto qualification_state = available->pre_run().qualification_state();
                    for (const auto& block : blocks->blocks)
                        for (const auto& factual : block.observations) {
                            const auto factual_variant = factual.slot.comparison_variant() ==
                                measurement::ComparisonVariantSlot::first ? 0U : 1U;
                            if (factual_variant != variant) continue;
                            const auto state = factual.placements[worker_index].state();
                            if ((available->request().verify_before() &&
                                 (!qualification_state || available->pre_run().state() !=
                                      memory::PreRunDiagnosticState::observed)) ||
                                (!available->request().verify_before() &&
                                 (qualification_state || available->pre_run().state() !=
                                      memory::PreRunDiagnosticState::skipped)) ||
                                (qualification_state == memory::PlacementQualificationState::qualified &&
                                 state != measurement::WorkerPlacementValidityState::qualified) ||
                                (qualification_state == memory::PlacementQualificationState::not_qualified &&
                                 state != measurement::WorkerPlacementValidityState::not_qualified) ||
                                (!qualification_state &&
                                 state != measurement::WorkerPlacementValidityState::not_applicable))
                                return error(TypedReconstructionErrorCode::inconsistent_projection,
                                             "finalization.placement_association");
                        }
                }
        }
        return ReconstructedTerminalAndFinalization{std::move(*terminal), std::move(finalization)};
    } catch (const nlohmann::json::out_of_range& exception) {
        return error(TypedReconstructionErrorCode::missing_member, ReconstructionPath::reference(exception.what()));
    } catch (const nlohmann::json::type_error& exception) {
        return error(TypedReconstructionErrorCode::invalid_scalar, ReconstructionPath::reference(exception.what()));
    }
}

auto fixed_reconstruct_terminal_and_finalization(const nlohmann::json& body)
    -> std::expected<ReconstructedTerminalAndFinalization, FixedTypedReconstructionError> {
    return reconstruct_terminal_and_finalization_impl(body, {});
}

auto fixed_reconstruct_terminal_and_finalization(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, FixedTypedReconstructionError> {
    return fixed_reconstruct_terminal_and_finalization(bounded_body.value());
}

auto fixed_reconstruct_finalized_evidence(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FinalizedPairwiseRunEvidence, FixedTypedReconstructionError> {
    auto header = fixed_reconstruct_evidence_header(bounded_body);
    if (!header) return std::unexpected{std::move(header.error())};
    auto calibration_warmup = fixed_reconstruct_calibration_warmup(bounded_body);
    if (!calibration_warmup)
        return std::unexpected{std::move(calibration_warmup.error())};
    auto blocks = fixed_reconstruct_observation_blocks(bounded_body, authenticated_cpus);
    if (!blocks) return std::unexpected{std::move(blocks.error())};
    auto stopping = reconstruct_stopping_and_failures_impl(
        bounded_body.value(), authenticated_cpus, &*blocks);
    if (!stopping) return std::unexpected{std::move(stopping.error())};
    auto terminal = reconstruct_terminal_and_finalization_impl(
        bounded_body.value(), authenticated_cpus, &*header, &*blocks, &*stopping);
    if (!terminal) return std::unexpected{std::move(terminal.error())};

    const auto maximum_blocks = header->repetition_policy.maximum_blocks();
    if (maximum_blocks > std::vector<PairwiseBlockAttemptEvidence>{}.max_size() ||
        maximum_blocks > std::vector<RunnerStoppingEvaluationEvidence>{}.max_size())
        throw std::bad_alloc{};
    auto storage = runtime::detail::PreparedPairwiseLedgerStorage::prepare(static_cast<std::size_t>(maximum_blocks));
    if (!storage) throw std::bad_alloc{};
    auto ledger = runtime::detail::PairwiseLedgerPreparedAccess::create(
        std::move(header->provenance), std::move(header->statistics_policy),
        std::move(header->monitoring_policy), std::move(header->repetition_policy),
        std::move(header->comparison_order), std::move(*storage));
    if (!ledger)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "evidence.ledger.create");
    if (calibration_warmup->calibration) {
        auto associated = ledger->associate_calibration(
            std::move(*calibration_warmup->calibration));
        if (!associated)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "calibration.ledger_association");
    }
    if (calibration_warmup->warmup) {
        auto associated = ledger->associate_warmup(
            std::move(*calibration_warmup->warmup));
        if (!associated)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "warmup.ledger_association");
    }

    std::size_t failure_index{};
    std::size_t stopping_index{};
    for (std::size_t index{}; index < blocks->blocks.size(); ++index) {
        auto& block = blocks->blocks[index];
        const auto block_id = block.plan.block_id();
        std::optional<FailedObservationEvidence> failed;
        if (failure_index < stopping->failed_observations.size() &&
            stopping->failed_observations[failure_index].block_index == index) {
            failed.emplace(std::move(
                stopping->failed_observations[failure_index].evidence));
            ++failure_index;
        }
        auto appended = ledger->append_block(PairwiseBlockAttemptEvidence{
            std::move(block.plan), std::move(block.observations),
            std::move(failed), block.stated_completion});
        if (!appended)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "blocks[" + ReconstructionPath::ordinal(index) + "].ledger_association");
        while (stopping_index < stopping->stopping_evaluations.size() &&
               stopping->stopping_evaluations[stopping_index].boundary_block_id ==
                   block_id) {
            auto stopping_appended = ledger->append_stopping_evaluation(
                std::move(stopping->stopping_evaluations[stopping_index]));
            if (!stopping_appended)
                return error(TypedReconstructionErrorCode::inconsistent_projection,
                             "stopping_evaluations[" +
                                 ReconstructionPath::ordinal(stopping_index) +
                                 "].ledger_association");
            ++stopping_index;
        }
    }
    if (failure_index != stopping->failed_observations.size())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "blocks.failed_observation_association");
    if (stopping_index != stopping->stopping_evaluations.size())
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "stopping_evaluations.ledger_association");
    if (terminal->finalization) {
        auto associated = ledger->associate_finalization(
            std::move(*terminal->finalization));
        if (!associated)
            return error(TypedReconstructionErrorCode::inconsistent_projection,
                         "finalization.ledger_association");
    }
    bool terminal_recorded{};
    if (auto* normal = std::get_if<NormalRunTerminalEvidence>(&terminal->terminal)) {
        terminal_recorded = ledger->record_normal_terminal(std::move(*normal)).has_value();
    } else {
        terminal_recorded = ledger->record_operational_terminal(
            std::move(std::get<OperationalRunTerminalEvidence>(terminal->terminal))).has_value();
    }
    if (!terminal_recorded)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "terminal.ledger_association");
    auto finalized = std::move(*ledger).finalize();
    if (!finalized)
        return error(TypedReconstructionErrorCode::inconsistent_projection,
                     "evidence.ledger.finalize");
    return std::move(*finalized);
}

namespace {

template <class Json = nlohmann::json>
auto interval_projection(const measurement::MeasurementInterval& value) -> Json {
    return component_projection::object<Json>([&](auto& object_0) {
        component_projection::set(object_0, "start", projection_decimal(value.start().nanoseconds()));
        component_projection::set(object_0, "end", projection_decimal(value.end().nanoseconds()));
        component_projection::set(object_0, "elapsed_nanoseconds", projection_decimal(value.elapsed().value()));
    });
}
template <class Json = nlohmann::json>
auto bootstrap_projection(const statistics::BootstrapPolicy& value) -> Json {
    return component_projection::object<Json>([&](auto& object_1) {
        component_projection::set(object_1, "replicate_count",
                               projection_decimal(value.replicate_count().value()));
        component_projection::set(object_1, "confidence", projection_binary64(value.confidence().value()));
        component_projection::set(object_1, "seed", projection_decimal(value.seed().value()));
    });
}
template <class Json = nlohmann::json>
auto repetition_policy_projection(const measurement::RepetitionPolicy& value)
    -> Json {
    return component_projection::object<Json>([&](auto& object_2) {
        component_projection::set(object_2, "minimum_blocks", projection_decimal(value.minimum_blocks()));
        component_projection::set(object_2, "maximum_blocks", projection_decimal(value.maximum_blocks()));
        component_projection::set(object_2, "minimum_valid_observations_per_variant",
                               projection_decimal(value.minimum_valid_observations_per_variant()));
        component_projection::set(object_2, "stopping_enabled", value.stopping_enabled());
    });
}
auto comparison_strategy_token(measurement::ComparisonOrderStrategy value) -> std::string_view {
    switch (value) {
    case measurement::ComparisonOrderStrategy::randomized_abba: return "randomized_abba";
    case measurement::ComparisonOrderStrategy::randomized_baab: return "randomized_baab";
    case measurement::ComparisonOrderStrategy::randomized_balanced: return "randomized_balanced";
    }
    std::unreachable();
}
template <class Json = nlohmann::json>
auto comparison_order_projection(const measurement::ComparisonOrderPolicy& value)
    -> Json {
    return component_projection::object<Json>([&](auto& object_3) {
        component_projection::set(object_3, "strategy", comparison_strategy_token(value.strategy()));
        component_projection::set(object_3, "seed", projection_decimal(value.comparison_order_seed()));
    });
}
auto channel_disposition_token(monitoring::MonitoringChannelDisposition value)
    -> std::string_view {
    switch (value) {
    case monitoring::MonitoringChannelDisposition::disabled: return "disabled";
    case monitoring::MonitoringChannelDisposition::optional: return "optional";
    case monitoring::MonitoringChannelDisposition::required: return "required";
    }
    std::unreachable();
}
template <class Json = nlohmann::json>
auto monitoring_policy_projection(const monitoring::MonitoringExecutionPolicy& value)
    -> Json {
    Json interval_value = nullptr;
    component_projection::Cleanup interval_value_cleanup{interval_value};
    if (value.during_interval()) interval_value = projection_decimal(value.during_interval()->value());
    return component_projection::object<Json>([&](auto& object_4) {
        component_projection::set(object_4, "page_faults",
                               channel_disposition_token(
                                   value.disposition(monitoring::MonitoringChannel::page_faults)));
        component_projection::set(object_4, "context_switches",
                               channel_disposition_token(value.disposition(
                                   monitoring::MonitoringChannel::context_switches)));
        component_projection::set(object_4, "temperature",
                               channel_disposition_token(
                                   value.disposition(monitoring::MonitoringChannel::temperature)));
        component_projection::set(
            object_4, "frequency",
            channel_disposition_token(value.disposition(monitoring::MonitoringChannel::frequency)));
        component_projection::set(object_4, "before_observation", value.before_observation());
        component_projection::set(object_4, "after_observation", value.after_observation());
        component_projection::set(object_4, "during_observation", value.during_observation());
        component_projection::set(object_4, "during_interval", std::move(interval_value));
        component_projection::set(object_4, "optional_unsupported",
                               value.optional_unsupported() ==
                                       monitoring::OptionalUnavailableSourcePolicy::warn
                                   ? "warn"
                                   : "fail");
    });
}
auto calibration_disposition_token(measurement::CalibrationDisposition value)
    -> std::string_view {
    switch (value) {
    case measurement::CalibrationDisposition::selected: return "selected";
    case measurement::CalibrationDisposition::minimum_candidate_above_maximum_duration:
        return "minimum_candidate_above_maximum_duration";
    case measurement::CalibrationDisposition::maximum_candidate_below_minimum_duration:
        return "maximum_candidate_below_minimum_duration";
    case measurement::CalibrationDisposition::no_candidate_in_acceptable_band:
        return "no_candidate_in_acceptable_band";
    }
    std::unreachable();
}
template <class Json, class Outcome>
auto calibration_projection_t(const Outcome& value, std::string_view kind) -> Json {
    const auto& policy = value.duration_policy();
    Json empty = Json::array();
    component_projection::Cleanup empty_cleanup{empty};
    for (const auto& trial : value.empty_harness_trials())
        component_projection::append(empty, interval_projection<Json>(trial.interval()));
    Json candidates = Json::array();
    component_projection::Cleanup candidates_cleanup{candidates};
    for (const auto& trial : value.candidate_trials())
        component_projection::append(
            candidates, component_projection::object<Json>([&](auto& object_5) {
                component_projection::set(object_5, "candidate", projection_decimal(trial.candidate().value()));
                component_projection::set(object_5, "interval", interval_projection<Json>(trial.interval()));
            }));
    Json selected = nullptr;
    component_projection::Cleanup selected_cleanup{selected};
    if (value.selected_candidate()) selected = projection_decimal(value.selected_candidate()->value());
    return component_projection::object<Json>([&](auto& object_6) {
        component_projection::set(object_6, "kind", kind);
        component_projection::set(
            object_6, "duration_policy",
            component_projection::object<Json>([&](auto& object_7) {
                component_projection::set(object_7, "minimum_duration",
                                       projection_decimal(policy.minimum_duration().value()));
                component_projection::set(object_7, "target_duration",
                                       projection_decimal(policy.target_duration().value()));
                component_projection::set(object_7, "maximum_duration",
                                       projection_decimal(policy.maximum_duration().value()));
            }));
        component_projection::set(object_6, "empty_harness_trials", std::move(empty));
        component_projection::set(object_6, "timer_overhead_estimate",
                               projection_decimal(value.timer_overhead_estimate().duration().value()));
        component_projection::set(object_6, "candidate_trials", std::move(candidates));
        component_projection::set(object_6, "disposition",
                               calibration_disposition_token(value.disposition()));
        component_projection::set(object_6, "selected_candidate", std::move(selected));
    });
}
template <class Json = nlohmann::json>
auto calibration_projection(const PairwiseCalibrationEvidence& value) -> Json {
    return std::visit([](const auto& branch) {
        using T = std::remove_cvref_t<decltype(branch)>;
        return calibration_projection_t<Json>(branch,
            std::same_as<T, measurement::SequentialReadBandwidthCalibrationOutcome>
                ? std::string_view{"bandwidth"} : std::string_view{"latency"});
    }, value);
}
auto warmup_disposition_token(measurement::WarmupDisposition value) -> std::string_view {
    switch (value) {
    case measurement::WarmupDisposition::skipped: return "skipped";
    case measurement::WarmupDisposition::stabilized: return "stabilized";
    case measurement::WarmupDisposition::maximum_trials_reached_without_stability:
        return "maximum_trials_reached_without_stability";
    }
    std::unreachable();
}
template <class Json, class Outcome>
auto warmup_projection_t(const Outcome& value, std::string_view kind) -> Json {
    const auto& policy = value.policy();
    const auto threshold = policy.stability_threshold();
    Json trials = Json::array();
    component_projection::Cleanup trials_cleanup{trials};
    for (const auto& trial : value.trials())
        component_projection::append(trials, interval_projection<Json>(trial.interval()));
    Json evaluation = nullptr;
    component_projection::Cleanup evaluation_cleanup{evaluation};
    if (const auto final = value.final_stability_evaluation())
        evaluation = component_projection::object<Json>([&](auto& object_8) {
            component_projection::set(object_8, "median_duration",
                                   projection_decimal(final->median_duration().value()));
            component_projection::set(object_8, "maximum_absolute_deviation",
                                   projection_decimal(final->maximum_absolute_deviation().value()));
            component_projection::set(object_8, "stable", final->stable());
        });
    return component_projection::object<Json>([&](auto& object_9) {
        component_projection::set(object_9, "kind", kind);
        component_projection::set(object_9, "policy",
                               component_projection::object<Json>([&](auto& object_10) {
                                   component_projection::set(object_10, "minimum_trials",
                                                          projection_decimal(policy.minimum_trials()));
                                   component_projection::set(object_10, "maximum_trials",
                                                          projection_decimal(policy.maximum_trials()));
                                   component_projection::set(object_10, "stability_window",
                                                          projection_decimal(policy.stability_window()));
                                   component_projection::set(object_10, "threshold_numerator",
                                                          projection_decimal(threshold.numerator()));
                                   component_projection::set(object_10, "threshold_denominator",
                                                          projection_decimal(threshold.denominator()));
                               }));
        component_projection::set(object_9, "selected_work", projection_decimal(value.selected_work().value()));
        component_projection::set(object_9, "trials", std::move(trials));
        component_projection::set(object_9, "disposition",
                               warmup_disposition_token(value.disposition()));
        component_projection::set(object_9, "final_stability_evaluation", std::move(evaluation));
    });
}
template <class Json = nlohmann::json>
auto warmup_projection(const PairwiseWarmupEvidence& value) -> Json {
    return std::visit([](const auto& branch) {
        using T = std::remove_cvref_t<decltype(branch)>;
        return warmup_projection_t<Json>(branch,
            std::same_as<T, measurement::SequentialReadBandwidthWarmupOutcome>
                ? std::string_view{"bandwidth"} : std::string_view{"latency"});
    }, value);
}

auto slot_token(measurement::ComparisonVariantSlot value) -> std::string_view {
    return value == measurement::ComparisonVariantSlot::first ? "first" : "second";
}
template <class Json = nlohmann::json>
auto slot_projection(const measurement::ScheduledObservationSlot& value) -> Json {
    return component_projection::object<Json>([&](auto& object_11) {
        component_projection::set(object_11, "block_id", projection_decimal(value.block_id().value()));
        component_projection::set(object_11, "slot_index", projection_decimal(value.slot_index()));
        component_projection::set(object_11, "variant_slot", slot_token(value.comparison_variant()));
    });
}
template <class Json = nlohmann::json>
auto plan_projection(const measurement::MeasurementBlockPlan& value) -> Json {
    Json observations = Json::array();
    component_projection::Cleanup observations_cleanup{observations};
    for (const auto& slot : value.observations())
        component_projection::append(observations, slot_projection<Json>(slot));
    return component_projection::object<Json>([&](auto& object_12) {
        component_projection::set(object_12, "block_id", projection_decimal(value.block_id().value()));
        component_projection::set(object_12, "comparison_order",
                               comparison_order_projection<Json>(value.comparison_order()));
        component_projection::set(object_12, "preparation",
                               component_projection::object<Json>([&](auto& object_13) {
                                   component_projection::set(
                                       object_13, "calibration_required",
                                       value.preparation().calibration_required());
                                   component_projection::set(object_13, "warmup_required",
                                                          value.preparation().warmup_required());
                               }));
        component_projection::set(object_12, "observations", std::move(observations));
    });
}
auto validity_outcome_token(measurement::ObservationValidityCheckOutcome value)
    -> std::string_view {
    switch (value) {
    case measurement::ObservationValidityCheckOutcome::pass: return "pass";
    case measurement::ObservationValidityCheckOutcome::fail: return "fail";
    case measurement::ObservationValidityCheckOutcome::warning: return "warning";
    case measurement::ObservationValidityCheckOutcome::unavailable: return "unavailable";
    case measurement::ObservationValidityCheckOutcome::not_applicable: return "not_applicable";
    }
    std::unreachable();
}
template <class Json = nlohmann::json>
auto validity_projection(const measurement::ObservationValidityEvidence& value) -> Json {
    Json checks = Json::array();
    component_projection::Cleanup checks_cleanup{checks};
    for (const auto& check : value.checks())
        component_projection::append(
            checks, component_projection::object<Json>([&](auto& object_14) {
                component_projection::set(object_14, "id",
                                       projection_decimal(static_cast<std::uint64_t>(check.id())));
                component_projection::set(object_14, "outcome",
                                       validity_outcome_token(check.outcome()));
            }));
    const auto& timer = value.timer_overhead();
    Json fraction = nullptr;
    component_projection::Cleanup fraction_cleanup{fraction};
    if (timer.fraction())
        fraction = component_projection::object<Json>([&](auto& object_15) {
            component_projection::set(object_15, "numerator_nanoseconds",
                                   projection_decimal(timer.fraction()->numerator_nanoseconds()));
            component_projection::set(object_15, "denominator_nanoseconds",
                                   projection_decimal(timer.fraction()->denominator_nanoseconds()));
        });
    return component_projection::object<Json>([&](auto& object_16) {
        component_projection::set(object_16, "observation_id",
                               projection_decimal(value.observation_id().value()));
        component_projection::set(
            object_16, "disposition",
            value.disposition() == measurement::ObservationValidityDisposition::valid ? "valid"
                                                                                      : "invalid");
        component_projection::set(object_16, "checks", std::move(checks));
        component_projection::set(
            object_16, "policy", component_projection::object<Json>([&](auto& object_17) {
                component_projection::set(
                    object_17, "maximum_harness_overhead",
                    component_projection::object<Json>([&](auto& object_18) {
                        component_projection::set(
                            object_18, "numerator",
                            projection_decimal(value.policy().maximum_harness_overhead().numerator()));
                        component_projection::set(
                            object_18, "denominator",
                            projection_decimal(value.policy().maximum_harness_overhead().denominator()));
                    }));
            }));
        component_projection::set(
            object_16, "timer_overhead",
            component_projection::object<Json>([&](auto& object_19) {
                component_projection::set(object_19, "estimate",
                                       projection_decimal(timer.estimate().duration().value()));
                component_projection::set(
                    object_19, "threshold",
                    component_projection::object<Json>([&](auto& object_20) {
                        component_projection::set(object_20, "numerator",
                                               projection_decimal(timer.threshold().numerator()));
                        component_projection::set(object_20, "denominator",
                                               projection_decimal(timer.threshold().denominator()));
                    }));
                component_projection::set(object_19, "fraction", std::move(fraction));
            }));
    });
}
template <class Json = nlohmann::json>
auto measurement_projection(const measurement::MeasurementObservation& value) -> Json {
    return std::visit([](const auto& measured) -> Json {
        using T = std::remove_cvref_t<decltype(measured)>;
        Json workers = Json::array();
        component_projection::Cleanup workers_cleanup{workers};
        if constexpr (std::same_as<T, measurement::SequentialReadBandwidthObservation>) {
            for (const auto& worker : measured.workers()) {
                const auto verification = worker.verification();
                component_projection::append(
                    workers, component_projection::object<Json>([&](auto& object_21) {
                        component_projection::set(object_21, "worker_id",
                                               projection_decimal(worker.worker_id().value()));
                        component_projection::set(object_21, "interval",
                                               interval_projection<Json>(worker.interval()));
                        component_projection::set(
                            object_21, "work",
                            component_projection::object<Json>([&](auto& object_22) {
                                component_projection::set(
                                    object_22, "logical_bytes_read",
                                    projection_decimal(worker.work().logical_bytes_read().value()));
                            }));
                        component_projection::set(
                            object_21, "verification",
                            component_projection::object<Json>([&](auto& object_23) {
                                component_projection::set(object_23, "expected",
                                                       projection_decimal(verification.expected().value()));
                                component_projection::set(object_23, "actual",
                                                       projection_decimal(verification.actual().value()));
                                component_projection::set(object_23, "matched",
                                                       verification.matched());
                            }));
                    }));
            }
            return component_projection::object<Json>([&](auto& object_24) {
                component_projection::set(object_24, "kind", "bandwidth");
                component_projection::set(object_24, "observation_id",
                                       projection_decimal(measured.observation_id().value()));
                component_projection::set(object_24, "experiment_id",
                                       projection_decimal(measured.experiment_id().value()));
                component_projection::set(object_24, "block_id", projection_decimal(measured.block_id().value()));
                component_projection::set(object_24, "implementation_id", "scalar_u64_sequential_v1");
                component_projection::set(
                    object_24, "configuration",
                    component_projection::object<Json>([&](auto& object_25) {
                        component_projection::set(
                            object_25, "working_set_per_worker",
                            projection_decimal(measured.configuration().working_set_per_worker().value()));
                    }));
                component_projection::set(object_24, "pass_count",
                                       projection_decimal(measured.pass_count().value()));
                component_projection::set(object_24, "workers", std::move(workers));
                component_projection::set(object_24, "enclosing_interval",
                                       interval_projection<Json>(measured.enclosing_interval()));
                component_projection::set(object_24, "aggregate_work",
                                       projection_decimal(measured.aggregate_work().value()));
            });
        } else {
            for (const auto& worker : measured.workers()) {
                const auto schedule = worker.schedule(); const auto verification = worker.verification();
                component_projection::append(
                    workers, component_projection::object<Json>([&](auto& object_26) {
                        component_projection::set(object_26, "worker_id",
                                               projection_decimal(worker.worker_id().value()));
                        component_projection::set(object_26, "interval",
                                               interval_projection<Json>(worker.interval()));
                        component_projection::set(
                            object_26, "schedule",
                            component_projection::object<Json>([&](auto& object_27) {
                                component_projection::set(object_27, "start_index",
                                                       projection_decimal(schedule.start_index()));
                                component_projection::set(object_27, "step_count",
                                                       projection_decimal(schedule.step_count().value()));
                            }));
                        component_projection::set(object_26, "completed_work",
                                               projection_decimal(worker.completed_work().value()));
                        component_projection::set(
                            object_26, "verification",
                            component_projection::object<Json>([&](auto& object_28) {
                                component_projection::set(object_28, "expected",
                                                       projection_decimal(verification.expected().value()));
                                component_projection::set(object_28, "actual",
                                                       projection_decimal(verification.actual().value()));
                                component_projection::set(object_28, "matched",
                                                       verification.matched());
                            }));
                    }));
            }
            const auto seed = measured.configuration().pointer_cycle_seed();
            Json seed_value = nullptr;
            component_projection::Cleanup seed_value_cleanup{seed_value};
            if (seed.mode() == kernel::PointerCycleSeedMode::explicit_value)
                seed_value = projection_decimal(*seed.value());
            return component_projection::object<Json>([&](auto& object_29) {
                component_projection::set(object_29, "kind", "latency");
                component_projection::set(object_29, "observation_id",
                                       projection_decimal(measured.observation_id().value()));
                component_projection::set(object_29, "experiment_id",
                                       projection_decimal(measured.experiment_id().value()));
                component_projection::set(object_29, "block_id", projection_decimal(measured.block_id().value()));
                component_projection::set(object_29, "implementation_id",
                                       "scalar_u64_random_cycle_v1");
                component_projection::set(
                    object_29, "configuration",
                    component_projection::object<Json>([&](auto& object_30) {
                        component_projection::set(
                            object_30, "working_set",
                            projection_decimal(measured.configuration().working_set().value()));
                        component_projection::set(
                            object_30, "pointer_cycle_seed",
                            component_projection::object<Json>([&](auto& object_31) {
                                component_projection::set(
                                    object_31, "mode",
                                    seed.mode() == kernel::PointerCycleSeedMode::explicit_value
                                        ? "explicit_value"
                                        : "derive_per_experiment");
                                component_projection::set(object_31, "value", std::move(seed_value));
                            }));
                    }));
                component_projection::set(object_29, "workers", std::move(workers));
                component_projection::set(object_29, "enclosing_interval",
                                       interval_projection<Json>(measured.enclosing_interval()));
                component_projection::set(object_29, "aggregate_work",
                                       projection_decimal(measured.aggregate_work().value()));
            });
        }
    }, value);
}
[[maybe_unused]] auto placement_state_token(measurement::WorkerPlacementValidityState value) -> std::string_view {
    switch (value) { case measurement::WorkerPlacementValidityState::qualified:return "qualified";
    case measurement::WorkerPlacementValidityState::not_qualified:return "not_qualified";
    case measurement::WorkerPlacementValidityState::unavailable:return "unavailable";
    case measurement::WorkerPlacementValidityState::not_applicable:return "not_applicable"; }
    std::unreachable();
}
[[maybe_unused]] auto migration_state_token(measurement::WorkerMigrationValidityState value) -> std::string_view {
    switch (value) { case measurement::WorkerMigrationValidityState::not_migrated:return "not_migrated";
    case measurement::WorkerMigrationValidityState::migrated:return "migrated";
    case measurement::WorkerMigrationValidityState::unavailable:return "unavailable";
    case measurement::WorkerMigrationValidityState::not_applicable:return "not_applicable"; }
    std::unreachable();
}
auto finalized_code_token(PairwiseFailureCode code) -> std::string_view {
    switch (code) {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(name, ...) case PairwiseFailureCode::name: return #name;
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    case PairwiseFailureCode::count: break;
    }
    std::unreachable();
}
template <class E, std::size_t N>
auto indexed_token(E value, const std::array<std::string_view, N>& tokens) -> std::string_view {
    return tokens[static_cast<std::size_t>(value)];
}
template <class Json = nlohmann::json>
auto flat_context_projection(const PairwiseFlatFailureContext& context) -> Json {
    return std::visit([](const auto& value) -> Json {
        using T=std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, NoPairwiseFailureContext>)
            return component_projection::object<Json>(
                [&](auto& object_32) { component_projection::set(object_32, "kind", "none"); });
        else if constexpr (std::same_as<T, LifecyclePhaseContext>)
            return component_projection::object<Json>([&](auto& object_33) {
                component_projection::set(object_33, "kind", "lifecycle_phase");
                component_projection::set(object_33, "phase",
                                       indexed_token(value.phase, phase_tokens));
            });
        else if constexpr (std::same_as<T, ExpectedObservedCountContext>)
            return component_projection::object<Json>([&](auto& object_34) {
                component_projection::set(object_34, "kind", "expected_observed_count");
                component_projection::set(object_34, "expected", projection_decimal(value.expected.value));
                component_projection::set(object_34, "observed", projection_decimal(value.observed.value));
            });
        else if constexpr (std::same_as<T, ExpectedObservedOrdinalContext>)
            return component_projection::object<Json>([&](auto& object_35) {
                component_projection::set(object_35, "kind", "expected_observed_ordinal");
                component_projection::set(object_35, "expected", projection_decimal(value.expected.value));
                component_projection::set(object_35, "observed", projection_decimal(value.observed.value));
            });
        else if constexpr (std::same_as<T, WorkerAssociationContext>)
            return component_projection::object<Json>([&](auto& object_36) {
                component_projection::set(object_36, "kind", "worker_association");
                component_projection::set(object_36, "role",
                                       value.role == PairwiseRole::first ? "first" : "second");
                component_projection::set(object_36, "worker_ordinal",
                                       projection_decimal(value.worker_ordinal.value));
            });
        else if constexpr(std::same_as<T,KernelIdentityContext>) {
            constexpr auto phases=std::to_array<std::string_view>({"none","pass_count_preparation","work_accounting","execution_input_validation","execution_validation","start_state_schedule","terminal_verification_preparation"});
            return component_projection::object<Json>([&](auto& object_37) {
                component_projection::set(object_37, "kind", "kernel_identity");
                component_projection::set(object_37, "family",
                                       value.family == PairwiseKernelFamily::bandwidth ? "bandwidth"
                                                                                       : "latency");
                component_projection::set(
                    object_37, "implementation",
                    value.implementation == PairwiseKernelImplementation::scalar_u64_sequential_v1
                        ? "scalar_u64_sequential_v1"
                        : "scalar_u64_random_cycle_v1");
                component_projection::set(object_37, "kernel_phase",
                                       indexed_token(value.phase, phases));
            });
        } else if constexpr (std::same_as<T, VerificationPhaseContext>)
            return component_projection::object<Json>([&](auto& object_38) {
                component_projection::set(object_38, "kind", "verification_phase");
                component_projection::set(object_38, "verification_phase",
                                       value.phase == PairwiseVerificationPhase::before ? "before"
                                                                                        : "after");
            });
        else if constexpr (std::same_as<T, MonitoringChannelContext>) {
            constexpr auto tokens = std::to_array<std::string_view>(
                {"page_faults", "context_switches", "temperature", "frequency"});
            return component_projection::object<Json>([&](auto& object_39) {
                component_projection::set(object_39, "kind", "monitoring_channel");
                component_projection::set(object_39, "channel", indexed_token(value.channel, tokens));
            });
        } else if constexpr (std::same_as<T, MissingOperationContext>) {
            constexpr auto tokens = std::to_array<std::string_view>(
                {"now", "bind_service_cpu", "support", "read", "clock_now"});
            return component_projection::object<Json>([&](auto& object_40) {
                component_projection::set(object_40, "kind", "missing_operation");
                component_projection::set(object_40, "missing_operation",
                                       indexed_token(value.operation, tokens));
            });
        } else if constexpr (std::same_as<T, MemoryProtectionStateContext>) {
            constexpr auto quals =
                std::to_array<std::string_view>({"qualified", "skipped", "not_qualified"});
            return component_projection::object<Json>([&](auto& object_41) {
                component_projection::set(object_41, "kind", "memory_protection_state");
                component_projection::set(object_41, "protection", "read_only");
                component_projection::set(object_41, "range", "full_mapping");
                component_projection::set(object_41, "qualification",
                                       indexed_token(value.qualification, quals));
                component_projection::set(object_41, "reason",
                                       indexed_token(value.reason, reason_tokens));
            });
        } else if constexpr (std::same_as<T, RequestedProtectionAdviceContext>) {
            constexpr auto protections =
                std::to_array<std::string_view>({"unspecified", "read_write", "read_only"});
            constexpr auto advice = std::to_array<std::string_view>(
                {"none", "base_pages", "huge_pages", "no_huge_pages"});
            return component_projection::object<Json>([&](auto& object_42) {
                component_projection::set(object_42, "kind", "requested_protection_advice");
                component_projection::set(object_42, "protection",
                                       indexed_token(value.protection, protections));
                component_projection::set(object_42, "advice", indexed_token(value.advice, advice));
            });
        } else if constexpr (std::same_as<T, PairwiseMonitoringCapacityAcquisitionContext>)
            return component_projection::object<Json>([&](auto& object_43) {
                component_projection::set(object_43, "kind", "capacity_acquisition");
                component_projection::set(
                    object_43, "leg",
                    value.leg == PairwiseMonitoringCapacityAcquisitionLeg::first_controller
                        ? "first_controller"
                        : "second_runner");
                component_projection::set(object_43, "uncertain", value.uncertain);
            });
        else {
            const auto optional = [](const auto& count) -> Json {
                return count ? Json(projection_decimal(count->value)) : Json(nullptr);
            };
            constexpr auto roles = std::to_array<std::string_view>({"unscoped", "initial", "scheduled", "final"});
            return component_projection::object<Json>([&](auto& object) {
                component_projection::set(object, "kind", "perf_loss");
                component_projection::set(object, "record", component_projection::object<Json>([&](auto& item) {
                    component_projection::set(item, "loss_type", value.record.loss_type == PairwisePerfLossType::lost ? "lost" : "lost_samples");
                    component_projection::set(item, "header_size", projection_decimal(value.record.header_size.value));
                    component_projection::set(item, "lost_count", optional(value.record.lost_count));
                }));
                component_projection::set(object, "ring", component_projection::object<Json>([&](auto& item) {
                    component_projection::set(item, "captured_head", projection_decimal(value.ring.captured_head.value));
                    component_projection::set(item, "captured_tail", projection_decimal(value.ring.captured_tail.value));
                    component_projection::set(item, "ring_bytes", projection_decimal(value.ring.ring_bytes.value));
                }));
                component_projection::set(object, "drain", component_projection::object<Json>([&](auto& item) {
                    component_projection::set(item, "role", indexed_token(value.drain.role, roles));
                    component_projection::set(item, "ordinal", optional(value.drain.ordinal));
                    component_projection::set(item, "started_ns", optional(value.drain.started_ns));
                }));
                component_projection::set(object, "last_successful_drain_completed_ns", optional(value.last_successful_drain_completed_ns));
            });
        }
    },context);
}
template <class Json = nlohmann::json>
auto nested_failure_projection(const PairwiseNestedFailureValue& value) -> Json {
    Json native = nullptr;
    component_projection::Cleanup native_cleanup{native};
    if (value.posix_errno)
        native = projection_decimal(*value.posix_errno);
    return component_projection::object<Json>([&](auto& object_44) {
        component_projection::set(object_44, "code", finalized_code_token(value.code));
        component_projection::set(object_44, "operation",
                               indexed_token(value.operation, operation_tokens));
        component_projection::set(object_44, "reason", indexed_token(value.reason, reason_tokens));
        component_projection::set(object_44, "context", flat_context_projection<Json>(value.context));
        component_projection::set(object_44, "posix_errno", std::move(native));
    });
}
template <class Json = nlohmann::json>
auto context_projection(const PairwiseFailureContext& context) -> Json {
    return std::visit([](const auto& value)->Json{
        using T=std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, NestedFailureContext>) {
            Json phase = nullptr;
            component_projection::Cleanup phase_cleanup{phase};
            Json platform = nullptr;
            component_projection::Cleanup platform_cleanup{platform};
            if (value.phase)
                phase = indexed_token(*value.phase, phase_tokens);
            if (value.nested_platform_failure)
                platform = nested_failure_projection<Json>(*value.nested_platform_failure);
            return component_projection::object<Json>([&](auto& object_45) {
                component_projection::set(object_45, "kind", "nested_failure");
                component_projection::set(object_45, "failure",
                                       nested_failure_projection<Json>(value.failure));
                component_projection::set(object_45, "nested_phase", std::move(phase));
                component_projection::set(object_45, "nested_platform_failure", std::move(platform));
            });
        } else {
            PairwiseFlatFailureContext flat = value;
            return flat_context_projection<Json>(flat);
        }
    },context);
}
template <class Json = nlohmann::json>
auto finalized_error_projection(const FinalizedEvidenceError& value) -> Json {
    const auto row=std::find_if(error_failure_rows.begin(),error_failure_rows.end(),[&](const auto& item){return item.code==finalized_code_token(value.code());});
    Json native = nullptr;
    component_projection::Cleanup native_cleanup{native};
    if (value.os_error())
        native = component_projection::object<Json>([&](auto& object_46) {
            component_projection::set(object_46, "domain", "posix_errno");
            component_projection::set(object_46, "value", projection_decimal(value.os_error()->value()));
        });
    return component_projection::object<Json>([&](auto& object_47) {
        component_projection::set(object_47, "code", row->code);
        component_projection::set(object_47, "subsystem", row->subsystem);
        component_projection::set(object_47, "category", row->category);
        component_projection::set(object_47, "stable_code", row->stable_code);
        component_projection::set(object_47, "severity", row->severity);
        component_projection::set(object_47, "continuation", row->continuation);
        component_projection::set(object_47, "renderer", row->renderer);
        component_projection::set(object_47, "operation", value.operation_token());
        component_projection::set(object_47, "reason", value.reason_token());
        component_projection::set(object_47, "context", context_projection<Json>(value.context()));
        component_projection::set(object_47, "os_error", std::move(native));
    });
}

auto repetition_disposition_token(measurement::RepetitionDisposition value) -> std::string_view {
    switch (value) {
    case measurement::RepetitionDisposition::precision_target_reached:
        return "precision_target_reached";
    case measurement::RepetitionDisposition::maximum_blocks_reached:
        return "maximum_blocks_reached";
    case measurement::RepetitionDisposition::selected_work_unavailable:
        return "selected_work_unavailable";
    }
    std::unreachable();
}
template <class Json = nlohmann::json>
auto stopping_projection(const RunnerStoppingEvaluationEvidence& value) -> Json {
    const auto log=value.bootstrap.log_effect_interval();
    const auto ratio=value.bootstrap.oriented_geometric_ratio_interval();
    const bool met=value.evaluation.attainment()==statistics::PrecisionTargetAttainment::target_met;
    std::string_view subsequent;
    switch(value.subsequent_disposition){case RepetitionBoundaryDisposition::continued:subsequent="continued";break;
    case RepetitionBoundaryDisposition::precision_target_reached:subsequent="precision_target_reached";break;
    case RepetitionBoundaryDisposition::maximum_blocks_reached:subsequent="maximum_blocks_reached";break;}
    return component_projection::object<Json>([&](auto& object_48) {
        component_projection::set(object_48, "boundary_block_id",
                               projection_decimal(value.boundary_block_id.value()));
        component_projection::set(object_48, "completed_blocks", projection_decimal(value.completed_blocks));
        component_projection::set(object_48, "complete_valid_blocks",
                               projection_decimal(value.complete_valid_blocks));
        component_projection::set(object_48, "valid_first_observations",
                               projection_decimal(value.valid_first_observations));
        component_projection::set(object_48, "valid_second_observations",
                               projection_decimal(value.valid_second_observations));
        component_projection::set(
            object_48, "bootstrap", component_projection::object<Json>([&](auto& object_49) {
                component_projection::set(object_49, "policy",
                                       bootstrap_projection<Json>(value.bootstrap.policy()));
                component_projection::set(object_49, "log_lower", projection_binary64(log.lower().value()));
                component_projection::set(object_49, "log_upper", projection_binary64(log.upper().value()));
                component_projection::set(object_49, "ratio_lower",
                                       projection_binary64(ratio.lower().value()));
                component_projection::set(object_49, "ratio_upper",
                                       projection_binary64(ratio.upper().value()));
            }));
        component_projection::set(
            object_48, "evaluation",
            component_projection::object<Json>([&](auto& object_50) {
                component_projection::set(
                    object_50, "observed_relative_half_width",
                    projection_binary64(value.evaluation.observed_precision().value()));
                component_projection::set(object_50, "attainment",
                                       met ? "target_met" : "target_not_met");
            }));
        component_projection::set(object_48, "decision",
                               value.decision == measurement::PrecisionStoppingDecision::target_met
                                   ? "target_met"
                                   : "target_not_met");
        component_projection::set(object_48, "subsequent_disposition", subsequent);
    });
}
template <class Json> auto project_monitoring_as(const monitoring::MonitoringCaptureResult& value)
    -> Json {
    if constexpr (std::same_as<Json, ComponentProjectionCount>) return count_monitoring_projection(value);
    else return project_monitoring(value);
}
template <class Json = nlohmann::json>
auto factual_observation_projection(const FactualObservationEvidence& value) -> Json {
    Json monitoring_value = nullptr;
    component_projection::Cleanup monitoring_value_cleanup{monitoring_value};
    if(value.monitoring) monitoring_value=project_monitoring_as<Json>(*value.monitoring);
    Json placements = Json::array();
    component_projection::Cleanup placements_cleanup{placements};
    for (const auto& item : value.placements)
        component_projection::append(
            placements, component_projection::object<Json>([&](auto& object_51) {
                component_projection::set(object_51, "worker_id", projection_decimal(item.worker_id().value()));
                component_projection::set(object_51, "state", placement_state_token(item.state()));
            }));
    Json migrations = Json::array();
    component_projection::Cleanup migrations_cleanup{migrations};
    for (const auto& item : value.migrations)
        component_projection::append(
            migrations, component_projection::object<Json>([&](auto& object_52) {
                component_projection::set(object_52, "worker_id", projection_decimal(item.worker_id().value()));
                component_projection::set(object_52, "state", migration_state_token(item.state()));
            }));
    return component_projection::object<Json>([&](auto& object_53) {
        component_projection::set(object_53, "slot", slot_projection<Json>(value.slot));
        component_projection::set(object_53, "observation_id", projection_decimal(value.observation_id.value()));
        component_projection::set(object_53, "measurement", measurement_projection<Json>(value.observation));
        component_projection::set(object_53, "validity", validity_projection<Json>(value.validity));
        component_projection::set(object_53, "monitoring", std::move(monitoring_value));
        component_projection::set(object_53, "placements", std::move(placements));
        component_projection::set(object_53, "migrations", std::move(migrations));
    });
}
template <class Json = nlohmann::json>
auto failed_observation_projection(const FailedObservationEvidence& value) -> Json {
    Json workers = Json::array();
    component_projection::Cleanup workers_cleanup{workers};
    for (const auto& item : value.worker_errors)
        component_projection::append(workers, finalized_error_projection<Json>(item));
    return component_projection::object<Json>([&](auto& object_54) {
        component_projection::set(object_54, "slot",
                               slot_projection<Json>(value.slot));
        component_projection::set(object_54, "observation_id",
                               projection_decimal(value.observation_id.value()));
        component_projection::set(
            object_54, "primary_error",
            finalized_error_projection<Json>(value.primary_error));
        component_projection::set(object_54, "worker_errors", std::move(workers));
    });
}
template <class Json = nlohmann::json>
auto block_projection(const PairwiseBlockAttemptEvidence& value) -> Json {
    Json observations = Json::array();
    component_projection::Cleanup observations_cleanup{observations};
    for (const auto& item : value.observations)
        component_projection::append(observations, factual_observation_projection<Json>(item));
    Json failed = nullptr;
    component_projection::Cleanup failed_cleanup{failed};
    if (value.failed_observation)
        failed = failed_observation_projection<Json>(*value.failed_observation);
    std::string_view completion;switch(value.completion){case BlockAttemptCompletion::complete_valid:completion="complete_valid";break;case BlockAttemptCompletion::complete_invalid:completion="complete_invalid";break;case BlockAttemptCompletion::operationally_incomplete:completion="operationally_incomplete";break;}
    return component_projection::object<Json>([&](auto& object_55) {
        component_projection::set(object_55, "plan", plan_projection<Json>(value.plan));
        component_projection::set(object_55, "observations", std::move(observations));
        component_projection::set(object_55, "failed_observation", std::move(failed));
        component_projection::set(object_55, "completion", completion);
    });
}
template <class Json = nlohmann::json>
auto terminal_projection(const PairwiseRunTerminalEvidence& value) -> Json {
    if (const auto* normal = std::get_if<NormalRunTerminalEvidence>(&value)) {
        const auto& outcome = normal->outcome;
        return component_projection::object<Json>([&](auto& object_56) {
            component_projection::set(object_56, "kind", "normal");
            component_projection::set(
                object_56, "repetition_outcome",
                component_projection::object<Json>([&](auto& object_57) {
                    component_projection::set(object_57, "experiment_id",
                                           projection_decimal(outcome.experiment_id().value()));
                    component_projection::set(object_57, "policy",
                                           repetition_policy_projection<Json>(outcome.policy()));
                    component_projection::set(object_57, "disposition",
                                           repetition_disposition_token(outcome.disposition()));
                    component_projection::set(object_57, "completed_blocks",
                                           projection_decimal(outcome.completed_blocks()));
                    component_projection::set(object_57, "complete_valid_blocks",
                                           projection_decimal(outcome.complete_valid_blocks()));
                    component_projection::set(object_57, "valid_first_variant_observations",
                                           projection_decimal(outcome.valid_first_variant_observations()));
                    component_projection::set(object_57, "valid_second_variant_observations",
                                           projection_decimal(outcome.valid_second_variant_observations()));
                }));
        });
    }
    const auto& operational=std::get<OperationalRunTerminalEvidence>(value);
    Json secondary = Json::array();
    component_projection::Cleanup secondary_cleanup{secondary};
    for (const auto& item : operational.secondary_errors)
        component_projection::append(secondary, finalized_error_projection<Json>(item));
    Json active = nullptr;
    component_projection::Cleanup active_cleanup{active};
    if (operational.active_block_id)
        active = projection_decimal(operational.active_block_id->value());
    return component_projection::object<Json>([&](auto& object_58) {
        component_projection::set(object_58, "kind", "operational");
        component_projection::set(object_58, "primary_error",
                               finalized_error_projection<Json>(operational.primary_error));
        component_projection::set(object_58, "secondary_errors", std::move(secondary));
        component_projection::set(object_58, "active_block_id", std::move(active));
    });
}

} // namespace

// §29.6: these are type inventories, never instantiated owners. Each field names
// a source local, argument, constructor layer or non-elided return. Both family
// lanes are summed; a lane's loop-local slots are reused across iterations.
namespace {
template<class T> using OutcomeResult=std::expected<T,FixedTypedReconstructionError>;
template<class T> using OutcomeFactoryResult=std::expected<T,core::Error>;
template<class Count,class Trial,class Outcome> struct CalibrationReplayLane final {
    const nlohmann::json *value, *empty_json, *candidate_json;
    std::uint64_t quantum, maximum_units, minimum_overhead, expected, best_distance, distance;
    std::size_t empty_index, candidate_index, replay_index;
    OutcomeResult<measurement::CalibrationDurationPolicy> policy;
    std::vector<measurement::EmptyHarnessTrial> empty;
    OutcomeResult<measurement::MeasurementInterval> empty_measured, candidate_measured;
    measurement::EmptyHarnessTrial empty_insertion;
    OutcomeResult<std::uint64_t> overhead, candidate, selected_raw;
    std::vector<Trial> trials;
    std::vector<std::uint64_t> units;
    OutcomeFactoryResult<Count> count, made;
    core::detail::ExpectedErrorCleanup<OutcomeFactoryResult<Count>> count_cleanup, made_cleanup;
    std::optional<std::uint64_t> lower, upper;
    core::Duration replay_elapsed, terminal_elapsed, selection_elapsed;
    std::optional<Count> derived_selected, stated_selected;
    measurement::CalibrationDisposition derived_disposition;
    OutcomeResult<measurement::CalibrationDisposition> stated_disposition;
    Trial insertion;
    const Trial* selection_trial;
    // Empty capture factory closures each have one byte of object representation.
    std::array<std::byte,1> make_trial, make_outcome;
    Outcome family_return;
    PairwiseCalibrationEvidence variant_return;
    OutcomeResult<PairwiseCalibrationEvidence> replay_return, named_value, named_return;
    std::string_view kind;
};
struct CalibrationReplayStaging final {
    CalibrationReplayLane<kernel::SequentialReadBandwidthPassCount,
        measurement::SequentialReadBandwidthCalibrationTrial,measurement::SequentialReadBandwidthCalibrationOutcome> bandwidth;
    CalibrationReplayLane<kernel::DependentLoadLatencyStepCount,
        measurement::DependentLoadLatencyCalibrationTrial,measurement::DependentLoadLatencyCalibrationOutcome> latency;
};
template<class Count,class Outcome> struct WarmupReplayLane final {
    const nlohmann::json *value, *policy_json, *trial_json, *evaluation_json;
    OutcomeResult<std::uint64_t> minimum, maximum, window, numerator, denominator, selected_raw, median, deviation;
    OutcomeFactoryResult<measurement::WarmupStabilityThreshold> threshold;
    core::detail::ExpectedErrorCleanup<decltype(threshold)> threshold_cleanup;
    OutcomeFactoryResult<measurement::WarmupPolicy> policy;
    core::detail::ExpectedErrorCleanup<decltype(policy)> policy_cleanup;
    OutcomeFactoryResult<Count> selected;
    core::detail::ExpectedErrorCleanup<decltype(selected)> selected_cleanup;
    std::vector<measurement::WarmupTrial> trials;
    OutcomeResult<measurement::MeasurementInterval> measured;
    measurement::WarmupTrial insertion;
    std::size_t index, count;
    std::optional<measurement::WarmupStabilityEvaluation> derived;
    measurement::WarmupStabilityEvaluation current;
    measurement::WarmupDisposition expected;
    OutcomeResult<measurement::WarmupDisposition> stated_disposition;
    std::span<const measurement::WarmupTrial> prefix;
    std::array<std::byte,1> make_outcome;
    Outcome family_return;
    PairwiseWarmupEvidence variant_return;
    OutcomeResult<PairwiseWarmupEvidence> replay_return, named_value, named_return;
    std::string_view kind;
};
struct WarmupReplayStaging final {
    WarmupReplayLane<kernel::SequentialReadBandwidthPassCount,measurement::SequentialReadBandwidthWarmupOutcome> bandwidth;
    WarmupReplayLane<kernel::DependentLoadLatencyStepCount,measurement::DependentLoadLatencyWarmupOutcome> latency;
};
struct OutcomeLeafStaging final {
    const nlohmann::json *interval_value, *policy_value, *scalar_value;
    const std::string* scalar_text;
    OutcomeResult<std::uint64_t> start, end, elapsed, minimum, target, maximum, scalar_return;
    platform_linux::RawMonotonicTimestamp start_argument, end_argument, start_parameter, end_parameter;
    core::Duration elapsed_return, minimum_argument, target_argument, maximum_argument;
    OutcomeFactoryResult<measurement::MeasurementInterval> interval_factory;
    core::detail::ExpectedErrorCleanup<decltype(interval_factory)> interval_cleanup;
    OutcomeResult<measurement::MeasurementInterval> interval_return;
    measurement::MeasurementInterval interval_constructor_return, empty_trial_argument, empty_trial_parameter,
        warmup_trial_argument, warmup_trial_parameter, bandwidth_trial_argument, bandwidth_trial_parameter,
        latency_trial_argument, latency_trial_parameter;
    OutcomeFactoryResult<measurement::CalibrationDurationPolicy> policy_factory;
    core::detail::ExpectedErrorCleanup<decltype(policy_factory)> policy_cleanup;
    OutcomeResult<measurement::CalibrationDurationPolicy> policy_return;
    measurement::CalibrationDurationPolicy policy_constructor_return;
    core::Duration policy_minimum_parameter, policy_target_parameter, policy_maximum_parameter;
    std::uint64_t scalar, threshold_numerator_argument, threshold_denominator_argument,
        threshold_numerator_parameter, threshold_denominator_parameter,
        warmup_minimum_argument, warmup_maximum_argument, warmup_window_argument,
        warmup_minimum_parameter, warmup_maximum_parameter, warmup_window_parameter,
        bandwidth_count_argument, bandwidth_count_parameter, latency_count_argument, latency_count_parameter;
    std::from_chars_result conversion;
    measurement::WarmupStabilityThreshold threshold_return, warmup_threshold_argument, warmup_threshold_parameter;
    measurement::WarmupPolicy warmup_policy_return;
    OutcomeFactoryResult<measurement::WarmupStabilityThreshold> threshold_factory_return;
    OutcomeFactoryResult<measurement::WarmupPolicy> warmup_factory_return;
    OutcomeFactoryResult<kernel::SequentialReadBandwidthPassCount> bandwidth_factory_return;
    OutcomeFactoryResult<kernel::DependentLoadLatencyStepCount> latency_factory_return;
    kernel::SequentialReadBandwidthPassCount bandwidth_count_return, bandwidth_trial_argument_count, bandwidth_trial_parameter_count;
    kernel::DependentLoadLatencyStepCount latency_count_return, latency_trial_argument_count, latency_trial_parameter_count;
    measurement::EmptyHarnessTrial empty_trial_return;
    measurement::WarmupTrial warmup_trial_return;
    measurement::SequentialReadBandwidthCalibrationTrial bandwidth_trial_return;
    measurement::DependentLoadLatencyCalibrationTrial latency_trial_return;
    core::Duration estimate_argument, estimate_parameter;
    measurement::TimerOverheadEstimate estimate_return;
    core::Duration evaluation_median_argument, evaluation_deviation_argument, evaluation_median_parameter, evaluation_deviation_parameter;
    bool evaluation_stable_argument, evaluation_stable_parameter;
    measurement::WarmupStabilityEvaluation evaluation_return;
};
struct WarmupWindowStaging final {
    std::vector<std::uint64_t> values;
    std::span<const measurement::WarmupTrial> trials, tail;
    const measurement::WarmupTrial* trial;
    measurement::WarmupStabilityThreshold threshold;
    std::size_t window;
    std::uint64_t median, maximum_deviation, sample, deviation;
    bool stable, reversed;
    std::uint64_t left_numerator, left_denominator, right_numerator, right_denominator,
        left_quotient, right_quotient, left_remainder, right_remainder;
    measurement::WarmupStabilityEvaluation returned;
};
template<class Count,class Trial> struct CalibrationMoveArguments final {
    measurement::CalibrationDurationPolicy policy;
    std::vector<measurement::EmptyHarnessTrial> empty;
    measurement::TimerOverheadEstimate estimate;
    std::vector<Trial> trials;
    measurement::CalibrationDisposition disposition;
    std::optional<Count> selected;
};
template<class Count> struct WarmupMoveArguments final {
    measurement::WarmupPolicy policy;
    Count selected;
    std::vector<measurement::WarmupTrial> trials;
    measurement::WarmupDisposition disposition;
    std::optional<measurement::WarmupStabilityEvaluation> evaluation;
};
struct OutcomeMoveStaging final {
    CalibrationMoveArguments<kernel::SequentialReadBandwidthPassCount,measurement::SequentialReadBandwidthCalibrationTrial> bandwidth_access, bandwidth_constructor;
    CalibrationMoveArguments<kernel::DependentLoadLatencyStepCount,measurement::DependentLoadLatencyCalibrationTrial> latency_access, latency_constructor;
    WarmupMoveArguments<kernel::SequentialReadBandwidthPassCount> bandwidth_warmup_access, bandwidth_warmup_constructor;
    WarmupMoveArguments<kernel::DependentLoadLatencyStepCount> latency_warmup_access, latency_warmup_constructor;
    measurement::SequentialReadBandwidthCalibrationOutcome bandwidth_access_return, bandwidth_constructor_return;
    measurement::DependentLoadLatencyCalibrationOutcome latency_access_return, latency_constructor_return;
    measurement::SequentialReadBandwidthWarmupOutcome bandwidth_warmup_access_return, bandwidth_warmup_constructor_return;
    measurement::DependentLoadLatencyWarmupOutcome latency_warmup_access_return, latency_warmup_constructor_return;
    PairwiseCalibrationEvidence calibration_variant_return, calibration_move_parameter;
    PairwiseWarmupEvidence warmup_variant_return, warmup_move_parameter;
    std::optional<PairwiseCalibrationEvidence> calibration_emplacement;
    std::optional<PairwiseWarmupEvidence> warmup_emplacement;
    OutcomeResult<PairwiseCalibrationEvidence> calibration_expected_return;
    OutcomeResult<PairwiseWarmupEvidence> warmup_expected_return;
};
// The complete finite visitor graph is summed rather than selecting one deepest
// branch. Each node includes the object helper, set/append argument and return.
template<class Json> struct OutcomeProjectionNode final {
    Json object, child_argument, returned;
    component_projection::Cleanup<Json> object_cleanup, child_cleanup;
    core::detail::JsonCleanup<Json> set_cleanup, append_cleanup;
    Json *parent, *child;
    std::string_view key;
    ProjectionDecimal decimal_argument, decimal_return;
    std::uint64_t number_argument;
    std::to_chars_result conversion;
    // ComponentProjectionCount's constructor/add/child and set's key count
    // retain these checked results. Summing the finite branches also covers
    // nested+sum and encoded+sum coexistence, including non-elided returns.
    std::expected<std::size_t,WireJsonError> string_count, add_sum, child_nested, set_encoded;
    std::expected<std::size_t,WireJsonError> string_count_return, add_sum_return, child_nested_return, set_encoded_return;
    std::string_view count_string_argument, encoded_key_argument;
    std::size_t* count_add_target;
    std::size_t count_add_value, count_add_left, count_add_right;
    const ComponentProjectionCount* count_child_argument;
};
template<class Json> struct OutcomeProjectionLane final {
    OutcomeProjectionNode<Json> calibration, empty_interval, candidate, candidate_interval,
        duration_policy, warmup, warmup_interval, warmup_policy, evaluation;
    Json empty, candidates, selected, trials, evaluated, visit_return, wrapper_return;
    component_projection::Cleanup<Json> empty_cleanup, candidates_cleanup, selected_cleanup, trials_cleanup, evaluated_cleanup;
    const void *outcome, *policy, *trial, *visitor_capture;
    // Captured references in the nested object lambdas, explicitly summed.
    const void *calibration_value_capture, *calibration_policy_capture, *calibration_empty_capture,
        *calibration_candidates_capture, *calibration_selected_capture, *candidate_capture,
        *duration_capture, *warmup_value_capture, *warmup_policy_capture, *warmup_trials_capture,
        *warmup_evaluation_capture, *warmup_threshold_capture, *evaluation_capture, *interval_capture;
    std::string_view kind, disposition;
    measurement::WarmupStabilityThreshold threshold;
    std::optional<measurement::WarmupStabilityEvaluation> final;
    std::optional<kernel::SequentialReadBandwidthPassCount> bandwidth_selected;
    std::optional<kernel::DependentLoadLatencyStepCount> latency_selected;
};
struct OutcomeProjectionStaging final {
    OutcomeProjectionLane<nlohmann::json> json;
    OutcomeProjectionLane<ComponentProjectionCount> count;
    ReconstructionPath interval_path, scalar_path, copied_base, ordinal_return,
        first_concatenation, second_concatenation, suffix_concatenation, error_parameter;
    FixedTypedReconstructionError failure, error_return;
    std::unexpected<FixedTypedReconstructionError> unexpected_return;
    OutcomeResult<std::uint64_t> scalar_error_return;
    OutcomeResult<measurement::MeasurementInterval> interval_error_return;
    OutcomeResult<PairwiseCalibrationEvidence> calibration_error_return;
    OutcomeResult<PairwiseWarmupEvidence> warmup_error_return;
    std::uint64_t ordinal;
    std::to_chars_result ordinal_conversion;
};
static_assert(sizeof(decltype(OutcomeProjectionNode<ComponentProjectionCount>::string_count_return))==
    sizeof(decltype(count_wire_json_string(std::string_view{}))));
static_assert(sizeof(decltype(OutcomeProjectionNode<ComponentProjectionCount>::add_sum_return))==
    sizeof(decltype(add_wire_json_count(std::size_t{},std::size_t{}))));
static_assert(std::is_nothrow_move_constructible_v<PairwiseCalibrationEvidence>);
static_assert(std::is_nothrow_move_constructible_v<PairwiseWarmupEvidence>);
static_assert(sizeof(decltype(CalibrationReplayStaging::bandwidth.replay_return))==sizeof(OutcomeResult<PairwiseCalibrationEvidence>));
static_assert(sizeof(decltype(WarmupReplayStaging::latency.named_return))==sizeof(OutcomeResult<PairwiseWarmupEvidence>));
static_assert(sizeof(decltype(OutcomeMoveStaging::calibration_emplacement))==sizeof(std::optional<PairwiseCalibrationEvidence>));
static_assert(sizeof(decltype(OutcomeMoveStaging::warmup_emplacement))==sizeof(std::optional<PairwiseWarmupEvidence>));
}
auto preparation_outcome_reconstruction_staging() noexcept -> std::array<std::size_t,6> {
    return {sizeof(CalibrationReplayStaging),sizeof(WarmupReplayStaging),sizeof(OutcomeLeafStaging),
        sizeof(WarmupWindowStaging),sizeof(OutcomeMoveStaging),sizeof(OutcomeProjectionStaging)};
}
auto count_calibration_component(const PairwiseCalibrationEvidence& value) -> ComponentProjectionCount {
    return calibration_projection<ComponentProjectionCount>(value);
}
auto project_calibration_component(const PairwiseCalibrationEvidence& value) -> nlohmann::json {
    return calibration_projection(value);
}
auto count_warmup_component(const PairwiseWarmupEvidence& value) -> ComponentProjectionCount {
    return warmup_projection<ComponentProjectionCount>(value);
}
auto project_warmup_component(const PairwiseWarmupEvidence& value) -> nlohmann::json {
    return warmup_projection(value);
}
auto count_block_plan_component(const measurement::MeasurementBlockPlan& value) -> ComponentProjectionCount {
    return plan_projection<ComponentProjectionCount>(value);
}
auto project_block_plan_component(const measurement::MeasurementBlockPlan& value) -> nlohmann::json {
    return plan_projection(value);
}
auto count_factual_component(const FactualObservationEvidence& value) -> ComponentProjectionCount {
    return factual_observation_projection<ComponentProjectionCount>(value);
}
auto project_factual_component(const FactualObservationEvidence& value) -> nlohmann::json {
    return factual_observation_projection(value);
}
auto count_stopping_component(const RunnerStoppingEvaluationEvidence& value) -> ComponentProjectionCount {
    return stopping_projection<ComponentProjectionCount>(value);
}
auto project_stopping_component(const RunnerStoppingEvaluationEvidence& value) -> nlohmann::json {
    return stopping_projection(value);
}
auto count_failed_component(const FailedObservationEvidence& value) -> ComponentProjectionCount {
    return failed_observation_projection<ComponentProjectionCount>(value);
}
auto project_failed_component(const FailedObservationEvidence& value) -> nlohmann::json {
    return failed_observation_projection(value);
}
template <class Json> auto project_finalization_as(const PairwiseFinalizationEvidence& value) -> Json {
    if constexpr (std::same_as<Json, ComponentProjectionCount>) return count_pairwise_finalization_evidence(value);
    else return project_pairwise_finalization_evidence(value);
}
template <class Json = nlohmann::json>
auto root_projection(const FinalizedPairwiseRunEvidence& evidence) -> Json {
    const auto& provenance = evidence.provenance();
    const auto& statistics = evidence.statistics_policy();
    Json calibration_value = nullptr;
    component_projection::Cleanup calibration_value_cleanup{calibration_value};
    if (evidence.calibration()) calibration_value = calibration_projection<Json>(*evidence.calibration());
    Json warmup_value = nullptr;
    component_projection::Cleanup warmup_value_cleanup{warmup_value};
    if (evidence.warmup()) warmup_value = warmup_projection<Json>(*evidence.warmup());
    Json blocks = Json::array();
    component_projection::Cleanup blocks_cleanup{blocks};
    for (const auto& block : evidence.blocks())
        component_projection::append(blocks, block_projection<Json>(block));
    Json stopping = Json::array();
    component_projection::Cleanup stopping_cleanup{stopping};
    for (const auto& item : evidence.stopping_evaluations())
        component_projection::append(stopping, stopping_projection<Json>(item));
    Json finalization_value = nullptr;
    component_projection::Cleanup finalization_value_cleanup{finalization_value};
    if (evidence.finalization()) finalization_value=project_finalization_as<Json>(*evidence.finalization());
    Json root = component_projection::object<Json>([&](auto& object_59) {
        component_projection::set(object_59, "schema", "0");
        component_projection::set(
            object_59, "provenance",
            component_projection::object<Json>([&](auto& object_60) {
                component_projection::set(object_60, "experiment_id",
                                       projection_decimal(provenance.experiment_id.value()));
                component_projection::set(object_60, "scenario_profile",
                                       provenance.scenario_profile);
                component_projection::set(object_60, "scenario_digest",
                                       provenance.scenario_digest);
                component_projection::set(object_60, "resolved_plan_digest",
                                       provenance.resolved_plan_digest);
                component_projection::set(object_60, "workload_id", provenance.workload_id);
                component_projection::set(object_60, "first_variant_id",
                                       provenance.first_variant_id);
                component_projection::set(object_60, "second_variant_id",
                                       provenance.second_variant_id);
            }));
        component_projection::set(
            object_59, "statistics_policy",
            component_projection::object<Json>([&](auto& object_61) {
                component_projection::set(
                    object_61, "comparison_bootstrap_policy",
                    bootstrap_projection<Json>(statistics.comparison_bootstrap_policy()));
                component_projection::set(
                    object_61, "stopping_bootstrap_policy",
                    bootstrap_projection<Json>(statistics.stopping_bootstrap_policy()));
                component_projection::set(
                    object_61, "effective_practical_effect_fraction",
                    projection_binary64(statistics.effective_practical_effect_fraction()));
                component_projection::set(
                    object_61, "practical_effect_delta",
                    projection_binary64(statistics.practical_effect_threshold().delta()));
                component_projection::set(
                    object_61, "relative_half_width_target",
                    projection_binary64(statistics.relative_half_width_target().value()));
            }));
        component_projection::set(object_59, "monitoring_policy",
                               monitoring_policy_projection<Json>(evidence.monitoring_policy()));
        component_projection::set(object_59, "repetition_policy",
                               repetition_policy_projection<Json>(evidence.repetition_policy()));
        component_projection::set(object_59, "comparison_order",
                               comparison_order_projection<Json>(evidence.comparison_order()));
        component_projection::set(object_59, "calibration", std::move(calibration_value));
        component_projection::set(object_59, "warmup", std::move(warmup_value));
        component_projection::set(object_59, "blocks", std::move(blocks));
        component_projection::set(object_59, "stopping_evaluations", std::move(stopping));
        component_projection::set(object_59, "terminal", terminal_projection<Json>(evidence.terminal()));
        component_projection::set(object_59, "finalization", std::move(finalization_value));
    });
    component_projection::Cleanup root_cleanup{root};
    return Json(std::move(root));
}

auto count_finalized_evidence(const FinalizedPairwiseRunEvidence& value) -> ComponentProjectionCount {
    return root_projection<ComponentProjectionCount>(value);
}
auto encode_finalized_evidence(const FinalizedPairwiseRunEvidence& evidence,
                               const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<std::string, LogicalCodecError> {
    try {
        const auto counted = root_projection<ComponentProjectionCount>(evidence);
        if (counted.overflow || counted.invalid)
            return std::unexpected{LogicalCodecError{LogicalCodecErrorCode::allocation_failure}};
        if (counted.bytes > capacity.resource_plan.controller_receive_bytes.value)
            return std::unexpected{LogicalCodecError{LogicalCodecErrorCode::byte_limit,
                capacity.resource_plan.controller_receive_bytes, {counted.bytes}}};
        auto root = root_projection(evidence);
        component_projection::Cleanup root_cleanup{root};
        auto canonical = render_wire_json(
            root, counted.depth, counted.nodes,
            static_cast<std::size_t>(capacity.resource_plan.controller_receive_bytes.value));
        if (!canonical) {
            if (canonical.error().code == WireJsonFailure::byte_limit)
                return std::unexpected{
                    LogicalCodecError{LogicalCodecErrorCode::byte_limit,
                                      capacity.resource_plan.controller_receive_bytes,
                                      {canonical.error().observed}}};
            return std::unexpected{LogicalCodecError{LogicalCodecErrorCode::allocation_failure}};
        }
        auto admitted = decode_evidence_body(*canonical, capacity);
        if (!admitted) return std::unexpected{admitted.error()};
        return encode_logical_body(*admitted, capacity);
    } catch (...) {
        return std::unexpected{LogicalCodecError{LogicalCodecErrorCode::allocation_failure}};
    }
}

auto reconstruct_evidence_header(const nlohmann::json& body)
    -> std::expected<ReconstructedEvidenceHeader, TypedReconstructionError> {
    auto result = fixed_reconstruct_evidence_header(body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_evidence_header(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, TypedReconstructionError> {
    auto result = fixed_reconstruct_evidence_header(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_calibration_warmup(const nlohmann::json& body)
    -> std::expected<ReconstructedCalibrationWarmup, TypedReconstructionError> {
    auto result = fixed_reconstruct_calibration_warmup(body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_calibration_warmup(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, TypedReconstructionError> {
    auto result = fixed_reconstruct_calibration_warmup(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_block_plan_component(const nlohmann::json& plan_json)
    -> std::expected<measurement::MeasurementBlockPlan, TypedReconstructionError> {
    auto result = fixed_reconstruct_block_plan_component(plan_json);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_factual_component(const nlohmann::json& observation_json,
                                   const measurement::MeasurementBlockPlan& plan,
                                   std::size_t expected_slot,
                                   std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FactualObservationEvidence, TypedReconstructionError> {
    auto result = fixed_reconstruct_factual_component(observation_json, plan, expected_slot, authenticated_cpus);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_observation_blocks(
    const nlohmann::json& body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError> {
    auto result = fixed_reconstruct_observation_blocks(body, authenticated_cpus);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_failed_component_header(const nlohmann::json& failed,
                                         const measurement::MeasurementBlockPlan& plan,
                                         std::string_view path)
    -> std::expected<ReconstructedFailedComponentHeader, TypedReconstructionError> {
    auto result = fixed_reconstruct_failed_component_header(failed, plan, ReconstructionPath::reference(path));
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_failed_component_workers(const nlohmann::json& failed,
                                           ReconstructedFailedComponentHeader header,
                                           std::string_view path)
    -> std::expected<FailedObservationEvidence, TypedReconstructionError> {
    auto result = fixed_reconstruct_failed_component_workers(failed, std::move(header), ReconstructionPath::reference(path));
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_finalized_error_value(const nlohmann::json& bounded_error)
    -> std::expected<FinalizedEvidenceError, TypedReconstructionError> {
    auto result = fixed_reconstruct_finalized_error_value(bounded_error);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_stopping_and_failures(const nlohmann::json& body)
    -> std::expected<ReconstructedStoppingAndFailures, TypedReconstructionError> {
    auto result = fixed_reconstruct_stopping_and_failures(body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_stopping_and_failures(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, TypedReconstructionError> {
    auto result = fixed_reconstruct_stopping_and_failures(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_observation_blocks(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError> {
    auto result = fixed_reconstruct_observation_blocks(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_observation_blocks(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError> {
    auto result = fixed_reconstruct_observation_blocks(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_observation_blocks(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError> {
    auto result = fixed_reconstruct_observation_blocks(bounded_body, authenticated_cpus);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_terminal_and_finalization(const nlohmann::json& body)
    -> std::expected<ReconstructedTerminalAndFinalization, TypedReconstructionError> {
    auto result = fixed_reconstruct_terminal_and_finalization(body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_terminal_and_finalization(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, TypedReconstructionError> {
    auto result = fixed_reconstruct_terminal_and_finalization(bounded_body);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    return std::move(*result);
}

auto reconstruct_finalized_evidence(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FinalizedPairwiseRunEvidence, TypedReconstructionError> {
    auto result = fixed_reconstruct_finalized_evidence(bounded_body, authenticated_cpus);
    if (!result) return std::unexpected{TypedReconstructionError{
        result.error().code, result.error().path.render()}};
    if (check_root_reconstruction_shape(bounded_body.value())) {
        const auto usage = count_root_reconstruction(bounded_body.value());
        if (!usage) throw std::bad_alloc{};
    }
    return std::move(*result);
}

auto reconstruct_finalized_evidence_bounded(const LogicalBody& body,
    std::span<const core::LinuxCpuId> cpus, const RootReconstructionLimits& limits)
    -> std::expected<FinalizedPairwiseRunEvidence, RootReconstructionFailure> {
    try {
        auto usage = count_root_reconstruction(body.value());
        if (!usage) return std::unexpected{std::move(usage.error())};
        if (auto admitted = check_root_reconstruction_limits(*usage, limits); !admitted)
            return std::unexpected{std::move(admitted.error())};
        auto result = fixed_reconstruct_finalized_evidence(body, cpus);
        if (!result) {
            const bool overflow = result.error().path.overflowed() || !result.error().path.fixed();
            return std::unexpected{RootReconstructionFailure{
                overflow ? RootReconstructionFailureCode::diagnostic_overflow
                         : RootReconstructionFailureCode::reconstruction, std::move(result.error())}};
        }
        return std::move(*result);
    } catch (const std::bad_alloc&) {
        return std::unexpected{RootReconstructionFailure{RootReconstructionFailureCode::allocation_failure}};
    }
}

} // namespace epycbench::runtime::wire_v1::detail
