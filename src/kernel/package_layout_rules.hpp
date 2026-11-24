#pragma once

#include "epycbench/kernel/package.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace epycbench::kernel::packages {
inline bool valid_package_region(std::string_view name, std::uint64_t kind, std::uint64_t bytes,
                                 std::uint64_t alignment, bool shared) noexcept {
    constexpr auto limit = std::numeric_limits<std::ptrdiff_t>::max();
    const bool valid_kind =
        shared ? kind == EPYCBENCH_PACKAGE_SHARED || kind == EPYCBENCH_PACKAGE_SHARED_INPUT
               : kind >= EPYCBENCH_PACKAGE_INPUT && kind <= EPYCBENCH_PACKAGE_SCRATCH;
    return valid_kind && bytes > 0 && bytes <= limit && std::has_single_bit(alignment) &&
           alignment <= limit && !name.empty() &&
           name.size() < EPYCBENCH_PACKAGE_REGION_NAME_CAPACITY &&
           std::ranges::all_of(
               name,
               [](char c) {
                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
               });
}
} // namespace epycbench::kernel::packages
