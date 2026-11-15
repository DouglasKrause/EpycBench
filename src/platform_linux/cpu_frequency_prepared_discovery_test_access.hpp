#pragma once

#include "platform_linux/cpu_frequency_access.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace epycbench::platform_linux::detail {

struct PreparedBoundedCpuFrequencyDiscoveryObservation final {
    const void* state{};
    const void* present{};
    std::size_t present_size{};
    std::size_t present_capacity{};
    const void* workers{};
    std::size_t worker_size{};
    std::size_t worker_capacity{};
    const void* names{};
    std::size_t names_capacity{};
    const void* text{};
    std::size_t text_capacity{};
    const void* policies{};
    std::size_t policy_capacity{};
    const void* views{};
    std::size_t view_capacity{};
    const void* parsed{};
    std::size_t parsed_capacity{};
    const void* selected{};
    std::size_t selected_capacity{};
    const void* seen{};
    std::size_t seen_capacity{};
    const void* selected_policies{};
    std::size_t selected_policy_capacity{};
    const void* cleanup{};
    std::size_t cleanup_capacity{};
    const void* final_state{};
    const void* membership_arena{};
    std::size_t membership_size{};
    std::size_t membership_capacity{};
    const void* domains{};
    std::size_t domain_size{};
    std::size_t domain_capacity{};
    std::size_t policy_count{};
    std::size_t view_count{};
    std::size_t cleanup_count{};
    std::size_t membership_count{};
    std::size_t domain_count{};
};

struct PreparedBoundedCpuFrequencyGateStorageObservation final {
    const void* data{};
    std::size_t size{};
    std::size_t capacity{};
};

struct PreparedBoundedCpuFrequencyStringObservation final {
    std::array<const void*, 8U> policy_data{};
    std::array<std::size_t, 8U> policy_capacity{};
    std::array<const void*, 4U> domain_data{};
    std::array<std::size_t, 4U> domain_capacity{};
};

class PreparedBoundedCpuFrequencyDiscoveryTestAccess final {
  public:
    [[nodiscard]] static auto observe(
        const PreparedBoundedCpuFrequencyDiscoveryStorage&) noexcept
        -> std::optional<PreparedBoundedCpuFrequencyDiscoveryObservation>;
    [[nodiscard]] static auto observe(
        const BoundedCpuFrequencySources&) noexcept
        -> std::optional<PreparedBoundedCpuFrequencyDiscoveryObservation>;
    [[nodiscard]] static auto pre_gate_storage(
        const PreparedBoundedCpuFrequencyDiscoveryStorage&) noexcept
        -> std::optional<PreparedBoundedCpuFrequencyGateStorageObservation>;
    [[nodiscard]] static auto strings(
        const PreparedBoundedCpuFrequencyDiscoveryStorage&,
        std::size_t) noexcept
        -> std::optional<PreparedBoundedCpuFrequencyStringObservation>;
    [[nodiscard]] static auto strings(
        const BoundedCpuFrequencySources&, std::size_t) noexcept
        -> std::optional<PreparedBoundedCpuFrequencyStringObservation>;
};

} // namespace epycbench::platform_linux::detail
