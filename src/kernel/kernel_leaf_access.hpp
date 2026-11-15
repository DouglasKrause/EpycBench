#pragma once

#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"

namespace epycbench::kernel::detail {

struct KernelLeafAccess final {
    [[nodiscard]] static auto bandwidth_config(core::ByteCount bytes) noexcept
        -> SequentialReadBandwidthConfig {
        return SequentialReadBandwidthConfig{bytes};
    }
    [[nodiscard]] static auto latency_config(core::ByteCount bytes,
                                             PointerCycleSeedIntent seed) noexcept
        -> DependentLoadLatencyConfig {
        return DependentLoadLatencyConfig{bytes, std::move(seed)};
    }
    [[nodiscard]] static auto bandwidth_input(SequentialReadBandwidthConfig configuration,
                                              std::span<const std::byte> region) noexcept
        -> SequentialReadBandwidthInput {
        return SequentialReadBandwidthInput{std::move(configuration), region};
    }
    [[nodiscard]] static auto latency_input(DependentLoadLatencyConfig configuration,
                                            std::span<const std::byte> region) noexcept
        -> DependentLoadLatencyInput {
        return DependentLoadLatencyInput{std::move(configuration), region};
    }
    [[nodiscard]] static auto pass_count(std::uint64_t value) noexcept
        -> SequentialReadBandwidthPassCount {
        return SequentialReadBandwidthPassCount{value};
    }
    [[nodiscard]] static auto step_count(std::uint64_t value) noexcept
        -> DependentLoadLatencyStepCount {
        return DependentLoadLatencyStepCount{value};
    }
    [[nodiscard]] static auto bandwidth_work(core::ByteCount value) noexcept
        -> SequentialReadBandwidthWork {
        return SequentialReadBandwidthWork{value};
    }
    [[nodiscard]] static auto bandwidth_verification(std::uint64_t expected,
                                                     std::uint64_t actual) noexcept
        -> SequentialReadBandwidthVerification {
        return SequentialReadBandwidthVerification{
            SequentialReadBandwidthExpectedTerminalValue{expected},
            SequentialReadBandwidthTerminalValue{actual}};
    }
    [[nodiscard]] static auto schedule(std::size_t start,
                                       DependentLoadLatencyStepCount count) noexcept
        -> DependentLoadLatencySchedule {
        return DependentLoadLatencySchedule{start, count};
    }
    [[nodiscard]] static auto latency_verification(std::size_t expected,
                                                   std::size_t actual) noexcept
        -> DependentLoadLatencyVerification {
        return DependentLoadLatencyVerification{
            DependentLoadLatencyExpectedTerminalValue{expected},
            DependentLoadLatencyTerminalValue{actual}};
    }
    [[nodiscard]] static auto verification(std::vector<std::uint64_t> permutation,
                                           std::vector<std::size_t> inverse) noexcept
        -> PreparedDependentLoadLatencyVerification {
        return PreparedDependentLoadLatencyVerification{std::move(permutation),
                                                        std::move(inverse)};
    }
    [[nodiscard]] static auto verification_storage(
        const PreparedDependentLoadLatencyVerification& value) noexcept {
        return std::array<const void*, 2U>{value.permutation_.data(),
                                           value.inverse_position_.data()};
    }
    [[nodiscard]] static auto verification_capacities(
        const PreparedDependentLoadLatencyVerification& value) noexcept {
        return std::array<std::size_t, 2U>{value.permutation_.capacity(),
                                           value.inverse_position_.capacity()};
    }
};

} // namespace epycbench::kernel::detail
