#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <span>

namespace epycbench::platform_linux {

inline constexpr std::uint64_t hugetlb_2m_bytes = 2U * 1024U * 1024U;

struct HugeTlbBacking {
    std::uint64_t extent_bytes{}, kernel_page_bytes{}, mmu_page_bytes{};
    std::uint64_t private_hugetlb_bytes{}, shared_hugetlb_bytes{};
    bool hugetlb_vma{};

    [[nodiscard]] auto qualifies(std::uint64_t owned_bytes) const noexcept -> bool {
        return hugetlb_vma && owned_bytes != 0U && owned_bytes % hugetlb_2m_bytes == 0U &&
               extent_bytes == owned_bytes && kernel_page_bytes == hugetlb_2m_bytes &&
               mmu_page_bytes == hugetlb_2m_bytes && private_hugetlb_bytes <= extent_bytes &&
               private_hugetlb_bytes % hugetlb_2m_bytes == 0U &&
               shared_hugetlb_bytes == extent_bytes - private_hugetlb_bytes;
    }
};

// Reads only this process's live VMA after its measured writes. No addresses
// or adjacent VMAs are part of the returned evidence.
[[nodiscard]] auto observe_hugetlb_backing(std::span<const std::byte> mapping)
    -> std::expected<HugeTlbBacking, core::Error>;

namespace detail {
[[nodiscard]] auto parse_hugetlb_smaps(std::istream&, std::uintptr_t start, std::size_t extent)
    -> std::expected<HugeTlbBacking, core::Error>;
[[nodiscard]] auto explicit_hugetlb_2m_flags() noexcept -> int;
} // namespace detail
} // namespace epycbench::platform_linux
