#include "epycbench/memory/region_initialization.hpp"

#include "memory/region_binding_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"

#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace epycbench::memory {
namespace {

static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_assignable_v<core::Error>);
static_assert(std::is_nothrow_move_constructible_v<core::StableErrorCode>);

[[nodiscard]] auto initialization_error(std::string_view code,
                                        std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region initialization failed",
            .operation = "memory.region_initialization.create",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto production_prefault_write(void*, std::span<std::byte> requested_region,
                                             std::size_t offset, std::byte value) noexcept
    -> std::expected<void, core::Error> {
    requested_region[offset] = value;
    return {};
}

[[nodiscard]] auto callback_contained_standard() -> core::Error {
    return initialization_error("memory.region_initialization.contained_exception",
                                "contained_exception");
}

[[nodiscard]] auto callback_contained_unknown() -> core::Error {
    return initialization_error("memory.region_initialization.contained_unknown_failure",
                                "contained_unknown_failure");
}

} // namespace

auto RegionInitializer::create(void* context, InitializeFunction initialize) noexcept
    -> std::expected<RegionInitializer, core::Error> {
    try {
        auto invalid_failure =
            initialization_error("memory.region_initialization.invalid_argument",
                                 "initializer_invalid");
        auto standard_failure = callback_contained_standard();
        auto unknown_failure = callback_contained_unknown();
        if (initialize == nullptr) {
            return std::unexpected{std::move(invalid_failure)};
        }
        return RegionInitializer{context, initialize, std::move(invalid_failure),
                                 std::move(standard_failure), std::move(unknown_failure)};
    } catch (const std::exception&) {
        return std::unexpected{callback_contained_standard()};
    } catch (...) {
        return std::unexpected{callback_contained_unknown()};
    }
}

auto RegionInitializer::initialize(std::span<std::byte> requested_region) const noexcept
    -> std::expected<void, core::Error> {
    try {
        if (initialize_ == nullptr) {
            return std::unexpected{std::move(invalid_failure_)};
        }
        try {
            return initialize_(context_, requested_region);
        } catch (const std::exception&) {
            return std::unexpected{std::move(standard_failure_)};
        } catch (...) {
            return std::unexpected{std::move(unknown_failure_)};
        }
    } catch (const std::exception&) {
        return std::unexpected{callback_contained_standard()};
    } catch (...) {
        return std::unexpected{callback_contained_unknown()};
    }
}

auto InitializedRegion::create(BoundRegion region, RegionInitializer initializer) noexcept
    -> std::expected<InitializedRegion, RegionInitializationFailure> {
    return detail::InitializedRegionAccess::create_with_operations(
        std::move(region), std::move(initializer), detail::production_region_initialization_operations());
}

auto detail::production_region_initialization_operations() noexcept
    -> RegionInitializationOperations {
    return {.context = nullptr, .write_prefault_byte = production_prefault_write};
}

auto detail::InitializedRegionAccess::create_with_operations(
    BoundRegion region, RegionInitializer initializer,
    const RegionInitializationOperations& operations) noexcept
    -> std::expected<InitializedRegion, RegionInitializationFailure> {
    const auto fail = [&region](core::Error error, InitializationWriteState write_state,
                                std::optional<core::StableErrorCode> underlying = std::nullopt)
        noexcept -> std::expected<InitializedRegion, RegionInitializationFailure> {
        return std::unexpected{failure(std::move(error), write_state, std::move(region),
                                       std::move(underlying))};
    };

    try {
        auto& mapping = BoundRegionAccess::mapping(region);
        const auto& request = mapping.request();
        auto& owner = RegionMappingAccess::mapping(mapping);

        if (!owner.active()) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "mapping_inactive"),
                        InitializationWriteState::no_writes);
        }
        if (owner.size() != mapping.mapping_bytes() ||
            std::as_const(owner).bytes().size() != mapping.mapping_bytes() ||
            std::as_const(mapping).full_mapping().size() != mapping.mapping_bytes()) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "initialization_owner_invalid"),
                        InitializationWriteState::no_writes);
        }
        if (owner.protection() != platform_linux::MemoryProtection::read_write ||
            owner.writable_bytes().size() != mapping.mapping_bytes() ||
            mapping.full_mapping().size() != mapping.mapping_bytes()) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "mapping_not_writable"),
                        InitializationWriteState::no_writes);
        }

        auto requested_region = mapping.requested_region();
        const auto requested = request.requested_bytes().value();
        if (requested_region.empty()) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "requested_region_unavailable"),
                        InitializationWriteState::no_writes);
        }
        if (requested > std::numeric_limits<std::size_t>::max() ||
            requested_region.size() != static_cast<std::size_t>(requested) ||
            requested_region.size() > mapping.full_mapping().size()) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "requested_range_invalid"),
                        InitializationWriteState::no_writes);
        }
        if (requested == 0U) {
            return fail(initialization_error("memory.region_initialization.invariant_failed",
                                             "requested_range_invalid"),
                        InitializationWriteState::no_writes);
        }

        const auto page = request.page_size().value();
        if (page == 0U || page > std::numeric_limits<std::size_t>::max()) {
            return fail(initialization_error("memory.region_initialization.invalid_argument",
                                             "page_size_invalid"),
                        InitializationWriteState::no_writes);
        }
        if (requested % page != 0U) {
            return fail(initialization_error("memory.region_initialization.invalid_argument",
                                             "page_alignment_invalid"),
                        InitializationWriteState::no_writes);
        }
        if (!valid(initializer) || operations.write_prefault_byte == nullptr) {
            return fail(initialization_error("memory.region_initialization.invalid_argument",
                                             "initializer_invalid"),
                        InitializationWriteState::no_writes);
        }

        const auto prepared_evidence = evidence(request.prefault());
        auto prefault_no_write = initialization_error(
            "memory.region_initialization.prefault_failed", "prefault_failed");
        auto prefault_partial = initialization_error(
            "memory.region_initialization.prefault_failed", "partial_initialization");
        auto initialization_partial = initialization_error(
            "memory.region_initialization.initialization_failed", "partial_initialization");
        auto contained_partial = initialization_error(
            "memory.region_initialization.contained_exception", "partial_initialization");
        auto contained_unknown_partial = initialization_error(
            "memory.region_initialization.contained_unknown_failure",
            "partial_initialization");

        bool wrote_prefault = false;
        if (request.prefault()) {
            const auto page_offset = static_cast<std::size_t>(page);
            std::size_t offset = 0;
            while (offset < requested_region.size()) {
                try {
                    auto written = operations.write_prefault_byte(
                        operations.context, requested_region, offset, std::byte{0});
                    if (!written) {
                        auto underlying = std::move(written.error().code);
                        return fail(wrote_prefault ? std::move(prefault_partial)
                                                   : std::move(prefault_no_write),
                                    wrote_prefault ? InitializationWriteState::partial
                                                   : InitializationWriteState::no_writes,
                                    std::move(underlying));
                    }
                    wrote_prefault = true;
                } catch (const std::exception&) {
                    return fail(std::move(contained_partial),
                                InitializationWriteState::partial);
                } catch (...) {
                    return fail(std::move(contained_unknown_partial),
                                InitializationWriteState::partial);
                }

                const auto remaining = requested_region.size() - offset;
                if (remaining <= page_offset) {
                    offset = requested_region.size();
                } else {
                    offset += page_offset;
                }
            }
        }

        auto initialized = initializer.initialize(requested_region);
        if (!initialized) {
            const bool contained_standard =
                initialized.error().code.value() ==
                "memory.region_initialization.contained_exception";
            const bool contained_unknown =
                initialized.error().code.value() ==
                "memory.region_initialization.contained_unknown_failure";
            auto underlying = std::move(initialized.error().code);
            if (contained_standard) {
                return fail(std::move(contained_partial), InitializationWriteState::partial,
                            std::move(underlying));
            }
            if (contained_unknown) {
                return fail(std::move(contained_unknown_partial),
                            InitializationWriteState::partial, std::move(underlying));
            }
            return fail(std::move(initialization_partial), InitializationWriteState::partial,
                        std::move(underlying));
        }

        return success(std::move(region), prepared_evidence);
    } catch (const std::exception&) {
        return fail(initialization_error("memory.region_initialization.contained_exception",
                                         "contained_exception"),
                    InitializationWriteState::no_writes);
    } catch (...) {
        return fail(initialization_error(
                        "memory.region_initialization.contained_unknown_failure",
                        "contained_unknown_failure"),
                    InitializationWriteState::no_writes);
    }
}

} // namespace epycbench::memory
