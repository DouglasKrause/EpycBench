#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/platform_linux/file_descriptor.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <optional>

namespace epycbench::platform_linux {

struct HardwareCounterValues {
    std::uint64_t cpu_cycles;
    std::uint64_t retired_instructions;
    core::Duration enabled;
    core::Duration running;
};

enum class HardwareCounterCoverage { complete, multiplexed, not_scheduled };

struct CounterTransition {
    RawMonotonicTimestamp before;
    RawMonotonicTimestamp after;
};

struct HardwareCounterWindow {
    CounterTransition enable;
    CounterTransition disable;
    HardwareCounterValues counts;

    [[nodiscard]] auto coverage() const noexcept -> HardwareCounterCoverage {
        if (counts.running.value() == 0U) return HardwareCounterCoverage::not_scheduled;
        if (counts.running < counts.enabled) return HardwareCounterCoverage::multiplexed;
        return HardwareCounterCoverage::complete;
    }
};

class HardwareCounterGroup {
  public:
    // The caller owns the admitted worker throughout the attached lifetime.
    [[nodiscard]] static auto attach(LinuxThreadId) -> std::expected<HardwareCounterGroup, core::Error>;

    HardwareCounterGroup(HardwareCounterGroup&&) noexcept = default;
    auto operator=(HardwareCounterGroup&&) -> HardwareCounterGroup& = delete;
    HardwareCounterGroup(const HardwareCounterGroup&) = delete;
    auto operator=(const HardwareCounterGroup&) -> HardwareCounterGroup& = delete;

    // All calls are serialized with the worker admission and timing lifecycle.
    [[nodiscard]] auto begin() -> std::expected<void, core::Error>;
    [[nodiscard]] auto finish() -> std::expected<HardwareCounterWindow, core::Error>;

  private:
    struct Baseline {
        HardwareCounterValues values;
        CounterTransition enable;
    };

    HardwareCounterGroup(FileDescriptor cycles, FileDescriptor instructions,
                         std::uint64_t cycles_id, std::uint64_t instructions_id) noexcept;
    [[nodiscard]] auto read() const -> std::expected<HardwareCounterValues, core::Error>;
    void retire_uncertain_group() noexcept;

    FileDescriptor cycles_;
    FileDescriptor instructions_;
    std::uint64_t cycles_id_;
    std::uint64_t instructions_id_;
    std::optional<Baseline> baseline_;
};

} // namespace epycbench::platform_linux
