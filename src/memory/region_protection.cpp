#include "epycbench/memory/region_protection.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_placement_query_access.hpp"
#include "memory/region_protection_access.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace epycbench::memory {
namespace {

static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_constructible_v<PlacementEvaluatedRegion>);
static_assert(std::is_nothrow_move_constructible_v<RegionProtectionEvidence>);

[[nodiscard]] auto protection_error(
    std::string_view code, std::string_view classification,
    PlacementQualificationState qualification_state,
    const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{
        {"classification", classification},
        {"protection_kind", "read_only"},
        {"range_kind", "full_mapping"},
        {"qualification_state",
         qualification_state == PlacementQualificationState::qualified
             ? "qualified"
             : qualification_state == PlacementQualificationState::skipped
                   ? "skipped"
                   : "not_qualified"}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region read-only transition failed",
            .operation = "memory.region_protection.protect",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto mapping(PlacementEvaluatedRegion& region) noexcept
    -> RegionMapping& {
    auto& observed = detail::PlacementEvaluatedRegionAccess::region(region);
    auto& initialized = detail::PlacementObservedRegionAccess::region(observed);
    auto& bound = detail::InitializedRegionAccess::region(initialized);
    return detail::BoundRegionAccess::mapping(bound);
}

[[nodiscard]] auto mapping(const PlacementEvaluatedRegion& region) noexcept
    -> const RegionMapping& {
    const auto& observed = detail::PlacementEvaluatedRegionAccess::region(region);
    const auto& initialized = detail::PlacementObservedRegionAccess::region(observed);
    const auto& bound = detail::InitializedRegionAccess::region(initialized);
    return detail::BoundRegionAccess::mapping(bound);
}

[[nodiscard]] auto production_protect_read_only(
    void*, platform_linux::AnonymousMapping& owner)
    -> std::expected<void, core::Error> {
    return owner.protect(platform_linux::MemoryProtection::read_only);
}

[[nodiscard]] auto production_operations() noexcept
    -> detail::RegionProtectionOperations {
    return {.context = nullptr,
            .protect_read_only = production_protect_read_only};
}

} // namespace

auto ReadOnlyRegion::requested_region() const noexcept
    -> std::span<const std::byte> {
    return mapping(region_).requested_region();
}

auto ReadOnlyRegion::create(PlacementEvaluatedRegion region) noexcept
    -> std::expected<ReadOnlyRegion, RegionProtectionFailure> {
    return detail::ReadOnlyRegionAccess::create_with_operations(
        std::move(region), production_operations());
}

auto detail::ReadOnlyRegionAccess::create_with_operations(
    PlacementEvaluatedRegion region,
    const RegionProtectionOperations& operations) noexcept
    -> std::expected<ReadOnlyRegion, RegionProtectionFailure> {
    try {
        const auto state = region.qualification_state();
        const auto fail = [&region](core::Error error)
            -> std::expected<ReadOnlyRegion, RegionProtectionFailure> {
            return std::unexpected{ReadOnlyRegionAccess::failure(
                std::move(error), std::move(region))};
        };
        const auto make_error = [state](std::string_view code,
                                        std::string_view classification) {
            return protection_error(code, classification, state);
        };

        if (!PlacementEvaluatedRegionAccess::execution_eligible(region)) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "execution_not_eligible"));
        }
        if (state == PlacementQualificationState::skipped &&
            region.request().verify_before()) {
            return fail(make_error("memory.region_protection.invariant_failed",
                                   "protection_owner_invalid"));
        }
        if (state != PlacementQualificationState::qualified &&
            state != PlacementQualificationState::skipped) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "execution_not_eligible"));
        }
        if (!region.request().requires_read_only()) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "read_only_not_required"));
        }

        auto& region_mapping = mapping(region);
        auto& owner = RegionMappingAccess::mapping(region_mapping);
        if (!owner.active()) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "mapping_inactive"));
        }
        if (owner.protection() == platform_linux::MemoryProtection::read_only) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "mapping_already_read_only"));
        }
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != region_mapping.mapping_bytes()) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "mapping_not_writable"));
        }
        if (owner.size() != region_mapping.mapping_bytes() ||
            owner.bytes().size() != region_mapping.mapping_bytes() ||
            region_mapping.full_mapping().size() != region_mapping.mapping_bytes() ||
            region_mapping.requested_region().size() !=
                region_mapping.requested_bytes().value()) {
            return fail(make_error("memory.region_protection.invariant_failed",
                                   "mapping_range_invalid"));
        }
        if (operations.protect_read_only == nullptr) {
            return fail(make_error("memory.region_protection.invalid_argument",
                                   "protection_owner_invalid"));
        }

        auto evidence_value = evidence();
        auto inconsistent = make_error(
            "memory.region_protection.invariant_failed",
            "protection_state_inconsistent");

        auto protected_result = operations.protect_read_only(
            operations.context, owner);
        if (!protected_result) {
            return fail(protection_error(
                "memory.region_protection.protection_failed", "protection_failed",
                state, &protected_result.error()));
        }

        if (!owner.active() ||
            owner.protection() != platform_linux::MemoryProtection::read_only ||
            !owner.writable_bytes().empty() ||
            owner.bytes().size() != region_mapping.mapping_bytes()) {
            return fail(std::move(inconsistent));
        }

        return success(std::move(region), evidence_value);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(
            protection_error("memory.region_protection.contained_exception",
                             "allocation_failure",
                             region.qualification_state()),
            std::move(region))};
    } catch (const std::exception&) {
        return std::unexpected{failure(
            protection_error("memory.region_protection.contained_exception",
                             "contained_exception",
                             region.qualification_state()),
            std::move(region))};
    } catch (...) {
        return std::unexpected{failure(
            protection_error(
                "memory.region_protection.contained_unknown_failure",
                "contained_unknown_failure", region.qualification_state()),
            std::move(region))};
    }
}

} // namespace epycbench::memory
