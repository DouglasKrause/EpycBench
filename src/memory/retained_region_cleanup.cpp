#include "epycbench/memory/retained_region_cleanup.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_query_access.hpp"
#include "memory/retained_region_cleanup_access.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::memory {
namespace {

[[nodiscard]] auto source_name(RetainedRegionSourceState source) noexcept
    -> std::string_view {
    switch (source) {
    case RetainedRegionSourceState::mapping: return "mapping";
    case RetainedRegionSourceState::bound: return "bound";
    case RetainedRegionSourceState::initialized: return "initialized";
    case RetainedRegionSourceState::placement_observed: return "placement_observed";
    }
    return "mapping";
}

[[nodiscard]] auto cleanup_error(std::string_view code, std::string_view classification,
                                 RetainedRegionSourceState source,
                                 const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{{"source_state", source_name(source)},
                                  {"cleanup_phase", "retained_region_release"},
                                  {"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Retained memory region cleanup failed",
            .operation = "memory.retained_region_cleanup.cleanup",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto region_mapping(RegionMapping& owner) noexcept -> RegionMapping& {
    return owner;
}
[[nodiscard]] auto region_mapping(BoundRegion& owner) noexcept -> RegionMapping& {
    return detail::BoundRegionAccess::mapping(owner);
}
[[nodiscard]] auto region_mapping(InitializedRegion& owner) noexcept -> RegionMapping& {
    return detail::BoundRegionAccess::mapping(detail::InitializedRegionAccess::region(owner));
}
[[nodiscard]] auto region_mapping(PlacementObservedRegion& owner) noexcept -> RegionMapping& {
    return region_mapping(detail::PlacementObservedRegionAccess::region(owner));
}

[[nodiscard]] auto valid(RegionMapping& region) noexcept -> bool {
    auto& mapping = detail::RegionMappingAccess::mapping(region);
    return mapping.active() && mapping.protection() == platform_linux::MemoryProtection::read_write &&
           mapping.size() > 0U && mapping.size() == region.mapping_bytes() &&
           mapping.bytes().size() == region.mapping_bytes() &&
           mapping.writable_bytes().size() == region.mapping_bytes() &&
           region.full_mapping().size() == region.mapping_bytes();
}

[[nodiscard]] auto released(const RegionMapping& region) noexcept -> bool {
    const auto& mapping = detail::RegionMappingAccess::mapping(region);
    return !mapping.active() && mapping.size() == 0U &&
           mapping.protection() == platform_linux::MemoryProtection::none &&
           mapping.bytes().empty();
}

template <typename Owner, typename Failure>
[[nodiscard]] auto cleanup_impl(Owner owner, RetainedRegionSourceState source) noexcept
    -> std::expected<CleanedRetainedRegion, Failure> {
    bool attempted = false;
    try {
        auto& region = region_mapping(owner);
        if (!valid(region)) {
            auto error = cleanup_error("memory.retained_region_cleanup.invalid_argument",
                                       "lifecycle_state_invalid", source);
            return std::unexpected{detail::RetainedRegionCleanupAccess::failure(
                std::move(error), detail::RetainedRegionCleanupAccess::evidence(
                                      false, false,
                                      detail::RegionMappingAccess::mapping(region).active()),
                source, std::move(owner))};
        }

        attempted = true;
        auto result = detail::RegionMappingAccess::mapping(region).release();
        if (!result) {
            auto error = cleanup_error("memory.retained_region_cleanup.release_failed",
                                       "release_failed", source, &result.error());
            return std::unexpected{detail::RetainedRegionCleanupAccess::failure(
                std::move(error),
                detail::RetainedRegionCleanupAccess::evidence(true, false, true), source,
                std::move(owner))};
        }
        if (!released(region)) {
            auto error = cleanup_error("memory.retained_region_cleanup.invariant_failed",
                                       "release_state_inconsistent", source);
            return std::unexpected{detail::RetainedRegionCleanupAccess::failure(
                std::move(error),
                detail::RetainedRegionCleanupAccess::evidence(true, false,
                    detail::RegionMappingAccess::mapping(region).active()),
                source, std::move(owner))};
        }
        return detail::RetainedRegionCleanupAccess::success(
            source, detail::RetainedRegionCleanupAccess::evidence(true, true, false));
    } catch (const std::exception&) {
        auto error = cleanup_error("memory.retained_region_cleanup.contained_exception",
                                   "contained_exception", source);
        return std::unexpected{detail::RetainedRegionCleanupAccess::failure(
            std::move(error), detail::RetainedRegionCleanupAccess::evidence(
                                  attempted, false,
                                  detail::RegionMappingAccess::mapping(region_mapping(owner)).active()),
            source, std::move(owner))};
    } catch (...) {
        auto error = cleanup_error("memory.retained_region_cleanup.contained_unknown_failure",
                                   "contained_unknown_failure", source);
        return std::unexpected{detail::RetainedRegionCleanupAccess::failure(
            std::move(error), detail::RetainedRegionCleanupAccess::evidence(
                                  attempted, false,
                                  detail::RegionMappingAccess::mapping(region_mapping(owner)).active()),
            source, std::move(owner))};
    }
}

} // namespace

auto RetainedRegionCleanup::cleanup(RegionMapping&& region) noexcept
    -> std::expected<CleanedRetainedRegion, RegionMappingCleanupFailure> {
    return cleanup_impl<RegionMapping, RegionMappingCleanupFailure>(
        std::move(region), RetainedRegionSourceState::mapping);
}
auto RetainedRegionCleanup::cleanup(BoundRegion&& region) noexcept
    -> std::expected<CleanedRetainedRegion, BoundRegionCleanupFailure> {
    return cleanup_impl<BoundRegion, BoundRegionCleanupFailure>(
        std::move(region), RetainedRegionSourceState::bound);
}
auto RetainedRegionCleanup::cleanup(InitializedRegion&& region) noexcept
    -> std::expected<CleanedRetainedRegion, InitializedRegionCleanupFailure> {
    return cleanup_impl<InitializedRegion, InitializedRegionCleanupFailure>(
        std::move(region), RetainedRegionSourceState::initialized);
}
auto RetainedRegionCleanup::cleanup(PlacementObservedRegion&& region) noexcept
    -> std::expected<CleanedRetainedRegion, PlacementObservedRegionCleanupFailure> {
    return cleanup_impl<PlacementObservedRegion, PlacementObservedRegionCleanupFailure>(
        std::move(region), RetainedRegionSourceState::placement_observed);
}

} // namespace epycbench::memory
