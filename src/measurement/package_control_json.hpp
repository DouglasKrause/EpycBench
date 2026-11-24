#pragma once

#include "measurement/package_control.hpp"

#include <nlohmann/json.hpp>

namespace epycbench::measurement::package_control {
inline CalibrationSettings calibration_settings(const nlohmann::json& execution) {
    const auto& c = execution.at("calibration");
    return {c.at("minimum_ns").get<std::uint64_t>(), c.at("target_ns").get<std::uint64_t>(),
            c.at("maximum_ns").get<std::uint64_t>(),
            execution.at("repetitions").get<std::uint64_t>(),
            c.at("max_probes").get<std::size_t>()};
}

inline WarmupSettings warmup_settings(const nlohmann::json& execution) {
    const auto& w = execution.at("warmup");
    return {w.at("minimum_trials").get<std::size_t>(), w.at("maximum_trials").get<std::size_t>(),
            w.at("stability_window").get<std::size_t>(),
            w.at("stability_numerator").get<std::uint64_t>(),
            w.at("stability_denominator").get<std::uint64_t>()};
}

inline std::string_view disposition(CalibrationStatus status) {
    switch (status) {
    case CalibrationStatus::next_probe:
        return "next_probe";
    case CalibrationStatus::selected:
        return "selected";
    case CalibrationStatus::no_common_count:
        return "no_common_count";
    case CalibrationStatus::probe_failed:
        return "probe_failed";
    }
    throw std::logic_error("Invalid common calibration disposition");
}

inline std::string_view disposition(WarmupStatus status) {
    switch (status) {
    case WarmupStatus::continue_trials:
        return "continue_trials";
    case WarmupStatus::stabilized:
        return "stabilized";
    case WarmupStatus::maximum_trials_reached_without_stability:
        return "maximum_trials_reached_without_stability";
    case WarmupStatus::trial_failed:
        return "trial_failed";
    }
    throw std::logic_error("Invalid warmup disposition");
}
} // namespace epycbench::measurement::package_control
