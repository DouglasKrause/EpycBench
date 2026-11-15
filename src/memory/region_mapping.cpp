#include "epycbench/memory/region_mapping.hpp"

#include "memory/region_mapping_access.hpp"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::memory {
namespace {

[[nodiscard]] auto mapping_error(std::string_view code, std::string_view classification,
                                 const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        os_error = underlying->os_error;
    }
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory region mapping creation failed",
            .operation = "memory.region_mapping.create",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto primary_failure(std::string_view code, std::string_view classification,
                                   const core::Error* underlying = nullptr)
    -> std::unexpected<RegionMappingFailure> {
    return std::unexpected{RegionMappingFailure{mapping_error(code, classification, underlying)}};
}

[[nodiscard]] auto cleanup_failure(const core::Error& underlying) -> core::Error {
    return mapping_error("memory.region_mapping.invariant_failed", "rollback_failed", &underlying);
}

[[nodiscard]] auto production_create_mapping(void*, std::size_t mapping_bytes)
    -> std::expected<platform_linux::AnonymousMapping, core::Error> {
    return platform_linux::AnonymousMapping::create(mapping_bytes);
}

[[nodiscard]] auto production_operations() noexcept -> detail::RegionMappingOperations {
    return {.context = nullptr, .create_mapping = production_create_mapping};
}

[[nodiscard]] auto fail_after_release(platform_linux::AnonymousMapping& mapping,
                                      core::Error primary) -> RegionMappingFailure {
    auto released = mapping.release();
    if (!released) {
        return RegionMappingFailure{std::move(primary), cleanup_failure(released.error())};
    }
    return RegionMappingFailure{std::move(primary)};
}

} // namespace

auto RegionMapping::create(RegionRequest request) noexcept
    -> std::expected<RegionMapping, RegionMappingFailure> {
    return detail::RegionMappingAccess::create_with_operations(std::move(request),
                                                               production_operations());
}

auto RegionMapping::requested_region() noexcept -> std::span<std::byte> {
    auto full = full_mapping();
    const auto requested = requested_bytes().value();
    if (requested > full.size())
        return {};
    return full.first(static_cast<std::size_t>(requested));
}

auto RegionMapping::requested_region() const noexcept -> std::span<const std::byte> {
    const auto full = full_mapping();
    const auto requested = requested_bytes().value();
    if (requested > full.size())
        return {};
    return full.first(static_cast<std::size_t>(requested));
}

auto RegionMapping::full_mapping() noexcept -> std::span<std::byte> {
    return mapping_.writable_bytes();
}

auto RegionMapping::full_mapping() const noexcept -> std::span<const std::byte> {
    return mapping_.bytes();
}

auto detail::RegionMappingAccess::calculate_mapping_bytes(core::ByteCount requested,
                                                          core::ByteCount page) noexcept
    -> std::expected<std::size_t, core::Error> {
    try {
        if (requested.value() == 0U || page.value() == 0U) {
            return std::unexpected{
                mapping_error("memory.region_mapping.invariant_failed", "mapping_owner_invalid")};
        }

        auto rounded = requested;
        const auto remainder = requested.value() % page.value();
        if (remainder != 0U) {
            const auto padding = core::ByteCount::from_base_units(page.value() - remainder);
            const auto sum = requested.checked_add(padding);
            if (!sum) {
                return std::unexpected{
                    mapping_error("memory.region_mapping.size_overflow", "page_rounding_overflow")};
            }
            rounded = *sum;
        }

        if (rounded.value() > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{
                mapping_error("memory.region_mapping.native_size_unrepresentable",
                              "mapping_size_unrepresentable")};
        }
        return static_cast<std::size_t>(rounded.value());
    } catch (const std::exception&) {
        return std::unexpected{
            mapping_error("memory.region_mapping.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{mapping_error("memory.region_mapping.contained_unknown_failure",
                                             "contained_unknown_failure")};
    }
}

auto detail::RegionMappingAccess::create_with_operations(
    RegionRequest request, const RegionMappingOperations& operations) noexcept
    -> std::expected<RegionMapping, RegionMappingFailure> {
    try {
        const auto mapping_bytes =
            calculate_mapping_bytes(request.requested_bytes(), request.page_size());
        if (!mapping_bytes) {
            return std::unexpected{RegionMappingFailure{mapping_bytes.error()}};
        }
        if (operations.create_mapping == nullptr) {
            return primary_failure("memory.region_mapping.invariant_failed",
                                   "mapping_owner_invalid");
        }

        auto created = operations.create_mapping(operations.context, *mapping_bytes);
        if (!created) {
            return primary_failure("memory.region_mapping.creation_failed",
                                   "mapping_creation_failed", &created.error());
        }
        auto mapping = std::move(*created);

        const auto owner_valid = mapping.active() && mapping.size() == *mapping_bytes &&
                                 mapping.writable_bytes().size() == *mapping_bytes &&
                                 std::as_const(mapping).bytes().size() == *mapping_bytes;
        if (!owner_valid) {
            auto primary =
                mapping_error("memory.region_mapping.invariant_failed", "mapping_owner_invalid");
            if (mapping.active()) {
                return std::unexpected{fail_after_release(mapping, std::move(primary))};
            }
            return std::unexpected{RegionMappingFailure{std::move(primary)}};
        }

        const auto advice = mapping.disable_transparent_huge_pages();
        if (!advice) {
            auto primary = mapping_error("memory.region_mapping.creation_failed",
                                         "base_page_advice_failed", &advice.error());
            return std::unexpected{fail_after_release(mapping, std::move(primary))};
        }

        return RegionMapping{std::move(request), std::move(mapping), *mapping_bytes};
    } catch (const std::exception&) {
        return primary_failure("memory.region_mapping.contained_exception", "contained_exception");
    } catch (...) {
        return primary_failure("memory.region_mapping.contained_unknown_failure",
                               "contained_unknown_failure");
    }
}

} // namespace epycbench::memory
