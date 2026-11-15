#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#ifdef EPYCBENCH_KERNEL_REFERENCE_EXECUTION
#include "epycbench/kernel/detail/memory_work.hpp"
#endif
#include <exception>

namespace epycbench::kernel {
SequentialReadBandwidthTerminalValue::SequentialReadBandwidthTerminalValue(
    std::uint64_t value) noexcept : value_{value} {}

[[gnu::noinline]] auto execute_sequential_read_bandwidth(
    const PreparedSequentialReadBandwidth& prepared,
    SequentialReadBandwidthPassCount passes) noexcept -> SequentialReadBandwidthTerminalValue {
    const auto bytes = prepared.input().logical_region();
    if (const auto* module = prepared.module()) {
        const EpycBenchKernelRequestV1 request{
            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), passes.value(), 0};
        return SequentialReadBandwidthTerminalValue{module->execute(&request)};
    }
#ifdef EPYCBENCH_KERNEL_REFERENCE_EXECUTION
    return SequentialReadBandwidthTerminalValue{detail::sequential_read_work(
        reinterpret_cast<const unsigned char*>(bytes.data()), prepared.word_count(), passes.value())};
#else
    std::terminate(); // A public Run must bind its selected module before timing.
#endif
}
} // namespace epycbench::kernel
