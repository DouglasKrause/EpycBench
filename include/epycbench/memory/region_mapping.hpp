#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_request.hpp"
#include "epycbench/platform_linux/memory_mapping.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct RegionMappingAccess;
}

class RegionMappingFailure {
  public:
    RegionMappingFailure(core::Error primary, std::optional<core::Error> cleanup = std::nullopt)
        : primary_{std::move(primary)}, cleanup_{std::move(cleanup)} {}

    [[nodiscard]] auto primary() const noexcept -> const core::Error& { return primary_; }
    [[nodiscard]] auto cleanup() const noexcept -> const std::optional<core::Error>& {
        return cleanup_;
    }

  private:
    core::Error primary_;
    std::optional<core::Error> cleanup_;
};

class RegionMapping {
  public:
    [[nodiscard]] static auto create(RegionRequest request) noexcept
        -> std::expected<RegionMapping, RegionMappingFailure>;

    RegionMapping(RegionMapping&&) noexcept = default;
    auto operator=(RegionMapping&&) noexcept -> RegionMapping& = default;

    RegionMapping(const RegionMapping&) = delete;
    auto operator=(const RegionMapping&) -> RegionMapping& = delete;

    ~RegionMapping() noexcept = default;

    [[nodiscard]] auto request() const noexcept -> const RegionRequest& { return request_; }
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId { return request_.worker_id(); }
    [[nodiscard]] auto requested_bytes() const noexcept -> core::ByteCount {
        return request_.requested_bytes();
    }
    [[nodiscard]] auto mapping_bytes() const noexcept -> std::size_t { return mapping_bytes_; }
    [[nodiscard]] auto page_size() const noexcept -> core::ByteCount {
        return request_.page_size();
    }

    [[nodiscard]] auto requested_region() noexcept -> std::span<std::byte>;
    [[nodiscard]] auto requested_region() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto full_mapping() noexcept -> std::span<std::byte>;
    [[nodiscard]] auto full_mapping() const noexcept -> std::span<const std::byte>;

  private:
    RegionMapping(RegionRequest request, platform_linux::AnonymousMapping mapping,
                  std::size_t mapping_bytes) noexcept
        : request_{std::move(request)}, mapping_{std::move(mapping)},
          mapping_bytes_{mapping_bytes} {}

    RegionRequest request_;
    platform_linux::AnonymousMapping mapping_;
    std::size_t mapping_bytes_;

    friend struct detail::RegionMappingAccess;
};

} // namespace epycbench::memory
