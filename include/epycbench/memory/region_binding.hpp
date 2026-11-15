#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_mapping.hpp"
#include "epycbench/memory/region_request.hpp"

#include <expected>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct BoundRegionAccess;
struct RegionFinalizationDiagnosticTestAccess;
}

enum class BindingRange { full_mapping };

class RegionBindingEvidence {
  public:
    [[nodiscard]] auto policy() const noexcept -> PlacementPolicy { return PlacementPolicy::bind; }
    [[nodiscard]] auto range() const noexcept -> BindingRange { return BindingRange::full_mapping; }
    [[nodiscard]] auto policy_call_accepted() const noexcept -> bool { return true; }
    [[nodiscard]] auto migration_requested() const noexcept -> bool { return false; }
    [[nodiscard]] auto strict_verification_requested() const noexcept -> bool { return false; }

  private:
    RegionBindingEvidence() noexcept = default;

    friend struct detail::BoundRegionAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class RegionBindingFailure {
  public:
    RegionBindingFailure(RegionBindingFailure&&) noexcept = default;
    auto operator=(RegionBindingFailure&&) noexcept -> RegionBindingFailure& = default;

    RegionBindingFailure(const RegionBindingFailure&) = delete;
    auto operator=(const RegionBindingFailure&) -> RegionBindingFailure& = delete;

    ~RegionBindingFailure() noexcept = default;

    [[nodiscard]] auto error() const noexcept -> const core::Error& { return error_; }
    [[nodiscard]] auto mapping() const noexcept -> const RegionMapping& { return mapping_; }
    [[nodiscard]] auto take_mapping() && noexcept -> RegionMapping { return std::move(mapping_); }

  private:
    RegionBindingFailure(core::Error error, RegionMapping mapping) noexcept
        : error_{std::move(error)}, mapping_{std::move(mapping)} {}

    core::Error error_;
    RegionMapping mapping_;

    friend struct detail::BoundRegionAccess;
};

class BoundRegion {
  public:
    [[nodiscard]] static auto create(RegionMapping mapping) noexcept
        -> std::expected<BoundRegion, RegionBindingFailure>;

    BoundRegion(BoundRegion&&) noexcept = default;
    auto operator=(BoundRegion&&) noexcept -> BoundRegion& = default;

    BoundRegion(const BoundRegion&) = delete;
    auto operator=(const BoundRegion&) -> BoundRegion& = delete;

    ~BoundRegion() noexcept = default;

    [[nodiscard]] auto mapping() const noexcept -> const RegionMapping& { return mapping_; }
    [[nodiscard]] auto request() const noexcept -> const RegionRequest& {
        return mapping_.request();
    }
    [[nodiscard]] auto binding_evidence() const noexcept -> const RegionBindingEvidence& {
        return evidence_;
    }

  private:
    BoundRegion(RegionMapping mapping, RegionBindingEvidence evidence) noexcept
        : mapping_{std::move(mapping)}, evidence_{evidence} {}

    RegionMapping mapping_;
    RegionBindingEvidence evidence_;

    friend struct detail::BoundRegionAccess;
};

} // namespace epycbench::memory
