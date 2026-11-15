#pragma once

#include "epycbench/platform_linux/numa_policy.hpp"
#include "epycbench/platform_linux/page_location.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::platform_linux::detail {

struct RawNumaResult {
    long value;
    int error_number;
};

enum class AllowedMemsDisposition : std::uint8_t {
    selected_nodes_within_allowed_mems,
    selected_nodes_outside_allowed_mems,
    allowed_mems_observation_failed
};

enum class NumaBindEinvalDiagnosis : std::uint8_t {
    selected_nodes_outside_allowed_mems,
    allowed_mems_observation_failed,
    mapping_not_base_page_aligned,
    native_range_wraps,
    native_range_not_base_page_multiple,
    bind_mode_not_static_absolute,
    bind_flags_not_zero,
    selected_mask_empty,
    selected_maxnode_outside_possible_domain,
    selected_nodes_not_effective,
    selected_nodes_not_online,
    selected_nodes_not_memory_backed,
    well_formed_request_rejected,
    count
};

class NumaBindInvocationFacts final {
  public:
    NumaBindInvocationFacts(const NumaBindInvocationFacts&) noexcept = default;
    NumaBindInvocationFacts(NumaBindInvocationFacts&&) noexcept = default;
    auto operator=(const NumaBindInvocationFacts&) -> NumaBindInvocationFacts& = delete;
    auto operator=(NumaBindInvocationFacts&&) -> NumaBindInvocationFacts& = delete;

    [[nodiscard]] static auto create(std::uint64_t base_page_bytes, bool effective,
                                     bool online, bool memory_backed) noexcept
        -> std::optional<NumaBindInvocationFacts> {
        if (base_page_bytes == 0U || base_page_bytes > std::numeric_limits<std::size_t>::max() ||
            base_page_bytes > std::numeric_limits<std::uintptr_t>::max())
            return std::nullopt;
        return NumaBindInvocationFacts{base_page_bytes, effective, online, memory_backed};
    }
    [[nodiscard]] auto base_page_bytes() const noexcept -> std::uint64_t {
        return base_page_bytes_;
    }
    [[nodiscard]] auto selected_nodes_effective() const noexcept -> bool {
        return selected_nodes_effective_;
    }
    [[nodiscard]] auto selected_nodes_online() const noexcept -> bool {
        return selected_nodes_online_;
    }
    [[nodiscard]] auto selected_nodes_memory_backed() const noexcept -> bool {
        return selected_nodes_memory_backed_;
    }

  private:
    NumaBindInvocationFacts(std::uint64_t base_page_bytes, bool effective, bool online,
                            bool memory_backed) noexcept
        : base_page_bytes_{base_page_bytes}, selected_nodes_effective_{effective},
          selected_nodes_online_{online}, selected_nodes_memory_backed_{memory_backed} {}
    std::uint64_t base_page_bytes_;
    bool selected_nodes_effective_;
    bool selected_nodes_online_;
    bool selected_nodes_memory_backed_;
};

class NumaDomainMaxNode final {
  public:
    [[nodiscard]] auto value() const noexcept -> unsigned long { return value_; }
    [[nodiscard]] auto word_count() const noexcept -> std::size_t { return words_; }

  private:
    NumaDomainMaxNode(unsigned long value, std::size_t words) noexcept
        : value_{value}, words_{words} {}

    unsigned long value_{};
    std::size_t words_{};
    friend class NumaAccess;
};

struct AllowedMemsWorkspace final {
    std::vector<unsigned long> words;
    std::optional<core::Error> einval_error;
    std::optional<core::Error> contained_standard;
    std::optional<core::Error> contained_unknown;
};

struct NumaBindFailure final {
    core::Error primary;
    std::optional<NumaBindEinvalDiagnosis> diagnosis;
};

struct NumaOperations {
    void* context;
    RawNumaResult (*bind)(void*, void*, unsigned long, int, const unsigned long*, unsigned long,
                          unsigned) noexcept;
    RawNumaResult (*query_pages)(void*, int, unsigned long, void**, const int*, int*, int) noexcept;
    RawNumaResult (*allowed_mems)(void*, unsigned long*, unsigned long,
                                  unsigned long) noexcept{};
};

struct MaskLayout {
    unsigned long maxnode;
    unsigned long native_bind_maxnode;
    std::size_t word_count;
};
using WordAllocator = std::vector<unsigned long> (*)(std::size_t);
using StatusAllocator = std::vector<int> (*)(std::size_t);
using ResultAllocator = std::vector<PageQueryStatus> (*)(std::size_t);

[[nodiscard]] auto production_numa_operations() noexcept -> NumaOperations;

class NumaAccess {
  public:
    [[nodiscard]] static auto page_error_status(int error_number) noexcept
        -> PageQueryStatus {
        return PageQueryStatus{PageQueryStatusKind::error, std::nullopt, error_number};
    }
    [[nodiscard]] static auto adopt_mask(std::vector<unsigned long> words,
                                         unsigned long maxnode,
                                         unsigned long native_bind_maxnode) noexcept
        -> NativeNumaMask {
        return NativeNumaMask{std::move(words), maxnode, native_bind_maxnode};
    }
    [[nodiscard]] static auto native_bind_maxnode(const NativeNumaMask& mask) noexcept
        -> unsigned long {
        return mask.native_bind_maxnode_;
    }
    [[nodiscard]] static auto checked_layout(
        std::uint64_t highest, std::uint64_t native_max = std::numeric_limits<unsigned long>::max(),
        std::size_t size_max = std::numeric_limits<std::size_t>::max())
        -> std::expected<MaskLayout, core::Error>;
    [[nodiscard]] static auto create_mask(const core::LinuxNumaNodeSet& nodes,
                                          WordAllocator allocator)
        -> std::expected<NativeNumaMask, core::Error>;
    [[nodiscard]] static auto bind_range(void* address, std::size_t length,
                                         const NativeNumaMask& nodes,
                                         const NumaOperations& operations,
                                         std::uint64_t native_length_max =
                                             std::numeric_limits<unsigned long>::max())
        -> std::expected<void, core::Error>;
    static auto checked_domain(
        const core::NodeSet& nodes,
        std::uint64_t native_max = std::numeric_limits<unsigned long>::max(),
        std::size_t size_max = std::numeric_limits<std::size_t>::max())
        -> std::expected<NumaDomainMaxNode, core::Error>;
    static auto create_allowed_workspace(NumaDomainMaxNode domain,
                                         WordAllocator allocator = nullptr)
        -> std::expected<AllowedMemsWorkspace, core::Error>;
    static auto reset_allowed_workspace(AllowedMemsWorkspace& workspace,
                                        NumaDomainMaxNode domain,
                                        WordAllocator allocator = nullptr)
        -> std::expected<void, core::Error>;
    static auto take_contained(AllowedMemsWorkspace& workspace, bool standard) noexcept
        -> NumaBindFailure;
    [[nodiscard]] static auto diagnose_invocation(
        void* address, std::size_t length, std::uint64_t base_page_bytes,
        int mode, unsigned flags, bool selected_mask_nonempty,
        bool selected_maxnode_within_possible_domain,
        bool selected_nodes_effective, bool selected_nodes_online,
        bool selected_nodes_memory_backed) noexcept -> NumaBindEinvalDiagnosis;
    static auto bind_range_diagnosed(
        void* address, std::size_t length, const NativeNumaMask& nodes,
        NumaDomainMaxNode domain, const NumaBindInvocationFacts& facts,
        AllowedMemsWorkspace& workspace,
        const NumaOperations& operations,
        std::uint64_t native_length_max = std::numeric_limits<unsigned long>::max())
        -> std::expected<void, NumaBindFailure>;
    [[nodiscard]] static auto query(std::span<void* const> pages,
                                    const NumaOperations& operations,
                                    StatusAllocator status_allocator,
                                    ResultAllocator result_allocator,
                                    std::uint64_t native_count_max =
                                        std::numeric_limits<unsigned long>::max())
        -> std::expected<std::vector<PageQueryStatus>, core::Error>;
};

} // namespace epycbench::platform_linux::detail
