#include "epycbench/kernel/dependent_load_latency.hpp"
#ifdef EPYCBENCH_KERNEL_REFERENCE_EXECUTION
#include "epycbench/kernel/detail/memory_work.hpp"
#endif
#include <exception>

namespace epycbench::kernel {
DependentLoadLatencyTerminalValue::DependentLoadLatencyTerminalValue(
    std::size_t value) noexcept : value_{value} {}

[[gnu::noinline]] auto execute_dependent_load_latency(
    const PreparedDependentLoadLatency& prepared,
    DependentLoadLatencySchedule schedule) noexcept -> DependentLoadLatencyTerminalValue {
    const auto bytes = prepared.input().logical_region();
    if (const auto* module = prepared.module()) {
        const EpycBenchKernelRequestV1 request{
            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
            schedule.step_count().value(), schedule.start_index()};
        return DependentLoadLatencyTerminalValue{static_cast<std::size_t>(module->execute(&request))};
    }
#ifdef EPYCBENCH_KERNEL_REFERENCE_EXECUTION
    return DependentLoadLatencyTerminalValue{detail::dependent_load_work(
        reinterpret_cast<const unsigned char*>(bytes.data()), schedule.start_index(),
        schedule.step_count().value())};
#else
    std::terminate();
#endif
}
} // namespace epycbench::kernel
