#include "epycbench/runtime/pairwise_finalized_error_profile.hpp"

#include <array>
#include <cctype>
#include <type_traits>

namespace epycbench::runtime {
namespace {

constexpr auto category_for(PairwiseFailureSubsystem subsystem) noexcept -> core::ErrorCategory {
    switch (subsystem) {
    case PairwiseFailureSubsystem::platform: return core::ErrorCategory::platform_linux;
    case PairwiseFailureSubsystem::memory: return core::ErrorCategory::memory;
    case PairwiseFailureSubsystem::monitoring: return core::ErrorCategory::monitoring;
    case PairwiseFailureSubsystem::kernel: return core::ErrorCategory::kernel;
    case PairwiseFailureSubsystem::measurement: return core::ErrorCategory::measurement;
    case PairwiseFailureSubsystem::statistics: return core::ErrorCategory::statistics;
    case PairwiseFailureSubsystem::worker:
    case PairwiseFailureSubsystem::runtime:
    case PairwiseFailureSubsystem::count: return core::ErrorCategory::runtime;
    }
    return core::ErrorCategory::runtime;
}

constexpr auto continuation_for(PairwiseFailureCode code,
                                PairwiseFailureSubsystem subsystem) noexcept
    -> core::ContinuationPolicy {
    if (subsystem == PairwiseFailureSubsystem::platform ||
        subsystem == PairwiseFailureSubsystem::statistics ||
        code == PairwiseFailureCode::kernel_contract_invalid)
        return core::ContinuationPolicy::abort_command;
    return core::ContinuationPolicy::abort_experiment;
}

constexpr std::array profiles{
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, token, renderer)                                           \
    PairwiseFinalizedErrorProfile{PairwiseFailureCode::code,                                \
                                  PairwiseFailureSubsystem::subsystem, token,                \
                                  PairwiseRendererId::renderer, #renderer,                   \
                                  category_for(PairwiseFailureSubsystem::subsystem),         \
                                  core::ErrorSeverity::error,                                \
                                  continuation_for(PairwiseFailureCode::code,                \
                                                   PairwiseFailureSubsystem::subsystem)},
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
};

constexpr std::array tuples{
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(code, operation, reason, context, native_rule)                                \
    PairwiseFinalizedErrorTuple{PairwiseFailureCode::code,                                  \
                                PairwiseFailureOperation::operation,                         \
                                PairwiseFailureReason::reason,                               \
                                PairwiseContextConstraint::context,                          \
                                PairwiseNativeRule::native_rule},
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
};

struct PairwiseNestedLowerTuple final {
    PairwiseContextConstraint wrapper;
    PairwiseFinalizedErrorTuple lower;
};

constexpr std::array lower_tuples{
#define LOWER_TUPLE(wrapper, code, operation, reason, context, native_rule)             \
    PairwiseNestedLowerTuple{PairwiseContextConstraint::wrapper,                        \
        {PairwiseFailureCode::code, PairwiseFailureOperation::operation,                \
         PairwiseFailureReason::reason, PairwiseContextConstraint::context,             \
         PairwiseNativeRule::native_rule}},
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef LOWER_TUPLE
};

constexpr std::array operation_tokens{
#define OPERATION(name, token) std::string_view{token},
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
};
constexpr std::array reason_tokens{
#define OPERATION(...)
#define REASON(name, token) std::string_view{token},
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
};
constexpr std::array context_tokens{
#define CONTEXT_KIND(name, token, fields) std::string_view{token},
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef CONTEXT_KIND
};

constexpr auto tuple_counts_by_code = [] consteval {
    std::array<std::size_t, pairwise_failure_code_count> result{};
    for (const auto& tuple : tuples)
        ++result[static_cast<std::size_t>(tuple.code)];
    return result;
}();

constexpr auto maximum_tuples_per_code = [] consteval {
    std::size_t result{};
    for (const auto count : tuple_counts_by_code) result = std::max(result, count);
    return result;
}();

constexpr auto tuple_indices_by_code = [] consteval {
    std::array<std::array<std::size_t, maximum_tuples_per_code>,
               pairwise_failure_code_count> result{};
    std::array<std::size_t, pairwise_failure_code_count> positions{};
    for (std::size_t index = 0; index < tuples.size(); ++index) {
        const auto code = static_cast<std::size_t>(tuples[index].code);
        result[code][positions[code]++] = index;
    }
    return result;
}();

template <class Enum> constexpr auto valid_enum(Enum value) noexcept -> bool {
    return static_cast<std::size_t>(value) < static_cast<std::size_t>(Enum::count);
}

constexpr auto constraint_kind(PairwiseContextConstraint constraint) noexcept
    -> PairwiseContextKind {
    switch (constraint) {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(name, kind)                                                       \
    case PairwiseContextConstraint::name: return PairwiseContextKind::kind;
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    case PairwiseContextConstraint::count: break;
    }
    return PairwiseContextKind::count;
}

constexpr auto expected_phase(PairwiseContextConstraint constraint) noexcept
    -> std::optional<PairwiseLifecyclePhase> {
    switch (constraint) {
    case PairwiseContextConstraint::phase_preparation: return PairwiseLifecyclePhase::preparation;
    case PairwiseContextConstraint::phase_active: return PairwiseLifecyclePhase::active;
    case PairwiseContextConstraint::phase_finalization: return PairwiseLifecyclePhase::finalization;
    case PairwiseContextConstraint::phase_cleanup: return PairwiseLifecyclePhase::cleanup;
    case PairwiseContextConstraint::phase_create: return PairwiseLifecyclePhase::create;
    case PairwiseContextConstraint::phase_service_exception: return PairwiseLifecyclePhase::service_exception;
    case PairwiseContextConstraint::phase_begin_capture: return PairwiseLifecyclePhase::begin_capture;
    case PairwiseContextConstraint::phase_abort: return PairwiseLifecyclePhase::abort_capture;
    case PairwiseContextConstraint::phase_complete: return PairwiseLifecyclePhase::complete_capture;
    case PairwiseContextConstraint::phase_stop_and_join: return PairwiseLifecyclePhase::stop_and_join;
    case PairwiseContextConstraint::phase_prepare: return PairwiseLifecyclePhase::prepare;
    case PairwiseContextConstraint::phase_page_read: return PairwiseLifecyclePhase::page_read;
    case PairwiseContextConstraint::phase_context_read: return PairwiseLifecyclePhase::context_read;
    case PairwiseContextConstraint::phase_temperature_read: return PairwiseLifecyclePhase::temperature_read;
    case PairwiseContextConstraint::phase_frequency_read: return PairwiseLifecyclePhase::frequency_read;
    case PairwiseContextConstraint::phase_retained_region_release_mapping:
        return PairwiseLifecyclePhase::retained_region_release_mapping;
    case PairwiseContextConstraint::phase_retained_region_release_bound:
        return PairwiseLifecyclePhase::retained_region_release_bound;
    case PairwiseContextConstraint::phase_retained_region_release_initialized:
        return PairwiseLifecyclePhase::retained_region_release_initialized;
    case PairwiseContextConstraint::phase_retained_region_release_placement_observed:
        return PairwiseLifecyclePhase::retained_region_release_placement_observed;
    case PairwiseContextConstraint::phase_worker_launch: return PairwiseLifecyclePhase::worker_launch;
    case PairwiseContextConstraint::phase_worker_startup: return PairwiseLifecyclePhase::worker_startup;
    case PairwiseContextConstraint::phase_worker_callback: return PairwiseLifecyclePhase::worker_callback;
    case PairwiseContextConstraint::phase_worker_command: return PairwiseLifecyclePhase::worker_command;
    case PairwiseContextConstraint::phase_worker_execute: return PairwiseLifecyclePhase::worker_execute;
    case PairwiseContextConstraint::phase_worker_generation: return PairwiseLifecyclePhase::worker_generation;
    case PairwiseContextConstraint::phase_worker_projection: return PairwiseLifecyclePhase::worker_projection;
    case PairwiseContextConstraint::phase_worker_join: return PairwiseLifecyclePhase::worker_join;
    default: return std::nullopt;
    }
}

constexpr auto expected_channel(PairwiseContextConstraint constraint) noexcept
    -> std::optional<PairwiseMonitoringChannel> {
    switch (constraint) {
    case PairwiseContextConstraint::channel_page_faults: return PairwiseMonitoringChannel::page_faults;
    case PairwiseContextConstraint::channel_context_switches: return PairwiseMonitoringChannel::context_switches;
    case PairwiseContextConstraint::channel_temperature: return PairwiseMonitoringChannel::temperature;
    case PairwiseContextConstraint::channel_frequency: return PairwiseMonitoringChannel::frequency;
    default: return std::nullopt;
    }
}

constexpr auto expected_kernel_phase(PairwiseContextConstraint constraint) noexcept
    -> std::optional<PairwiseKernelPhase> {
    switch (constraint) {
    case PairwiseContextConstraint::kernel_bandwidth_pass_count_identity:
        return PairwiseKernelPhase::pass_count_preparation;
    case PairwiseContextConstraint::kernel_bandwidth_accounting_identity:
        return PairwiseKernelPhase::work_accounting;
    case PairwiseContextConstraint::kernel_bandwidth_validation_identity:
        return PairwiseKernelPhase::execution_input_validation;
    case PairwiseContextConstraint::kernel_latency_validation_identity:
        return PairwiseKernelPhase::execution_validation;
    case PairwiseContextConstraint::kernel_latency_schedule_identity:
        return PairwiseKernelPhase::start_state_schedule;
    case PairwiseContextConstraint::kernel_latency_verification_identity:
        return PairwiseKernelPhase::terminal_verification_preparation;
    default: return std::nullopt;
    }
}

constexpr auto expected_missing(PairwiseContextConstraint constraint) noexcept
    -> std::optional<PairwiseMissingOperation> {
    switch (constraint) {
    case PairwiseContextConstraint::missing_now: return PairwiseMissingOperation::now;
    case PairwiseContextConstraint::missing_bind_service_cpu: return PairwiseMissingOperation::bind_service_cpu;
    case PairwiseContextConstraint::missing_support: return PairwiseMissingOperation::support;
    case PairwiseContextConstraint::missing_read: return PairwiseMissingOperation::read;
    case PairwiseContextConstraint::missing_clock_now: return PairwiseMissingOperation::clock_now;
    default: return std::nullopt;
    }
}

auto flat_to_context(const PairwiseFlatFailureContext& context) noexcept
    -> PairwiseFailureContext {
    return std::visit([](const auto& value) -> PairwiseFailureContext { return value; }, context);
}

auto native_matches(PairwiseNativeRule rule, std::optional<int> value) noexcept -> bool {
    if (value && *value <= 0) return false;
    switch (rule) {
    case PairwiseNativeRule::forbidden: return !value;
    case PairwiseNativeRule::optional_posix_errno: return true;
    case PairwiseNativeRule::required_posix_errno: return value.has_value();
    }
    return false;
}

auto context_matches(PairwiseContextConstraint constraint, PairwiseFailureReason reason,
                     const PairwiseFailureContext& context) noexcept -> bool;

auto nested_lower_context(const NestedFailureContext& value) noexcept
    -> std::optional<PairwiseFailureContext> {
    if (value.phase && !valid_enum(*value.phase)) return std::nullopt;
    PairwiseFailureContext lower_context = flat_to_context(value.failure.context);
    if (value.nested_platform_failure) {
        if (!std::holds_alternative<NoPairwiseFailureContext>(value.failure.context))
            return std::nullopt;
        const auto& platform = *value.nested_platform_failure;
        if (!valid_enum(platform.code) ||
            pairwise_failure_subsystem(platform.code) != PairwiseFailureSubsystem::platform ||
            !pairwise_validate_failure(PairwiseFailureSubsystem::platform, platform.code,
                                       platform.operation, platform.reason,
                                       flat_to_context(platform.context), platform.posix_errno))
            return std::nullopt;
        lower_context = NestedFailureContext{platform, std::nullopt, std::nullopt};
    }
    return lower_context;
}

auto nested_failure_values_valid(const NestedFailureContext& value) noexcept -> bool {
    const auto lower_context = nested_lower_context(value);
    if (!lower_context) return false;
    const auto& lower = value.failure;
    return valid_enum(lower.code) &&
           pairwise_validate_failure(pairwise_failure_subsystem(lower.code), lower.code,
                                     lower.operation, lower.reason, *lower_context,
                                     lower.posix_errno);
}

auto lower_relation_matches(PairwiseContextConstraint wrapper,
                            const NestedFailureContext& nested) noexcept -> bool {
    const auto lower_context = nested_lower_context(nested);
    if (!lower_context) return false;
    return std::ranges::any_of(lower_tuples, [&](const auto& relation) {
        const auto& lower = relation.lower;
        return relation.wrapper == wrapper && lower.code == nested.failure.code &&
               lower.operation == nested.failure.operation &&
               lower.reason == nested.failure.reason &&
               native_matches(lower.native_rule, nested.failure.posix_errno) &&
               context_matches(lower.context, nested.failure.reason, *lower_context);
    });
}

auto context_values_valid(const PairwiseFailureContext& context) noexcept -> bool {
    return std::visit([](const auto& value) noexcept -> bool {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NoPairwiseFailureContext> ||
                      std::is_same_v<T, ExpectedObservedCountContext> ||
                      std::is_same_v<T, ExpectedObservedOrdinalContext>) return true;
        else if constexpr (std::is_same_v<T, LifecyclePhaseContext>) return valid_enum(value.phase);
        else if constexpr (std::is_same_v<T, WorkerAssociationContext>) return valid_enum(value.role);
        else if constexpr (std::is_same_v<T, KernelIdentityContext>)
            return valid_enum(value.family) && valid_enum(value.implementation) &&
                   valid_enum(value.phase) &&
                   ((value.family == PairwiseKernelFamily::bandwidth &&
                     value.implementation == PairwiseKernelImplementation::scalar_u64_sequential_v1) ||
                    (value.family == PairwiseKernelFamily::latency &&
                     value.implementation == PairwiseKernelImplementation::scalar_u64_random_cycle_v1));
        else if constexpr (std::is_same_v<T, VerificationPhaseContext>) return valid_enum(value.phase);
        else if constexpr (std::is_same_v<T, MonitoringChannelContext>) return valid_enum(value.channel);
        else if constexpr (std::is_same_v<T, MissingOperationContext>) return valid_enum(value.operation);
        else if constexpr (std::is_same_v<T, MemoryProtectionStateContext>)
            return valid_enum(value.protection) && valid_enum(value.range) &&
                   valid_enum(value.qualification) && valid_enum(value.reason);
        else if constexpr (std::is_same_v<T, RequestedProtectionAdviceContext>)
            return valid_enum(value.protection) && valid_enum(value.advice);
        else if constexpr (std::is_same_v<T, PairwiseMonitoringCapacityAcquisitionContext>)
            return valid_enum(value.leg);
        else if constexpr (std::is_same_v<T, PairwisePerfLossContext>) {
            const auto& record = value.record;
            const auto& ring = value.ring;
            const auto& drain = value.drain;
            if (!valid_enum(record.loss_type) || !valid_enum(drain.role) ||
                record.header_size.value < 8U || record.header_size.value > 65535U ||
                ring.ring_bytes.value == 0U ||
                ring.captured_head.value < ring.captured_tail.value ||
                ring.captured_head.value - ring.captured_tail.value > ring.ring_bytes.value ||
                record.header_size.value > ring.captured_head.value - ring.captured_tail.value)
                return false;
            const auto minimum = record.loss_type == PairwisePerfLossType::lost ? 24U : 16U;
            if (record.lost_count.has_value() != (record.header_size.value >= minimum))
                return false;
            if (drain.role == PairwisePerfDrainRole::unscoped)
                return !drain.ordinal && !drain.started_ns &&
                       !value.last_successful_drain_completed_ns;
            if (!drain.ordinal || !drain.started_ns)
                return false;
            if (drain.role == PairwisePerfDrainRole::initial)
                return drain.ordinal->value == 0U &&
                       !value.last_successful_drain_completed_ns;
            return drain.ordinal->value != 0U &&
                   (!value.last_successful_drain_completed_ns ||
                    value.last_successful_drain_completed_ns->value <= drain.started_ns->value);
        }
        else if constexpr (std::is_same_v<T, NestedFailureContext>)
            return nested_failure_values_valid(value);
        return false;
    }, context);
}

auto context_matches(PairwiseContextConstraint constraint, PairwiseFailureReason reason,
                     const PairwiseFailureContext& context) noexcept -> bool {
    if (!valid_enum(constraint) || constraint_kind(constraint) != context_kind(context) ||
        !context_values_valid(context)) return false;
    if (const auto phase = expected_phase(constraint))
        return std::get<LifecyclePhaseContext>(context).phase == *phase;
    if (const auto channel = expected_channel(constraint))
        return std::get<MonitoringChannelContext>(context).channel == *channel;
    if (const auto missing = expected_missing(constraint))
        return std::get<MissingOperationContext>(context).operation == *missing;
    if (constraint_kind(constraint) == PairwiseContextKind::memory_protection_state &&
        std::get<MemoryProtectionStateContext>(context).reason != reason)
        return false;
    if (constraint == PairwiseContextConstraint::kernel_bandwidth_identity) {
        const auto& identity = std::get<KernelIdentityContext>(context);
        return identity.family == PairwiseKernelFamily::bandwidth;
    }
    if (constraint == PairwiseContextConstraint::kernel_latency_identity) {
        const auto& identity = std::get<KernelIdentityContext>(context);
        return identity.family == PairwiseKernelFamily::latency;
    }
    if (constraint == PairwiseContextConstraint::mapping_protection) {
        const auto& requested = std::get<RequestedProtectionAdviceContext>(context);
        return requested.protection != PairwiseMappingProtection::unspecified &&
               requested.advice == PairwiseMappingAdvice::none;
    }
    if (constraint == PairwiseContextConstraint::mapping_advice) {
        const auto& requested = std::get<RequestedProtectionAdviceContext>(context);
        return requested.protection == PairwiseMappingProtection::unspecified &&
               requested.advice == PairwiseMappingAdvice::no_huge_pages;
    }
    if (constraint == PairwiseContextConstraint::capacity_acquisition) {
        const auto& acquisition =
            std::get<PairwiseMonitoringCapacityAcquisitionContext>(context);
        return acquisition.uncertain == (reason == PairwiseFailureReason::close);
    }
    if (const auto phase = expected_kernel_phase(constraint)) {
        const auto& identity = std::get<KernelIdentityContext>(context);
        const auto expected_family =
            constraint == PairwiseContextConstraint::kernel_bandwidth_pass_count_identity ||
                    constraint == PairwiseContextConstraint::kernel_bandwidth_accounting_identity ||
                    constraint == PairwiseContextConstraint::kernel_bandwidth_validation_identity
                ? PairwiseKernelFamily::bandwidth
                : PairwiseKernelFamily::latency;
        return identity.family == expected_family && identity.phase == *phase;
    }
    if (constraint_kind(constraint) == PairwiseContextKind::nested_failure) {
        const auto& nested = std::get<NestedFailureContext>(context);
        const auto direct_platform = !nested.phase && !nested.nested_platform_failure &&
            pairwise_failure_subsystem(nested.failure.code) ==
                PairwiseFailureSubsystem::platform;
        if (constraint == PairwiseContextConstraint::nested_platform_numa_bind_einval) {
            const auto& failure = nested.failure;
            return direct_platform &&
                   failure.code == PairwiseFailureCode::platform_numa_bind_failed &&
                   failure.operation == PairwiseFailureOperation::bind_memory &&
                   failure.reason == PairwiseFailureReason::invalid_bind_request &&
                   failure.posix_errno == 22 && lower_relation_matches(constraint, nested);
        }
        if (constraint <= PairwiseContextConstraint::nested_platform_unmap)
            return direct_platform && lower_relation_matches(constraint, nested);
        if (constraint == PairwiseContextConstraint::nested_memory_placement_query)
            return nested.phase == PairwiseLifecyclePhase::finalization &&
                   lower_relation_matches(constraint, nested);
        if (constraint == PairwiseContextConstraint::nested_memory_placement_evaluation)
            return nested.phase == PairwiseLifecyclePhase::finalization &&
                   lower_relation_matches(constraint, nested);
        if (constraint == PairwiseContextConstraint::nested_worker_operation_failure)
            return nested.phase == PairwiseLifecyclePhase::worker_callback &&
                   lower_relation_matches(constraint, nested);
        return false;
    }
    return true;
}

consteval auto registry_base_well_formed() -> bool {
    if (profiles.size() != pairwise_failure_code_count ||
        tuples.size() != pairwise_failure_tuple_count ||
        operation_tokens.size() != static_cast<std::size_t>(PairwiseFailureOperation::count) ||
        reason_tokens.size() != static_cast<std::size_t>(PairwiseFailureReason::count)) return false;
    std::array<std::size_t, pairwise_failure_code_count> reachable{};
    std::array<std::size_t, static_cast<std::size_t>(PairwiseFailureOperation::count)>
        reachable_operations{};
    std::array<std::size_t, static_cast<std::size_t>(PairwiseFailureReason::count)>
        reachable_reasons{};
    std::array<std::size_t, static_cast<std::size_t>(PairwiseContextConstraint::count)>
        reachable_contexts{};
    std::array<std::size_t, pairwise_failure_tuple_count * 2U + 1U> tuple_slots{};
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const auto& profile = profiles[index];
        if (static_cast<std::size_t>(profile.code) != index ||
            static_cast<std::size_t>(profile.renderer) != index ||
            profile.stable_code.empty() || profile.renderer_token.empty()) return false;
        for (const char byte : profile.stable_code)
            if (!(byte == '.' || byte == '_' || (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9'))) return false;
        for (std::size_t other = index + 1; other < profiles.size(); ++other)
            if (profile.stable_code == profiles[other].stable_code ||
                profile.renderer_token == profiles[other].renderer_token) return false;
    }
    for (std::size_t left = 0; left < tuples.size(); ++left) {
        const auto& tuple = tuples[left];
        const auto code = static_cast<std::size_t>(tuple.code);
        if (code >= profiles.size() || !valid_enum(tuple.operation) ||
            !valid_enum(tuple.reason) || !valid_enum(tuple.context)) return false;
        ++reachable[code];
        ++reachable_operations[static_cast<std::size_t>(tuple.operation)];
        ++reachable_reasons[static_cast<std::size_t>(tuple.reason)];
        ++reachable_contexts[static_cast<std::size_t>(tuple.context)];
        auto slot = (static_cast<std::size_t>(tuple.code) * 131U +
                     static_cast<std::size_t>(tuple.operation) * 31U +
                     static_cast<std::size_t>(tuple.reason) * 17U +
                     static_cast<std::size_t>(tuple.context) * 7U +
                     static_cast<std::size_t>(tuple.native_rule)) % tuple_slots.size();
        while (tuple_slots[slot] != 0U) {
            const auto& prior = tuples[tuple_slots[slot] - 1U];
            if (tuple.code == prior.code && tuple.operation == prior.operation &&
                tuple.reason == prior.reason && tuple.context == prior.context &&
                tuple.native_rule == prior.native_rule) return false;
            slot = (slot + 1U) % tuple_slots.size();
        }
        tuple_slots[slot] = left + 1U;
    }
    for (const auto count : reachable) if (count == 0U) return false;
    for (const auto count : reachable_operations) if (count == 0U) return false;
    for (const auto count : reachable_reasons) if (count == 0U) return false;
    for (const auto count : reachable_contexts) if (count == 0U) return false;
    return true;
}

consteval auto registry_lower_relations_well_formed() -> bool {
    std::array<std::size_t, static_cast<std::size_t>(PairwiseContextConstraint::count)>
        reachable_lower_constraints{};
    std::array<std::size_t, lower_tuples.size() * 2U + 1U> lower_slots{};
    for (std::size_t index = 0; index < lower_tuples.size(); ++index) {
        const auto& relation = lower_tuples[index];
        if (constraint_kind(relation.wrapper) != PairwiseContextKind::nested_failure)
            return false;
        ++reachable_lower_constraints[static_cast<std::size_t>(relation.wrapper)];
        bool registered{};
        const auto code = static_cast<std::size_t>(relation.lower.code);
        for (std::size_t position = 0; position < tuple_counts_by_code[code]; ++position) {
            const auto& tuple = tuples[tuple_indices_by_code[code][position]];
            registered = registered ||
                (tuple.operation == relation.lower.operation &&
                 tuple.reason == relation.lower.reason &&
                 tuple.context == relation.lower.context &&
                 tuple.native_rule == relation.lower.native_rule);
            if (registered) break;
        }
        if (!registered) return false;
        auto slot = (static_cast<std::size_t>(relation.wrapper) * 257U + code * 131U +
                     static_cast<std::size_t>(relation.lower.operation) * 31U +
                     static_cast<std::size_t>(relation.lower.reason) * 17U +
                     static_cast<std::size_t>(relation.lower.context) * 7U +
                     static_cast<std::size_t>(relation.lower.native_rule)) %
                    lower_slots.size();
        while (lower_slots[slot] != 0U) {
            const auto& prior = lower_tuples[lower_slots[slot] - 1U];
            if (relation.wrapper == prior.wrapper &&
                relation.lower.code == prior.lower.code &&
                relation.lower.operation == prior.lower.operation &&
                relation.lower.reason == prior.lower.reason &&
                relation.lower.context == prior.lower.context &&
                relation.lower.native_rule == prior.lower.native_rule)
                return false;
            slot = (slot + 1U) % lower_slots.size();
        }
        lower_slots[slot] = index + 1U;
    }
    for (std::size_t constraint = 0; constraint < reachable_lower_constraints.size();
         ++constraint)
        if (constraint_kind(static_cast<PairwiseContextConstraint>(constraint)) ==
                PairwiseContextKind::nested_failure &&
            reachable_lower_constraints[constraint] == 0U)
            return false;
    return true;
}

static_assert(registry_base_well_formed());
static_assert(registry_lower_relations_well_formed());
static_assert(pairwise_stable_code_max_bytes == 74);
static_assert(pairwise_operation_max_bytes == 52);
static_assert(pairwise_reason_max_bytes == 40);

} // namespace

auto pairwise_finalized_error_profiles() noexcept
    -> std::span<const PairwiseFinalizedErrorProfile, pairwise_failure_code_count> { return profiles; }
auto pairwise_finalized_error_tuples() noexcept
    -> std::span<const PairwiseFinalizedErrorTuple, pairwise_failure_tuple_count> { return tuples; }

auto pairwise_operation_token(PairwiseFailureOperation operation) noexcept -> std::string_view {
    const auto index = static_cast<std::size_t>(operation);
    return index < operation_tokens.size() ? operation_tokens[index] : std::string_view{};
}
auto pairwise_reason_token(PairwiseFailureReason reason) noexcept -> std::string_view {
    const auto index = static_cast<std::size_t>(reason);
    return index < reason_tokens.size() ? reason_tokens[index] : std::string_view{};
}
auto pairwise_context_token(PairwiseContextKind kind) noexcept -> std::string_view {
    const auto index = static_cast<std::size_t>(kind);
    return index < context_tokens.size() ? context_tokens[index] : std::string_view{};
}

auto pairwise_validate_failure(PairwiseFailureSubsystem subsystem, PairwiseFailureCode code,
                               PairwiseFailureOperation operation, PairwiseFailureReason reason,
                               const PairwiseFailureContext& context,
                               std::optional<int> posix_errno) noexcept -> bool {
    if (!valid_enum(subsystem) || !valid_enum(code) || !valid_enum(operation) ||
        !valid_enum(reason) || pairwise_failure_subsystem(code) != subsystem) return false;
    const auto code_index = static_cast<std::size_t>(code);
    for (std::size_t position = 0; position < tuple_counts_by_code[code_index]; ++position) {
        const auto& tuple = tuples[tuple_indices_by_code[code_index][position]];
        if (tuple.code == code && tuple.operation == operation && tuple.reason == reason &&
            native_matches(tuple.native_rule, posix_errno) &&
            context_matches(tuple.context, reason, context))
            return true;
    }
    return false;
}

auto pairwise_context_satisfies_constraint(PairwiseContextConstraint constraint,
                                           PairwiseFailureReason reason,
                                           const PairwiseFailureContext& context) noexcept
    -> bool {
    return context_matches(constraint, reason, context);
}

auto pairwise_context_for_constraint(PairwiseContextConstraint constraint,
                                     PairwiseFailureReason reason) noexcept
    -> PairwiseFailureContext {
    if (const auto phase = expected_phase(constraint)) return LifecyclePhaseContext{*phase};
    if (const auto channel = expected_channel(constraint)) return MonitoringChannelContext{*channel};
    if (const auto missing = expected_missing(constraint)) return MissingOperationContext{*missing};
    switch (constraint_kind(constraint)) {
    case PairwiseContextKind::none: return NoPairwiseFailureContext{};
    case PairwiseContextKind::lifecycle_phase: return LifecyclePhaseContext{PairwiseLifecyclePhase::active};
    case PairwiseContextKind::expected_observed_count: return ExpectedObservedCountContext{{1U}, {2U}};
    case PairwiseContextKind::expected_observed_ordinal: return ExpectedObservedOrdinalContext{{1U}, {2U}};
    case PairwiseContextKind::worker_association: return WorkerAssociationContext{PairwiseRole::first, {0U}};
    case PairwiseContextKind::kernel_identity:
        if (constraint == PairwiseContextConstraint::kernel_latency_identity ||
            constraint == PairwiseContextConstraint::kernel_latency_validation_identity ||
            constraint == PairwiseContextConstraint::kernel_latency_schedule_identity ||
            constraint == PairwiseContextConstraint::kernel_latency_verification_identity)
            return KernelIdentityContext{
                PairwiseKernelFamily::latency,
                PairwiseKernelImplementation::scalar_u64_random_cycle_v1,
                expected_kernel_phase(constraint).value_or(PairwiseKernelPhase::none)};
        return KernelIdentityContext{
            PairwiseKernelFamily::bandwidth,
            PairwiseKernelImplementation::scalar_u64_sequential_v1,
            expected_kernel_phase(constraint).value_or(PairwiseKernelPhase::none)};
    case PairwiseContextKind::verification_phase: return VerificationPhaseContext{PairwiseVerificationPhase::before};
    case PairwiseContextKind::monitoring_channel: return MonitoringChannelContext{PairwiseMonitoringChannel::page_faults};
    case PairwiseContextKind::missing_operation: return MissingOperationContext{PairwiseMissingOperation::now};
    case PairwiseContextKind::memory_protection_state: return MemoryProtectionStateContext{PairwiseProtectionKind::read_only, PairwiseRangeKind::full_mapping, PairwiseQualificationState::qualified, reason};
    case PairwiseContextKind::requested_protection_advice:
        if (constraint == PairwiseContextConstraint::mapping_advice)
            return RequestedProtectionAdviceContext{
                PairwiseMappingProtection::unspecified,
                PairwiseMappingAdvice::no_huge_pages};
        return RequestedProtectionAdviceContext{
            PairwiseMappingProtection::read_write, PairwiseMappingAdvice::none};
    case PairwiseContextKind::capacity_acquisition:
        return PairwiseMonitoringCapacityAcquisitionContext{
            PairwiseMonitoringCapacityAcquisitionLeg::first_controller,
            reason == PairwiseFailureReason::close};
    case PairwiseContextKind::perf_loss:
        return PairwisePerfLossContext{
            {PairwisePerfLossType::lost, {32U}, PairwiseCount{1U}},
            {{32U}, {0U}, {4096U}}};
    case PairwiseContextKind::nested_failure: {
        auto nested_context = PairwiseFlatFailureContext{NoPairwiseFailureContext{}};
        auto nested = PairwiseNestedFailureValue{PairwiseFailureCode::platform_raw_clock_conversion_failed, PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none, nested_context, std::nullopt};
        if (constraint == PairwiseContextConstraint::nested_platform_numa_mask)
            nested = {PairwiseFailureCode::platform_numa_mask_overflow,
                      PairwiseFailureOperation::numa_mask,
                      PairwiseFailureReason::nodemask_overflow,
                      NoPairwiseFailureContext{}, std::nullopt};
        else if (constraint == PairwiseContextConstraint::nested_platform_numa_bind)
            nested = {PairwiseFailureCode::platform_numa_bind_failed,
                      PairwiseFailureOperation::bind_memory,
                      PairwiseFailureReason::invalid_bind_request,
                      NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_numa_bind_einval)
            nested = {PairwiseFailureCode::platform_numa_bind_failed,
                      PairwiseFailureOperation::bind_memory,
                      PairwiseFailureReason::invalid_bind_request,
                      NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_numa_query)
            nested = {PairwiseFailureCode::platform_numa_query_failed,
                      PairwiseFailureOperation::query_page_locations,
                      PairwiseFailureReason::invalid_query, NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_affinity_set)
            nested = {PairwiseFailureCode::platform_affinity_set_failed,
                      PairwiseFailureOperation::set_affinity, PairwiseFailureReason::none,
                      NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_affinity_get)
            nested = {PairwiseFailureCode::platform_affinity_get_failed,
                      PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none,
                      NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_current_cpu)
            nested = {PairwiseFailureCode::platform_current_cpu_query_failed,
                      PairwiseFailureOperation::current_cpu, PairwiseFailureReason::query_failed,
                      NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_thread_identity)
            nested = {PairwiseFailureCode::platform_thread_identity_query_failed,
                      PairwiseFailureOperation::thread_identity,
                      PairwiseFailureReason::query_failed, NoPairwiseFailureContext{}, 22};
        else if (constraint == PairwiseContextConstraint::nested_platform_unmap)
            nested = {PairwiseFailureCode::platform_mapping_unmap_failed,
                      PairwiseFailureOperation::unmap_memory,
                      PairwiseFailureReason::cleanup_failure, NoPairwiseFailureContext{}, 22};
        if (constraint == PairwiseContextConstraint::nested_memory_placement_query)
            return NestedFailureContext{
                {PairwiseFailureCode::memory_placement_query_invalid_argument,
                 PairwiseFailureOperation::memory_region_placement_observe,
                 PairwiseFailureReason::page_alignment_invalid,
                 NoPairwiseFailureContext{}, std::nullopt},
                PairwiseLifecyclePhase::finalization, std::nullopt};
        if (constraint == PairwiseContextConstraint::nested_memory_placement_evaluation)
            return NestedFailureContext{
                {PairwiseFailureCode::memory_placement_evaluation_invalid_argument,
                 PairwiseFailureOperation::memory_region_placement_evaluate,
                 PairwiseFailureReason::denominator_invalid,
                 NoPairwiseFailureContext{}, std::nullopt},
                PairwiseLifecyclePhase::finalization, std::nullopt};
        if (constraint == PairwiseContextConstraint::nested_worker_operation_failure)
            return NestedFailureContext{
            {PairwiseFailureCode::kernel_bandwidth_accounting_overflow,
             PairwiseFailureOperation::kernel_bandwidth_accounting,
             PairwiseFailureReason::logical_byte_count_overflow,
             KernelIdentityContext{
                 PairwiseKernelFamily::bandwidth,
                 PairwiseKernelImplementation::scalar_u64_sequential_v1,
                 PairwiseKernelPhase::work_accounting},
             std::nullopt},
            PairwiseLifecyclePhase::worker_callback, std::nullopt};
        return NestedFailureContext{nested, std::nullopt, std::nullopt};
    }
    case PairwiseContextKind::count: break;
    }
    return NoPairwiseFailureContext{};
}

} // namespace epycbench::runtime
