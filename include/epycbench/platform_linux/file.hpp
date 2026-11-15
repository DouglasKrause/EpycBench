#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id_set.hpp"

#include <charconv>
#include <concepts>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace epycbench::platform_linux {

[[nodiscard]] auto read_text_file(const std::filesystem::path& path)
    -> std::expected<std::string, core::Error>;

[[nodiscard]] auto read_optional_text_file(const std::filesystem::path& path)
    -> std::expected<std::optional<std::string>, core::Error>;

namespace detail {

[[nodiscard]] constexpr auto is_ascii_space(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] constexpr auto trim_ascii_space(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] inline auto integer_parse_error(std::string_view input, std::string_view operation,
                                              bool out_of_range) -> core::Error {
    auto context = nlohmann::json::object();
    // This producer owns only a flat object whose sole child is string or
    // insertion's partial null. Retire that child before JSON's stack-allocating
    // structured destructor, including failed code/message construction.
    struct FlatInputCleanup {
        nlohmann::json& value;
        ~FlatInputCleanup() noexcept { value.clear(); }
    } cleanup{context};
    context["input"] = input;
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{out_of_range ? "linux.integer.out_of_range"
                                                   : "linux.integer.malformed"},
        .message = out_of_range ? "Linux integer value is out of range"
                                : "Linux integer data is malformed",
        .operation = std::string{operation},
        .os_error = std::nullopt,
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

} // namespace detail

template <std::integral Integer>
    requires(!std::same_as<Integer, bool>)
[[nodiscard]] auto parse_integer(std::string_view text,
                                 std::string_view operation = "parse_integer")
    -> std::expected<Integer, core::Error> {
    const auto trimmed = detail::trim_ascii_space(text);
    if (trimmed.empty()) {
        return std::unexpected{detail::integer_parse_error(text, operation, false)};
    }

    Integer value{};
    const auto* begin = trimmed.data();
    const auto* end = begin + trimmed.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec == std::errc::result_out_of_range) {
        return std::unexpected{detail::integer_parse_error(text, operation, true)};
    }
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::unexpected{detail::integer_parse_error(text, operation, false)};
    }
    return value;
}

[[nodiscard]] auto parse_cpu_list(std::string_view text)
    -> std::expected<core::CpuSet, core::Error>;

} // namespace epycbench::platform_linux
