#pragma once

#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/warmup.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace epycbench::measurement::detail {

inline constexpr std::size_t prepared_empty_trial_count = 5U;
inline constexpr std::size_t prepared_candidate_trial_bound = 128U;

[[nodiscard]] constexpr auto calibration_distance(std::uint64_t value,
                                                   std::uint64_t target) noexcept
    -> std::uint64_t {
    return value < target ? target - value : value - target;
}

template <class Count, class Trial, class Outcome, class EmptyHistory,
          class AcquireCandidates, class PrepareEmpty, class PrepareCandidates,
          class RunEmpty, class RunCandidate, class MakeCount, class MakeTrial,
          class MakeOutcome>
[[nodiscard]] auto calibration_control(
    CalibrationDurationPolicy policy, EmptyHistory& empty_trials,
    AcquireCandidates&& acquire_candidates, PrepareEmpty&& prepare_empty,
    PrepareCandidates&& prepare_candidates, RunEmpty&& run_empty,
    RunCandidate&& run_candidate, std::uint64_t maximum_units,
    MakeCount&& make_count, MakeTrial&& make_trial, MakeOutcome&& make_outcome) {
    using EmptyResult = decltype(run_empty());
    using Error = typename EmptyResult::error_type;

    prepare_empty();
    for (std::size_t index = 0; index < prepared_empty_trial_count; ++index) {
        auto trial = run_empty();
        if (!trial)
            return std::expected<Outcome, Error>{
                std::unexpected{std::move(trial.error())}};
        empty_trials.push_back(std::move(*trial));
    }
    auto minimum = empty_trials.front().interval().elapsed();
    for (const auto& trial : std::span{empty_trials}.subspan(1U))
        minimum = std::min(minimum, trial.interval().elapsed());
    const auto estimate = TimerOverheadEstimate::create(minimum);

    auto& history = acquire_candidates();
    prepare_candidates(history);
    auto evaluate = [&](std::uint64_t units) -> std::expected<bool, Error> {
        const auto candidate = make_count(units);
        auto interval = run_candidate(candidate);
        if (!interval)
            return std::unexpected{std::move(interval.error())};
        history.push_back(make_trial(candidate, std::move(*interval)));
        return history.back().interval().elapsed() == policy.target_duration();
    };

    std::uint64_t current_units = 1U;
    auto initial = evaluate(current_units);
    if (!initial)
        return std::expected<Outcome, Error>{
            std::unexpected{std::move(initial.error())}};
    bool exact = *initial;

    std::optional<std::uint64_t> lower_units;
    std::optional<std::uint64_t> upper_units;
    if (!exact) {
        if (history.back().interval().elapsed() < policy.target_duration())
            lower_units = current_units;
        else
            upper_units = current_units;
    }

    while (!exact && history.back().interval().elapsed() < policy.target_duration() &&
           current_units < maximum_units) {
        const auto remaining = maximum_units - current_units;
        const auto next_units =
            current_units > remaining ? maximum_units : current_units * 2U;
        current_units = next_units;
        auto evaluated = evaluate(current_units);
        if (!evaluated)
            return std::expected<Outcome, Error>{
                std::unexpected{std::move(evaluated.error())}};
        exact = *evaluated;
        if (history.back().interval().elapsed() < policy.target_duration())
            lower_units = current_units;
        else
            upper_units = current_units;
    }

    while (!exact && lower_units && upper_units &&
           *upper_units - *lower_units > 1U) {
        const auto mid = *lower_units + ((*upper_units - *lower_units) / 2U);
        auto evaluated = evaluate(mid);
        if (!evaluated)
            return std::expected<Outcome, Error>{
                std::unexpected{std::move(evaluated.error())}};
        exact = *evaluated;
        if (history.back().interval().elapsed() < policy.target_duration())
            lower_units = mid;
        else
            upper_units = mid;
    }

    std::optional<Count> selected;
    std::uint64_t selected_distance{};
    for (const auto& trial : history) {
        const auto elapsed = trial.interval().elapsed();
        if (elapsed < policy.minimum_duration() ||
            elapsed > policy.maximum_duration())
            continue;
        const auto trial_distance = calibration_distance(
            elapsed.value(), policy.target_duration().value());
        if (!selected || trial_distance < selected_distance ||
            (trial_distance == selected_distance &&
             trial.candidate().value() < selected->value())) {
            selected.reset();
            selected.emplace(trial.candidate());
            selected_distance = trial_distance;
        }
    }

    auto disposition = CalibrationDisposition::no_candidate_in_acceptable_band;
    if (selected)
        disposition = CalibrationDisposition::selected;
    else if (history.front().interval().elapsed() > policy.maximum_duration())
        disposition = CalibrationDisposition::minimum_candidate_above_maximum_duration;
    else if (current_units == maximum_units &&
             history.back().interval().elapsed() < policy.minimum_duration())
        disposition = CalibrationDisposition::maximum_candidate_below_minimum_duration;

    return std::expected<Outcome, Error>{make_outcome(
        std::move(policy), std::move(empty_trials), estimate, std::move(history),
        disposition, selected)};
}

[[nodiscard]] inline auto rational_less_equal(
    std::uint64_t left_numerator, std::uint64_t left_denominator,
    std::uint64_t right_numerator, std::uint64_t right_denominator) noexcept -> bool {
    bool reversed = false;
    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient)
            return reversed ? left_quotient > right_quotient
                            : left_quotient < right_quotient;
        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            if (left_remainder == 0U && right_remainder == 0U)
                return true;
            return reversed ? right_remainder == 0U : left_remainder == 0U;
        }
        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        reversed = !reversed;
    }
}

[[nodiscard]] inline auto evaluate_latest_window_control(
    std::span<const WarmupTrial> trials, std::size_t window_size,
    WarmupStabilityThreshold threshold, std::span<std::uint64_t> scratch) noexcept
    -> WarmupStabilityEvaluation {
    if (scratch.size() != window_size)
        std::terminate();
    const auto window_begin = trials.size() - window_size;
    for (std::size_t index = 0; index < window_size; ++index)
        scratch[index] = trials[window_begin + index].interval().elapsed().value();
    std::sort(scratch.begin(), scratch.end());
    const auto median = scratch[(scratch.size() - 1U) / 2U];
    std::uint64_t maximum_deviation = 0U;
    bool stable = true;
    for (const auto sample : scratch) {
        const auto deviation = sample < median ? median - sample : sample - median;
        maximum_deviation = std::max(maximum_deviation, deviation);
        if (median == 0U)
            stable = stable && sample == 0U;
        else
            stable = stable && rational_less_equal(
                                   deviation, median, threshold.numerator(),
                                   threshold.denominator());
    }
    return WarmupStabilityEvaluation::create(
        core::Duration::from_base_units(median),
        core::Duration::from_base_units(maximum_deviation), stable);
}

template <class Count, class Outcome, class History, class Run, class Evaluate,
          class MakeOutcome>
[[nodiscard]] auto warmup_control(WarmupPolicy policy, Count selected_work,
                                  History& trials, Run&& run,
                                  Evaluate&& evaluate, MakeOutcome&& make_outcome) {
    using RunResult = decltype(run(selected_work));
    using Error = typename RunResult::error_type;
    if (policy.maximum_trials() == 0U)
        return std::expected<Outcome, Error>{make_outcome(
            std::move(policy), selected_work, std::move(trials),
            WarmupDisposition::skipped, std::nullopt)};

    std::optional<WarmupStabilityEvaluation> final_evaluation;
    for (std::uint64_t trial_count = 1U;
         trial_count <= policy.maximum_trials(); ++trial_count) {
        auto interval = run(selected_work);
        if (!interval)
            return std::expected<Outcome, Error>{
                std::unexpected{std::move(interval.error())}};
        trials.push_back(WarmupTrial::create(std::move(*interval)));
        if (trial_count >= policy.minimum_trials() &&
            trial_count >= policy.stability_window()) {
            final_evaluation.emplace(evaluate(
                std::span<const WarmupTrial>{trials}, policy.stability_window(),
                policy.stability_threshold()));
            if (final_evaluation->stable())
                return std::expected<Outcome, Error>{make_outcome(
                    std::move(policy), selected_work, std::move(trials),
                    WarmupDisposition::stabilized, final_evaluation)};
        }
    }
    return std::expected<Outcome, Error>{make_outcome(
        std::move(policy), selected_work, std::move(trials),
        WarmupDisposition::maximum_trials_reached_without_stability,
        final_evaluation)};
}

} // namespace epycbench::measurement::detail
