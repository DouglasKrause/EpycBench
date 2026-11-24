#pragma once

#include "measurement/package_monitoring.hpp"
#include "platform_linux/package_execution.hpp"

namespace epycbench::platform_linux::package_execution {

// One optional, pinned polling owner per measured group. Worker endpoints and
// per-thread event streams are collected by each worker itself.
class PackageMonitor {
  public:
    PackageMonitor(measurement::package_monitoring::Policy, std::span<const core::LinuxCpuId>,
                   bool supplied);
    ~PackageMonitor();
    PackageMonitor(const PackageMonitor&) = delete;
    auto operator=(const PackageMonitor&) -> PackageMonitor& = delete;

    void stop() noexcept;
    void complete(nlohmann::json&, core::LinuxCpuId, std::uint64_t begin, std::uint64_t end) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
    friend class PackageMonitorClock;
};

class PackageMonitorClock {
  public:
    PackageMonitorClock(Clock, PackageMonitor*, core::LinuxCpuId, const nlohmann::json* supplied,
                        nlohmann::json& retained);
    ~PackageMonitorClock();
    PackageMonitorClock(const PackageMonitorClock&) = delete;
    auto operator=(const PackageMonitorClock&) -> PackageMonitorClock& = delete;
    [[nodiscard]] auto clock() noexcept -> Clock;
    void begin_bracket();
    void end_bracket();

  private:
    struct State;
    static auto now(void*) -> std::expected<std::uint64_t, core::Error>;
    Clock clock_;
    std::unique_ptr<State> state_;
};

} // namespace epycbench::platform_linux::package_execution
