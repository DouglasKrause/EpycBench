#include "epycbench/monitoring/hardware_counter_evidence.hpp"

namespace epycbench::monitoring {
namespace {
auto rejected(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.hardware_counters.invalid_window"},
        .message = "Invalid worker hardware counter window",
        .operation = "hardware_counter_evidence",
        .os_error = {},
        .context = {{"classification", classification}},
        .continuation = core::ContinuationPolicy::skip_item,
    }};
}
} // namespace

auto WorkerHardwareCounterEvidence::create(core::WorkerId worker_id,
    platform_linux::HardwareCounterWindow window,
    platform_linux::RawMonotonicTimestamp enclosing_start,
    platform_linux::RawMonotonicTimestamp enclosing_end)
    -> std::expected<WorkerHardwareCounterEvidence, core::Error> {
    if (window.enable.after < window.enable.before ||
        window.disable.before < window.enable.after ||
        window.disable.after < window.disable.before) {
        return rejected("transition_order");
    }
    // Use the conservative inner boundary of each transition bracket so the
    // entire measured interval is known to fall between enable and disable.
    if (enclosing_end < enclosing_start || enclosing_start < window.enable.after ||
        window.disable.before < enclosing_end) {
        return rejected("interval_coverage");
    }
    if (window.counts.enabled < window.counts.running) {
        return rejected("time_coverage");
    }
    return WorkerHardwareCounterEvidence{worker_id, window};
}

auto WorkerHardwareCounterEvidence::unavailable(core::WorkerId worker_id,
    HardwareCounterUnavailable reason) noexcept -> WorkerHardwareCounterEvidence {
    return WorkerHardwareCounterEvidence{worker_id, reason};
}

auto WorkerHardwareCounterEvidence::window() const noexcept
    -> const platform_linux::HardwareCounterWindow* {
    return std::get_if<platform_linux::HardwareCounterWindow>(&acquisition_);
}

auto WorkerHardwareCounterEvidence::coverage() const noexcept
    -> std::optional<platform_linux::HardwareCounterCoverage> {
    if (const auto* available = window()) return available->coverage();
    return std::nullopt;
}

auto WorkerHardwareCounterEvidence::unavailable_reason() const noexcept
    -> std::optional<HardwareCounterUnavailable> {
    if (const auto* unavailable = std::get_if<HardwareCounterUnavailable>(&acquisition_)) {
        return *unavailable;
    }
    return std::nullopt;
}

} // namespace epycbench::monitoring
