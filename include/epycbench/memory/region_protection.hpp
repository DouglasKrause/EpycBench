#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_placement_evaluation.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct ReadOnlyRegionAccess;
struct RegionFinalizationDiagnosticTestAccess;
}

enum class RegionProtectionKind { read_only };
enum class RegionProtectionRange { full_mapping };

class RegionProtectionEvidence {
  public:
    [[nodiscard]] auto protection_kind() const noexcept -> RegionProtectionKind {
        return RegionProtectionKind::read_only;
    }
    [[nodiscard]] auto range() const noexcept -> RegionProtectionRange {
        return RegionProtectionRange::full_mapping;
    }
    [[nodiscard]] auto protection_accepted() const noexcept -> bool { return true; }
    [[nodiscard]] auto writable() const noexcept -> bool { return false; }
    [[nodiscard]] auto executable() const noexcept -> bool { return false; }

  private:
    RegionProtectionEvidence() noexcept = default;

    friend struct detail::ReadOnlyRegionAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class RegionProtectionFailure {
  public:
    RegionProtectionFailure(RegionProtectionFailure&&) noexcept = default;
    auto operator=(RegionProtectionFailure&&) noexcept
        -> RegionProtectionFailure& = default;

    RegionProtectionFailure(const RegionProtectionFailure&) = delete;
    auto operator=(const RegionProtectionFailure&)
        -> RegionProtectionFailure& = delete;

    ~RegionProtectionFailure() noexcept = default;

    [[nodiscard]] auto error() const noexcept -> const core::Error& { return error_; }
    [[nodiscard]] auto region() const noexcept -> const PlacementEvaluatedRegion& {
        return region_;
    }
    [[nodiscard]] auto take_region() && noexcept -> PlacementEvaluatedRegion {
        return std::move(region_);
    }

  private:
    RegionProtectionFailure(core::Error error,
                            PlacementEvaluatedRegion region) noexcept
        : error_{std::move(error)}, region_{std::move(region)} {}

    core::Error error_;
    PlacementEvaluatedRegion region_;

    friend struct detail::ReadOnlyRegionAccess;
};

class ReadOnlyRegion {
  public:
    [[nodiscard]] static auto create(PlacementEvaluatedRegion region) noexcept
        -> std::expected<ReadOnlyRegion, RegionProtectionFailure>;

    ReadOnlyRegion(ReadOnlyRegion&&) noexcept = default;
    auto operator=(ReadOnlyRegion&&) noexcept -> ReadOnlyRegion& = default;

    ReadOnlyRegion(const ReadOnlyRegion&) = delete;
    auto operator=(const ReadOnlyRegion&) -> ReadOnlyRegion& = delete;

    ~ReadOnlyRegion() noexcept = default;

    [[nodiscard]] auto region() const noexcept -> const PlacementEvaluatedRegion& {
        return region_;
    }
    [[nodiscard]] auto request() const noexcept -> const RegionRequest& {
        return region_.request();
    }
    [[nodiscard]] auto protection_evidence() const noexcept
        -> const RegionProtectionEvidence& {
        return evidence_;
    }
    [[nodiscard]] auto requested_region() const noexcept
        -> std::span<const std::byte>;

  private:
    ReadOnlyRegion(PlacementEvaluatedRegion region,
                   RegionProtectionEvidence evidence) noexcept
        : region_{std::move(region)}, evidence_{evidence} {}

    PlacementEvaluatedRegion region_;
    RegionProtectionEvidence evidence_;

    friend struct detail::ReadOnlyRegionAccess;
};

} // namespace epycbench::memory
