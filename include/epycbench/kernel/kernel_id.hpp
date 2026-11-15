#pragma once

#include <compare>
#include <optional>
#include <string_view>

namespace epycbench::kernel {

enum class KernelId {
    sequential_read_bandwidth_v1,
    dependent_load_latency_v1,
};

[[nodiscard]] auto kernel_id_token(KernelId id) noexcept -> std::string_view;
[[nodiscard]] auto parse_kernel_id(std::string_view token) noexcept -> std::optional<KernelId>;

} // namespace epycbench::kernel
