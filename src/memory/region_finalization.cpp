#include "epycbench/memory/region_finalization.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_finalization_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_placement_query_access.hpp"
#include "memory/region_protection_access.hpp"
#include "epycbench/platform_linux/page_location.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace epycbench::memory {
namespace {

using Owner = std::variant<ReadOnlyRegion, PlacementEvaluatedRegion>;

static_assert(std::is_nothrow_move_constructible_v<Owner>);
static_assert(std::is_nothrow_move_constructible_v<PostRunPlacementEvidence>);
static_assert(std::is_nothrow_move_constructible_v<FinalizedRegion>);
static_assert(std::is_nothrow_move_constructible_v<RegionFinalizationFailure>);

[[nodiscard]] auto finalization_error(
    std::string_view code, std::string_view classification,
    const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region finalization failed",
            .operation = "memory.region_finalization.finalize",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto production_query(
    void*, std::span<void* const> pages)
    -> std::expected<std::vector<platform_linux::PageQueryStatus>, core::Error> {
    return platform_linux::query_page_locations(pages);
}

[[nodiscard]] auto production_query_operations() noexcept
    -> detail::RegionPlacementQueryOperations {
    return {.context = nullptr, .query = production_query};
}

[[nodiscard]] auto evaluated(Owner& owner) noexcept -> PlacementEvaluatedRegion& {
    return std::visit(
        []<typename T>(T& value) -> PlacementEvaluatedRegion& {
            if constexpr (std::is_same_v<T, ReadOnlyRegion>) {
                return detail::ReadOnlyRegionAccess::region(value);
            } else {
                return value;
            }
        },
        owner);
}

[[nodiscard]] auto evaluated(const Owner& owner) noexcept
    -> const PlacementEvaluatedRegion& {
    return std::visit(
        []<typename T>(const T& value) -> const PlacementEvaluatedRegion& {
            if constexpr (std::is_same_v<T, ReadOnlyRegion>) return value.region();
            else return value;
        },
        owner);
}

[[nodiscard]] auto initialized(PlacementEvaluatedRegion& value) noexcept
    -> InitializedRegion& {
    return detail::PlacementObservedRegionAccess::region(
        detail::PlacementEvaluatedRegionAccess::region(value));
}

[[nodiscard]] auto region_mapping(PlacementEvaluatedRegion& value) noexcept
    -> RegionMapping& {
    return detail::BoundRegionAccess::mapping(
        detail::InitializedRegionAccess::region(initialized(value)));
}

[[nodiscard]] auto mapping(Owner& owner) noexcept
    -> platform_linux::AnonymousMapping& {
    return detail::RegionMappingAccess::mapping(region_mapping(evaluated(owner)));
}

[[nodiscard]] auto validate_owner(PlacementEvaluatedRegion& value,
                                  bool protected_owner) noexcept
    -> std::optional<core::Error> {
    try {
        auto& owner = detail::RegionMappingAccess::mapping(region_mapping(value));
        const auto& mapped = detail::BoundRegionAccess::mapping(
            detail::InitializedRegionAccess::region(initialized(value)));
        const auto expected = protected_owner
                                  ? platform_linux::MemoryProtection::read_only
                                  : platform_linux::MemoryProtection::read_write;
        if (!owner.active()) {
            return finalization_error(
                "memory.region_finalization.invariant_failed", "finalization_owner_invalid");
        }
        if (owner.protection() != expected) {
            return finalization_error(
                "memory.region_finalization.invalid_argument", "lifecycle_state_invalid");
        }
        if (owner.size() != mapped.mapping_bytes() ||
            owner.bytes().size() != mapped.mapping_bytes()) {
            return finalization_error(
                "memory.region_finalization.invariant_failed", "finalization_owner_invalid");
        }
        if (protected_owner) {
            if (!owner.writable_bytes().empty()) {
                return finalization_error(
                    "memory.region_finalization.invariant_failed", "lifecycle_state_invalid");
            }
        } else if (owner.writable_bytes().size() != mapped.mapping_bytes()) {
            return finalization_error(
                "memory.region_finalization.invariant_failed", "lifecycle_state_invalid");
        }
        return std::nullopt;
    } catch (const std::exception&) {
        return finalization_error(
            "memory.region_finalization.contained_exception", "contained_exception");
    } catch (...) {
        return finalization_error(
            "memory.region_finalization.contained_unknown_failure",
            "contained_unknown_failure");
    }
}

[[nodiscard]] auto cleanup_state_valid(const platform_linux::AnonymousMapping& owner,
                                       bool released) noexcept -> bool {
    if (released) {
        return !owner.active() && owner.size() == 0U &&
               owner.protection() == platform_linux::MemoryProtection::none &&
               owner.bytes().empty();
    }
    return owner.active();
}

[[nodiscard]] auto finish(
    Owner owner, std::optional<PostRunPlacementEvidence> evidence,
    RegionFinalizationPath path,
    std::optional<core::Error> primary) noexcept
    -> std::expected<FinalizedRegion, RegionFinalizationFailure> {
    try {
        auto release_state_error = finalization_error(
            "memory.region_finalization.invariant_failed", "release_state_inconsistent");
        if (primary) {
            auto failure = detail::RegionFinalizationAccess::failure(
                std::move(owner), std::move(*primary), std::nullopt,
                std::move(evidence),
                detail::RegionFinalizationAccess::cleanup(true), path);
            auto& retained = detail::RegionFinalizationAccess::owner(failure);
            auto released = mapping(retained).release();
            if (released) {
                if (!cleanup_state_valid(mapping(retained), true)) {
                    detail::RegionFinalizationAccess::set_cleanup_failure(
                        failure, std::move(release_state_error));
                }
                return std::unexpected{std::move(failure)};
            }
            auto cleanup_error = finalization_error(
                "memory.region_finalization.release_failed", "release_failed",
                &released.error());
            detail::RegionFinalizationAccess::set_cleanup_failure(
                failure, std::move(cleanup_error));
            return std::unexpected{std::move(failure)};
        }

        auto success = detail::RegionFinalizationAccess::success(
            std::move(owner), std::move(*evidence),
            detail::RegionFinalizationAccess::cleanup(true), path);
        auto released = mapping(detail::RegionFinalizationAccess::owner(success)).release();
        if (!released) {
            auto release_error = finalization_error(
                "memory.region_finalization.release_failed", "release_failed",
                &released.error());
            return std::unexpected{detail::RegionFinalizationAccess::failure(
                detail::RegionFinalizationAccess::take_owner(success),
                std::move(release_error), std::nullopt,
                std::optional<PostRunPlacementEvidence>{
                    detail::RegionFinalizationAccess::take_post_run(success)},
                detail::RegionFinalizationAccess::cleanup(false), path)};
        }
        if (!cleanup_state_valid(
                mapping(detail::RegionFinalizationAccess::owner(success)), true)) {
            return std::unexpected{detail::RegionFinalizationAccess::failure(
                detail::RegionFinalizationAccess::take_owner(success),
                std::move(release_state_error), std::nullopt,
                std::optional<PostRunPlacementEvidence>{
                    detail::RegionFinalizationAccess::take_post_run(success)},
                detail::RegionFinalizationAccess::cleanup(true), path)};
        }
        return success;
    } catch (const std::exception&) {
        return std::unexpected{detail::RegionFinalizationAccess::failure(
            std::move(owner),
            finalization_error("memory.region_finalization.contained_exception",
                               "contained_exception"),
            std::nullopt, std::move(evidence),
            detail::RegionFinalizationAccess::cleanup(false), path)};
    } catch (...) {
        return std::unexpected{detail::RegionFinalizationAccess::failure(
            std::move(owner),
            finalization_error("memory.region_finalization.contained_unknown_failure",
                               "contained_unknown_failure"),
            std::nullopt, std::move(evidence),
            detail::RegionFinalizationAccess::cleanup(false), path)};
    }
}

} // namespace

auto FinalizedRegion::finalize(
    ReadOnlyRegion region,
    PostRunVerificationApplicability applicability) noexcept
    -> std::expected<FinalizedRegion, RegionFinalizationFailure> {
    return detail::RegionFinalizationAccess::finalize_with_operations(
        std::move(region), applicability, production_query_operations());
}

auto detail::RegionFinalizationAccess::finalize_with_operations(
    ReadOnlyRegion region, PostRunVerificationApplicability applicability,
    const RegionPlacementQueryOperations& operations) noexcept
    -> std::expected<FinalizedRegion, RegionFinalizationFailure> {
    Owner owner{std::in_place_type<ReadOnlyRegion>, std::move(region)};
    const auto path = RegionFinalizationPath::protected_execution;
    try {
        if (auto invalid = validate_owner(evaluated(owner), true)) {
            return std::unexpected{failure(
                std::move(owner), std::move(*invalid), std::nullopt, std::nullopt,
                cleanup_not_attempted(true), path)};
        }
        if (applicability != PostRunVerificationApplicability::applicable &&
            applicability != PostRunVerificationApplicability::not_applicable) {
            return finish(
                std::move(owner), std::nullopt, path,
                std::optional<core::Error>{finalization_error(
                    "memory.region_finalization.invalid_argument",
                    "post_verification_not_applicable")});
        }

        if (applicability == PostRunVerificationApplicability::not_applicable) {
            return finish(std::move(owner), post_run(PostRunVerificationState::not_applicable),
                          path, std::nullopt);
        }
        const auto& request = evaluated(owner).request();
        if (!request.verify_after()) {
            return finish(std::move(owner), post_run(PostRunVerificationState::skipped), path,
                          std::nullopt);
        }

        auto observation = PlacementObservedRegionAccess::observe_region_placement(
            initialized(evaluated(owner)), operations);
        if (!observation) {
            auto primary = finalization_error(
                "memory.region_finalization.post_query_failed", "post_observation_inconsistent",
                &observation.error());
            return finish(std::move(owner), std::nullopt, path,
                          std::optional<core::Error>{std::move(primary)});
        }
        auto evaluated_result = PlacementEvaluatedRegionAccess::evaluate_region_placement(
            request, *observation);
        if (!evaluated_result) {
            auto primary = finalization_error(
                "memory.region_finalization.post_evaluation_failed", "post_evaluation_failed",
                &evaluated_result.error());
            return finish(std::move(owner), std::nullopt, path,
                          std::optional<core::Error>{std::move(primary)});
        }
        auto evidence = post_run(
            PostRunVerificationState::observed,
            std::optional<RegionPlacementObservation>{std::move(*observation)},
            std::optional<RegionPlacementEvaluation>{
                std::move(evaluated_result->evaluation)},
            evaluated_result->qualification_state);
        return finish(std::move(owner), std::move(evidence), path, std::nullopt);
    } catch (const std::exception&) {
        return finish(
            std::move(owner), std::nullopt, path,
            std::optional<core::Error>{finalization_error(
                "memory.region_finalization.contained_exception", "contained_exception")});
    } catch (...) {
        return finish(
            std::move(owner), std::nullopt, path,
            std::optional<core::Error>{finalization_error(
                "memory.region_finalization.contained_unknown_failure",
                "contained_unknown_failure")});
    }
}

auto FinalizedRegion::finalize_unprotected(
    PlacementEvaluatedRegion region, UnprotectedFinalizationReason reason) noexcept
    -> std::expected<FinalizedRegion, RegionFinalizationFailure> {
    Owner owner{std::in_place_type<PlacementEvaluatedRegion>, std::move(region)};
    auto path = RegionFinalizationPath::unprotected_execution_ineligible;
    try {
        const auto reason_valid =
            reason == UnprotectedFinalizationReason::execution_ineligible ||
            reason == UnprotectedFinalizationReason::protection_failed;
        if (reason == UnprotectedFinalizationReason::protection_failed) {
            path = RegionFinalizationPath::unprotected_protection_failed;
        }
        if (auto invalid = validate_owner(evaluated(owner), false)) {
            return std::unexpected{detail::RegionFinalizationAccess::failure(
                std::move(owner), std::move(*invalid), std::nullopt, std::nullopt,
                detail::RegionFinalizationAccess::cleanup_not_attempted(true), path)};
        }
        if (!reason_valid) {
            return finish(
                std::move(owner), std::nullopt, path,
                std::optional<core::Error>{finalization_error(
                    "memory.region_finalization.invalid_argument",
                    "unprotected_reason_invalid")});
        }
        const auto eligible = evaluated(owner).execution_eligible();
        const auto valid_reason =
            (reason == UnprotectedFinalizationReason::execution_ineligible && !eligible &&
             evaluated(owner).qualification_state() ==
                 PlacementQualificationState::not_qualified) ||
            (reason == UnprotectedFinalizationReason::protection_failed && eligible);
        if (!valid_reason) {
            return finish(
                std::move(owner), std::nullopt, path,
                std::optional<core::Error>{finalization_error(
                    "memory.region_finalization.invalid_argument",
                    "unprotected_reason_invalid")});
        }
        return finish(
            std::move(owner),
            detail::RegionFinalizationAccess::post_run(
                PostRunVerificationState::not_applicable),
            path, std::nullopt);
    } catch (const std::exception&) {
        return finish(
            std::move(owner),
            detail::RegionFinalizationAccess::post_run(
                PostRunVerificationState::not_applicable),
            path, std::optional<core::Error>{finalization_error(
                      "memory.region_finalization.contained_exception",
                      "contained_exception")});
    } catch (...) {
        return finish(
            std::move(owner),
            detail::RegionFinalizationAccess::post_run(
                PostRunVerificationState::not_applicable),
            path, std::optional<core::Error>{finalization_error(
                      "memory.region_finalization.contained_unknown_failure",
                      "contained_unknown_failure")});
    }
}

auto FinalizedRegion::request() const noexcept -> const RegionRequest& {
    return evaluated(owner_).request();
}

auto FinalizedRegion::evaluated_region() const noexcept
    -> const PlacementEvaluatedRegion& { return evaluated(owner_); }

auto FinalizedRegion::protection_evidence() const noexcept
    -> std::optional<RegionProtectionEvidence> {
    if (const auto* protected_owner = std::get_if<ReadOnlyRegion>(&owner_)) {
        return protected_owner->protection_evidence();
    }
    return std::nullopt;
}

auto RegionFinalizationFailure::evaluated_region() const noexcept
    -> const PlacementEvaluatedRegion& { return evaluated(owner_); }

auto RegionFinalizationFailure::recover_protected_region() && noexcept
    -> std::optional<ReadOnlyRegion> {
    if (cleanup_evidence_.release_succeeded()) return std::nullopt;
    if (auto* value = std::get_if<ReadOnlyRegion>(&owner_)) return std::move(*value);
    return std::nullopt;
}

auto RegionFinalizationFailure::recover_unprotected_region() && noexcept
    -> std::optional<PlacementEvaluatedRegion> {
    if (cleanup_evidence_.release_succeeded()) return std::nullopt;
    if (auto* value = std::get_if<PlacementEvaluatedRegion>(&owner_)) return std::move(*value);
    return std::nullopt;
}

} // namespace epycbench::memory
