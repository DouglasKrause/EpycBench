#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace epycbench::core {

enum class ErrorCategory {
    core,
    build,
    platform_linux,
    topology,
    scenario,
    runtime,
    memory,
    kernel,
    measurement,
    monitoring,
    statistics,
    results,
    reporting,
    validation,
    usage,
};

enum class ErrorSeverity {
    information,
    warning,
    error,
    fatal,
};

enum class ContinuationPolicy {
    continue_operation,
    skip_item,
    invalidate_observation,
    abort_experiment,
    abort_command,
};

class StableErrorCode {
  public:
    StableErrorCode() = default;
    explicit StableErrorCode(std::string value) : value_{std::move(value)} {}

    [[nodiscard]] auto value() const noexcept -> const std::string& { return value_; }

    auto operator<=>(const StableErrorCode&) const noexcept = default;

  private:
    std::string value_;
};

struct OsError {
    int value{};
    std::string domain;
    std::string message;

    [[nodiscard]] static auto capture(const std::error_code& error) -> OsError {
        return OsError{
            .value = error.value(),
            .domain = error.category().name(),
            .message = error.message(),
        };
    }

    auto operator<=>(const OsError&) const noexcept = default;
};

struct Error {
    ErrorCategory category{ErrorCategory::core};
    StableErrorCode code;
    std::string message;
    std::string operation;
    std::optional<OsError> os_error;
    nlohmann::json context = nlohmann::json::object();
    ErrorSeverity severity{ErrorSeverity::error};
    ContinuationPolicy continuation{ContinuationPolicy::abort_command};

    auto operator==(const Error&) const noexcept -> bool = default;
};

namespace detail {

template <typename Enum, std::size_t Size>
[[nodiscard]] constexpr auto
enum_name(Enum value, const std::array<std::pair<Enum, std::string_view>, Size>& names)
    -> std::string_view {
    for (const auto& [candidate, name] : names) {
        if (candidate == value) {
            return name;
        }
    }
    throw std::invalid_argument{"Unknown structured-error enum value"};
}

template <typename Enum, std::size_t Size>
[[nodiscard]] auto parse_enum(std::string_view value,
                              const std::array<std::pair<Enum, std::string_view>, Size>& names)
    -> Enum {
    for (const auto& [candidate, name] : names) {
        if (name == value) {
            return candidate;
        }
    }
    throw std::invalid_argument{"Unknown structured-error enum name: " + std::string{value}};
}

inline constexpr std::array error_category_names{
    std::pair{ErrorCategory::core, std::string_view{"core"}},
    std::pair{ErrorCategory::build, std::string_view{"build"}},
    std::pair{ErrorCategory::platform_linux, std::string_view{"platform_linux"}},
    std::pair{ErrorCategory::topology, std::string_view{"topology"}},
    std::pair{ErrorCategory::scenario, std::string_view{"scenario"}},
    std::pair{ErrorCategory::runtime, std::string_view{"runtime"}},
    std::pair{ErrorCategory::memory, std::string_view{"memory"}},
    std::pair{ErrorCategory::kernel, std::string_view{"kernel"}},
    std::pair{ErrorCategory::measurement, std::string_view{"measurement"}},
    std::pair{ErrorCategory::monitoring, std::string_view{"monitoring"}},
    std::pair{ErrorCategory::statistics, std::string_view{"statistics"}},
    std::pair{ErrorCategory::results, std::string_view{"results"}},
    std::pair{ErrorCategory::reporting, std::string_view{"reporting"}},
    std::pair{ErrorCategory::validation, std::string_view{"validation"}},
    std::pair{ErrorCategory::usage, std::string_view{"usage"}},
};

inline constexpr std::array error_severity_names{
    std::pair{ErrorSeverity::information, std::string_view{"information"}},
    std::pair{ErrorSeverity::warning, std::string_view{"warning"}},
    std::pair{ErrorSeverity::error, std::string_view{"error"}},
    std::pair{ErrorSeverity::fatal, std::string_view{"fatal"}},
};

inline constexpr std::array continuation_policy_names{
    std::pair{ContinuationPolicy::continue_operation, std::string_view{"continue_operation"}},
    std::pair{ContinuationPolicy::skip_item, std::string_view{"skip_item"}},
    std::pair{ContinuationPolicy::invalidate_observation,
              std::string_view{"invalidate_observation"}},
    std::pair{ContinuationPolicy::abort_experiment, std::string_view{"abort_experiment"}},
    std::pair{ContinuationPolicy::abort_command, std::string_view{"abort_command"}},
};

} // namespace detail

inline void to_json(nlohmann::json& json, ErrorCategory category) {
    json = detail::enum_name(category, detail::error_category_names);
}

inline void from_json(const nlohmann::json& json, ErrorCategory& category) {
    category = detail::parse_enum(json.get<std::string_view>(), detail::error_category_names);
}

inline void to_json(nlohmann::json& json, ErrorSeverity severity) {
    json = detail::enum_name(severity, detail::error_severity_names);
}

inline void from_json(const nlohmann::json& json, ErrorSeverity& severity) {
    severity = detail::parse_enum(json.get<std::string_view>(), detail::error_severity_names);
}

inline void to_json(nlohmann::json& json, ContinuationPolicy continuation) {
    json = detail::enum_name(continuation, detail::continuation_policy_names);
}

inline void from_json(const nlohmann::json& json, ContinuationPolicy& continuation) {
    continuation =
        detail::parse_enum(json.get<std::string_view>(), detail::continuation_policy_names);
}

inline void to_json(nlohmann::json& json, const StableErrorCode& code) { json = code.value(); }

inline void from_json(const nlohmann::json& json, StableErrorCode& code) {
    code = StableErrorCode{json.get<std::string>()};
}

inline void to_json(nlohmann::json& json, const OsError& error) {
    json = nlohmann::json{
        {"value", error.value},
        {"domain", error.domain},
        {"message", error.message},
    };
}

inline void from_json(const nlohmann::json& json, OsError& error) {
    json.at("value").get_to(error.value);
    json.at("domain").get_to(error.domain);
    json.at("message").get_to(error.message);
}

inline void to_json(nlohmann::json& json, const Error& error) {
    json = nlohmann::json{
        {"category", error.category}, {"code", error.code},
        {"message", error.message},   {"operation", error.operation},
        {"os_error", error.os_error}, {"context", error.context},
        {"severity", error.severity}, {"continuation", error.continuation},
    };
}

inline void from_json(const nlohmann::json& json, Error& error) {
    json.at("category").get_to(error.category);
    json.at("code").get_to(error.code);
    json.at("message").get_to(error.message);
    json.at("operation").get_to(error.operation);
    const auto& os_error = json.at("os_error");
    if (os_error.is_null()) {
        error.os_error.reset();
    } else {
        error.os_error = os_error.get<OsError>();
    }
    json.at("context").get_to(error.context);
    json.at("severity").get_to(error.severity);
    json.at("continuation").get_to(error.continuation);
}

} // namespace epycbench::core
