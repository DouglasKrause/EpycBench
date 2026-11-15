#pragma once

#include "monitoring/host_q0b_backend_driver.hpp"
#include "platform_linux/host_baseline_file_export.hpp"
#include "platform_linux/host_work_admission.hpp"

namespace epycbench::monitoring::qualification::detail {

enum class HostQ0bWorkReason : unsigned char {
    invalid_input, association, admission, native_limits, projection,
    allocation, state, retirement, contained, source_close, projection_close, calibration, clock, deadline, cancelled
};
enum class HostQ0bWorkPhase : unsigned char {
    prepared, exported, bandwidth_ready, bandwidth_running, bandwidth_retired,
    latency_ready, latency_running, completed, terminal
};
struct HostQ0bGovernedLeaves final {
    platform_linux::host_work::ReadOperations headroom;
    platform_linux::host_work::LimitOperations limits;
    platform_linux::host_baseline_file::Operations projection;
};
// Borrows the actual invocation clock/cancellation owner. The enclosing parent
// supplies the unchanged RAW interval from before child creation.
struct HostQ0bInvocationGuard final {
    platform_linux::host_work::GuardOperations operations;
    std::uint64_t origin_ns{}, end_ns{}, last_observed_ns{};
};
struct HostQ0bWorkSnapshot final {
    HostQ0bWorkPhase phase{HostQ0bWorkPhase::terminal};
    platform_linux::host_work::Resources outstanding{};
    bool poisoned{}, original_retained{}, projection_exported{}, successful{};
    std::size_t preparations{}, finalizations{}, retirements{};
    std::optional<HostQ0bWorkReason> failure{};
    std::optional<platform_linux::host_work::Reason> native_failure{};
    std::optional<platform_linux::detail::RawStatusResult> guard_native{};
    std::uint64_t last_accepted_raw_ns{};
};
struct HostQ0bGovernedState;
struct HostQ0bGovernedWorkTestAccess;
class HostFrozenSourceChildQuota;
struct HostFrozenSourceLaunchState;

// Private C0 composition. Leaf contexts and lower dependencies are
// borrowed until destruction. Successful preparation alone selects no work.
class HostQ0bGovernedWork final {
  public:
    HostQ0bGovernedWork(const HostQ0bGovernedWork&) = delete;
    auto operator=(const HostQ0bGovernedWork&) -> HostQ0bGovernedWork& = delete;
    HostQ0bGovernedWork(HostQ0bGovernedWork&&) noexcept;
    auto operator=(HostQ0bGovernedWork&&) -> HostQ0bGovernedWork& = delete;
    ~HostQ0bGovernedWork() noexcept;

    // Failure preserves the caller's descriptor. All fallible new storage is
    // prepared before the final noexcept ownership transfer.
    [[nodiscard]] static auto prepare(int expected_parent,
        platform_linux::FileDescriptor&, HostQ0bBackendDependencies,
        HostQ0bGovernedLeaves) noexcept
        -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason>;
    [[nodiscard]] static auto prepare_governed(int expected_parent,
        platform_linux::FileDescriptor&, HostQ0bBackendDependencies,
        HostQ0bGovernedLeaves, HostQ0bInvocationGuard) noexcept
        -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason>;
    [[nodiscard]] auto run_once(const std::filesystem::path&, std::string_view,
        runtime::ControllerProcessProductionAuthority&) noexcept
        -> std::expected<MonitoringHostQ0bBackendResult, Q0bBackendFailure>;
    [[nodiscard]] auto snapshot() const noexcept -> HostQ0bWorkSnapshot;
    [[nodiscard]] auto original_projection() const noexcept -> std::string_view;

  private:
    [[nodiscard]] static auto prepare_source(const HostFrozenSourceChildQuota&,
        platform_linux::FileDescriptor&, HostQ0bBackendDependencies, HostQ0bGovernedLeaves) noexcept
        -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason>;
    [[nodiscard]] static auto prepare_impl(int expected_parent,
        platform_linux::FileDescriptor&, HostQ0bBackendDependencies, HostQ0bGovernedLeaves,
        const HostFrozenSourceChildQuota*, std::optional<HostQ0bInvocationGuard> = std::nullopt) noexcept
        -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason>;
    explicit HostQ0bGovernedWork(std::shared_ptr<HostQ0bGovernedState>) noexcept;
    [[nodiscard]] auto begin() noexcept -> bool;
    [[nodiscard]] auto dependencies() const noexcept -> HostQ0bBackendDependencies;
    void end() noexcept;
    std::shared_ptr<HostQ0bGovernedState> state_;
    friend struct HostQ0bGovernedWorkTestAccess;
    friend struct HostFrozenSourceLaunchState;
};

[[nodiscard]] auto production_host_q0b_governed_leaves() noexcept -> HostQ0bGovernedLeaves;

} // namespace epycbench::monitoring::qualification::detail
