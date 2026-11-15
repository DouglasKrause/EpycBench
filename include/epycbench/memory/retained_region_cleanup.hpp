#pragma once

#include "epycbench/memory/region_finalization.hpp"

#include <expected>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct RetainedRegionCleanupAccess;
}

enum class RetainedRegionSourceState {
    mapping,
    bound,
    initialized,
    placement_observed,
};

class CleanedRetainedRegion {
  public:
    [[nodiscard]] auto source_state() const noexcept -> RetainedRegionSourceState {
        return source_state_;
    }
    [[nodiscard]] auto cleanup_evidence() const noexcept
        -> const RegionCleanupEvidence& { return cleanup_evidence_; }

  private:
    CleanedRetainedRegion(RetainedRegionSourceState source_state,
                          RegionCleanupEvidence cleanup_evidence) noexcept
        : source_state_{source_state}, cleanup_evidence_{cleanup_evidence} {}

    RetainedRegionSourceState source_state_;
    RegionCleanupEvidence cleanup_evidence_;
    friend struct detail::RetainedRegionCleanupAccess;
};

#define EPYCBENCH_RETAINED_CLEANUP_FAILURE(Name, OwnerType)                         \
    class Name {                                                                    \
      public:                                                                       \
        Name(Name&&) noexcept = default;                                             \
        auto operator=(Name&&) noexcept -> Name& = default;                          \
        Name(const Name&) = delete;                                                  \
        auto operator=(const Name&) -> Name& = delete;                               \
        ~Name() noexcept = default;                                                  \
        [[nodiscard]] auto error() const noexcept -> const core::Error& {            \
            return error_;                                                          \
        }                                                                            \
        [[nodiscard]] auto cleanup_evidence() const noexcept                         \
            -> const RegionCleanupEvidence& { return cleanup_evidence_; }            \
        [[nodiscard]] auto source_state() const noexcept                             \
            -> RetainedRegionSourceState { return source_state_; }                   \
      private:                                                                       \
        Name(core::Error error, RegionCleanupEvidence cleanup_evidence,              \
             RetainedRegionSourceState source_state, OwnerType owner) noexcept       \
            : error_{std::move(error)}, cleanup_evidence_{cleanup_evidence},          \
              source_state_{source_state}, owner_{std::move(owner)} {}                \
        core::Error error_;                                                          \
        RegionCleanupEvidence cleanup_evidence_;                                     \
        RetainedRegionSourceState source_state_;                                     \
        OwnerType owner_;                                                            \
        friend struct detail::RetainedRegionCleanupAccess;                           \
    }

EPYCBENCH_RETAINED_CLEANUP_FAILURE(RegionMappingCleanupFailure, RegionMapping);
EPYCBENCH_RETAINED_CLEANUP_FAILURE(BoundRegionCleanupFailure, BoundRegion);
EPYCBENCH_RETAINED_CLEANUP_FAILURE(InitializedRegionCleanupFailure, InitializedRegion);
EPYCBENCH_RETAINED_CLEANUP_FAILURE(PlacementObservedRegionCleanupFailure,
                                   PlacementObservedRegion);

#undef EPYCBENCH_RETAINED_CLEANUP_FAILURE

class RetainedRegionCleanup {
  public:
    [[nodiscard]] static auto cleanup(RegionMapping&& region) noexcept
        -> std::expected<CleanedRetainedRegion, RegionMappingCleanupFailure>;
    [[nodiscard]] static auto cleanup(BoundRegion&& region) noexcept
        -> std::expected<CleanedRetainedRegion, BoundRegionCleanupFailure>;
    [[nodiscard]] static auto cleanup(InitializedRegion&& region) noexcept
        -> std::expected<CleanedRetainedRegion, InitializedRegionCleanupFailure>;
    [[nodiscard]] static auto cleanup(PlacementObservedRegion&& region) noexcept
        -> std::expected<CleanedRetainedRegion, PlacementObservedRegionCleanupFailure>;
};

} // namespace epycbench::memory
