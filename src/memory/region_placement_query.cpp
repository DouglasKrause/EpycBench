#include "epycbench/memory/region_placement_query.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_query_access.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace epycbench::memory {
namespace {

static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_constructible_v<InitializedRegion>);
static_assert(std::is_nothrow_move_constructible_v<RegionPlacementObservation>);

[[nodiscard]] auto placement_error(std::string_view code,
                                   std::string_view classification,
                                   const core::Error* underlying = nullptr,
                                   std::optional<int> error_number = std::nullopt)
    -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    if (error_number && *error_number > 0) {
        os_error = core::OsError::capture(
            std::error_code{*error_number, std::generic_category()});
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region placement query failed",
            .operation = "memory.region_placement_query.observe",
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

[[nodiscard]] auto production_operations() noexcept
    -> detail::RegionPlacementQueryOperations {
    return {.context = nullptr, .query = production_query};
}

[[nodiscard]] auto mapping_of(const InitializedRegion& region) noexcept
    -> const RegionMapping& {
    return detail::BoundRegionAccess::mapping(
        detail::InitializedRegionAccess::region(region));
}

[[nodiscard]] auto validate_owner(const InitializedRegion& region) noexcept
    -> std::expected<const RegionMapping*, core::Error> {
    try {
        const auto& mapping = mapping_of(region);
        const auto& owner = detail::RegionMappingAccess::mapping(mapping);
        if (!owner.active()) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed", "mapping_inactive")};
        }
        if (owner.size() != mapping.mapping_bytes() ||
            owner.bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes()) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed",
                "placement_owner_invalid")};
        }
        if (owner.protection() == platform_linux::MemoryProtection::none) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed", "mapping_inactive")};
        }
        return &mapping;
    } catch (const std::exception&) {
        return std::unexpected{placement_error(
            "memory.region_placement_query.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{placement_error(
            "memory.region_placement_query.contained_unknown_failure",
            "contained_unknown_failure")};
    }
}

[[nodiscard]] auto increment(std::uint64_t& value) noexcept -> bool {
    if (value == std::numeric_limits<std::uint64_t>::max()) return false;
    ++value;
    return true;
}

} // namespace

auto PlacementObservedRegion::create(InitializedRegion region) noexcept
    -> std::expected<PlacementObservedRegion, RegionPlacementQueryFailure> {
    return detail::PlacementObservedRegionAccess::create_with_operations(
        std::move(region), production_operations());
}

auto detail::PlacementObservedRegionAccess::create_with_operations(
    InitializedRegion region, const RegionPlacementQueryOperations& operations) noexcept
    -> std::expected<PlacementObservedRegion, RegionPlacementQueryFailure> {
    const auto fail = [&region](core::Error error) noexcept
        -> std::expected<PlacementObservedRegion, RegionPlacementQueryFailure> {
        return std::unexpected{failure(std::move(error), std::move(region))};
    };

    try {
        const auto owner = validate_owner(region);
        if (!owner) return fail(owner.error());

        if (!region.request().verify_before()) {
            return success(std::move(region), PreRunPlacementQueryState::skipped,
                           std::nullopt);
        }

        auto observed = observe_region_placement(region, operations);
        if (!observed) return fail(std::move(observed.error()));
        return success(std::move(region), PreRunPlacementQueryState::observed,
                       std::optional<RegionPlacementObservation>{std::move(*observed)});
    } catch (const std::exception&) {
        return fail(placement_error(
            "memory.region_placement_query.contained_exception", "contained_exception"));
    } catch (...) {
        return fail(placement_error(
            "memory.region_placement_query.contained_unknown_failure",
            "contained_unknown_failure"));
    }
}

auto detail::PlacementObservedRegionAccess::observe_region_placement(
    const InitializedRegion& region,
    const RegionPlacementQueryOperations& operations) noexcept
    -> std::expected<RegionPlacementObservation, core::Error> {
    try {
        const auto validated = validate_owner(region);
        if (!validated) return std::unexpected{validated.error()};
        const auto& mapping = **validated;
        const auto& request = mapping.request();
        const auto requested = request.requested_bytes().value();
        const auto page = request.page_size().value();

        if (requested == 0U) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed",
                "requested_range_invalid")};
        }
        if (page == 0U) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invalid_argument", "page_size_invalid")};
        }
        if (requested % page != 0U) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invalid_argument",
                "page_alignment_invalid")};
        }
        if (requested > std::numeric_limits<std::size_t>::max() ||
            page > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invalid_argument",
                "page_address_unrepresentable")};
        }

        const auto requested_region = mapping.requested_region();
        if (requested_region.empty() ||
            requested_region.size() != static_cast<std::size_t>(requested)) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed",
                "requested_range_invalid")};
        }

        const auto total_pages = requested / page;
        if (total_pages == 0U) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed", "page_count_overflow")};
        }
        if (operations.query == nullptr) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed", "query_input_invalid")};
        }

        std::array<void*, kPlacementQueryBatchPages> page_addresses{};
        std::vector<LinuxNodePageCount> node_counts;
        node_counts.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(total_pages, kPlacementQueryBatchPages)));

        std::uint64_t resident_pages = 0;
        std::uint64_t unresolved_pages = 0;
        std::uint64_t processed_pages = 0;
        std::size_t offset = 0;
        const auto page_size = static_cast<std::size_t>(page);

        while (processed_pages < total_pages) {
            const auto count = batch_size(total_pages, processed_pages);
            if (count == 0U || count > page_addresses.size()) {
                return std::unexpected{placement_error(
                    "memory.region_placement_query.invariant_failed",
                    "observation_inconsistent")};
            }

            for (std::size_t index = 0; index < count; ++index) {
                if (offset >= requested_region.size()) {
                    return std::unexpected{placement_error(
                        "memory.region_placement_query.invariant_failed",
                        "page_address_overflow")};
                }
                page_addresses[index] =
                    const_cast<std::byte*>(requested_region.data() + offset);
                if (page_size > requested_region.size() - offset) {
                    return std::unexpected{placement_error(
                        "memory.region_placement_query.invariant_failed",
                        "page_address_overflow")};
                }
                offset += page_size;
            }

            auto statuses = operations.query(
                operations.context,
                std::span<void* const>{page_addresses.data(), count});
            if (!statuses) {
                return std::unexpected{placement_error(
                    "memory.region_placement_query.query_failed", "query_failed",
                    &statuses.error())};
            }
            if (statuses->size() != count) {
                return std::unexpected{placement_error(
                    "memory.region_placement_query.invariant_failed",
                    "returned_count_mismatch")};
            }

            for (const auto& status : *statuses) {
                switch (status.kind()) {
                case platform_linux::PageQueryStatusKind::resident: {
                    const auto node = status.node();
                    if (!node || status.error_number()) {
                        return std::unexpected{placement_error(
                            "memory.region_placement_query.invariant_failed",
                            "page_status_invalid")};
                    }
                    if (!increment(resident_pages)) {
                        return std::unexpected{placement_error(
                            "memory.region_placement_query.invariant_failed",
                            "observation_inconsistent")};
                    }
                    const auto position = std::lower_bound(
                        node_counts.begin(), node_counts.end(), *node,
                        [](const LinuxNodePageCount& count_entry,
                           core::LinuxNumaNodeId value) {
                            return count_entry.linux_node() < value;
                        });
                    if (position != node_counts.end() && position->linux_node() == *node) {
                        const auto pages = position->page_count();
                        if (pages == std::numeric_limits<std::uint64_t>::max()) {
                            return std::unexpected{placement_error(
                                "memory.region_placement_query.invariant_failed",
                                "observation_inconsistent")};
                        }
                        *position = node_count(*node, pages + 1U);
                    } else {
                        node_counts.insert(position, node_count(*node, 1U));
                    }
                    break;
                }
                case platform_linux::PageQueryStatusKind::unresolved:
                    if (status.node() || status.error_number() != ENOENT) {
                        return std::unexpected{placement_error(
                            "memory.region_placement_query.invariant_failed",
                            "page_status_invalid")};
                    }
                    if (!increment(unresolved_pages)) {
                        return std::unexpected{placement_error(
                            "memory.region_placement_query.invariant_failed",
                            "observation_inconsistent")};
                    }
                    break;
                case platform_linux::PageQueryStatusKind::error: {
                    const auto number = status.error_number();
                    if (status.node() || !number || *number <= 0 || *number == ENOENT) {
                        return std::unexpected{placement_error(
                            "memory.region_placement_query.invariant_failed",
                            "page_status_invalid")};
                    }
                    return std::unexpected{placement_error(
                        "memory.region_placement_query.query_failed", "page_status_error",
                        nullptr, number)};
                }
                }
            }

            processed_pages += count;
        }

        if (processed_pages != total_pages || offset != requested_region.size() ||
            resident_pages > total_pages ||
            unresolved_pages != total_pages - resident_pages) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed",
                "observation_inconsistent")};
        }

        std::uint64_t aggregate_pages = 0;
        for (const auto& count : node_counts) {
            if (count.page_count() == 0U ||
                count.page_count() >
                    std::numeric_limits<std::uint64_t>::max() - aggregate_pages) {
                return std::unexpected{placement_error(
                    "memory.region_placement_query.invariant_failed",
                    "observation_inconsistent")};
            }
            aggregate_pages += count.page_count();
        }
        if (aggregate_pages != resident_pages) {
            return std::unexpected{placement_error(
                "memory.region_placement_query.invariant_failed",
                "observation_inconsistent")};
        }

        return observation(total_pages, resident_pages, unresolved_pages,
                           std::move(node_counts));
    } catch (const std::bad_alloc&) {
        return std::unexpected{placement_error(
            "memory.region_placement_query.contained_exception", "allocation_failure")};
    } catch (const std::exception&) {
        return std::unexpected{placement_error(
            "memory.region_placement_query.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{placement_error(
            "memory.region_placement_query.contained_unknown_failure",
            "contained_unknown_failure")};
    }
}

} // namespace epycbench::memory
