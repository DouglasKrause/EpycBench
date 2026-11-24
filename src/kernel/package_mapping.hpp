#pragma once

#include "epycbench/core/quantity.hpp"
#include "kernel/package_manifest.hpp"

#include <bit>

namespace epycbench::kernel::packages {

inline constexpr auto maximum_mapping_bytes = 64U << 20U;
inline constexpr auto maximum_first_touch_pages = 4096U;
inline constexpr auto huge_page_bytes = 2U << 20U;

[[nodiscard]] constexpr auto valid_allocation_extent(core::ByteCount bytes) noexcept -> bool {
    return bytes.value() != 0U && bytes.value() <= maximum_mapping_bytes;
}

[[nodiscard]] constexpr auto first_touch_extent(core::ByteCount page_size, std::uint64_t pages) noexcept
    -> std::optional<core::ByteCount> {
    const auto size = page_size.value();
    if (!std::has_single_bit(size) || pages == 0U || pages > maximum_first_touch_pages ||
        size > maximum_mapping_bytes / pages)
        return std::nullopt;
    return core::ByteCount::from_base_units(size * pages);
}

inline constexpr auto mapping_scope(MappingResource kind) noexcept -> std::string_view {
    return kind == MappingResource::first_touch ? "first-byte-write-per-page" : "anonymous-mmap";
}

inline auto mapping_geometry(const nlohmann::json& role) -> nlohmann::json {
    using Json = nlohmann::json;
    return Json::array({{{"role", role.at("id")}, {"rank", 0U}, {"partition_begin", 0U},
        {"partition_count", 0U}, {"preparation_bytes", 0U}, {"shared_preparation_bytes", 0U},
        {"regions", Json::array()}, {"shared_regions", Json::array()}}});
}

} // namespace epycbench::kernel::packages
