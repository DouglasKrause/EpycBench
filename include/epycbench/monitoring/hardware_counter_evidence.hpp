#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/platform_linux/hardware_counters.hpp"

#include <expected>
#include <optional>
#include <variant>

namespace epycbench::monitoring {

enum class HardwareCounterUnavailable {
    unsupported_event,
    access_denied,
    worker_retired,
    attachment_failed,
    begin_failed,
    finish_failed,
    invalid_window,
    supplied_workers,
};

class WorkerHardwareCounterEvidence {
  public:
    [[nodiscard]] static auto create(core::WorkerId, platform_linux::HardwareCounterWindow,
                                     platform_linux::RawMonotonicTimestamp enclosing_start,
                                     platform_linux::RawMonotonicTimestamp enclosing_end)
        -> std::expected<WorkerHardwareCounterEvidence, core::Error>;
    [[nodiscard]] static auto unavailable(core::WorkerId, HardwareCounterUnavailable) noexcept
        -> WorkerHardwareCounterEvidence;

    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId { return worker_id_; }
    [[nodiscard]] auto window() const noexcept -> const platform_linux::HardwareCounterWindow*;
    [[nodiscard]] auto coverage() const noexcept
        -> std::optional<platform_linux::HardwareCounterCoverage>;
    [[nodiscard]] auto unavailable_reason() const noexcept -> std::optional<HardwareCounterUnavailable>;

  private:
    using Acquisition = std::variant<platform_linux::HardwareCounterWindow, HardwareCounterUnavailable>;
    WorkerHardwareCounterEvidence(core::WorkerId worker_id, Acquisition acquisition) noexcept
        : worker_id_{worker_id}, acquisition_{acquisition} {}

    core::WorkerId worker_id_;
    Acquisition acquisition_;
};

} // namespace epycbench::monitoring
