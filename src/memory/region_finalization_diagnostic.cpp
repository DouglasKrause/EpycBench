#include "epycbench/memory/region_finalization_diagnostic.hpp"

#include "memory/placement_diagnostic_access.hpp"
#include "memory/region_finalization_access.hpp"
#include "memory/region_finalization_diagnostic_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_binding_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_protection_access.hpp"

#include <exception>
#include <type_traits>

namespace epycbench::memory {
namespace {
auto pre_run(const PlacementEvaluatedRegion& evaluated) -> PreRunPlacementDiagnostic {
    const auto& observed = evaluated.region();
    if (observed.query_state() == PreRunPlacementQueryState::skipped) {
        return detail::RegionPlacementDiagnosticAccess::pre_run(
            PreRunDiagnosticState::skipped, std::nullopt, std::nullopt, std::nullopt);
    }
    return detail::RegionPlacementDiagnosticAccess::pre_run(
        PreRunDiagnosticState::observed, observed.observation(), evaluated.evaluation(),
        evaluated.qualification_state());
}

template <class Terminal>
auto make_available(const Terminal& terminal, std::optional<RegionProtectionEvidence> protection,
               std::optional<PostRunPlacementEvidence> post_run,
               RegionFinalizationDisposition disposition)
    -> RegionFinalizationDiagnostic {
    const auto& evaluated = terminal.evaluated_region();
    const auto& initialized = evaluated.region().region();
    const auto& bound = initialized.region();
    return detail::RegionFinalizationDiagnosticAccess::available(evaluated.request(),
        bound.binding_evidence(), initialized.initialization_evidence(), pre_run(evaluated),
        std::move(protection), std::move(post_run), terminal.cleanup_evidence(),
        terminal.finalization_path(), disposition);
}

auto make_unavailable(RegionFinalizationUnavailableReason reason) noexcept
    -> RegionFinalizationDiagnostic {
    return detail::RegionFinalizationDiagnosticAccess::unavailable(reason);
}

auto take_available(PlacementEvaluatedRegion& evaluated,
                    std::optional<RegionProtectionEvidence> protection,
                    std::optional<PostRunPlacementEvidence> post_run,
                    RegionCleanupEvidence cleanup, RegionFinalizationPath path,
                    RegionFinalizationDisposition disposition) noexcept
    -> RegionFinalizationDiagnostic {
    auto& observed = detail::PlacementEvaluatedRegionAccess::region(evaluated);
    auto& initialized = detail::PlacementObservedRegionAccess::region(observed);
    auto& bound = detail::InitializedRegionAccess::region(initialized);
    PreRunPlacementDiagnostic pre = observed.query_state() ==
                                             PreRunPlacementQueryState::skipped
        ? detail::RegionPlacementDiagnosticAccess::pre_run(
              PreRunDiagnosticState::skipped, {}, {}, {})
        : detail::RegionPlacementDiagnosticAccess::pre_run(
              PreRunDiagnosticState::observed,
              detail::PlacementObservedRegionAccess::take_observation(observed),
              detail::PlacementEvaluatedRegionAccess::take_evaluation(evaluated),
              evaluated.qualification_state());
    auto request = detail::RegionMappingAccess::take_request(
        detail::BoundRegionAccess::mapping(
            detail::InitializedRegionAccess::region(initialized)));
    return detail::RegionFinalizationDiagnosticAccess::available(
        std::move(request), bound.binding_evidence(),
        initialized.initialization_evidence(), std::move(pre),
        std::move(protection), std::move(post_run), cleanup, path, disposition);
}
} // namespace

auto detail::RegionFinalizationAccess::take_diagnostic(
    FinalizedRegion&& terminal) noexcept
    -> std::optional<RegionFinalizationDiagnostic> {
    if (terminal.diagnostic_taken_)
        return std::nullopt;
    terminal.diagnostic_taken_ = true;
    auto& owner = detail::RegionFinalizationAccess::owner(terminal);
    auto& evaluated = std::visit([](auto& value) -> PlacementEvaluatedRegion& {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, ReadOnlyRegion>)
            return detail::ReadOnlyRegionAccess::region(value);
        else
            return value;
    }, owner);
    return take_available(evaluated, terminal.protection_evidence(),
                          take_post_run(terminal), terminal.cleanup_evidence(),
                          terminal.finalization_path(),
                          RegionFinalizationDisposition::succeeded);
}

auto detail::RegionFinalizationAccess::take_failure_diagnostic(
    FinalizedRegion::Owner&& owner,
    std::optional<PostRunPlacementEvidence>&& post_run,
    RegionCleanupEvidence cleanup, RegionFinalizationPath path) noexcept
    -> RegionFinalizationDiagnostic {
    auto& evaluated = std::visit([](auto& value) -> PlacementEvaluatedRegion& {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, ReadOnlyRegion>)
            return detail::ReadOnlyRegionAccess::region(value);
        else
            return value;
    }, owner);
    std::optional<RegionProtectionEvidence> protection;
    if (auto* protected_owner = std::get_if<ReadOnlyRegion>(&owner))
        protection = protected_owner->protection_evidence();
    return take_available(evaluated, std::move(protection), std::move(post_run),
                          cleanup, path,
                          RegionFinalizationDisposition::failed);
}

auto FinalizedRegion::diagnostic() const noexcept -> RegionFinalizationDiagnostic {
    return RegionFinalizationDiagnostic::capture(*this);
}
auto RegionFinalizationFailure::diagnostic() const noexcept -> RegionFinalizationDiagnostic {
    return RegionFinalizationDiagnostic::capture(*this);
}

auto RegionFinalizationDiagnostic::capture(const FinalizedRegion& terminal) noexcept
    -> RegionFinalizationDiagnostic {
    return capture_with(terminal, Fault::none);
}
auto RegionFinalizationDiagnostic::capture_with(const FinalizedRegion& terminal, Fault fault) noexcept
    -> RegionFinalizationDiagnostic {
    try {
        if (fault == Fault::allocation) throw std::bad_alloc{};
        if (fault == Fault::unknown) throw 1;
        return make_available(terminal, terminal.protection_evidence(), terminal.post_run_evidence(),
                         RegionFinalizationDisposition::succeeded);
    } catch (const std::exception&) {
        return make_unavailable(RegionFinalizationUnavailableReason::contained_exception);
    } catch (...) {
        return make_unavailable(RegionFinalizationUnavailableReason::contained_unknown_failure);
    }
}

auto RegionFinalizationDiagnostic::capture(const RegionFinalizationFailure& terminal) noexcept
    -> RegionFinalizationDiagnostic {
    return capture_with(terminal, Fault::none);
}
auto RegionFinalizationDiagnostic::capture_with(const RegionFinalizationFailure& terminal,
                                                 Fault fault) noexcept
    -> RegionFinalizationDiagnostic {
    try {
        if (fault == Fault::allocation) throw std::bad_alloc{};
        if (fault == Fault::unknown) throw 1;
        std::optional<RegionProtectionEvidence> protection;
        if (terminal.finalization_path() == RegionFinalizationPath::protected_execution) {
            const auto* owner = std::get_if<ReadOnlyRegion>(
                &detail::RegionFinalizationAccess::owner(terminal));
            if (owner) protection = owner->protection_evidence();
        }
        return make_available(terminal, std::move(protection), terminal.post_run_evidence(),
                         RegionFinalizationDisposition::failed);
    } catch (const std::exception&) {
        return make_unavailable(RegionFinalizationUnavailableReason::contained_exception);
    } catch (...) {
        return make_unavailable(RegionFinalizationUnavailableReason::contained_unknown_failure);
    }
}

} // namespace epycbench::memory
