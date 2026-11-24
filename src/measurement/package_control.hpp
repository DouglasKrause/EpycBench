#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::measurement::package_control {

struct CalibrationSettings {
    std::uint64_t minimum_ns, target_ns, maximum_ns, maximum_repetitions;
    std::size_t maximum_probes;
};

struct CalibrationProbe {
    std::uint64_t repetitions;
    // An empty list records a failed probe; otherwise every role has one
    // positive, independently verified interval envelope in nanoseconds.
    std::vector<std::uint64_t> role_durations_ns;
};

enum class CalibrationStatus { next_probe, selected, no_common_count, probe_failed };

struct CalibrationDecision {
    CalibrationStatus status;
    std::optional<std::uint64_t> repetitions;
};

// Replay the exact verified observation prefix. A count is returned only when
// it is the next required probe or the selected count.
[[nodiscard]] auto next_probe(const CalibrationSettings&, std::size_t role_count,
                              std::span<const CalibrationProbe> = {})
    -> std::expected<CalibrationDecision, core::Error>;

struct WarmupSettings {
    std::size_t minimum_trials, maximum_trials, stability_window;
    std::uint64_t stability_numerator, stability_denominator;
};

enum class WarmupStatus { continue_trials, stabilized, maximum_trials_reached_without_stability, trial_failed };

// Each duration is a role's verified wall envelope. Zero retains a failed
// attempt. The first terminal decision ends the permitted observation prefix.
[[nodiscard]] auto warmup_decision(const WarmupSettings&, std::span<const std::uint64_t> = {})
    -> std::expected<WarmupStatus, core::Error>;

} // namespace epycbench::measurement::package_control
