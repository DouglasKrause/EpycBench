#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_binding.hpp"
#include "epycbench/memory/region_request.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct InitializedRegionAccess;
struct RegionFinalizationDiagnosticTestAccess;
}

class RegionInitializer {
  public:
    using InitializeFunction = std::expected<void, core::Error> (*)(
        void* context, std::span<std::byte> requested_region);

    [[nodiscard]] static auto create(void* context, InitializeFunction initialize) noexcept
        -> std::expected<RegionInitializer, core::Error>;

    [[nodiscard]] auto initialize(std::span<std::byte> requested_region) const noexcept
        -> std::expected<void, core::Error>;

  private:
    RegionInitializer(void* context, InitializeFunction function, core::Error invalid_failure,
                      core::Error standard_failure, core::Error unknown_failure) noexcept
        : context_{context}, initialize_{function}, invalid_failure_{std::move(invalid_failure)},
          standard_failure_{std::move(standard_failure)},
          unknown_failure_{std::move(unknown_failure)} {}

    void* context_;
    InitializeFunction initialize_;
    mutable core::Error invalid_failure_;
    mutable core::Error standard_failure_;
    mutable core::Error unknown_failure_;

    friend struct detail::InitializedRegionAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

enum class InitializationRange { logical_requested_region };

class RegionInitializationEvidence {
  public:
    [[nodiscard]] auto range() const noexcept -> InitializationRange {
        return InitializationRange::logical_requested_region;
    }
    [[nodiscard]] auto prefault_requested() const noexcept -> bool {
        return prefault_requested_;
    }
    [[nodiscard]] auto prefault_pass_performed() const noexcept -> bool {
        return prefault_requested_;
    }
    [[nodiscard]] auto initialization_completed() const noexcept -> bool { return true; }
    [[nodiscard]] auto padding_touched() const noexcept -> bool { return false; }

  private:
    explicit RegionInitializationEvidence(bool prefault_requested) noexcept
        : prefault_requested_{prefault_requested} {}

    bool prefault_requested_;

    friend struct detail::InitializedRegionAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

enum class InitializationWriteState { no_writes, partial };

class RegionInitializationFailure {
  public:
    RegionInitializationFailure(RegionInitializationFailure&&) noexcept = default;
    auto operator=(RegionInitializationFailure&&) noexcept
        -> RegionInitializationFailure& = default;

    RegionInitializationFailure(const RegionInitializationFailure&) = delete;
    auto operator=(const RegionInitializationFailure&)
        -> RegionInitializationFailure& = delete;

    ~RegionInitializationFailure() noexcept = default;

    [[nodiscard]] auto error() const noexcept -> const core::Error& { return error_; }
    [[nodiscard]] auto write_state() const noexcept -> InitializationWriteState {
        return write_state_;
    }
    [[nodiscard]] auto underlying_code() const noexcept
        -> const std::optional<core::StableErrorCode>& {
        return underlying_code_;
    }
    [[nodiscard]] auto region() const noexcept -> const BoundRegion& { return region_; }
    [[nodiscard]] auto take_region() && noexcept -> BoundRegion { return std::move(region_); }

  private:
    RegionInitializationFailure(core::Error error, InitializationWriteState write_state,
                                BoundRegion region,
                                std::optional<core::StableErrorCode> underlying_code) noexcept
        : error_{std::move(error)}, write_state_{write_state}, region_{std::move(region)},
          underlying_code_{std::move(underlying_code)} {}

    core::Error error_;
    InitializationWriteState write_state_;
    BoundRegion region_;
    std::optional<core::StableErrorCode> underlying_code_;

    friend struct detail::InitializedRegionAccess;
};

class InitializedRegion {
  public:
    [[nodiscard]] static auto create(BoundRegion region, RegionInitializer initializer) noexcept
        -> std::expected<InitializedRegion, RegionInitializationFailure>;

    InitializedRegion(InitializedRegion&&) noexcept = default;
    auto operator=(InitializedRegion&&) noexcept -> InitializedRegion& = default;

    InitializedRegion(const InitializedRegion&) = delete;
    auto operator=(const InitializedRegion&) -> InitializedRegion& = delete;

    ~InitializedRegion() noexcept = default;

    [[nodiscard]] auto region() const noexcept -> const BoundRegion& { return region_; }
    [[nodiscard]] auto request() const noexcept -> const RegionRequest& {
        return region_.request();
    }
    [[nodiscard]] auto initialization_evidence() const noexcept
        -> const RegionInitializationEvidence& {
        return evidence_;
    }

  private:
    InitializedRegion(BoundRegion region, RegionInitializationEvidence evidence) noexcept
        : region_{std::move(region)}, evidence_{evidence} {}

    BoundRegion region_;
    RegionInitializationEvidence evidence_;

    friend struct detail::InitializedRegionAccess;
};

} // namespace epycbench::memory
