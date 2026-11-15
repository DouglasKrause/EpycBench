#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include "kernel/mix64.hpp"

#include <cstdint>

namespace epycbench::kernel {

PreparedSequentialReadBandwidthChecksum::PreparedSequentialReadBandwidthChecksum(
    std::uint64_t one_pass_expected) noexcept
    : one_pass_expected_{one_pass_expected} {}

auto PreparedSequentialReadBandwidthChecksum::prepare(
    const PreparedSequentialReadBandwidth& prepared) noexcept
    -> PreparedSequentialReadBandwidthChecksum {
    std::uint64_t one_pass_expected = 0U;
    for (std::size_t index = 0; index < prepared.word_count(); ++index) {
        one_pass_expected += detail::mix64_v1(static_cast<std::uint64_t>(index));
    }
    return PreparedSequentialReadBandwidthChecksum{one_pass_expected};
}

auto PreparedSequentialReadBandwidthChecksum::expected_terminal(
    SequentialReadBandwidthPassCount passes) const noexcept
    -> SequentialReadBandwidthExpectedTerminalValue {
    return SequentialReadBandwidthExpectedTerminalValue{one_pass_expected_ * passes.value()};
}

SequentialReadBandwidthExpectedTerminalValue::
    SequentialReadBandwidthExpectedTerminalValue(std::uint64_t value) noexcept
    : value_{value} {}

SequentialReadBandwidthVerification::SequentialReadBandwidthVerification(
    SequentialReadBandwidthExpectedTerminalValue expected,
    SequentialReadBandwidthTerminalValue actual) noexcept
    : expected_{expected}, actual_{actual} {}

auto verify_sequential_read_bandwidth_checksum(
    SequentialReadBandwidthExpectedTerminalValue expected,
    SequentialReadBandwidthTerminalValue actual) noexcept
    -> SequentialReadBandwidthVerification {
    return SequentialReadBandwidthVerification{expected, actual};
}

} // namespace epycbench::kernel
