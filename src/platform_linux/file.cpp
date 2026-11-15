#include "epycbench/platform_linux/file.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux {
namespace {

class FileDescriptor {
  public:
    explicit FileDescriptor(int value) noexcept : value_{value} {}

    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}

    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
        if (this != &other) {
            close();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    ~FileDescriptor() { close(); }

    [[nodiscard]] auto get() const noexcept -> int { return value_; }

  private:
    void close() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
            value_ = -1;
        }
    }

    int value_;
};

[[nodiscard]] auto file_error(const std::filesystem::path& path, std::string_view operation,
                              int error_number) -> core::Error {
    const auto permission_denied = error_number == EACCES || error_number == EPERM;
    const auto error_code = std::error_code{error_number, std::generic_category()};
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{permission_denied ? "linux.file.permission_denied"
                                                        : "linux.file.open_failed"},
        .message = permission_denied ? "Permission denied while opening Linux text file"
                                     : "Unable to open Linux text file",
        .operation = std::string{operation},
        .os_error = core::OsError::capture(error_code),
        .context = nlohmann::json{{"path", path.string()}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto read_error(const std::filesystem::path& path, std::string_view operation,
                              int error_number) -> core::Error {
    const auto permission_denied = error_number == EACCES || error_number == EPERM;
    const auto error_code = std::error_code{error_number, std::generic_category()};
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{permission_denied ? "linux.file.permission_denied"
                                                        : "linux.file.read_failed"},
        .message = permission_denied ? "Permission denied while reading Linux text file"
                                     : "Unable to read complete Linux text file",
        .operation = std::string{operation},
        .os_error = core::OsError::capture(error_code),
        .context = nlohmann::json{{"path", path.string()}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto read_open_file(const std::filesystem::path& path, FileDescriptor descriptor,
                                  std::string_view operation)
    -> std::expected<std::string, core::Error> {
    std::string contents;
    std::array<char, 16 * 1024> buffer{};

    while (true) {
        const auto bytes_read = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (bytes_read > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            return contents;
        }
        if (errno == EINTR) {
            continue;
        }
        const auto error_number = errno;
        return std::unexpected{read_error(path, operation, error_number)};
    }
}

[[nodiscard]] auto cpu_list_error_name(core::IdSetParseErrorCode code) -> std::string_view {
    switch (code) {
    case core::IdSetParseErrorCode::empty_input:
        return "empty_input";
    case core::IdSetParseErrorCode::empty_component:
        return "empty_component";
    case core::IdSetParseErrorCode::invalid_number:
        return "invalid_number";
    case core::IdSetParseErrorCode::value_out_of_range:
        return "value_out_of_range";
    case core::IdSetParseErrorCode::descending_range:
        return "descending_range";
    }
    return "unknown";
}

} // namespace

auto read_text_file(const std::filesystem::path& path) -> std::expected<std::string, core::Error> {
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const auto error_number = errno;
        return std::unexpected{file_error(path, "read_text_file", error_number)};
    }
    return read_open_file(path, FileDescriptor{descriptor}, "read_text_file");
}

auto read_optional_text_file(const std::filesystem::path& path)
    -> std::expected<std::optional<std::string>, core::Error> {
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const auto error_number = errno;
        if (error_number == ENOENT) {
            return std::optional<std::string>{};
        }
        return std::unexpected{file_error(path, "read_optional_text_file", error_number)};
    }

    auto contents = read_open_file(path, FileDescriptor{descriptor}, "read_optional_text_file");
    if (!contents) {
        return std::unexpected{std::move(contents.error())};
    }
    return std::optional<std::string>{std::move(*contents)};
}

auto parse_cpu_list(std::string_view text) -> std::expected<core::CpuSet, core::Error> {
    const auto trimmed = detail::trim_ascii_space(text);
    auto parsed = core::CpuSet::parse(trimmed);
    if (parsed) {
        return std::move(*parsed);
    }

    return std::unexpected{core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{"linux.cpu_list.malformed"},
        .message = "Linux CPU-list data is malformed",
        .operation = "parse_cpu_list",
        .os_error = std::nullopt,
        .context =
            nlohmann::json{
                {"input", text},
                {"parse_error", cpu_list_error_name(parsed.error().code)},
                {"offset", parsed.error().offset},
            },
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    }};
}

} // namespace epycbench::platform_linux
