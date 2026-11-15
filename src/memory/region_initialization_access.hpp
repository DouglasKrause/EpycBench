#pragma once

#include "epycbench/memory/region_initialization.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace epycbench::memory::detail {

struct RegionInitializationOperations {
    void* context;
    std::expected<void, core::Error> (*write_prefault_byte)(
        void* context, std::span<std::byte> requested_region, std::size_t offset,
        std::byte value);
};

[[nodiscard]] auto production_region_initialization_operations() noexcept
    -> RegionInitializationOperations;

struct InitializedRegionAccess {
    [[nodiscard]] static auto region(InitializedRegion& initialized) noexcept -> BoundRegion& {
        return initialized.region_;
    }
    [[nodiscard]] static auto region(const InitializedRegion& initialized) noexcept
        -> const BoundRegion& {
        return initialized.region_;
    }

    [[nodiscard]] static auto create_with_operations(
        BoundRegion region, RegionInitializer initializer,
        const RegionInitializationOperations& operations) noexcept
        -> std::expected<InitializedRegion, RegionInitializationFailure>;

  private:
    friend struct PairwiseMemoryAccess;
    [[nodiscard]] static auto evidence(bool prefault_requested) noexcept
        -> RegionInitializationEvidence {
        return RegionInitializationEvidence{prefault_requested};
    }
    [[nodiscard]] static auto success(BoundRegion region,
                                      RegionInitializationEvidence evidence) noexcept
        -> InitializedRegion {
        return InitializedRegion{std::move(region), evidence};
    }
    [[nodiscard]] static auto failure(
        core::Error error, InitializationWriteState write_state, BoundRegion region,
        std::optional<core::StableErrorCode> underlying_code = std::nullopt) noexcept
        -> RegionInitializationFailure {
        return RegionInitializationFailure{std::move(error), write_state, std::move(region),
                                           std::move(underlying_code)};
    }
    [[nodiscard]] static auto valid(const RegionInitializer& initializer) noexcept -> bool {
        return initializer.initialize_ != nullptr;
    }
};

} // namespace epycbench::memory::detail
