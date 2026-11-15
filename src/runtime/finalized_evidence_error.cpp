#include "epycbench/runtime/finalized_evidence_error.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace epycbench::runtime {
namespace {

[[nodiscard]] auto profile_for(PairwiseFailureCode code) noexcept
    -> const PairwiseFinalizedErrorProfile& {
    return pairwise_finalized_error_profiles()[static_cast<std::size_t>(code)];
}

template <class Integer> void append_decimal(std::string& output, Integer value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
        std::terminate();
    output.append(buffer.data(), result.ptr);
}

#define EPYCBENCH_ENUM_TOKEN_CASE(Enum, name)                                                      \
    case Enum::name:                                                                               \
        return #name

[[nodiscard]] constexpr auto token(PairwiseLifecyclePhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, active);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, finalization);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, cleanup);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, create);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, service_exception);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, begin_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, abort_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, complete_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, stop_and_join);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, prepare);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, page_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, context_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, temperature_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, frequency_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_mapping);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_bound);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_initialized);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase,
                                  retained_region_release_placement_observed);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, execution_input_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, execution_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, start_state_schedule);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, terminal_verification_preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_launch);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_startup);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_callback);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_command);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_execute);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_generation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_projection);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_result_publication);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_join);
    case PairwiseLifecyclePhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseRole value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRole, first);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRole, second);
    case PairwiseRole::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelFamily value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelFamily, bandwidth);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelFamily, latency);
    case PairwiseKernelFamily::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelImplementation value) noexcept
    -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelImplementation, scalar_u64_sequential_v1);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelImplementation, scalar_u64_random_cycle_v1);
    case PairwiseKernelImplementation::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelPhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, none);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, pass_count_preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, work_accounting);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, execution_input_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, execution_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, start_state_schedule);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, terminal_verification_preparation);
    case PairwiseKernelPhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseVerificationPhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseVerificationPhase, before);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseVerificationPhase, after);
    case PairwiseVerificationPhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMonitoringChannel value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, page_faults);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, context_switches);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, temperature);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, frequency);
    case PairwiseMonitoringChannel::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMissingOperation value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, now);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, bind_service_cpu);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, support);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, clock_now);
    case PairwiseMissingOperation::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseProtectionKind value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseProtectionKind, read_only);
    case PairwiseProtectionKind::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseRangeKind value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRangeKind, full_mapping);
    case PairwiseRangeKind::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseQualificationState value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, qualified);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, skipped);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, not_qualified);
    case PairwiseQualificationState::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMappingProtection value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, unspecified);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, read_write);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, read_only);
    case PairwiseMappingProtection::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMappingAdvice value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, none);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, base_pages);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, huge_pages);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, no_huge_pages);
    case PairwiseMappingAdvice::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(
    PairwiseMonitoringCapacityAcquisitionLeg value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringCapacityAcquisitionLeg,
                                  first_controller);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringCapacityAcquisitionLeg,
                                  second_runner);
    case PairwiseMonitoringCapacityAcquisitionLeg::count:
        break;
    }
    std::terminate();
}

#undef EPYCBENCH_ENUM_TOKEN_CASE

[[nodiscard]] constexpr auto token(PairwisePerfLossType value) noexcept -> std::string_view {
    switch (value) {
    case PairwisePerfLossType::lost: return "lost";
    case PairwisePerfLossType::lost_samples: return "lost_samples";
    case PairwisePerfLossType::count: break;
    }
    std::terminate();
}
[[nodiscard]] constexpr auto token(PairwisePerfDrainRole value) noexcept -> std::string_view {
    switch (value) {
    case PairwisePerfDrainRole::unscoped: return "unscoped";
    case PairwisePerfDrainRole::initial: return "initial";
    case PairwisePerfDrainRole::scheduled: return "scheduled";
    case PairwisePerfDrainRole::final: return "final";
    case PairwisePerfDrainRole::count: break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto os_domain_token(PairwiseOsDomain domain) noexcept -> std::string_view {
    switch (domain) {
#define OS_DOMAIN(name, spelling)                                                                  \
    case PairwiseOsDomain::name:                                                                   \
        return spelling;
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef OS_DOMAIN
    case PairwiseOsDomain::count:
        break;
    }
    std::terminate();
}

void append_flat_context(std::string& output, const PairwiseFlatFailureContext& context);

void append_os_error(std::string& output, std::optional<int> posix_errno) {
    output.append("os_error=");
    if (!posix_errno) {
        output.append("none");
        return;
    }
    output.append(os_domain_token(PairwiseOsDomain::posix_errno));
    output.push_back(':');
    append_decimal(output, *posix_errno);
}

void append_nested_value(std::string& output, const PairwiseNestedFailureValue& failure) {
    output.append("code=");
    output.append(profile_for(failure.code).stable_code);
    output.append(", operation=");
    output.append(pairwise_operation_token(failure.operation));
    output.append(", reason=");
    output.append(pairwise_reason_token(failure.reason));
    output.append(", context=");
    append_flat_context(output, failure.context);
    output.append(", ");
    append_os_error(output, failure.posix_errno);
}

template <class Context> void append_context_fields(std::string& output, const Context& value) {
    if constexpr (std::is_same_v<Context, NoPairwiseFailureContext>) {
        (void)output;
        (void)value;
    } else if constexpr (std::is_same_v<Context, LifecyclePhaseContext>) {
        output.append("phase=");
        output.append(token(value.phase));
    } else if constexpr (std::is_same_v<Context, ExpectedObservedCountContext>) {
        output.append("expected=");
        append_decimal(output, value.expected.value);
        output.append(", observed=");
        append_decimal(output, value.observed.value);
    } else if constexpr (std::is_same_v<Context, ExpectedObservedOrdinalContext>) {
        output.append("expected=");
        append_decimal(output, value.expected.value);
        output.append(", observed=");
        append_decimal(output, value.observed.value);
    } else if constexpr (std::is_same_v<Context, WorkerAssociationContext>) {
        output.append("role=");
        output.append(token(value.role));
        output.append(", worker_ordinal=");
        append_decimal(output, value.worker_ordinal.value);
    } else if constexpr (std::is_same_v<Context, KernelIdentityContext>) {
        output.append("family=");
        output.append(token(value.family));
        output.append(", implementation=");
        output.append(token(value.implementation));
        output.append(", phase=");
        output.append(token(value.phase));
    } else if constexpr (std::is_same_v<Context, VerificationPhaseContext>) {
        output.append("phase=");
        output.append(token(value.phase));
    } else if constexpr (std::is_same_v<Context, MonitoringChannelContext>) {
        output.append("channel=");
        output.append(token(value.channel));
    } else if constexpr (std::is_same_v<Context, MissingOperationContext>) {
        output.append("operation=");
        output.append(token(value.operation));
    } else if constexpr (std::is_same_v<Context, MemoryProtectionStateContext>) {
        output.append("protection=");
        output.append(token(value.protection));
        output.append(", range=");
        output.append(token(value.range));
        output.append(", qualification=");
        output.append(token(value.qualification));
        output.append(", reason=");
        output.append(pairwise_reason_token(value.reason));
    } else if constexpr (std::is_same_v<Context, RequestedProtectionAdviceContext>) {
        output.append("protection=");
        output.append(token(value.protection));
        output.append(", advice=");
        output.append(token(value.advice));
    } else if constexpr (
        std::is_same_v<Context, PairwiseMonitoringCapacityAcquisitionContext>) {
        output.append("leg=");
        output.append(token(value.leg));
        output.append(", uncertain=");
        output.append(value.uncertain ? "true" : "false");
    } else if constexpr (std::is_same_v<Context, PairwisePerfLossContext>) {
        const auto optional_count = [&](const auto& count) {
            if (count) append_decimal(output, count->value);
            else output.append("unavailable");
        };
        output.append("record={loss_type="); output.append(token(value.record.loss_type));
        output.append(", header_size="); append_decimal(output, value.record.header_size.value);
        output.append(", lost_count="); optional_count(value.record.lost_count);
        output.append("}, ring={captured_head="); append_decimal(output, value.ring.captured_head.value);
        output.append(", captured_tail="); append_decimal(output, value.ring.captured_tail.value);
        output.append(", ring_bytes="); append_decimal(output, value.ring.ring_bytes.value);
        output.append("}, drain={role="); output.append(token(value.drain.role));
        output.append(", ordinal="); optional_count(value.drain.ordinal);
        output.append(", started_ns="); optional_count(value.drain.started_ns);
        output.append("}, last_successful_drain_completed_ns=");
        optional_count(value.last_successful_drain_completed_ns);
    }
}

void append_flat_context(std::string& output, const PairwiseFlatFailureContext& context) {
    output.append(pairwise_context_token(context_kind(context)));
    std::visit(
        [&](const auto& value) {
            using Context = std::remove_cvref_t<decltype(value)>;
            if constexpr (!std::is_same_v<Context, NoPairwiseFailureContext>) {
                output.push_back('{');
                append_context_fields(output, value);
                output.push_back('}');
            }
        },
        context);
}

void append_context(std::string& output, const PairwiseFailureContext& context) {
    output.append(pairwise_context_token(context_kind(context)));
    std::visit(
        [&](const auto& value) {
            using Context = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Context, NestedFailureContext>) {
                output.append("{failure={");
                append_nested_value(output, value.failure);
                output.append("}, phase=");
                if (value.phase)
                    output.append(token(*value.phase));
                else
                    output.append("none");
                output.append(", nested_platform_failure=");
                if (value.nested_platform_failure) {
                    output.append("{");
                    append_nested_value(output, *value.nested_platform_failure);
                    output.append("}");
                } else {
                    output.append("none");
                }
                output.push_back('}');
            } else if constexpr (!std::is_same_v<Context, NoPairwiseFailureContext>) {
                output.push_back('{');
                append_context_fields(output, value);
                output.push_back('}');
            }
        },
        context);
}

} // namespace

auto FinalizedEvidenceError::from_failure(PairwiseEvidenceFailure failure) noexcept
    -> FinalizedEvidenceError {
    return std::visit(
        [](const auto& value) -> FinalizedEvidenceError {
            const auto os_error = [&] -> std::optional<FinalizedEvidenceOsError> {
                if (const auto native = value.posix_errno())
                    return FinalizedEvidenceOsError{PairwiseOsDomain::posix_errno, *native};
                return std::nullopt;
            }();
            return FinalizedEvidenceError{value.code(), value.operation(), value.reason(),
                                          value.context(), os_error};
        },
        failure);
}

auto FinalizedEvidenceError::subsystem() const noexcept -> PairwiseFailureSubsystem {
    return profile_for(code_).subsystem;
}

auto FinalizedEvidenceError::category() const noexcept -> core::ErrorCategory {
    return profile_for(code_).category;
}

auto FinalizedEvidenceError::stable_code() const noexcept -> std::string_view {
    return profile_for(code_).stable_code;
}

auto FinalizedEvidenceError::operation_token() const noexcept -> std::string_view {
    return pairwise_operation_token(operation_);
}

auto FinalizedEvidenceError::reason_token() const noexcept -> std::string_view {
    return pairwise_reason_token(reason_);
}

auto FinalizedEvidenceError::severity() const noexcept -> core::ErrorSeverity {
    return profile_for(code_).severity;
}

auto FinalizedEvidenceError::continuation() const noexcept -> core::ContinuationPolicy {
    return profile_for(code_).continuation;
}

auto FinalizedEvidenceError::renderer() const noexcept -> PairwiseRendererId {
    return profile_for(code_).renderer;
}

auto render_finalized_evidence_error(const FinalizedEvidenceError& error) -> std::string {
    const auto& profile = profile_for(error.code());
    std::string output;
    output.append("Finalized pairwise evidence failure [");
    output.append(profile.renderer_token);
    output.append("]: code=");
    output.append(error.stable_code());
    output.append(", operation=");
    output.append(error.operation_token());
    output.append(", reason=");
    output.append(error.reason_token());
    output.append(", context=");
    append_context(output, error.context());
    output.append(", os_error=");
    if (const auto& native = error.os_error()) {
        output.append(os_domain_token(native->domain()));
        output.push_back(':');
        append_decimal(output, native->value());
    } else {
        output.append("none");
    }
    return output;
}

static_assert(std::is_nothrow_copy_constructible_v<PairwiseFailureContext>);
static_assert(std::is_nothrow_copy_constructible_v<FinalizedEvidenceOsError>);
static_assert(std::is_nothrow_copy_constructible_v<FinalizedEvidenceError>);

} // namespace epycbench::runtime
