#pragma once

#include "epycbench/memory/placement_diagnostic.hpp"

#include <optional>
#include <variant>

namespace epycbench::memory {
namespace detail { struct RegionFinalizationDiagnosticAccess; }
namespace detail { struct RegionFinalizationDiagnosticTestAccess; }

enum class RegionFinalizationDisposition { succeeded, failed };
enum class RegionFinalizationUnavailableReason {
    contained_exception,
    contained_unknown_failure,
};

class AvailableRegionFinalizationDiagnostic final {
  public:
    AvailableRegionFinalizationDiagnostic(const AvailableRegionFinalizationDiagnostic&) = default;
    AvailableRegionFinalizationDiagnostic(AvailableRegionFinalizationDiagnostic&&) noexcept = default;
    auto operator=(const AvailableRegionFinalizationDiagnostic&)
        -> AvailableRegionFinalizationDiagnostic& = default;
    auto operator=(AvailableRegionFinalizationDiagnostic&&) noexcept
        -> AvailableRegionFinalizationDiagnostic& = default;
    ~AvailableRegionFinalizationDiagnostic() noexcept = default;

    [[nodiscard]] auto request() const noexcept -> const RegionRequest& { return request_; }
    [[nodiscard]] auto binding_evidence() const noexcept -> const RegionBindingEvidence& {
        return binding_;
    }
    [[nodiscard]] auto initialization_evidence() const noexcept
        -> const RegionInitializationEvidence& { return initialization_; }
    [[nodiscard]] auto pre_run() const noexcept -> const PreRunPlacementDiagnostic& {
        return pre_run_;
    }
    [[nodiscard]] auto protection_evidence() const noexcept
        -> const std::optional<RegionProtectionEvidence>& { return protection_; }
    [[nodiscard]] auto post_run() const noexcept
        -> const std::optional<PostRunPlacementEvidence>& { return post_run_; }
    [[nodiscard]] auto cleanup_evidence() const noexcept -> const RegionCleanupEvidence& {
        return cleanup_;
    }
    [[nodiscard]] auto finalization_path() const noexcept -> RegionFinalizationPath { return path_; }
    [[nodiscard]] auto disposition() const noexcept -> RegionFinalizationDisposition {
        return disposition_;
    }

  private:
    AvailableRegionFinalizationDiagnostic(
        RegionRequest request, RegionBindingEvidence binding,
        RegionInitializationEvidence initialization, PreRunPlacementDiagnostic pre_run,
        std::optional<RegionProtectionEvidence> protection,
        std::optional<PostRunPlacementEvidence> post_run, RegionCleanupEvidence cleanup,
        RegionFinalizationPath path, RegionFinalizationDisposition disposition) noexcept
        : request_{std::move(request)}, binding_{binding}, initialization_{initialization},
          pre_run_{std::move(pre_run)}, protection_{std::move(protection)},
          post_run_{std::move(post_run)}, cleanup_{cleanup}, path_{path},
          disposition_{disposition} {}
    RegionRequest request_;
    RegionBindingEvidence binding_;
    RegionInitializationEvidence initialization_;
    PreRunPlacementDiagnostic pre_run_;
    std::optional<RegionProtectionEvidence> protection_;
    std::optional<PostRunPlacementEvidence> post_run_;
    RegionCleanupEvidence cleanup_;
    RegionFinalizationPath path_;
    RegionFinalizationDisposition disposition_;
    friend class RegionFinalizationDiagnostic;
    friend struct detail::RegionFinalizationDiagnosticAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class UnavailableRegionFinalizationDiagnostic final {
  public:
    [[nodiscard]] auto reason() const noexcept -> RegionFinalizationUnavailableReason {
        return reason_;
    }
  private:
    explicit UnavailableRegionFinalizationDiagnostic(
        RegionFinalizationUnavailableReason reason) noexcept : reason_{reason} {}
    RegionFinalizationUnavailableReason reason_;
    friend class RegionFinalizationDiagnostic;
    friend struct detail::RegionFinalizationDiagnosticAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class RegionFinalizationDiagnostic final {
  public:
    RegionFinalizationDiagnostic(const RegionFinalizationDiagnostic&) = default;
    RegionFinalizationDiagnostic(RegionFinalizationDiagnostic&&) noexcept = default;
    auto operator=(const RegionFinalizationDiagnostic&) -> RegionFinalizationDiagnostic& = default;
    auto operator=(RegionFinalizationDiagnostic&&) noexcept
        -> RegionFinalizationDiagnostic& = default;
    ~RegionFinalizationDiagnostic() noexcept = default;

    [[nodiscard]] static auto capture(const FinalizedRegion&) noexcept
        -> RegionFinalizationDiagnostic;
    [[nodiscard]] static auto capture(const RegionFinalizationFailure&) noexcept
        -> RegionFinalizationDiagnostic;
    [[nodiscard]] auto available() const noexcept
        -> const AvailableRegionFinalizationDiagnostic* {
        return std::get_if<AvailableRegionFinalizationDiagnostic>(&value_);
    }
    [[nodiscard]] auto unavailable() const noexcept
        -> const UnavailableRegionFinalizationDiagnostic* {
        return std::get_if<UnavailableRegionFinalizationDiagnostic>(&value_);
    }

  private:
    enum class Fault : unsigned char { none, allocation, unknown };
    [[nodiscard]] static auto capture_with(const FinalizedRegion&, Fault) noexcept
        -> RegionFinalizationDiagnostic;
    [[nodiscard]] static auto capture_with(const RegionFinalizationFailure&, Fault) noexcept
        -> RegionFinalizationDiagnostic;
    using Value = std::variant<AvailableRegionFinalizationDiagnostic,
                               UnavailableRegionFinalizationDiagnostic>;
    explicit RegionFinalizationDiagnostic(Value value) noexcept : value_{std::move(value)} {}
    Value value_;
    friend struct detail::RegionFinalizationDiagnosticAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

} // namespace epycbench::memory
