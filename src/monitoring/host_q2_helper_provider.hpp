#pragma once
#include "monitoring/host_q2_controlled_event_session.hpp"
#include "monitoring/host_q2_native_thread_owner.hpp"
namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ2HelperCompactFailure final {
    MonitoringQ2CompactDiagnostic primary;
    MonitoringQ2CompactLedger<16> cleanup;
};
struct Q2HelperProviderOperations {
    void* context{};
    std::expected<std::uintptr_t, core::Error> (*create)(void*, void*,
                                                         void (*)(void*) noexcept) noexcept {};
    Q2LifecycleResult (*await_parked)(void*, std::uintptr_t,
                                      MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*release_start)(void*, std::uintptr_t) noexcept {};
    Q2LifecycleResult (*set_affinity)(void*, std::uintptr_t, core::LinuxCpuId) noexcept {};
    std::expected<core::CpuSet, core::Error> (*get_affinity)(void*, std::uintptr_t) noexcept {};
    std::expected<int, core::Error> (*get_policy)(void*, std::uintptr_t) noexcept {};
    Q2LifecycleResult (*registry_add)(void*, std::uintptr_t) noexcept {};
    Q2LifecycleResult (*stop)(void*, std::uintptr_t) noexcept {};
    Q2LifecycleResult (*wake)(void*, std::uintptr_t) noexcept {};
    Q2LifecycleResult (*terminal)(void*, std::uintptr_t,
                                  MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*join)(void*, std::uintptr_t,
                              MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*close)(void*, std::uintptr_t) noexcept {};
    std::expected<bool, core::Error> (*registry_absent)(void*, std::uintptr_t) noexcept {};
};
class MonitoringQ2HelperProvider final {
  public:
    static auto create() noexcept -> std::expected<MonitoringQ2HelperProvider, core::Error>;
    static auto create(Q2HelperProviderOperations) noexcept
        -> std::expected<MonitoringQ2HelperProvider, core::Error>;
    MonitoringQ2HelperProvider(MonitoringQ2HelperProvider&&) noexcept;
    MonitoringQ2HelperProvider(const MonitoringQ2HelperProvider&) = delete;
    ~MonitoringQ2HelperProvider() noexcept;
    auto operations() noexcept -> Q2HelperOperations;
    auto retire_recovery(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, MonitoringQ2HelperCompactFailure>;

  private:
    struct State;
    explicit MonitoringQ2HelperProvider(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};
} // namespace epycbench::monitoring::qualification::detail
