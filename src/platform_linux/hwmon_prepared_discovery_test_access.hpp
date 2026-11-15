#pragma once

#include "platform_linux/hwmon_temperature_access.hpp"

#include <cstddef>
#include <optional>

namespace epycbench::platform_linux::detail {

struct PreparedBoundedHwmonDiscoveryObservation final {
    const void* state{};
    const void* entry_name{};
    std::size_t entry_name_size{};
    std::size_t entry_name_capacity{};
    const void* contents{};
    std::size_t contents_size{};
    std::size_t contents_capacity{};
    const void* labels{};
    std::size_t labels_size{};
    std::size_t labels_capacity{};
    const void* entry_names{};
    std::size_t entry_names_size{};
    std::size_t entry_names_capacity{};
    const void* cleanup{};
    std::size_t cleanup_size{};
    std::size_t cleanup_capacity{};
    const void* selected_device{};
    std::size_t selected_device_capacity{};
    const void* selected_attribute{};
    std::size_t selected_attribute_capacity{};
    const void* current_device{};
    std::size_t current_device_capacity{};
    const void* current_input{};
    std::size_t current_input_capacity{};
    const void* final_device{};
    std::size_t final_device_capacity{};
    const void* final_attribute{};
    std::size_t final_attribute_capacity{};
    std::size_t operation_capacity{};
    std::size_t retirement_capacity{};
    std::size_t cleanup_count{};
    bool active{};
};

class PreparedBoundedHwmonDiscoveryTestAccess final {
  public:
    [[nodiscard]] static auto observe(
        const PreparedBoundedHwmonDiscoveryStorage&) noexcept
        -> std::optional<PreparedBoundedHwmonDiscoveryObservation>;
    [[nodiscard]] static auto observe(
        const BoundedHwmonTemperatureSource&) noexcept
        -> std::optional<PreparedBoundedHwmonDiscoveryObservation>;
};

} // namespace epycbench::platform_linux::detail
