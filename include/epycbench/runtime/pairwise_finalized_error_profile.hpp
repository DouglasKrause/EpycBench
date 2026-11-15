#pragma once

#include "epycbench/runtime/pairwise_evidence_failure.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace epycbench::runtime {

struct PairwiseFinalizedErrorProfile final {
    PairwiseFailureCode code;
    PairwiseFailureSubsystem subsystem;
    std::string_view stable_code;
    PairwiseRendererId renderer;
    std::string_view renderer_token;
    core::ErrorCategory category;
    core::ErrorSeverity severity;
    core::ContinuationPolicy continuation;
};

struct PairwiseFinalizedErrorTuple final {
    PairwiseFailureCode code;
    PairwiseFailureOperation operation;
    PairwiseFailureReason reason;
    PairwiseContextConstraint context;
    PairwiseNativeRule native_rule;
};

inline constexpr std::size_t pairwise_failure_code_count =
    static_cast<std::size_t>(PairwiseFailureCode::count);

inline constexpr std::size_t pairwise_failure_tuple_count = 0
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...) + 1
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    ;

[[nodiscard]] auto pairwise_finalized_error_profiles() noexcept
    -> std::span<const PairwiseFinalizedErrorProfile, pairwise_failure_code_count>;
[[nodiscard]] auto pairwise_finalized_error_tuples() noexcept
    -> std::span<const PairwiseFinalizedErrorTuple, pairwise_failure_tuple_count>;

[[nodiscard]] constexpr auto pairwise_failure_subsystem(PairwiseFailureCode code) noexcept
    -> PairwiseFailureSubsystem {
    switch (code) {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, token, renderer)                                            \
    case PairwiseFailureCode::code: return PairwiseFailureSubsystem::subsystem;
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    case PairwiseFailureCode::count: break;
    }
    return PairwiseFailureSubsystem::count;
}
[[nodiscard]] auto pairwise_context_for_constraint(
    PairwiseContextConstraint constraint,
    PairwiseFailureReason reason = PairwiseFailureReason::none) noexcept
    -> PairwiseFailureContext;
[[nodiscard]] auto pairwise_context_satisfies_constraint(
    PairwiseContextConstraint constraint, PairwiseFailureReason reason,
    const PairwiseFailureContext& context) noexcept -> bool;
[[nodiscard]] auto pairwise_operation_token(PairwiseFailureOperation operation) noexcept
    -> std::string_view;
[[nodiscard]] auto pairwise_reason_token(PairwiseFailureReason reason) noexcept
    -> std::string_view;
[[nodiscard]] auto pairwise_context_token(PairwiseContextKind kind) noexcept -> std::string_view;

inline constexpr std::size_t pairwise_stable_code_max_bytes = [] consteval {
    std::size_t result{};
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, token, renderer) result = std::max(result, std::string_view{token}.size());
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    return result;
}();
inline constexpr std::size_t pairwise_operation_max_bytes = [] consteval {
    std::size_t result{};
#define OPERATION(name, token) result = std::max(result, std::string_view{token}.size());
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    return result;
}();
inline constexpr std::size_t pairwise_reason_max_bytes = [] consteval {
    std::size_t result{};
#define OPERATION(...)
#define REASON(name, token) result = std::max(result, std::string_view{token}.size());
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    return result;
}();

inline constexpr std::size_t pairwise_os_domain_max_bytes = [] consteval {
    std::size_t result{};
#define OS_DOMAIN(name, token) result = std::max(result, std::string_view{token}.size());
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef OS_DOMAIN
    return result;
}();
inline constexpr std::size_t pairwise_context_discriminator_max_bytes = [] consteval {
    std::size_t result{};
#define CONTEXT_KIND(name, token, fields) result = std::max(result, std::string_view{token}.size());
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef CONTEXT_KIND
    return result;
}();
inline constexpr std::size_t pairwise_context_max_fields = [] consteval {
    std::size_t result{};
#define CONTEXT_KIND(name, token, fields) result = std::max(result, std::size_t{fields});
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef CONTEXT_KIND
    return result;
}();
inline constexpr std::size_t pairwise_context_max_depth = [] consteval {
    constexpr std::array edges{
#define NESTING_EDGE(outer, lower)                                                   \
        std::pair{PairwiseContextConstraint::outer, PairwiseContextConstraint::lower},
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef NESTING_EDGE
    };
    constexpr auto count = static_cast<std::size_t>(PairwiseContextConstraint::count);
    std::array<std::size_t, count> depths{};
    depths.fill(1U);
    for (std::size_t iteration = 0; iteration < count; ++iteration)
        for (const auto& [outer, lower] : edges)
            depths[static_cast<std::size_t>(outer)] = std::max(
                depths[static_cast<std::size_t>(outer)],
                depths[static_cast<std::size_t>(lower)] + 1U);
    return *std::max_element(depths.begin(), depths.end());
}();

static_assert(pairwise_stable_code_max_bytes == 74);
static_assert(pairwise_operation_max_bytes == 52);
static_assert(pairwise_reason_max_bytes == 40);
static_assert(pairwise_os_domain_max_bytes == 11);
static_assert(pairwise_context_discriminator_max_bytes == 27);
static_assert(pairwise_context_max_fields == 4);
static_assert(pairwise_context_max_depth == 3);

} // namespace epycbench::runtime
