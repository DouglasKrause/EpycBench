#pragma once

#include "epycbench/core/error.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <variant>

namespace epycbench::runtime {

enum class PairwiseFailureSubsystem : std::uint8_t {
    platform, memory, monitoring, kernel, measurement, statistics, worker, runtime, count,
};

enum class PairwiseFailureCode : std::uint16_t {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, token, renderer) code,
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    count,
};

enum class PairwiseFailureOperation : std::uint8_t {
#define OPERATION(name, token) name,
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
    count,
};

enum class PairwiseFailureReason : std::uint16_t {
#define OPERATION(...)
#define REASON(name, token) name,
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    count,
};

enum class PairwiseContextKind : std::uint8_t {
#define CONTEXT_KIND(name, token, fields) name,
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef CONTEXT_KIND
    count,
};

enum class PairwiseContextConstraint : std::uint8_t {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(name, kind) name,
#define FAILURE(...)
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    count,
};

enum class PairwiseRendererId : std::uint16_t {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define FAILURE(code, subsystem, token, renderer) renderer,
#define TUPLE(...)
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef TUPLE
#undef FAILURE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    count,
};

enum class PairwiseNativeRule : std::uint8_t { forbidden, optional_posix_errno, required_posix_errno };
enum class PairwiseOsDomain : std::uint8_t {
#define OS_DOMAIN(name, token) name,
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef OS_DOMAIN
    count,
};

// Detailed phases and cleanup/source states retain the frozen one-field
// lifecycle_phase representation.
enum class PairwiseLifecyclePhase : std::uint8_t {
    preparation, active, finalization, cleanup, create, service_exception,
    begin_capture, abort_capture, complete_capture, stop_and_join, prepare,
    page_read, context_read, temperature_read, frequency_read,
    retained_region_release_mapping, retained_region_release_bound,
    retained_region_release_initialized, retained_region_release_placement_observed,
    execution_input_validation, execution_validation, start_state_schedule,
    terminal_verification_preparation, worker_launch, worker_startup,
    worker_callback, worker_command, worker_execute, worker_generation,
    worker_projection, worker_result_publication, worker_join, count,
};
enum class PairwiseRole : std::uint8_t { first, second, count };
enum class PairwiseKernelFamily : std::uint8_t { bandwidth, latency, count };
enum class PairwiseKernelImplementation : std::uint8_t {
    scalar_u64_sequential_v1, scalar_u64_random_cycle_v1, count,
};
enum class PairwiseKernelPhase : std::uint8_t {
    none, pass_count_preparation, work_accounting, execution_input_validation,
    execution_validation, start_state_schedule, terminal_verification_preparation, count,
};
enum class PairwiseVerificationPhase : std::uint8_t { before, after, count };
enum class PairwiseMonitoringChannel : std::uint8_t {
    page_faults, context_switches, temperature, frequency, count,
};
enum class PairwiseMissingOperation : std::uint8_t {
    now, bind_service_cpu, support, read, clock_now, count,
};
enum class PairwiseProtectionKind : std::uint8_t { read_only, count };
enum class PairwiseRangeKind : std::uint8_t { full_mapping, count };
enum class PairwiseQualificationState : std::uint8_t { qualified, skipped, not_qualified, count };
enum class PairwiseMappingProtection : std::uint8_t { unspecified, read_write, read_only, count };
enum class PairwiseMappingAdvice : std::uint8_t {
    none, base_pages, huge_pages, no_huge_pages, count,
};
enum class PairwiseMonitoringCapacityAcquisitionLeg : std::uint8_t {
    first_controller, second_runner, count,
};

struct PairwiseCount final { std::uint64_t value{}; auto operator<=>(const PairwiseCount&) const = default; };
struct PairwiseOrdinal final { std::uint64_t value{}; auto operator<=>(const PairwiseOrdinal&) const = default; };
struct NoPairwiseFailureContext final { auto operator<=>(const NoPairwiseFailureContext&) const = default; };
struct LifecyclePhaseContext final { PairwiseLifecyclePhase phase; auto operator<=>(const LifecyclePhaseContext&) const = default; };
struct ExpectedObservedCountContext final { PairwiseCount expected; PairwiseCount observed; auto operator<=>(const ExpectedObservedCountContext&) const = default; };
struct ExpectedObservedOrdinalContext final { PairwiseOrdinal expected; PairwiseOrdinal observed; auto operator<=>(const ExpectedObservedOrdinalContext&) const = default; };
struct WorkerAssociationContext final { PairwiseRole role; PairwiseOrdinal worker_ordinal; auto operator<=>(const WorkerAssociationContext&) const = default; };
struct KernelIdentityContext final {
    PairwiseKernelFamily family;
    PairwiseKernelImplementation implementation;
    PairwiseKernelPhase phase;
    auto operator<=>(const KernelIdentityContext&) const = default;
};
struct VerificationPhaseContext final { PairwiseVerificationPhase phase; auto operator<=>(const VerificationPhaseContext&) const = default; };
struct MonitoringChannelContext final { PairwiseMonitoringChannel channel; auto operator<=>(const MonitoringChannelContext&) const = default; };
struct MissingOperationContext final { PairwiseMissingOperation operation; auto operator<=>(const MissingOperationContext&) const = default; };
struct MemoryProtectionStateContext final {
    PairwiseProtectionKind protection;
    PairwiseRangeKind range;
    PairwiseQualificationState qualification;
    PairwiseFailureReason reason;
    auto operator<=>(const MemoryProtectionStateContext&) const = default;
};
struct RequestedProtectionAdviceContext final {
    PairwiseMappingProtection protection;
    PairwiseMappingAdvice advice;
    auto operator<=>(const RequestedProtectionAdviceContext&) const = default;
};
struct PairwiseMonitoringCapacityAcquisitionContext final {
    PairwiseMonitoringCapacityAcquisitionLeg leg;
    bool uncertain;
    auto operator<=>(const PairwiseMonitoringCapacityAcquisitionContext&) const = default;
};

enum class PairwisePerfLossType : std::uint8_t { lost, lost_samples, count };
enum class PairwisePerfDrainRole : std::uint8_t { unscoped, initial, scheduled, final, count };
struct PairwisePerfLossRecord final {
    PairwisePerfLossType loss_type;
    PairwiseCount header_size;
    std::optional<PairwiseCount> lost_count;
    auto operator<=>(const PairwisePerfLossRecord&) const = default;
};
struct PairwisePerfRingPosition final {
    PairwiseCount captured_head;
    PairwiseCount captured_tail;
    PairwiseCount ring_bytes;
    auto operator<=>(const PairwisePerfRingPosition&) const = default;
};
struct PairwisePerfDrainPosition final {
    PairwisePerfDrainRole role{PairwisePerfDrainRole::unscoped};
    std::optional<PairwiseOrdinal> ordinal{};
    std::optional<PairwiseCount> started_ns{};
    auto operator<=>(const PairwisePerfDrainPosition&) const = default;
};
struct PairwisePerfLossContext final {
    PairwisePerfLossRecord record;
    PairwisePerfRingPosition ring;
    PairwisePerfDrainPosition drain{};
    std::optional<PairwiseCount> last_successful_drain_completed_ns{};
    auto operator<=>(const PairwisePerfLossContext&) const = default;
};

using PairwiseFlatFailureContext = std::variant<
    NoPairwiseFailureContext, LifecyclePhaseContext, ExpectedObservedCountContext,
    ExpectedObservedOrdinalContext, WorkerAssociationContext, KernelIdentityContext,
    VerificationPhaseContext, MonitoringChannelContext, MissingOperationContext,
    MemoryProtectionStateContext, RequestedProtectionAdviceContext,
    PairwiseMonitoringCapacityAcquisitionContext, PairwisePerfLossContext>;

struct PairwiseNestedFailureValue final {
    PairwiseFailureCode code;
    PairwiseFailureOperation operation;
    PairwiseFailureReason reason;
    PairwiseFlatFailureContext context;
    std::optional<int> posix_errno;
    auto operator<=>(const PairwiseNestedFailureValue&) const = default;
};
struct NestedFailureContext final {
    PairwiseNestedFailureValue failure;
    // The historical discriminator is retained, but its bounded payload follows the
    // production graph: a wrapper may identify its phase and may itself contain one
    // exact Platform failure.  There is no open recursive alternative.
    std::optional<PairwiseLifecyclePhase> phase{};
    std::optional<PairwiseNestedFailureValue> nested_platform_failure{};
    auto operator<=>(const NestedFailureContext&) const = default;
};

using PairwiseFailureContext = std::variant<
    NoPairwiseFailureContext, LifecyclePhaseContext, ExpectedObservedCountContext,
    ExpectedObservedOrdinalContext, WorkerAssociationContext, KernelIdentityContext,
    VerificationPhaseContext, MonitoringChannelContext, MissingOperationContext,
    MemoryProtectionStateContext, RequestedProtectionAdviceContext,
    PairwiseMonitoringCapacityAcquisitionContext,
    NestedFailureContext, PairwisePerfLossContext>;

[[nodiscard]] constexpr auto context_kind(const PairwiseFlatFailureContext& context) noexcept
    -> PairwiseContextKind {
    // Keep the historical full-context nested_failure index12 unchanged.
    return std::holds_alternative<PairwisePerfLossContext>(context)
        ? PairwiseContextKind::perf_loss
        : static_cast<PairwiseContextKind>(context.index());
}
[[nodiscard]] constexpr auto context_kind(const PairwiseFailureContext& context) noexcept
    -> PairwiseContextKind { return static_cast<PairwiseContextKind>(context.index()); }

[[nodiscard]] auto pairwise_validate_failure(PairwiseFailureSubsystem subsystem,
                                             PairwiseFailureCode code,
                                             PairwiseFailureOperation operation,
                                             PairwiseFailureReason reason,
                                             const PairwiseFailureContext& context,
                                             std::optional<int> posix_errno) noexcept -> bool;

template <PairwiseFailureSubsystem Subsystem>
class BasicPairwiseFailure final {
  public:
    [[nodiscard]] static auto create(PairwiseFailureCode code,
                                     PairwiseFailureOperation operation,
                                     PairwiseFailureReason reason,
                                     PairwiseFailureContext context,
                                     std::optional<int> posix_errno = std::nullopt) noexcept
        -> std::optional<BasicPairwiseFailure> {
        if (!pairwise_validate_failure(Subsystem, code, operation, reason, context, posix_errno)) return std::nullopt;
        return BasicPairwiseFailure{code, operation, reason, context, posix_errno};
    }
    [[nodiscard]] static constexpr auto subsystem() noexcept -> PairwiseFailureSubsystem { return Subsystem; }
    [[nodiscard]] constexpr auto code() const noexcept -> PairwiseFailureCode { return code_; }
    [[nodiscard]] constexpr auto operation() const noexcept -> PairwiseFailureOperation { return operation_; }
    [[nodiscard]] constexpr auto reason() const noexcept -> PairwiseFailureReason { return reason_; }
    [[nodiscard]] constexpr auto context() const noexcept -> const PairwiseFailureContext& { return context_; }
    [[nodiscard]] constexpr auto posix_errno() const noexcept -> std::optional<int> { return posix_errno_; }
    auto operator<=>(const BasicPairwiseFailure&) const = default;
  private:
    constexpr BasicPairwiseFailure(PairwiseFailureCode code, PairwiseFailureOperation operation,
                                   PairwiseFailureReason reason, PairwiseFailureContext context,
                                   std::optional<int> posix_errno) noexcept
        : code_{code}, operation_{operation}, reason_{reason}, context_{context}, posix_errno_{posix_errno} {}
    PairwiseFailureCode code_;
    PairwiseFailureOperation operation_;
    PairwiseFailureReason reason_;
    PairwiseFailureContext context_;
    std::optional<int> posix_errno_;
};

using PairwisePlatformFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::platform>;
using PairwiseMemoryFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::memory>;
using PairwiseMonitoringFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::monitoring>;
using PairwiseKernelFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::kernel>;
using PairwiseMeasurementFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::measurement>;
using PairwiseStatisticsFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::statistics>;
using PairwiseWorkerFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::worker>;
using PairwiseRuntimeFailure = BasicPairwiseFailure<PairwiseFailureSubsystem::runtime>;
using PairwiseEvidenceFailure = std::variant<PairwisePlatformFailure, PairwiseMemoryFailure,
    PairwiseMonitoringFailure, PairwiseKernelFailure, PairwiseMeasurementFailure,
    PairwiseStatisticsFailure, PairwiseWorkerFailure, PairwiseRuntimeFailure>;

static_assert(std::variant_size_v<PairwiseEvidenceFailure> == 8);
static_assert(static_cast<std::size_t>(PairwiseContextKind::nested_failure) == 12);

} // namespace epycbench::runtime
