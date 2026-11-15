#include "epycbench/platform_linux/file_descriptor.hpp"

#include "platform_linux/process_ipc_operations.hpp"

#include <exception>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto descriptor_error(std::string_view code, std::string_view message,
                                    std::string_view operation,
                                    std::string_view classification,
                                    std::optional<int> error_number = std::nullopt)
    -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(
            std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = nlohmann::json{{"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

} // namespace

auto DescriptorAccess::close_raw(FileDescriptor& descriptor) noexcept -> RawStatusResult {
    if (!descriptor.active()) return {0, 0};
    const auto value = std::exchange(descriptor.descriptor_, -1);
    auto* context = std::exchange(descriptor.context_, nullptr);
    const auto function = std::exchange(descriptor.close_, nullptr);
    return function(context, value);
}

auto DescriptorAccess::adopt(int descriptor, const DescriptorOperations& operations)
    -> std::expected<FileDescriptor, core::Error> {
    constexpr std::string_view operation = "FileDescriptor.adopt";
    try {
        if (descriptor < 0) {
            return std::unexpected{descriptor_error(
                "linux.descriptor.invalid_argument", "Linux descriptor is invalid", operation,
                "invalid_descriptor")};
        }
        return FileDescriptor{descriptor, operations.context, operations.close};
    } catch (const std::exception&) {
        return std::unexpected{descriptor_error(
            "linux.descriptor.contained_exception", "Descriptor adoption contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{descriptor_error(
            "linux.descriptor.contained_unknown_failure",
            "Descriptor adoption contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto DescriptorAccess::duplicate_at_least(const FileDescriptor& descriptor, int minimum,
                                          const DescriptorOperations& operations)
    -> std::expected<FileDescriptor, core::Error> {
    constexpr std::string_view operation = "FileDescriptor.duplicate_cloexec";
    try {
        const auto result = operations.duplicate_cloexec(
            operations.context, descriptor.native_handle(), minimum);
        if (result.value < minimum || result.error_number != 0) {
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{descriptor_error(
                "linux.descriptor.duplicate_failed", "Linux descriptor duplication failed",
                operation, result.value >= 0 ? "unexpected_native_result" : "native_failure",
                error_number)};
        }
        return FileDescriptor{static_cast<int>(result.value), operations.context,
                              operations.close};
    } catch (const std::exception&) {
        return std::unexpected{descriptor_error(
            "linux.descriptor.contained_exception",
            "Descriptor duplication contained an exception", operation,
            "contained_exception")};
    } catch (...) {
        return std::unexpected{descriptor_error(
            "linux.descriptor.contained_unknown_failure",
            "Descriptor duplication contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

} // namespace detail

auto FileDescriptor::adopt(int descriptor) -> std::expected<FileDescriptor, core::Error> {
    return detail::DescriptorAccess::adopt(descriptor,
                                           detail::production_descriptor_operations());
}

FileDescriptor::FileDescriptor(int descriptor, void* context,
                               CloseFunction close_function) noexcept
    : descriptor_{descriptor}, context_{context}, close_{close_function} {}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      context_{std::exchange(other.context_, nullptr)},
      close_{std::exchange(other.close_, nullptr)} {}

auto FileDescriptor::operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
    if (this != &other) {
        best_effort_close();
        descriptor_ = std::exchange(other.descriptor_, -1);
        context_ = std::exchange(other.context_, nullptr);
        close_ = std::exchange(other.close_, nullptr);
    }
    return *this;
}

FileDescriptor::~FileDescriptor() noexcept { best_effort_close(); }

auto FileDescriptor::native_handle() const noexcept -> int { return descriptor_; }
auto FileDescriptor::active() const noexcept -> bool { return descriptor_ >= 0; }

auto FileDescriptor::close() -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "FileDescriptor.close";
    if (!active()) {
        return {};
    }
    const auto descriptor = std::exchange(descriptor_, -1);
    auto* context = std::exchange(context_, nullptr);
    const auto close_function = std::exchange(close_, nullptr);
    try {
        const auto result = close_function(context, descriptor);
        if (result.value != 0 || result.error_number != 0) {
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{detail::descriptor_error(
                "linux.descriptor.close_failed", "Linux descriptor close failed", operation,
                result.value == -1 && result.error_number != 0 ? "native_failure"
                                                               : "unexpected_native_result",
                error_number)};
        }
        return {};
    } catch (const std::exception&) {
        return std::unexpected{detail::descriptor_error(
            "linux.descriptor.contained_exception", "Descriptor close contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::descriptor_error(
            "linux.descriptor.contained_unknown_failure",
            "Descriptor close contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

void FileDescriptor::best_effort_close() noexcept {
    if (!active()) {
        return;
    }
    const auto descriptor = std::exchange(descriptor_, -1);
    auto* context = std::exchange(context_, nullptr);
    const auto close_function = std::exchange(close_, nullptr);
    static_cast<void>(close_function(context, descriptor));
}

} // namespace epycbench::platform_linux
