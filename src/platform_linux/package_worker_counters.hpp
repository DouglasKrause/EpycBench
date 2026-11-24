#pragma once

#include "epycbench/monitoring/hardware_counter_evidence.hpp"

namespace epycbench::platform_linux::package_execution {

struct CounterEvidence {
    monitoring::WorkerHardwareCounterEvidence worker;
    std::optional<RawMonotonicTimestamp> enclosing_start;
    std::optional<RawMonotonicTimestamp> enclosing_end;
};

class WorkerPerfBracket {
  public:
    // Attach on the pinned thread that owns this rank. Supplied worker clocks
    // carry an explicit unavailable reason and never enter the perf provider.
    [[nodiscard]] static auto attach(core::WorkerId, bool supplied) -> WorkerPerfBracket;
    void begin();
    [[nodiscard]] auto finish(std::optional<std::uint64_t> callback_start, std::optional<std::uint64_t> callback_end)
        -> CounterEvidence;

  private:
    explicit WorkerPerfBracket(core::WorkerId id) : id_{id} {}
    core::WorkerId id_;
    std::optional<HardwareCounterGroup> group_;
    bool started_{};
    std::optional<monitoring::HardwareCounterUnavailable> unavailable_;
};

} // namespace epycbench::platform_linux::package_execution
