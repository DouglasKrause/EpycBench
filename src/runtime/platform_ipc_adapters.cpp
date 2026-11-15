#include "runtime/platform_ipc_adapters.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto classification_of(const core::Error& error) -> std::string_view {
    if (!error.context.is_object()) {
        return {};
    }
    const auto item = error.context.find("classification");
    if (item == error.context.end() || !item->is_string()) {
        return {};
    }
    return item->get_ref<const std::string&>();
}

[[nodiscard]] auto projected_classification(const core::Error& error, bool writing)
    -> std::string_view {
    const auto code = error.code.value();
    const auto classification = classification_of(error);
    if (code == "linux.ipc.invalid_state") {
        return "endpoint_closed";
    }
    const auto expected_code = writing ? std::string_view{"linux.ipc.write_failed"}
                                       : std::string_view{"linux.ipc.read_failed"};
    if (code != expected_code) {
        return classification == "unexpected_native_result" ? "unexpected_native_result"
                                                              : "native_failure";
    }
    if (classification == "interrupted" || classification == "would_block" ||
        classification == "connection_reset" || classification == "native_failure" ||
        classification == "unexpected_native_result" ||
        (writing && classification == "broken_pipe")) {
        return classification;
    }
    return "native_failure";
}

[[nodiscard]] auto io_error(const core::Error& platform_error, std::string_view operation,
                            std::string_view channel, bool writing) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"runtime.ipc.io_failure"},
        .message = "Runner IPC endpoint operation failed",
        .operation = std::string{operation},
        .os_error = platform_error.os_error,
        .context = nlohmann::json{
            {"classification", projected_classification(platform_error, writing)},
            {"channel", channel},
            {"underlying_code", platform_error.code.value()},
        },
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto contained_io_error(std::string_view operation, std::string_view channel,
                                      bool unknown) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"runtime.ipc.io_failure"},
        .message = "Runner IPC adapter contained a failure",
        .operation = std::string{operation},
        .os_error = std::nullopt,
        .context = nlohmann::json{
            {"classification", "native_failure"},
            {"channel", channel},
            {"underlying_code", unknown ? "runtime.runner.contained_unknown_failure"
                                         : "runtime.runner.contained_exception"},
        },
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

} // namespace

EndpointByteReader::EndpointByteReader(platform_linux::LocalIpcEndpoint& endpoint) noexcept
    : endpoint_{&endpoint} {}

auto EndpointByteReader::read(std::span<std::byte> buffer)
    -> std::expected<std::size_t, core::Error> {
    try {
        auto result = endpoint_->receive(buffer);
        if (!result) {
            return std::unexpected{io_error(result.error(), "EndpointByteReader.read", "control",
                                            false)};
        }
        return *result;
    } catch (const std::exception&) {
        return std::unexpected{
            contained_io_error("EndpointByteReader.read", "control", false)};
    } catch (...) {
        return std::unexpected{
            contained_io_error("EndpointByteReader.read", "control", true)};
    }
}

EndpointByteWriter::EndpointByteWriter(platform_linux::LocalIpcEndpoint& endpoint) noexcept
    : endpoint_{&endpoint} {}

auto EndpointByteWriter::write(std::span<const std::byte> buffer)
    -> std::expected<std::size_t, core::Error> {
    try {
        auto result = endpoint_->send(buffer);
        if (!result) {
            return std::unexpected{io_error(result.error(), "EndpointByteWriter.write", "event",
                                            true)};
        }
        return *result;
    } catch (const std::exception&) {
        return std::unexpected{
            contained_io_error("EndpointByteWriter.write", "event", false)};
    } catch (...) {
        return std::unexpected{
            contained_io_error("EndpointByteWriter.write", "event", true)};
    }
}

} // namespace epycbench::runtime::detail
