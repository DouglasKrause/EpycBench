#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/monitoring/host_environment.hpp"
#include <optional>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace epycbench::monitoring::qualification::detail {

[[nodiscard]] auto derive_host_environment_fallible(const RunnerContextAcquisitionSession&,
                                                    std::optional<core::Error>* = nullptr)
    -> std::expected<HostEnvironmentAuthority, core::Error>;
[[nodiscard]] auto checked_host_worker_id_fallible(std::size_t index)
    -> std::expected<std::uint32_t, core::Error>;

[[nodiscard]] auto checked_host_worker_id(std::size_t index) noexcept
    -> std::expected<std::uint32_t, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
