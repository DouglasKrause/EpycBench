#pragma once

#include "epycbench/measurement/observation_execution.hpp"
#include "epycbench/measurement/validity.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/platform_linux/memory_mapping.hpp"
#include "epycbench/platform_linux/numa_policy.hpp"
#include "epycbench/platform_linux/page_location.hpp"

#include <thread>

namespace epycbench::runtime {
namespace detail { struct PairwiseProductionOperationsAdapterAccess; }

// Injection stops at host-facing primitives. The production run composes the
// Memory, Kernel, worker, Monitoring, Measurement, and validity lifecycles.
struct PairwiseProductionHostOperations {
    std::shared_ptr<void> context;
    measurement::detail::RawMonotonicClockOperations clock{};
    std::expected<platform_linux::LinuxThreadId, core::Error> (*current_thread_id)(void*){};
    std::expected<void, core::Error> (*set_current_thread_affinity)(void*, const core::CpuSet&){};
    std::expected<core::CpuSet, core::Error> (*get_current_thread_affinity)(void*){};
    std::expected<core::LinuxCpuId, core::Error> (*current_cpu)(void*){};
    std::expected<std::jthread, core::Error> (*launch_worker)(
        void*, void (*)(void*, std::size_t) noexcept, void*, std::size_t){};
    std::expected<platform_linux::AnonymousMapping, core::Error> (*map)(void*, std::size_t){};
    std::expected<platform_linux::NativeNumaMask, core::Error> (*numa_mask)(
        void*, const core::LinuxNumaNodeSet&){};
    std::expected<void, core::Error> (*bind)(
        void*, const platform_linux::AnonymousMapping&, const platform_linux::NativeNumaMask&){};
    std::expected<void, core::Error> (*write_prefault_byte)(
        void*, std::span<std::byte>, std::size_t, std::byte){};
    std::expected<std::vector<platform_linux::PageQueryStatus>, core::Error> (*query_pages)(
        void*, std::span<void* const>){};
    std::expected<void, core::Error> (*protect_read_only)(
        void*, platform_linux::AnonymousMapping&){};
    monitoring::MonitoringDriverOperations monitoring{};
    kernel::KernelImplementationId bandwidth_implementation{
        kernel::KernelImplementationId::scalar_u64_sequential_v1};
    kernel::KernelImplementationId latency_implementation{
        kernel::KernelImplementationId::scalar_u64_random_cycle_v1};
};

class PairwiseProductionOperationsAdapter {
  public:
    struct State;
    explicit PairwiseProductionOperationsAdapter(PairwiseProductionHostOperations host);
    [[nodiscard]] auto calibration_operations() const noexcept
        -> std::expected<CalibrationOnlyProductionOperations, core::Error>;

  private:
    static auto prepare_calibration_run(
        void*, const CalibrationOnlyProductionAdmission&) noexcept
        -> CalibrationOnlyOperationResult<CalibrationOnlyProductionRun>;
    std::shared_ptr<State> state_;
    friend struct detail::PairwiseProductionOperationsAdapterAccess;
};

} // namespace epycbench::runtime
