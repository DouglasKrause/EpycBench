#include "measurement/package_control.hpp"

#include "measurement/execution_control_math.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace epycbench::measurement::package_control {
namespace {

constexpr std::size_t maximum_warmup_trials = 64U;

auto invalid(std::string_view code, std::string_view message) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{.category = core::ErrorCategory::validation,
        .code = core::StableErrorCode{std::string{code}}, .message = std::string{message},
        .operation = "Package.measurement_control", .os_error = std::nullopt, .context = {}}};
}

auto next_count(std::uint64_t current, std::uint64_t ceiling, std::optional<std::uint64_t> lower,
                std::optional<std::uint64_t> upper) noexcept -> std::optional<std::uint64_t> {
    if (upper) {
        if (lower && *upper - *lower > 1U) return detail::calibration_midpoint(*lower, *upper);
        return std::nullopt;
    }
    if (current == ceiling) return std::nullopt;
    return detail::calibration_double(current, ceiling);
}

auto stable(std::span<const std::uint64_t> durations, const WarmupSettings& settings) noexcept -> bool {
    std::array<std::uint64_t, maximum_warmup_trials> sorted{};
    const auto window = std::span{sorted}.first(settings.stability_window);
    std::ranges::copy(durations.last(settings.stability_window), window.begin());
    return detail::evaluate_elapsed_window_control(window, settings.stability_numerator,
                                                   settings.stability_denominator).stable;
}

} // namespace

auto next_probe(const CalibrationSettings& settings, std::size_t role_count,
                std::span<const CalibrationProbe> probes) -> std::expected<CalibrationDecision, core::Error> {
    if (role_count == 0 || settings.minimum_ns == 0 || settings.minimum_ns > settings.target_ns ||
        settings.target_ns > settings.maximum_ns || settings.maximum_repetitions == 0 ||
        settings.maximum_probes == 0 || settings.maximum_probes > detail::maximum_calibration_probes)
        return invalid("measurement.package.calibration_policy_invalid", "Package calibration policy is invalid");

    std::optional<std::uint64_t> candidate{1}, lower, upper, selected;
    std::uint64_t selected_distance{};
    auto status = CalibrationStatus::next_probe;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        const auto& probe = probes[i];
        const auto& durations = probe.role_durations_ns;
        if (!candidate || probe.repetitions != *candidate ||
            (!durations.empty() && (durations.size() != role_count || std::ranges::contains(durations, 0U))))
            return invalid("measurement.package.calibration_trace_invalid",
                           "Package calibration observations do not match the required probe sequence");
        if (durations.empty()) {
            status = CalibrationStatus::probe_failed;
            candidate.reset();
            continue;
        }

        const auto [fastest, slowest] = std::ranges::minmax(durations);
        const auto distance = std::max(detail::calibration_distance(fastest, settings.target_ns),
                                       detail::calibration_distance(slowest, settings.target_ns));
        const auto count = *candidate;
        if (fastest >= settings.minimum_ns && slowest <= settings.maximum_ns &&
            (!selected || distance < selected_distance || (distance == selected_distance && count < *selected))) {
            selected = count;
            selected_distance = distance;
        }
        const auto undershoot = fastest < settings.target_ns ? settings.target_ns - fastest : 0U;
        const auto overshoot = slowest > settings.target_ns ? slowest - settings.target_ns : 0U;
        const bool below_band = fastest < settings.minimum_ns;
        const bool above_band = slowest > settings.maximum_ns;
        const bool search_higher = below_band != above_band ? below_band : undershoot > overshoot;
        if (search_higher) lower = count;
        else upper = count;
        candidate = i + 1U == settings.maximum_probes || (selected && selected_distance == 0U)
            ? std::nullopt : next_count(count, settings.maximum_repetitions, lower, upper);
    }
    if (candidate) return CalibrationDecision{CalibrationStatus::next_probe, candidate};
    if (status == CalibrationStatus::probe_failed) return CalibrationDecision{status, std::nullopt};
    return CalibrationDecision{selected ? CalibrationStatus::selected : CalibrationStatus::no_common_count, selected};
}

auto warmup_decision(const WarmupSettings& settings, std::span<const std::uint64_t> durations)
    -> std::expected<WarmupStatus, core::Error> {
    if (settings.minimum_trials == 0 || settings.maximum_trials > maximum_warmup_trials ||
        settings.minimum_trials > settings.maximum_trials || settings.stability_window == 0 ||
        settings.stability_window > settings.maximum_trials || settings.stability_denominator == 0 ||
        settings.stability_numerator > settings.stability_denominator)
        return invalid("measurement.package.warmup_policy_invalid", "Package warmup policy is invalid");

    auto status = WarmupStatus::continue_trials;
    for (std::size_t i = 0; i < durations.size(); ++i) {
        const auto count = i + 1U;
        if (status != WarmupStatus::continue_trials)
            return invalid("measurement.package.warmup_trace_invalid",
                           "Package warmup observations continue beyond the first terminal attempt");
        if (durations[i] == 0U) status = WarmupStatus::trial_failed;
        else if (count >= settings.minimum_trials && count >= settings.stability_window &&
                 stable(durations.first(count), settings)) status = WarmupStatus::stabilized;
        else if (count == settings.maximum_trials) status = WarmupStatus::maximum_trials_reached_without_stability;
    }
    return status;
}

} // namespace epycbench::measurement::package_control
