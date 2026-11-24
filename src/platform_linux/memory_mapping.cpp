#include "epycbench/platform_linux/memory_mapping.hpp"

#include "platform_linux/hugetlb_backing.hpp"
#include "platform_linux/timed_anonymous_mapping.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/mman.h>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto production_map(void*, std::size_t length, int protection, int flags,
                                  int file_descriptor, std::int64_t offset) noexcept
    -> RawMapResult {
    auto* address = ::mmap(nullptr, length, protection, flags, file_descriptor,
                           static_cast<off_t>(offset));
    if (address == MAP_FAILED) {
        const auto captured_errno = errno;
        return {.address = address, .error_number = captured_errno};
    }
    return {.address = address, .error_number = 0};
}

[[nodiscard]] auto production_unmap(void*, void* address, std::size_t length) noexcept
    -> RawMappingStatusResult {
    const auto value = ::munmap(address, length);
    if (value != 0) {
        const auto captured_errno = errno;
        return {.value = value, .error_number = captured_errno};
    }
    return {.value = value, .error_number = 0};
}

[[nodiscard]] auto production_protect(void*, void* address, std::size_t length,
                                      int protection) noexcept -> RawMappingStatusResult {
    const auto value = ::mprotect(address, length, protection);
    if (value != 0) {
        const auto captured_errno = errno;
        return {.value = value, .error_number = captured_errno};
    }
    return {.value = value, .error_number = 0};
}

[[nodiscard]] auto production_advise(void*, void* address, std::size_t length,
                                     int advice) noexcept -> RawMappingStatusResult {
    const auto value = ::madvise(address, length, advice);
    if (value != 0) {
        const auto captured_errno = errno;
        return {.value = value, .error_number = captured_errno};
    }
    return {.value = value, .error_number = 0};
}

[[nodiscard]] auto mapping_error(std::string_view code, std::string_view message,
                                 std::string_view operation, std::string_view classification,
                                 std::optional<int> error_number = std::nullopt,
                                 std::optional<MemoryProtection> protection = std::nullopt,
                                 bool advice = false) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (protection) {
        switch (*protection) {
        case MemoryProtection::none:
            context["protection"] = "none";
            break;
        case MemoryProtection::read_only:
            context["protection"] = "read_only";
            break;
        case MemoryProtection::read_write:
            context["protection"] = "read_write";
            break;
        }
    }
    if (advice) {
        context["advice"] = "no_huge_pages";
    }
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto contained_exception(std::string_view operation) -> core::Error {
    return mapping_error("linux.mapping.contained_exception",
                         "Linux mapping operation contained an exception", operation,
                         "contained_exception");
}

[[nodiscard]] auto contained_unknown_failure(std::string_view operation) -> core::Error {
    return mapping_error("linux.mapping.contained_unknown_failure",
                         "Linux mapping operation contained an unknown failure", operation,
                         "contained_unknown_failure");
}

[[nodiscard]] auto unexpected_status(std::string_view operation,
                                     std::optional<MemoryProtection> protection = std::nullopt,
                                     bool advice = false) -> core::Error {
    return mapping_error("linux.mapping.unexpected_result",
                         "Linux mapping operation returned inconsistent status", operation,
                         "unexpected_native_result", std::nullopt, protection, advice);
}

[[nodiscard]] auto create_classification(int error_number) -> std::string_view {
    switch (error_number) {
    case ENOMEM:
        return "address_space_exhausted";
    case EAGAIN:
        return "resource_limit";
    case EACCES:
    case EPERM:
        return "permission_denied";
    case EINVAL:
        return "invalid_mapping_request";
    case EBADF:
    case ENODEV:
    case EEXIST:
        return "unexpected_mapping_configuration";
    default:
        return "create_failure";
    }
}

[[nodiscard]] auto unmap_classification(int error_number) -> std::string_view {
    return error_number == EINVAL ? "invalid_address_range" : "cleanup_failure";
}

[[nodiscard]] auto protect_classification(int error_number) -> std::string_view {
    switch (error_number) {
    case EACCES:
    case EPERM:
        return "permission_denied";
    case EINVAL:
        return "invalid_protection_or_range";
    case ENOMEM:
        return "range_unmapped_or_resource_failure";
    default:
        return "protection_failure";
    }
}

[[nodiscard]] auto advice_classification(int error_number) -> std::string_view {
    switch (error_number) {
    case EINVAL:
    case ENOSYS:
        return "unsupported_advice";
    case ENOMEM:
        return "range_unmapped";
    case EACCES:
    case EPERM:
        return "permission_denied";
    default:
        return "advice_failure";
    }
}

[[nodiscard]] auto native_protection(MemoryProtection protection) noexcept -> int {
    switch (protection) {
    case MemoryProtection::none:
        return PROT_NONE;
    case MemoryProtection::read_only:
        return PROT_READ;
    case MemoryProtection::read_write:
        return PROT_READ | PROT_WRITE;
    }
    return PROT_NONE;
}

} // namespace

auto production_mapping_operations() noexcept -> MappingOperations {
    return MappingOperations{
        .context = nullptr,
        .map_anonymous = production_map,
        .unmap = production_unmap,
        .protect = production_protect,
        .advise = production_advise,
    };
}

namespace {
void validate_mapping_range(void* address, std::size_t length) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (length - 1U > std::numeric_limits<std::uintptr_t>::max() - start) {
        throw std::overflow_error{"mapping address range overflow"};
    }
}

auto request_mapping(std::size_t length, const MappingOperations& operations, int extra_flags = 0) noexcept -> RawMapResult {
    return operations.map_anonymous(operations.context, length, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
}

auto finish_mapping(std::size_t length, const MappingOperations& operations, RawMapResult result,
                    RangeValidator validate_range) -> std::expected<AnonymousMapping, core::Error> {
    constexpr std::string_view operation = "AnonymousMapping.create";
    try {
        if (result.address == MAP_FAILED) {
            if (result.error_number == 0) {
                return std::unexpected{unexpected_status(operation)};
            }
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{mapping_error(
                "linux.mapping.create_failed", "Anonymous mapping creation failed", operation,
                create_classification(result.error_number), error_number)};
        }
        if (result.address == nullptr) {
            static_cast<void>(operations.unmap(operations.context, result.address, length));
            return std::unexpected{mapping_error(
                "linux.mapping.unexpected_result", "Anonymous mapping returned an unusable range",
                operation, "unexpected_native_result")};
        }
        if (result.error_number != 0) {
            static_cast<void>(operations.unmap(operations.context, result.address, length));
            return std::unexpected{mapping_error(
                "linux.mapping.unexpected_result", "Anonymous mapping returned inconsistent status",
                operation, "unexpected_native_result")};
        }
        try {
            validate_range(result.address, length);
        } catch (const std::overflow_error&) {
            static_cast<void>(operations.unmap(operations.context, result.address, length));
            return std::unexpected{mapping_error(
                "linux.mapping.unexpected_result", "Anonymous mapping returned an unusable range",
                operation, "address_range_overflow")};
        } catch (...) {
            static_cast<void>(operations.unmap(operations.context, result.address, length));
            throw;
        }
        return AnonymousMappingAccess::adopt(result.address, length, operations);
    } catch (const std::bad_alloc&) {
        return std::unexpected{mapping_error("linux.mapping.create_failed",
                                             "Anonymous mapping creation failed", operation,
                                             "allocation_failure")};
    } catch (const std::exception&) {
        return std::unexpected{contained_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_failure(operation)};
    }
}

} // namespace

void AnonymousMappingAccess::validate_address_range(void* address, std::size_t length) {
    validate_mapping_range(address, length);
}

auto AnonymousMappingAccess::create(std::size_t length, const MappingOperations& operations,
                                    RangeValidator validate_range)
    -> std::expected<AnonymousMapping, core::Error> {
    if (length == 0U)
        return std::unexpected{mapping_error("linux.mapping.invalid_length", "Anonymous mapping length must not be zero",
                                             "AnonymousMapping.create", "zero_length")};
    return finish_mapping(length, operations, request_mapping(length, operations), validate_range);
}

auto AnonymousMappingAccess::create_explicit_hugetlb_2m(std::size_t length, const MappingOperations& operations)
    -> std::expected<AnonymousMapping, core::Error> {
    if (length == 0U || length % hugetlb_2m_bytes != 0U)
        return std::unexpected{mapping_error("linux.mapping.invalid_length", "Expected a whole number of 2 MiB pages",
                                             "AnonymousMapping.create", "hugetlb_extent")};
    return finish_mapping(length, operations, request_mapping(length, operations, explicit_hugetlb_2m_flags()),
                          validate_mapping_range);
}

auto time_anonymous_mapping(std::size_t length, const MappingOperations& mapping,
                            const ClockOperations& clock) -> TimedAnonymousMapping {
    auto start = RawMonotonicClockAccess::now(clock);
    if (!start) return {std::move(start)};
    const auto mapped = request_mapping(length, mapping);
    auto end = RawMonotonicClockAccess::now(clock);
    return {std::move(start), std::move(end), finish_mapping(length, mapping, mapped, validate_mapping_range)};
}

} // namespace detail

AnonymousMapping::AnonymousMapping(void* address, std::size_t length, void* operation_context,
                                   UnmapFunction unmap, ProtectFunction protect_function,
                                   AdviseFunction advise) noexcept
    : address_{address}, length_{length}, protection_{MemoryProtection::read_write},
      operation_context_{operation_context}, unmap_{unmap}, protect_{protect_function},
      advise_{advise} {}

AnonymousMapping::AnonymousMapping(AnonymousMapping&& other) noexcept
    : address_{std::exchange(other.address_, nullptr)},
      length_{std::exchange(other.length_, 0)},
      protection_{std::exchange(other.protection_, MemoryProtection::none)},
      operation_context_{std::exchange(other.operation_context_, nullptr)},
      unmap_{std::exchange(other.unmap_, nullptr)},
      protect_{std::exchange(other.protect_, nullptr)},
      advise_{std::exchange(other.advise_, nullptr)} {}

auto AnonymousMapping::operator=(AnonymousMapping&& other) noexcept -> AnonymousMapping& {
    if (this != &other) {
        best_effort_release();
        address_ = std::exchange(other.address_, nullptr);
        length_ = std::exchange(other.length_, 0);
        protection_ = std::exchange(other.protection_, MemoryProtection::none);
        operation_context_ = std::exchange(other.operation_context_, nullptr);
        unmap_ = std::exchange(other.unmap_, nullptr);
        protect_ = std::exchange(other.protect_, nullptr);
        advise_ = std::exchange(other.advise_, nullptr);
    }
    return *this;
}

AnonymousMapping::~AnonymousMapping() noexcept { best_effort_release(); }

auto AnonymousMapping::data() noexcept -> std::byte* {
    return static_cast<std::byte*>(address_);
}

auto AnonymousMapping::data() const noexcept -> const std::byte* {
    return static_cast<const std::byte*>(address_);
}

auto AnonymousMapping::size() const noexcept -> std::size_t { return length_; }

auto AnonymousMapping::writable_bytes() noexcept -> std::span<std::byte> {
    if (!active() || protection_ != MemoryProtection::read_write) {
        return {};
    }
    return {data(), length_};
}

auto AnonymousMapping::bytes() const noexcept -> std::span<const std::byte> {
    if (!active()) {
        return {};
    }
    return {data(), length_};
}

auto AnonymousMapping::active() const noexcept -> bool { return address_ != nullptr; }

auto AnonymousMapping::protection() const noexcept -> MemoryProtection { return protection_; }

auto AnonymousMapping::protect(MemoryProtection protection)
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "AnonymousMapping.protect";
    if (!active()) {
        return std::unexpected{detail::mapping_error(
            "linux.mapping.invalid_state", "Anonymous mapping is not active", operation,
            "inactive_mapping", std::nullopt, protection)};
    }
    try {
        const auto result = protect_(operation_context_, address_, length_,
                                     detail::native_protection(protection));
        if (result.value == 0 && result.error_number != 0) {
            return std::unexpected{detail::unexpected_status(operation, protection)};
        }
        if (result.value != 0) {
            if (result.value != -1 || result.error_number == 0) {
                return std::unexpected{detail::unexpected_status(operation, protection)};
            }
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{detail::mapping_error(
                "linux.mapping.protect_failed", "Anonymous mapping protection failed", operation,
                detail::protect_classification(result.error_number), error_number, protection)};
        }
        protection_ = protection;
        return {};
    } catch (const std::exception&) {
        return std::unexpected{detail::contained_exception(operation)};
    } catch (...) {
        return std::unexpected{detail::contained_unknown_failure(operation)};
    }
}

auto AnonymousMapping::disable_transparent_huge_pages()
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "AnonymousMapping.disable_transparent_huge_pages";
    if (!active()) {
        return std::unexpected{detail::mapping_error(
            "linux.mapping.invalid_state", "Anonymous mapping is not active", operation,
            "inactive_mapping", std::nullopt, std::nullopt, true)};
    }
    try {
        const auto result = advise_(operation_context_, address_, length_, MADV_NOHUGEPAGE);
        if (result.value == 0 && result.error_number != 0) {
            return std::unexpected{detail::unexpected_status(operation, std::nullopt, true)};
        }
        if (result.value != 0) {
            if (result.value != -1 || result.error_number == 0) {
                return std::unexpected{detail::unexpected_status(operation, std::nullopt, true)};
            }
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{detail::mapping_error(
                "linux.mapping.advice_failed", "Anonymous mapping advice failed", operation,
                detail::advice_classification(result.error_number), error_number, std::nullopt,
                true)};
        }
        return {};
    } catch (const std::exception&) {
        return std::unexpected{detail::contained_exception(operation)};
    } catch (...) {
        return std::unexpected{detail::contained_unknown_failure(operation)};
    }
}

auto AnonymousMapping::release() -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "AnonymousMapping.release";
    if (!active()) {
        return {};
    }
    try {
        const auto result = unmap_(operation_context_, address_, length_);
        if (result.value == 0 && result.error_number != 0) {
            return std::unexpected{detail::unexpected_status(operation)};
        }
        if (result.value != 0) {
            if (result.value != -1 || result.error_number == 0) {
                return std::unexpected{detail::unexpected_status(operation)};
            }
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{detail::mapping_error(
                "linux.mapping.unmap_failed", "Anonymous mapping release failed", operation,
                detail::unmap_classification(result.error_number), error_number)};
        }
        clear();
        return {};
    } catch (const std::exception&) {
        return std::unexpected{detail::contained_exception(operation)};
    } catch (...) {
        return std::unexpected{detail::contained_unknown_failure(operation)};
    }
}

void AnonymousMapping::best_effort_release() noexcept {
    if (active()) {
        static_cast<void>(unmap_(operation_context_, address_, length_));
        clear();
    }
}

void AnonymousMapping::clear() noexcept {
    address_ = nullptr;
    length_ = 0;
    protection_ = MemoryProtection::none;
    operation_context_ = nullptr;
    unmap_ = nullptr;
    protect_ = nullptr;
    advise_ = nullptr;
}

auto AnonymousMapping::create(std::size_t length)
    -> std::expected<AnonymousMapping, core::Error> {
    return detail::AnonymousMappingAccess::create(length,
                                                  detail::production_mapping_operations());
}

} // namespace epycbench::platform_linux
