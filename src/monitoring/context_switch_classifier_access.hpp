#pragma once
#include "epycbench/core/error.hpp"
#include <cstdint>
#include <expected>
namespace epycbench::monitoring::detail {
[[nodiscard]] auto checked_context_switch_increment(std::uint64_t value)
    -> std::expected<std::uint64_t, core::Error>;
}
