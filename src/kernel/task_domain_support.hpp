#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace epycbench::kernel::detail {

constexpr auto maximum_sum_u64_elements() noexcept -> std::uint64_t {
    constexpr auto wide_maximum = std::numeric_limits<std::uint64_t>::max() / 8U;
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        return std::numeric_limits<std::size_t>::max() / 8U;
    }
    return wide_maximum;
}

inline auto sum_u64_error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!parameter.empty())
        context["parameter"] = parameter;
    return {.category = core::ErrorCategory::kernel,
            .code = core::StableErrorCode{"kernel.task.sum_u64." + std::string{classification}},
            .message = "Unsigned summation task validation failed",
            .operation = "kernel.task.sum_u64",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

inline auto xor_u64_error(std::string_view classification, std::string_view parameter = {}) -> core::Error {
    auto result=sum_u64_error(classification,parameter);
    result.code=core::StableErrorCode{"kernel.task.xor_u64."+std::string{classification}};
    result.operation="kernel.task.xor_u64";
    result.message="Unsigned XOR transform task validation failed";
    return result;
}

} // namespace epycbench::kernel::detail
