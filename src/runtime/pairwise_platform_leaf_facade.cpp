#include "runtime/pairwise_platform_leaf_facade.hpp"

#include "epycbench/platform_linux/task_stat.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <exception>
#include <limits>
#include <linux/mempolicy.h>
#include <new>
#include <optional>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <type_traits>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

constexpr std::uint64_t nanoseconds_per_second{1'000'000'000U};

[[nodiscard]] auto failure(PairwiseFailureCode code, PairwiseFailureOperation operation,
                           PairwiseFailureReason reason,
                           PairwiseFailureContext context = NoPairwiseFailureContext{},
                           std::optional<int> error_number = std::nullopt) noexcept
    -> PairwisePlatformFailure {
    auto value = PairwisePlatformFailure::create(code, operation, reason, std::move(context),
                                                 error_number);
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] auto mapping_advice_context() noexcept -> PairwiseFailureContext {
    return RequestedProtectionAdviceContext{PairwiseMappingProtection::unspecified,
                                             PairwiseMappingAdvice::no_huge_pages};
}

[[nodiscard]] auto mapping_protection_context() noexcept -> PairwiseFailureContext {
    return RequestedProtectionAdviceContext{PairwiseMappingProtection::read_only,
                                             PairwiseMappingAdvice::none};
}

void before(const PairwisePlatformLeafDependencies& dependencies,
            PairwisePlatformLeafOperation operation) {
    if (dependencies.before_operation != nullptr)
        dependencies.before_operation(dependencies.hook_context, operation);
}

[[nodiscard]] auto contained_mapping(PairwiseFailureOperation operation, bool unknown) noexcept
    -> PairwisePlatformFailure {
    return failure(unknown ? PairwiseFailureCode::platform_mapping_contained_unknown_failure
                           : PairwiseFailureCode::platform_mapping_contained_exception,
                   operation,
                   unknown ? PairwiseFailureReason::contained_unknown_failure
                           : PairwiseFailureReason::contained_exception);
}

[[nodiscard]] auto contained_numa(PairwiseFailureOperation operation, bool unknown) noexcept
    -> PairwisePlatformFailure {
    return failure(unknown ? PairwiseFailureCode::platform_numa_contained_unknown_failure
                           : PairwiseFailureCode::platform_numa_contained_exception,
                   operation,
                   unknown ? PairwiseFailureReason::contained_unknown_failure
                           : PairwiseFailureReason::contained_exception);
}

[[nodiscard]] auto map_create_reason(int error_number) noexcept -> PairwiseFailureReason {
    switch (error_number) {
    case ENOMEM:
        return PairwiseFailureReason::address_space_exhausted;
    case EAGAIN:
        return PairwiseFailureReason::resource_limit;
    case EACCES:
    case EPERM:
        return PairwiseFailureReason::permission_denied;
    case EINVAL:
        return PairwiseFailureReason::invalid_mapping_request;
    case EBADF:
    case ENODEV:
    case EEXIST:
        return PairwiseFailureReason::unexpected_mapping_configuration;
    default:
        return PairwiseFailureReason::create_failure;
    }
}

[[nodiscard]] auto mapping_advice_reason(int error_number) noexcept -> PairwiseFailureReason {
    switch (error_number) {
    case EINVAL:
    case ENOSYS:
        return PairwiseFailureReason::unsupported_advice;
    case ENOMEM:
        return PairwiseFailureReason::range_unmapped;
    case EACCES:
    case EPERM:
        return PairwiseFailureReason::permission_denied;
    default:
        return PairwiseFailureReason::advice_failure;
    }
}

[[nodiscard]] auto mapping_protect_reason(int error_number) noexcept -> PairwiseFailureReason {
    switch (error_number) {
    case EACCES:
    case EPERM:
        return PairwiseFailureReason::permission_denied;
    case EINVAL:
        return PairwiseFailureReason::invalid_protection_or_range;
    case ENOMEM:
        return PairwiseFailureReason::range_unmapped_or_resource_failure;
    default:
        return PairwiseFailureReason::protection_failure;
    }
}

[[nodiscard]] auto numa_bind_reason(int error_number) noexcept -> PairwiseFailureReason {
    switch (error_number) {
    case EFAULT:
        return PairwiseFailureReason::invalid_address_range;
    case EINVAL:
        return PairwiseFailureReason::invalid_bind_request;
    case ENOMEM:
        return PairwiseFailureReason::resource_unavailable;
    case EPERM:
        return PairwiseFailureReason::permission_denied;
    case ENODEV:
        return PairwiseFailureReason::node_unavailable;
    default:
        return PairwiseFailureReason::bind_failure;
    }
}

[[nodiscard]] auto numa_query_reason(int error_number) noexcept -> PairwiseFailureReason {
    switch (error_number) {
    case EACCES:
    case EPERM:
        return PairwiseFailureReason::permission_denied;
    case EFAULT:
        return PairwiseFailureReason::invalid_address_range;
    case EINVAL:
        return PairwiseFailureReason::invalid_query;
    case ESRCH:
        return PairwiseFailureReason::process_or_page_missing;
    case ENOMEM:
        return PairwiseFailureReason::resource_unavailable;
    default:
        return PairwiseFailureReason::query_failure;
    }
}

auto allocate_numa_words(std::size_t count) -> std::vector<unsigned long> {
    return std::vector<unsigned long>(count, 0UL);
}

auto allocate_numa_statuses(std::size_t count) -> std::vector<int> {
    return std::vector<int>(count);
}

auto allocate_numa_results(std::size_t count)
    -> std::vector<platform_linux::PageQueryStatus> {
    auto results = std::vector<platform_linux::PageQueryStatus>{};
    results.reserve(count);
    return results;
}

[[nodiscard]] constexpr auto prepare_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::prepare};
}
[[nodiscard]] constexpr auto context_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::context_read};
}
[[nodiscard]] constexpr auto cleanup_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::cleanup};
}

[[nodiscard]] constexpr auto page_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::page_read};
}

[[nodiscard]] constexpr auto temperature_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::temperature_read};
}

[[nodiscard]] constexpr auto frequency_phase() noexcept -> PairwiseFailureContext {
    return LifecyclePhaseContext{PairwiseLifecyclePhase::frequency_read};
}

[[nodiscard]] auto mapped_hwmon_failure(
    platform_linux::detail::BoundedHwmonSourceFailure primary,
    std::span<const platform_linux::detail::BoundedHwmonSourceFailure> cleanup,
    std::vector<PairwisePlatformFailure>&& mapped_cleanup) noexcept
    -> PairwiseSysfsPreparationFailure {
    if (cleanup.size() > mapped_cleanup.capacity() - mapped_cleanup.size())
        std::terminate();
    for (const auto value : cleanup) {
        mapped_cleanup.push_back(failure(
            PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
            PairwiseFailureOperation::hwmon_temperature,
            PairwiseFailureReason::enumerate, temperature_phase(),
            value.native_error > 0
                ? std::optional<int>{value.native_error}
                : std::nullopt));
    }
    const auto native = primary.native_error > 0
                            ? std::optional<int>{primary.native_error}
                            : std::nullopt;
    const auto reject = [&](PairwiseFailureCode code,
                            PairwiseFailureReason reason,
                            PairwiseFailureContext context,
                            std::optional<int> error = std::nullopt) {
        return PairwiseSysfsPreparationFailure{
            failure(code, PairwiseFailureOperation::hwmon_temperature,
                    reason, std::move(context), error),
            std::move(mapped_cleanup)};
    };
    using Kind = platform_linux::detail::BoundedHwmonSourceFailureKind;
    switch (primary.kind) {
    case Kind::enumeration_capacity:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_enumeration_capacity_exceeded,
            PairwiseFailureReason::enumeration_capacity_exceeded, prepare_phase());
    case Kind::token_capacity:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_token_capacity_exceeded,
            PairwiseFailureReason::token_capacity_exceeded, prepare_phase());
    case Kind::ambiguous:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_ambiguous_sensor,
            PairwiseFailureReason::ambiguous, temperature_phase());
    case Kind::value:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_invalid_value,
            PairwiseFailureReason::value, temperature_phase());
    case Kind::read:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_read_failed,
            PairwiseFailureReason::read, temperature_phase(),
            native.value_or(EIO));
    case Kind::enumerate:
    case Kind::contained:
    case Kind::preparation_rejected:
        return reject(
            PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
            PairwiseFailureReason::enumerate, temperature_phase(), native);
    }
    std::terminate();
}

[[nodiscard]] auto mapped_cpu_frequency_failure(
    platform_linux::detail::BoundedCpuFrequencySourceFailure primary,
    std::span<const platform_linux::detail::BoundedCpuFrequencySourceFailure>
        cleanup,
    std::vector<PairwisePlatformFailure>&& mapped_cleanup) noexcept
    -> PairwiseSysfsPreparationFailure {
    if (cleanup.size() > mapped_cleanup.capacity() - mapped_cleanup.size())
        std::terminate();
    for (const auto value : cleanup) {
        mapped_cleanup.push_back(failure(
            PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
            PairwiseFailureOperation::cpu_frequency,
            PairwiseFailureReason::enumerate, frequency_phase(),
            value.native > 0 ? std::optional<int>{value.native} : std::nullopt));
    }
    const auto native = primary.native > 0
                            ? std::optional<int>{primary.native}
                            : std::nullopt;
    const auto reject = [&](PairwiseFailureCode code,
                            PairwiseFailureReason reason,
                            PairwiseFailureContext context,
                            std::optional<int> error = std::nullopt) {
        return PairwiseSysfsPreparationFailure{
            failure(code, PairwiseFailureOperation::cpu_frequency,
                    reason, std::move(context), error),
            std::move(mapped_cleanup)};
    };
    using Kind = platform_linux::detail::BoundedCpuFrequencySourceFailureKind;
    switch (primary.kind) {
    case Kind::enumeration_capacity:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_enumeration_capacity_exceeded,
            PairwiseFailureReason::enumeration_capacity_exceeded, prepare_phase());
    case Kind::token_capacity:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_token_capacity_exceeded,
            PairwiseFailureReason::token_capacity_exceeded, prepare_phase());
    case Kind::ambiguous:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_ambiguous_policy,
            PairwiseFailureReason::ambiguous, frequency_phase());
    case Kind::membership:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_invalid_value,
            PairwiseFailureReason::membership, frequency_phase());
    case Kind::metadata:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_invalid_value,
            PairwiseFailureReason::metadata, frequency_phase());
    case Kind::value:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_invalid_value,
            PairwiseFailureReason::value, frequency_phase());
    case Kind::read:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_read_failed,
            PairwiseFailureReason::read, frequency_phase(),
            native.value_or(EIO));
    case Kind::enumerate:
    case Kind::contained:
    case Kind::preparation_rejected:
        return reject(
            PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
            PairwiseFailureReason::enumerate, frequency_phase(), native);
    }
    std::terminate();
}

[[nodiscard]] auto task_stat_failure(
    platform_linux::detail::BoundedTaskStatFailure value) noexcept
    -> PairwisePlatformFailure {
    using Kind = platform_linux::detail::BoundedTaskStatFailureKind;
    auto code = PairwiseFailureCode::platform_task_stat_read_failed;
    auto operation = PairwiseFailureOperation::read_task_page_fault_counters;
    auto reason = PairwiseFailureReason::invalid_operations;
    switch (value.kind) {
    case Kind::invalid_operations: break;
    case Kind::open_failed: reason = PairwiseFailureReason::open_failed; break;
    case Kind::read_failed: reason = PairwiseFailureReason::read_failed; break;
    case Kind::close_failed: reason = PairwiseFailureReason::close_failed; break;
    case Kind::contained_exception: reason = PairwiseFailureReason::contained_exception; break;
    case Kind::contained_unknown_failure:
        reason = PairwiseFailureReason::contained_unknown_failure;
        break;
    case Kind::task_id:
        code = PairwiseFailureCode::platform_task_stat_malformed;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::task_id;
        break;
    case Kind::task_identity_mismatch:
        code = PairwiseFailureCode::platform_task_stat_identity_mismatch;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::task_identity_mismatch;
        break;
    case Kind::command_delimiter:
        code = PairwiseFailureCode::platform_task_stat_malformed;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::command_delimiter;
        break;
    case Kind::truncated_record:
        code = PairwiseFailureCode::platform_task_stat_malformed;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::truncated_record;
        break;
    case Kind::minor_faults:
        code = PairwiseFailureCode::platform_task_stat_malformed;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::minor_faults;
        break;
    case Kind::minor_faults_out_of_range:
        code = PairwiseFailureCode::platform_task_stat_counter_out_of_range;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::minor_faults;
        break;
    case Kind::major_faults:
        code = PairwiseFailureCode::platform_task_stat_malformed;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::major_faults;
        break;
    case Kind::major_faults_out_of_range:
        code = PairwiseFailureCode::platform_task_stat_counter_out_of_range;
        operation = PairwiseFailureOperation::parse_task_stat;
        reason = PairwiseFailureReason::major_faults;
        break;
    }
    return failure(code, operation, reason, page_phase(),
                   value.error_number == 0
                       ? std::optional<int>{}
                       : std::optional<int>{value.error_number});
}

[[nodiscard]] auto perf_drain_failure(
    platform_linux::detail::BoundedPerfFailureKind kind) noexcept
    -> PairwisePlatformFailure {
    using Kind = platform_linux::detail::BoundedPerfFailureKind;
    auto code = PairwiseFailureCode::platform_perf_context_switch_record_malformed;
    auto reason = PairwiseFailureReason::record;
    switch (kind) {
    case Kind::metadata_capacity:
        code = PairwiseFailureCode::platform_perf_context_switch_metadata_capacity_exceeded;
        reason = PairwiseFailureReason::metadata_capacity_exceeded;
        return failure(code, PairwiseFailureOperation::prepare_perf_context_switch,
                       reason, prepare_phase());
    case Kind::cursor:
        code = PairwiseFailureCode::platform_perf_context_switch_ring_corrupt;
        reason = PairwiseFailureReason::cursor;
        break;
    case Kind::truncated_header: reason = PairwiseFailureReason::truncated_header; break;
    case Kind::record_size: reason = PairwiseFailureReason::record_size; break;
    case Kind::switch_size: reason = PairwiseFailureReason::switch_size; break;
    case Kind::record: reason = PairwiseFailureReason::record; break;
    case Kind::capacity:
        code = PairwiseFailureCode::platform_perf_context_switch_record_capacity;
        reason = PairwiseFailureReason::capacity;
        break;
    case Kind::record_type:
        code = PairwiseFailureCode::platform_perf_context_switch_record_unsupported;
        reason = PairwiseFailureReason::record_type;
        break;
    case Kind::lost:
        code = PairwiseFailureCode::platform_perf_context_switch_capture_incomplete;
        reason = PairwiseFailureReason::lost;
        break;
    }
    return failure(code, PairwiseFailureOperation::drain_perf_context_switch,
                   reason, context_phase());
}

[[nodiscard]] auto cpu_mask(const PairwisePlatformLeafDependencies& dependencies,
                            std::size_t requested, PairwiseFailureOperation operation) noexcept
    -> std::expected<platform_linux::DynamicCpuMask, PairwisePlatformFailure> {
    try {
        const auto native_size =
            dependencies.cpu_masks.allocation_size(dependencies.cpu_masks.context, requested);
        if (native_size == 0U || native_size > std::numeric_limits<std::size_t>::max() / CHAR_BIT)
            return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                           operation, PairwiseFailureReason::none)};
        const auto capacity = native_size * CHAR_BIT;
        if (capacity < requested ||
            static_cast<std::uint64_t>(capacity) > dependencies.affinity_capacity_limit)
            return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                           operation, PairwiseFailureReason::none)};
        auto* data = dependencies.cpu_masks.allocate(dependencies.cpu_masks.context, requested);
        if (data == nullptr)
            return std::unexpected{failure(
                operation == PairwiseFailureOperation::set_affinity
                    ? PairwiseFailureCode::platform_affinity_mask_allocation_failed
                    : PairwiseFailureCode::platform_affinity_conversion_failed,
                operation,
                operation == PairwiseFailureOperation::set_affinity
                    ? PairwiseFailureReason::allocation_failed
                    : PairwiseFailureReason::none)};
        auto mask = platform_linux::detail::DynamicCpuMaskAccess::adopt(
            data, native_size, capacity, dependencies.cpu_masks.context,
            dependencies.cpu_masks.release);
        mask.zero();
        return mask;
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(
            operation == PairwiseFailureOperation::set_affinity
                ? PairwiseFailureCode::platform_affinity_mask_allocation_failed
                : PairwiseFailureCode::platform_affinity_conversion_failed,
            operation,
            operation == PairwiseFailureOperation::set_affinity
                ? PairwiseFailureReason::allocation_failed
                : PairwiseFailureReason::none)};
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       operation, PairwiseFailureReason::none)};
    }
}

} // namespace

struct PairwisePreparedPerfContextSwitch::State final {
    int descriptor{-1};
    void* mapping{};
    std::size_t mapping_size{};
    std::size_t ring_offset{};
    std::size_t ring_size{};
    std::size_t record_capacity{};
    PairwisePerfContextSwitchOperations operations{};
    bool active{};
    bool drain_failed{};
    bool epoch_open{};
    bool epoch_enabled{};
    bool epoch_disable_attempted{};
    bool epoch_poisoned{};
    std::uint64_t verified_loss{};
    platform_linux::detail::BoundedPerfFirstLoss first_loss{};
};
// Existing HostFrozenCollection preparation charges1024 bytes per private
// source State, separately from sizeof(PairwisePerfDrainStorage).
static_assert(sizeof(PairwisePreparedPerfContextSwitch::State) <= 1024U);

PairwisePreparedPerfContextSwitch::PairwisePreparedPerfContextSwitch(
    std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
PairwisePreparedPerfContextSwitch::PairwisePreparedPerfContextSwitch(
    PairwisePreparedPerfContextSwitch&&) noexcept = default;
PairwisePreparedPerfContextSwitch::~PairwisePreparedPerfContextSwitch() noexcept {
    if (state_ && state_->active)
        std::terminate();
}
auto PairwisePreparedPerfContextSwitch::active() const noexcept -> bool {
    return state_ && state_->active;
}
PairwisePerfPreparationWorkspace::PairwisePerfPreparationWorkspace(
    std::unique_ptr<PairwisePreparedPerfContextSwitch::State> state) noexcept
    : state_{std::move(state)} {}
PairwisePerfPreparationWorkspace::PairwisePerfPreparationWorkspace(
    PairwisePerfPreparationWorkspace&&) noexcept = default;
PairwisePerfPreparationWorkspace::~PairwisePerfPreparationWorkspace() noexcept = default;

auto PairwisePerfDrainStorage::prepare(std::size_t capacity)
    -> PairwisePerfDrainStorage {
    auto records = std::vector<platform_linux::PerfContextSwitchRecord>{};
    records.reserve(capacity);
    for (std::size_t index{}; index < capacity; ++index)
        records.push_back(platform_linux::PerfContextSwitchRecord::create(
            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0U),
            platform_linux::PerfContextSwitchKind::switch_in));
    return PairwisePerfDrainStorage{std::move(records)};
}

auto PairwisePlatformLeafFacade::production() noexcept -> PairwisePlatformLeafFacade {
    return create(PairwisePlatformLeafDependencies{
        .clock = platform_linux::detail::production_clock_operations(),
        .threads = platform_linux::detail::production_thread_operations(),
        .cpu_masks = platform_linux::detail::production_cpu_mask_allocator(),
        .mapping = platform_linux::detail::production_mapping_operations(),
        .numa = platform_linux::detail::production_numa_operations(),
        .numa_words = allocate_numa_words,
        .numa_statuses = allocate_numa_statuses,
        .numa_results = allocate_numa_results,
        .task_stat = platform_linux::detail::production_bounded_task_stat_operations(),
        .perf_context =
            platform_linux::detail::production_bounded_perf_context_switch_operations(),
    });
}

auto PairwisePlatformLeafFacade::prepare_perf_context_switch(
    platform_linux::LinuxThreadId thread, std::uint64_t base_page_bytes,
    std::uint64_t record_capacity) const noexcept
    -> std::expected<PairwisePreparedPerfContextSwitch,
                     PairwisePerfPreparationFailure> {
    const auto rejected = [](PairwisePlatformFailure primary) {
        return std::unexpected{PairwisePerfPreparationFailure{
            std::move(primary), {std::nullopt, std::nullopt}}};
    };
    auto workspace = prepare_perf_context_switch_workspace();
    if (!workspace)
        return rejected(std::move(workspace.error()));
    return prepare_perf_context_switch_prepared(
        std::move(*workspace), thread, base_page_bytes, record_capacity);
}

auto PairwisePlatformLeafFacade::prepare_perf_context_switch_workspace()
    const noexcept
    -> std::expected<PairwisePerfPreparationWorkspace,
                     PairwisePlatformFailure> {
    try {
        return PairwisePerfPreparationWorkspace{
            std::make_unique<PairwisePreparedPerfContextSwitch::State>()};
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_construction_failed,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::allocation, prepare_phase())};
    } catch (...) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_construction_failed,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::allocation, prepare_phase())};
    }
}

auto PairwisePlatformLeafFacade::prepare_perf_context_switch_prepared(
    PairwisePerfPreparationWorkspace workspace,
    platform_linux::LinuxThreadId thread, std::uint64_t base_page_bytes,
    std::uint64_t record_capacity) const noexcept
    -> std::expected<PairwisePreparedPerfContextSwitch,
                     PairwisePerfPreparationFailure> {
    const auto rejected = [](PairwisePlatformFailure primary) {
        return std::unexpected{PairwisePerfPreparationFailure{
            std::move(primary), {std::nullopt, std::nullopt}}};
    };
    const auto& operations = dependencies_.perf_context;
    if (base_page_bytes == 0U || record_capacity == 0U ||
        base_page_bytes > std::numeric_limits<std::size_t>::max() / 9U)
        return rejected(failure(
            PairwiseFailureCode::platform_perf_context_switch_invalid_configuration,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::ring_pages));
    if (operations.open_capture == nullptr || operations.map == nullptr ||
        operations.unmap == nullptr || operations.close == nullptr ||
        operations.enable == nullptr || operations.disable == nullptr ||
        operations.read_loss == nullptr)
        return rejected(failure(
            PairwiseFailureCode::platform_perf_context_switch_invalid_configuration,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::operations));

    auto state = std::move(workspace.state_);
    if (!state)
        return rejected(failure(
            PairwiseFailureCode::platform_perf_context_switch_invalid_configuration,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::operations));
    state->operations = operations;
    state->mapping_size = static_cast<std::size_t>(base_page_bytes * 9U);
    state->record_capacity = static_cast<std::size_t>(record_capacity);

    const auto opened = operations.open_capture(operations.context, thread);
    if (opened.value < 0) {
        if (opened.error_number <= 0)
            std::terminate();
        return rejected(failure(
            PairwiseFailureCode::platform_perf_context_switch_open_failed,
            PairwiseFailureOperation::prepare_perf_context_switch,
            PairwiseFailureReason::open, NoPairwiseFailureContext{},
            opened.error_number));
    }
    state->descriptor = opened.value;

    const auto cleanup = [&](PairwisePerfPreparationFailure& result) noexcept {
        std::size_t next{};
        if (state->mapping != nullptr) {
            const auto value = operations.unmap(operations.context, state->mapping,
                                                state->mapping_size);
            if (value.value != 0) {
                if (value.error_number <= 0)
                    std::terminate();
                result.cleanup[next++].emplace(failure(
                    PairwiseFailureCode::platform_perf_context_switch_unmap_failed,
                    PairwiseFailureOperation::retire_perf_context_switch,
                    PairwiseFailureReason::unmap, cleanup_phase(), value.error_number));
            }
            state->mapping = nullptr;
        }
        const auto value = operations.close(operations.context, state->descriptor);
        if (value.value != 0) {
            if (value.error_number <= 0)
                std::terminate();
            result.cleanup[next].emplace(failure(
                PairwiseFailureCode::platform_perf_context_switch_close_failed,
                PairwiseFailureOperation::retire_perf_context_switch,
                PairwiseFailureReason::close, cleanup_phase(), value.error_number));
        }
        state->descriptor = -1;
    };

    const auto mapped = operations.map(operations.context, state->descriptor,
                                       state->mapping_size);
    if (mapped.value == nullptr) {
        if (mapped.error_number <= 0)
            std::terminate();
        PairwisePerfPreparationFailure result{
            failure(PairwiseFailureCode::platform_perf_context_switch_map_failed,
                    PairwiseFailureOperation::prepare_perf_context_switch,
                    mapped.failure == platform_linux::detail::BoundedPerfMapFailure::mmap
                        ? PairwiseFailureReason::mmap
                        : PairwiseFailureReason::map,
                    prepare_phase(), mapped.error_number),
            {std::nullopt, std::nullopt}};
        cleanup(result);
        return std::unexpected{std::move(result)};
    }
    state->mapping = mapped.value;

    if (operations.before_metadata != nullptr)
        operations.before_metadata(operations.context);
    auto metadata = platform_linux::detail::validate_bounded_perf_metadata(
        state->mapping, state->mapping_size,
        static_cast<std::size_t>(base_page_bytes * 8U),
        static_cast<std::size_t>(record_capacity));
    if (!metadata) {
        PairwisePerfPreparationFailure result{
            perf_drain_failure(metadata.error()),
            {std::nullopt, std::nullopt}};
        cleanup(result);
        return std::unexpected{std::move(result)};
    }
    state->ring_offset = metadata->ring_offset;
    state->ring_size = metadata->ring_size;
    state->active = true;
    return PairwisePreparedPerfContextSwitch{std::move(state)};
}

namespace {
auto checked_epoch_loss(const PairwisePerfContextSwitchOperations& operations,
                        int descriptor, PairwiseFailureOperation operation,
                        std::uint64_t expected)
    -> std::expected<std::uint64_t, PairwisePlatformFailure> {
    const auto result = operations.read_loss(operations.context, descriptor);
    if (result.bytes < 0) {
        if (result.error_number <= 0)
            return std::unexpected{failure(
                PairwiseFailureCode::platform_perf_context_switch_epoch_invalid,
                operation, PairwiseFailureReason::epoch_state)};
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_read_failed,
            operation, PairwiseFailureReason::read, NoPairwiseFailureContext{},
            result.error_number)};
    }
    if (result.error_number != 0)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_invalid,
            operation, PairwiseFailureReason::epoch_state)};
    if (result.bytes != 16)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_invalid,
            operation, PairwiseFailureReason::loss_read_size,
            ExpectedObservedCountContext{{16U}, {static_cast<std::uint64_t>(result.bytes)}})};
    if (result.lost < expected)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_invalid,
            operation, PairwiseFailureReason::counter_regression,
            ExpectedObservedCountContext{{expected}, {result.lost}})};
    if (result.lost != expected)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_loss,
            operation, PairwiseFailureReason::lost,
            ExpectedObservedCountContext{{expected}, {result.lost}})};
    return result.lost;
}
auto epoch_state_failure(PairwiseFailureOperation operation) -> PairwisePlatformFailure {
    return failure(PairwiseFailureCode::platform_perf_context_switch_epoch_invalid,
                   operation, PairwiseFailureReason::epoch_state);
}
} // namespace

auto PairwisePlatformLeafFacade::begin_perf_context_switch_capture(
    PairwisePreparedPerfContextSwitch& source) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::begin_perf_context_switch_capture;
    if (!source.state_ || !source.state_->active)
        return std::unexpected{epoch_state_failure(operation)};
    auto& state = *source.state_;
    if (state.epoch_open || state.epoch_enabled || state.epoch_poisoned || state.drain_failed) {
        state.epoch_poisoned = true;
        return std::unexpected{epoch_state_failure(operation)};
    }
    auto baseline = checked_epoch_loss(state.operations, state.descriptor, operation, state.verified_loss);
    if (!baseline) {
        state.epoch_poisoned = true;
        return std::unexpected{std::move(baseline.error())};
    }
    state.epoch_open = true;
    state.epoch_enabled = true; // An unsuccessful control is uncertain until cleanup.
    state.epoch_disable_attempted = false;
    const auto enabled = state.operations.enable(state.operations.context, state.descriptor);
    if (enabled.value != 0 || enabled.error_number != 0) {
        state.epoch_poisoned = true;
        if (enabled.value != -1 || enabled.error_number <= 0)
            return std::unexpected{epoch_state_failure(operation)};
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_control_failed,
            operation, PairwiseFailureReason::enable, NoPairwiseFailureContext{}, enabled.error_number)};
    }
    return {};
}

auto PairwisePlatformLeafFacade::disable_perf_context_switch_capture(
    PairwisePreparedPerfContextSwitch& source) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::disable_perf_context_switch_capture;
    if (!source.state_ || !source.state_->active)
        return std::unexpected{epoch_state_failure(operation)};
    auto& state = *source.state_;
    if (!state.epoch_enabled) return {};
    if (state.epoch_disable_attempted)
        return std::unexpected{epoch_state_failure(operation)};
    state.epoch_disable_attempted = true;
    const auto disabled = state.operations.disable(state.operations.context, state.descriptor);
    if (disabled.value != 0 || disabled.error_number != 0) {
        state.epoch_poisoned = true;
        if (disabled.value != -1 || disabled.error_number <= 0)
            return std::unexpected{epoch_state_failure(operation)};
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_epoch_control_failed,
            operation, PairwiseFailureReason::disable, NoPairwiseFailureContext{}, disabled.error_number)};
    }
    state.epoch_enabled = false;
    return {};
}

auto PairwisePlatformLeafFacade::verify_perf_context_switch_capture_loss(
    PairwisePreparedPerfContextSwitch& source) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::verify_perf_context_switch_capture_loss;
    if (!source.state_ || !source.state_->active)
        return std::unexpected{epoch_state_failure(operation)};
    auto& state = *source.state_;
    if (!state.epoch_open || state.epoch_enabled || state.epoch_poisoned || state.drain_failed) {
        state.epoch_poisoned = true;
        return std::unexpected{epoch_state_failure(operation)};
    }
    auto loss = checked_epoch_loss(state.operations, state.descriptor, operation, state.verified_loss);
    if (!loss) {
        state.epoch_poisoned = true;
        return std::unexpected{std::move(loss.error())};
    }
    state.verified_loss = *loss;
    state.epoch_open = false;
    return {};
}

auto PairwisePlatformLeafFacade::drain_perf_context_switch(
    PairwisePreparedPerfContextSwitch& source,
    PairwisePerfDrainStorage storage) const noexcept
    -> std::expected<platform_linux::PerfContextSwitchDrain,
                     PairwisePlatformFailure> {
    auto drained = drain_perf_context_switch_prepared(source, storage);
    if (!drained)
        return std::unexpected{std::move(drained.error())};
    storage.records_.erase(
        storage.records_.begin() +
            static_cast<std::ptrdiff_t>(drained->records.size()),
        storage.records_.end());
    return platform_linux::PerfContextSwitchDrain::create(
        std::move(storage.records_), drained->completeness);
}

auto PairwisePlatformLeafFacade::drain_perf_context_switch_prepared(
    PairwisePreparedPerfContextSwitch& source,
    PairwisePerfDrainStorage& storage) const noexcept
    -> std::expected<PairwisePerfDrainView, PairwisePlatformFailure> {
    if (!source.state_ || !source.state_->active)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_invalid_state,
            PairwiseFailureOperation::drain_perf_context_switch,
            PairwiseFailureReason::moved_from, context_phase())};
    auto& state = *source.state_;
    if (state.drain_failed)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_previous_drain_failed,
            PairwiseFailureOperation::drain_perf_context_switch,
            PairwiseFailureReason::terminal_drain, context_phase())};
    const auto capacity = std::min(storage.records_.size(), state.record_capacity);
    auto decoded = platform_linux::detail::drain_bounded_perf_records(
        state.mapping,
        platform_linux::detail::BoundedPerfMetadata{state.ring_offset, state.ring_size},
        std::span<platform_linux::PerfContextSwitchRecord>{storage.records_}.first(capacity),
        state.operations.context, state.operations.before_second_pass, false, true,
        &state.first_loss);
    if (!decoded) {
        state.drain_failed = true;
        if (decoded.error() == platform_linux::detail::BoundedPerfFailureKind::lost &&
            state.first_loss.available) {
            const auto& loss = state.first_loss;
            return std::unexpected{failure(
                PairwiseFailureCode::platform_perf_context_switch_capture_incomplete,
                PairwiseFailureOperation::drain_perf_context_switch,
                PairwiseFailureReason::lost,
                PairwisePerfLossContext{
                    {loss.record_type == PERF_RECORD_LOST
                        ? PairwisePerfLossType::lost : PairwisePerfLossType::lost_samples,
                    {loss.header_size},
                    loss.lost_count ? std::optional<PairwiseCount>{{*loss.lost_count}}
                                    : std::nullopt},
                    {{loss.captured_head}, {loss.captured_tail}, {loss.ring_bytes}}})};
        }
        return std::unexpected{perf_drain_failure(decoded.error())};
    }
    return PairwisePerfDrainView{
        std::span<const platform_linux::PerfContextSwitchRecord>{storage.records_}
            .first(decoded->record_count),
        platform_linux::PerfCaptureCompleteness::complete};
}

auto PairwisePlatformLeafFacade::reset_perf_context_switch_prepared(
    PairwisePreparedPerfContextSwitch& source,
    PairwisePerfDrainStorage& storage) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    if (!source.state_ || !source.state_->active)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_invalid_state,
            PairwiseFailureOperation::drain_perf_context_switch,
            PairwiseFailureReason::moved_from, context_phase())};
    auto& state = *source.state_;
    if (state.epoch_open || state.epoch_enabled || state.epoch_poisoned) {
        state.epoch_poisoned = true;
        return std::unexpected{epoch_state_failure(PairwiseFailureOperation::drain_perf_context_switch)};
    }
    if (state.drain_failed)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_perf_context_switch_previous_drain_failed,
            PairwiseFailureOperation::drain_perf_context_switch,
            PairwiseFailureReason::terminal_drain, context_phase())};
    const auto capacity = std::min(storage.records_.size(), state.record_capacity);
    auto decoded = platform_linux::detail::drain_bounded_perf_records(
        state.mapping,
        platform_linux::detail::BoundedPerfMetadata{state.ring_offset,
                                                     state.ring_size},
        std::span<platform_linux::PerfContextSwitchRecord>{storage.records_}
            .first(capacity),
        state.operations.context, state.operations.before_second_pass, true);
    if (!decoded) {
        state.drain_failed = true;
        return std::unexpected{perf_drain_failure(decoded.error())};
    }
    return {};
}

auto PairwisePlatformLeafFacade::retire_perf_context_switch(
    PairwisePreparedPerfContextSwitch source) const noexcept
    -> PairwisePerfRetirement {
    PairwisePerfRetirement result;
    if (!source.state_ || !source.state_->active)
        return result;
    auto& state = *source.state_;
    const auto unmap = state.operations.unmap(state.operations.context, state.mapping,
                                              state.mapping_size);
    state.mapping = nullptr;
    if (unmap.value != 0) {
        if (unmap.error_number <= 0)
            std::terminate();
        result.primary.emplace(failure(
            PairwiseFailureCode::platform_perf_context_switch_unmap_failed,
            PairwiseFailureOperation::retire_perf_context_switch,
            PairwiseFailureReason::unmap, cleanup_phase(), unmap.error_number));
    }
    const auto close = state.operations.close(state.operations.context, state.descriptor);
    state.descriptor = -1;
    if (close.value != 0) {
        if (close.error_number <= 0)
            std::terminate();
        auto value = failure(
            PairwiseFailureCode::platform_perf_context_switch_close_failed,
            PairwiseFailureOperation::retire_perf_context_switch,
            PairwiseFailureReason::close, cleanup_phase(), close.error_number);
        if (result.primary)
            result.cleanup.emplace(std::move(value));
        else
            result.primary.emplace(std::move(value));
    }
    state.active = false;
    return result;
}

auto PairwisePlatformLeafFacade::read_task_page_fault_counters(
    platform_linux::LinuxThreadId thread,
    PairwiseTaskStatWorkspace& workspace) const noexcept
    -> std::expected<platform_linux::LinuxTaskPageFaultCounters,
                     PairwiseTaskStatReadFailure> {
    if (dependencies_.task_stat.open == nullptr ||
        dependencies_.task_stat.read == nullptr ||
        dependencies_.task_stat.close == nullptr) {
        auto result = platform_linux::detail::TaskStatAccess::read_bounded(
            thread, workspace.bytes_, dependencies_.task_stat);
        return std::unexpected{PairwiseTaskStatReadFailure{
            task_stat_failure(result.error().primary), std::nullopt}};
    }
    auto operations = dependencies_.task_stat;
    struct Forward final {
        PairwiseTaskStatOperations original;
        const PairwisePlatformLeafDependencies* dependencies;
    };
    Forward forward{dependencies_.task_stat, &dependencies_};
    operations.context = &forward;
    operations.before = [](void* raw) {
        before(*static_cast<Forward*>(raw)->dependencies,
               PairwisePlatformLeafOperation::task_stat);
    };
    operations.open = [](void* raw, platform_linux::LinuxThreadId value) noexcept {
        auto& original = static_cast<Forward*>(raw)->original;
        return original.open(original.context, value);
    };
    operations.read = [](void* raw, int descriptor, void* output,
                         std::size_t size) noexcept {
        auto& original = static_cast<Forward*>(raw)->original;
        return original.read(original.context, descriptor, output, size);
    };
    operations.close = [](void* raw, int descriptor) noexcept {
        auto& original = static_cast<Forward*>(raw)->original;
        return original.close(original.context, descriptor);
    };
    auto result = platform_linux::detail::TaskStatAccess::read_bounded(
        thread, workspace.bytes_, operations);
    if (result)
        return *result;
    PairwiseTaskStatReadFailure mapped{task_stat_failure(result.error().primary),
                                       std::nullopt};
    if (result.error().cleanup)
        mapped.cleanup.emplace(task_stat_failure(*result.error().cleanup));
    return std::unexpected{std::move(mapped)};
}

PairwisePreparedTemperatureSource::~PairwisePreparedTemperatureSource() noexcept = default;

auto PairwisePlatformLeafFacade::discover_hwmon_temperature(
    std::uint64_t root_entries, std::uint64_t directory_count,
    std::span<const PairwiseHwmonDiscoveryEntry> entries,
    std::uint64_t maximum_directories, std::uint64_t entries_per_directory,
    std::uint64_t token_bytes, PairwiseBoundedTextReadOperations operations,
    std::optional<int> enumeration_error) const noexcept
    -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                     PairwisePlatformFailure> {
    const auto reject = [](PairwiseFailureCode code, PairwiseFailureReason reason,
                           PairwiseFailureContext context,
                           std::optional<int> native = std::nullopt) {
        return std::unexpected{failure(code, PairwiseFailureOperation::hwmon_temperature,
                                       reason, std::move(context), native)};
    };
    try {
        if (enumeration_error)
            return reject(PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
                          PairwiseFailureReason::enumerate, temperature_phase(),
                          enumeration_error);
        if (operations.read == nullptr)
            return reject(PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
                          PairwiseFailureReason::enumerate, temperature_phase());

        auto selected = platform_linux::detail::HwmonTemperatureAccess::discover_bounded(
            root_entries, directory_count, entries, maximum_directories,
            entries_per_directory, token_bytes);
        if (!selected) {
            using Kind = platform_linux::detail::BoundedHwmonDiscoveryFailure;
            switch (selected.error()) {
            case Kind::enumeration_capacity:
                return reject(
                    PairwiseFailureCode::
                        platform_hwmon_temperature_enumeration_capacity_exceeded,
                    PairwiseFailureReason::enumeration_capacity_exceeded, prepare_phase());
            case Kind::token_capacity:
                return reject(
                    PairwiseFailureCode::platform_hwmon_temperature_token_capacity_exceeded,
                    PairwiseFailureReason::token_capacity_exceeded, prepare_phase());
            case Kind::ambiguous:
                return reject(
                    PairwiseFailureCode::platform_hwmon_temperature_ambiguous_sensor,
                    PairwiseFailureReason::ambiguous, temperature_phase());
            }
        }
        if (!*selected)
            return std::optional<PairwisePreparedTemperatureSource>{};
        return std::optional<PairwisePreparedTemperatureSource>{
            PairwisePreparedTemperatureSource{**selected, operations}};
    } catch (const std::bad_alloc&) {
        return reject(PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
                      PairwiseFailureReason::enumerate, temperature_phase());
    } catch (...) {
        return reject(PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
                      PairwiseFailureReason::enumerate, temperature_phase());
    }
}

auto PairwisePlatformLeafFacade::read_hwmon_temperature(
    const PairwisePreparedTemperatureSource& source,
    PairwiseSysfsReadWorkspace& workspace) const noexcept
    -> std::expected<std::int64_t, PairwisePlatformFailure> {
    if (source.owned_) {
        std::array<platform_linux::detail::BoundedHwmonSourceFailure, 1U> cleanup{};
        const auto value = source.owned_->read(workspace.bytes_, cleanup);
        if (value)
            return *value;
        if (value.error().primary.kind ==
            platform_linux::detail::BoundedHwmonSourceFailureKind::value)
            return std::unexpected{failure(
                PairwiseFailureCode::platform_hwmon_temperature_invalid_value,
                PairwiseFailureOperation::hwmon_temperature,
                PairwiseFailureReason::value, temperature_phase())};
        return std::unexpected{failure(
            PairwiseFailureCode::platform_hwmon_temperature_read_failed,
            PairwiseFailureOperation::hwmon_temperature, PairwiseFailureReason::read,
            temperature_phase(), value.error().primary.native_error > 0 ? value.error().primary.native_error
                                                           : EOVERFLOW)};
    }
    const auto read = source.operations_.read(
        source.operations_.context, source.locator_, workspace.bytes_);
    if (read.error_number != 0 || read.overflow ||
        read.count > workspace.content_capacity_)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_hwmon_temperature_read_failed,
            PairwiseFailureOperation::hwmon_temperature, PairwiseFailureReason::read,
            temperature_phase(), read.error_number > 0 ? read.error_number : EOVERFLOW)};
    auto value = platform_linux::detail::HwmonTemperatureAccess::parse_bounded(
        std::string_view{workspace.bytes_.data(), read.count});
    if (!value)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_hwmon_temperature_invalid_value,
            PairwiseFailureOperation::hwmon_temperature, PairwiseFailureReason::value,
            temperature_phase())};
    return *value;
}

auto PairwisePlatformLeafFacade::retire_hwmon_temperature(
    PairwisePreparedTemperatureSource source,
    std::span<platform_linux::detail::BoundedSysfsDisposition> dispositions)
    const noexcept -> std::size_t {
    if (!source.owned_)
        return 0U;
    return source.owned_->retire(dispositions);
}

auto PairwisePlatformLeafFacade::discover_hwmon_temperature(
    platform_linux::detail::BoundedHwmonCapacity capacity,
    platform_linux::detail::BoundedSysfsOperations operations,
    platform_linux::detail::BoundedHwmonValidationGate gate) const noexcept
    -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                     PairwiseSysfsPreparationFailure> {
    auto prepared = prepare_hwmon_discovery_storage(capacity, operations);
    if (!prepared)
        return std::unexpected{std::move(prepared.error())};
    return discover_hwmon_temperature_prepared(std::move(*prepared), gate);
}

auto PairwisePlatformLeafFacade::prepare_hwmon_discovery_storage(
    platform_linux::detail::BoundedHwmonCapacity capacity,
    platform_linux::detail::BoundedSysfsOperations operations) const noexcept
    -> std::expected<PairwisePreparedHwmonDiscoveryStorage,
                     PairwiseSysfsPreparationFailure> {
    try {
        auto mapped_cleanup = std::vector<PairwisePlatformFailure>{};
        mapped_cleanup.reserve(3U);
        auto lower = platform_linux::detail::
            PreparedBoundedHwmonDiscoveryStorage::prepare(capacity, operations);
        if (!lower)
            return std::unexpected{mapped_hwmon_failure(
                lower.error().primary, {}, std::move(mapped_cleanup))};
        return PairwisePreparedHwmonDiscoveryStorage{
            std::move(*lower), std::move(mapped_cleanup)};
    } catch (...) {
        return std::unexpected{PairwiseSysfsPreparationFailure{
            failure(PairwiseFailureCode::platform_hwmon_temperature_discovery_failed,
                    PairwiseFailureOperation::hwmon_temperature,
                    PairwiseFailureReason::enumerate, temperature_phase()),
            {}}};
    }
}

auto PairwisePlatformLeafFacade::discover_hwmon_temperature_prepared(
    PairwisePreparedHwmonDiscoveryStorage&& prepared,
    platform_linux::detail::BoundedHwmonValidationGate gate) const noexcept
    -> std::expected<std::optional<PairwisePreparedTemperatureSource>,
                     PairwiseSysfsPreparationFailure> {
    if (std::exchange(prepared.consumed_, true))
        return std::unexpected{mapped_hwmon_failure(
            {platform_linux::detail::BoundedHwmonSourceFailureKind::enumerate,
             0, platform_linux::detail::BoundedSysfsDispositionKind::success},
            {}, std::move(prepared.mapped_cleanup_))};
    auto source = platform_linux::detail::BoundedHwmonTemperatureSource::
        discover_prepared(std::move(prepared.lower_), gate);
    if (!source) {
        const auto sequence = platform_linux::detail::
            PreparedBoundedHwmonDiscoveryAccess::sequence(source.error());
        const auto cleanup = platform_linux::detail::
            PreparedBoundedHwmonDiscoveryAccess::cleanup(source.error());
        if (!sequence || !cleanup)
            std::terminate();
        return std::unexpected{mapped_hwmon_failure(
            sequence->primary, *cleanup, std::move(prepared.mapped_cleanup_))};
    }
    if (!*source)
        return std::optional<PairwisePreparedTemperatureSource>{};
    return std::optional<PairwisePreparedTemperatureSource>{
        PairwisePreparedTemperatureSource{std::move(**source)}};
}

auto PairwisePlatformLeafFacade::discover_cpu_frequency(
    core::LinuxCpuId cpu,
    std::span<const platform_linux::detail::CpuFrequencyPolicyView> policies,
    std::uint64_t maximum_policies, std::uint64_t maximum_memberships,
    std::uint64_t token_bytes, PairwiseBoundedTextReadOperations operations,
    platform_linux::detail::CpuFrequencyBoundedScratch& scratch,
    std::optional<int> enumeration_error) const noexcept
    -> std::expected<std::optional<PairwisePreparedFrequencySource>,
                     PairwisePlatformFailure> {
    const auto reject = [](PairwiseFailureCode code, PairwiseFailureReason reason,
                           PairwiseFailureContext context,
                           std::optional<int> native = std::nullopt) {
        return std::unexpected{failure(code, PairwiseFailureOperation::cpu_frequency,
                                       reason, std::move(context), native)};
    };
    try {
        if (enumeration_error)
            return reject(PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
                          PairwiseFailureReason::enumerate, frequency_phase(),
                          enumeration_error);
        if (operations.read == nullptr)
            return reject(PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
                          PairwiseFailureReason::metadata, frequency_phase());
        auto projected = platform_linux::detail::CpuFrequencyAccess::discover_bounded(
            cpu, policies, maximum_policies, maximum_memberships, token_bytes, scratch);
        if (!projected) {
            using DiscoveryKind =
                platform_linux::detail::CpuFrequencyBoundedDiscoveryFailureKind;
            using SemanticKind = platform_linux::detail::CpuFrequencyBoundedFailure;
            if (projected.error().kind == DiscoveryKind::enumeration_capacity)
                return reject(
                    PairwiseFailureCode::platform_cpu_frequency_enumeration_capacity_exceeded,
                    PairwiseFailureReason::enumeration_capacity_exceeded, prepare_phase());
            if (projected.error().kind == DiscoveryKind::token_capacity)
                return reject(
                    PairwiseFailureCode::platform_cpu_frequency_token_capacity_exceeded,
                    PairwiseFailureReason::token_capacity_exceeded, prepare_phase());
            switch (projected.error().semantic) {
            case SemanticKind::capacity:
                return reject(
                    PairwiseFailureCode::platform_cpu_frequency_enumeration_capacity_exceeded,
                    PairwiseFailureReason::enumeration_capacity_exceeded, prepare_phase());
            case SemanticKind::ambiguous:
                return reject(PairwiseFailureCode::platform_cpu_frequency_ambiguous_policy,
                              PairwiseFailureReason::ambiguous, frequency_phase());
            case SemanticKind::membership:
                return reject(PairwiseFailureCode::platform_cpu_frequency_invalid_value,
                              PairwiseFailureReason::membership, frequency_phase());
            case SemanticKind::metadata:
                return reject(PairwiseFailureCode::platform_cpu_frequency_invalid_value,
                              PairwiseFailureReason::metadata, frequency_phase());
            case SemanticKind::value:
                return reject(PairwiseFailureCode::platform_cpu_frequency_invalid_value,
                              PairwiseFailureReason::value, frequency_phase());
            }
        }
        if (!projected->value)
            return std::optional<PairwisePreparedFrequencySource>{};
        const auto& value = *projected->value;
        auto cpus = std::vector<core::LinuxCpuId>{value.related_cpus.begin(),
                                                 value.related_cpus.end()};
        return std::optional<PairwisePreparedFrequencySource>{
            PairwisePreparedFrequencySource{
                std::move(cpus), value.kind, std::string{value.driver},
                std::string{value.governor}, value.minimum_hz, value.maximum_hz,
                value.locator, operations}};
    } catch (const std::bad_alloc&) {
        return reject(PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
                      PairwiseFailureReason::enumerate, frequency_phase());
    } catch (...) {
        return reject(PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
                      PairwiseFailureReason::enumerate, frequency_phase());
    }
}

auto PairwisePlatformLeafFacade::read_cpu_frequency(
    const PairwisePreparedFrequencySource& source,
    PairwiseSysfsReadWorkspace& workspace) const noexcept
    -> std::expected<std::uint64_t, PairwisePlatformFailure> {
    const auto read = source.operations_.read(
        source.operations_.context, source.locator_, workspace.bytes_);
    if (read.error_number != 0 || read.overflow ||
        read.count > workspace.content_capacity_)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_cpu_frequency_read_failed,
            PairwiseFailureOperation::cpu_frequency, PairwiseFailureReason::read,
            frequency_phase(), read.error_number > 0 ? read.error_number : EOVERFLOW)};
    auto value = platform_linux::detail::CpuFrequencyAccess::parse_khz_bounded(
        std::string_view{workspace.bytes_.data(), read.count});
    if (!value)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_cpu_frequency_invalid_value,
            PairwiseFailureOperation::cpu_frequency, PairwiseFailureReason::value,
            frequency_phase())};
    return *value;
}

auto PairwisePlatformLeafFacade::retire_cpu_frequencies(
    PairwisePreparedFrequencySources sources,
    std::span<platform_linux::detail::BoundedSysfsDisposition> dispositions)
    const noexcept -> std::size_t {
    return sources.sources_.retire(dispositions);
}

auto PairwisePlatformLeafFacade::discover_cpu_frequencies(
    platform_linux::detail::BoundedCpuFrequencyCapacity capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    platform_linux::detail::BoundedSysfsOperations operations,
    platform_linux::detail::BoundedCpuFrequencyValidationGate gate) const noexcept
    -> std::expected<std::optional<PairwisePreparedFrequencySources>,
                     PairwiseSysfsPreparationFailure> {
    auto prepared = prepare_cpu_frequency_discovery_storage(
        capacity, present_cpus, worker_cpus, operations);
    if (!prepared)
        return std::unexpected{std::move(prepared.error())};
    return discover_cpu_frequencies_prepared(std::move(*prepared), gate);
}

auto PairwisePlatformLeafFacade::prepare_cpu_frequency_discovery_storage(
    platform_linux::detail::BoundedCpuFrequencyCapacity capacity,
    std::span<const core::LinuxCpuId> present_cpus,
    std::span<const core::LinuxCpuId> worker_cpus,
    platform_linux::detail::BoundedSysfsOperations operations) const noexcept
    -> std::expected<PairwisePreparedCpuFrequencyDiscoveryStorage,
                     PairwiseSysfsPreparationFailure> {
    try {
        if (capacity.maximum_policies >
            (std::numeric_limits<std::size_t>::max() - 1U) / 2U)
            return std::unexpected{PairwiseSysfsPreparationFailure{
                failure(
                    PairwiseFailureCode::platform_cpu_frequency_enumeration_capacity_exceeded,
                    PairwiseFailureOperation::cpu_frequency,
                    PairwiseFailureReason::enumeration_capacity_exceeded,
                    prepare_phase()),
                {}}};
        const auto cleanup_capacity =
            static_cast<std::size_t>(2U * capacity.maximum_policies + 1U);
        auto mapped_cleanup = std::vector<PairwisePlatformFailure>{};
        mapped_cleanup.reserve(cleanup_capacity);
        auto lower = platform_linux::detail::
            PreparedBoundedCpuFrequencyDiscoveryStorage::prepare(
                capacity, present_cpus, worker_cpus, operations);
        if (!lower)
            return std::unexpected{mapped_cpu_frequency_failure(
                lower.error().primary, {}, std::move(mapped_cleanup))};
        return PairwisePreparedCpuFrequencyDiscoveryStorage{
            std::move(*lower), std::move(mapped_cleanup)};
    } catch (...) {
        return std::unexpected{PairwiseSysfsPreparationFailure{
            failure(PairwiseFailureCode::platform_cpu_frequency_discovery_failed,
                    PairwiseFailureOperation::cpu_frequency,
                    PairwiseFailureReason::enumerate, frequency_phase()),
            {}}};
    }
}

auto PairwisePlatformLeafFacade::discover_cpu_frequencies_prepared(
    PairwisePreparedCpuFrequencyDiscoveryStorage&& prepared,
    platform_linux::detail::BoundedCpuFrequencyValidationGate gate) const noexcept
    -> std::expected<std::optional<PairwisePreparedFrequencySources>,
                     PairwiseSysfsPreparationFailure> {
    if (std::exchange(prepared.consumed_, true))
        return std::unexpected{mapped_cpu_frequency_failure(
            {platform_linux::detail::BoundedCpuFrequencySourceFailureKind::enumerate,
             0, platform_linux::detail::BoundedSysfsDispositionKind::success},
            {}, std::move(prepared.mapped_cleanup_))};
    auto sources = platform_linux::detail::BoundedCpuFrequencySources::
        discover_prepared(std::move(prepared.lower_), gate);
    if (!sources) {
        const auto sequence = platform_linux::detail::
            PreparedBoundedCpuFrequencyDiscoveryAccess::sequence(
                sources.error());
        const auto cleanup = platform_linux::detail::
            PreparedBoundedCpuFrequencyDiscoveryAccess::cleanup(
                sources.error());
        if (!sequence || !cleanup)
            std::terminate();
        return std::unexpected{mapped_cpu_frequency_failure(
            sequence->primary, *cleanup, std::move(prepared.mapped_cleanup_))};
    }
    if (!*sources)
        return std::optional<PairwisePreparedFrequencySources>{};
    return std::optional<PairwisePreparedFrequencySources>{
        PairwisePreparedFrequencySources{std::move(**sources)}};
}

auto PairwisePlatformLeafFacade::read_cpu_frequency(
    const PairwisePreparedFrequencySources& sources, std::size_t domain,
    PairwiseSysfsReadWorkspace& workspace) const noexcept
    -> std::expected<std::uint64_t, PairwisePlatformFailure> {
    std::array<platform_linux::detail::BoundedCpuFrequencySourceFailure, 1U> cleanup{};
    const auto value = sources.sources_.read(domain, workspace.bytes_, cleanup);
    if (value)
        return *value;
    if (value.error().primary.kind ==
        platform_linux::detail::BoundedCpuFrequencySourceFailureKind::value)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_cpu_frequency_invalid_value,
            PairwiseFailureOperation::cpu_frequency, PairwiseFailureReason::value,
            frequency_phase())};
    return std::unexpected{failure(
        PairwiseFailureCode::platform_cpu_frequency_read_failed,
        PairwiseFailureOperation::cpu_frequency, PairwiseFailureReason::read,
        frequency_phase(), value.error().primary.native > 0 ? value.error().primary.native
                                                     : EIO)};
}

auto PairwisePlatformLeafFacade::create(PairwisePlatformLeafDependencies dependencies) noexcept
    -> PairwisePlatformLeafFacade {
    return PairwisePlatformLeafFacade{dependencies};
}

auto PairwisePlatformLeafFacade::raw_monotonic_now() const noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, PairwisePlatformFailure> {
    try {
        before(dependencies_, PairwisePlatformLeafOperation::raw_clock);
        const auto result = dependencies_.clock.clock_gettime_raw(
            dependencies_.clock.context, CLOCK_MONOTONIC_RAW);
        if (result.native_result == -1 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_raw_clock_read_failed,
                                           PairwiseFailureOperation::raw_monotonic_now,
                                           PairwiseFailureReason::read_failed,
                                           NoPairwiseFailureContext{}, result.error_number)};
        if (result.native_result != 0 || result.error_number != 0)
            std::terminate();
        if constexpr (std::is_signed_v<time_t>) {
            if (result.timestamp.tv_sec < 0)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_raw_clock_invalid_timestamp,
                    PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none)};
        }
        if (result.timestamp.tv_nsec < 0 ||
            result.timestamp.tv_nsec >= static_cast<decltype(result.timestamp.tv_nsec)>(
                                                nanoseconds_per_second))
            return std::unexpected{failure(
                PairwiseFailureCode::platform_raw_clock_invalid_timestamp,
                PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none)};
        if (!std::in_range<std::uint64_t>(result.timestamp.tv_sec))
            return std::unexpected{failure(
                PairwiseFailureCode::platform_raw_clock_conversion_failed,
                PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none)};
        const auto seconds = static_cast<std::uint64_t>(result.timestamp.tv_sec);
        if (seconds > std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second)
            return std::unexpected{failure(
                PairwiseFailureCode::platform_raw_clock_conversion_failed,
                PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none)};
        const auto base = seconds * nanoseconds_per_second;
        const auto nanoseconds = static_cast<std::uint64_t>(result.timestamp.tv_nsec);
        if (nanoseconds > std::numeric_limits<std::uint64_t>::max() - base)
            return std::unexpected{failure(
                PairwiseFailureCode::platform_raw_clock_conversion_failed,
                PairwiseFailureOperation::raw_monotonic_now, PairwiseFailureReason::none)};
        return platform_linux::RawMonotonicTimestamp::from_nanoseconds(base + nanoseconds);
    } catch (const std::exception&) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_raw_clock_contained_exception,
            PairwiseFailureOperation::raw_monotonic_now,
            PairwiseFailureReason::contained_exception)};
    } catch (...) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_raw_clock_contained_unknown_failure,
            PairwiseFailureOperation::raw_monotonic_now,
            PairwiseFailureReason::contained_unknown_failure)};
    }
}

auto PairwisePlatformLeafFacade::current_thread_id() const noexcept
    -> std::expected<platform_linux::LinuxThreadId, PairwisePlatformFailure> {
    try {
        before(dependencies_, PairwisePlatformLeafOperation::thread_identity);
        const auto result = dependencies_.threads.get_thread_id(dependencies_.threads.context);
        if (result.value <= 0) {
            if (result.error_number > 0)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_thread_identity_query_failed,
                    PairwiseFailureOperation::thread_identity,
                    PairwiseFailureReason::query_failed, NoPairwiseFailureContext{},
                    result.error_number)};
            return std::unexpected{failure(
                PairwiseFailureCode::platform_thread_identity_invalid_value,
                PairwiseFailureOperation::thread_identity, PairwiseFailureReason::none)};
        }
        const auto value = static_cast<std::uint64_t>(result.value);
        if (value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
            return std::unexpected{failure(
                PairwiseFailureCode::platform_thread_identity_invalid_value,
                PairwiseFailureOperation::thread_identity, PairwiseFailureReason::none)};
        return platform_linux::detail::ThreadIdentityAccess::from_validated(value);
    } catch (const std::exception&) {
        return std::unexpected{failure(PairwiseFailureCode::platform_thread_contained_exception,
                                       PairwiseFailureOperation::thread_identity,
                                       PairwiseFailureReason::contained_exception)};
    } catch (...) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_thread_contained_unknown_failure,
            PairwiseFailureOperation::thread_identity,
            PairwiseFailureReason::contained_unknown_failure)};
    }
}

auto PairwisePlatformLeafFacade::current_cpu() const noexcept
    -> std::expected<core::LinuxCpuId, PairwisePlatformFailure> {
    const auto result = dependencies_.threads.get_current_cpu(dependencies_.threads.context);
    if (result.value < 0 && result.error_number > 0)
        return std::unexpected{failure(PairwiseFailureCode::platform_current_cpu_query_failed,
                                       PairwiseFailureOperation::current_cpu,
                                       PairwiseFailureReason::query_failed,
                                       NoPairwiseFailureContext{}, result.error_number)};
    if (result.value < 0 ||
        static_cast<std::uint64_t>(result.value) > std::numeric_limits<std::uint32_t>::max())
        std::terminate();
    return core::LinuxCpuId{static_cast<std::uint32_t>(result.value)};
}

auto PairwisePlatformLeafFacade::set_current_thread_affinity(const core::CpuSet& cpus) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    if (cpus.empty())
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_empty_request,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::set_affinity);
        auto highest = std::uint32_t{};
        for (const auto cpu : cpus)
            highest = cpu.value();
        auto mask = cpu_mask(dependencies_, static_cast<std::size_t>(highest) + 1U,
                             PairwiseFailureOperation::set_affinity);
        if (!mask)
            return std::unexpected{std::move(mask.error())};
        for (const auto cpu : cpus) {
            if (!mask->set(cpu) || !mask->contains(cpu))
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_conversion_failed,
                    PairwiseFailureOperation::set_affinity, PairwiseFailureReason::none)};
        }
        const auto& const_mask = *mask;
        const auto result = dependencies_.threads.set_affinity(dependencies_.threads.context,
                                                                const_mask.native_view());
        if (result.value == 0 && result.error_number == 0)
            return {};
        if (result.value != 0 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_affinity_set_failed,
                                           PairwiseFailureOperation::set_affinity,
                                           PairwiseFailureReason::none,
                                           NoPairwiseFailureContext{}, result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_affinity_mask_allocation_failed,
            PairwiseFailureOperation::set_affinity, PairwiseFailureReason::allocation_failed)};
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    }
}

auto PairwisePlatformLeafFacade::get_current_thread_affinity() const noexcept
    -> std::expected<core::CpuSet, PairwisePlatformFailure> {
    auto requested = static_cast<std::uint64_t>(CPU_SETSIZE);
    try {
        before(dependencies_, PairwisePlatformLeafOperation::get_affinity);
        for (;;) {
            if (requested > dependencies_.affinity_capacity_limit ||
                requested > std::numeric_limits<std::size_t>::max())
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_readback_too_large,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
            auto mask = cpu_mask(dependencies_, static_cast<std::size_t>(requested),
                                 PairwiseFailureOperation::get_affinity);
            if (!mask)
                return std::unexpected{std::move(mask.error())};
            const auto result = dependencies_.threads.get_affinity(dependencies_.threads.context,
                                                                    mask->native_view());
            if (result.value == 0 && result.error_number == 0) {
                std::string ranges;
                for (std::size_t index = 0; index < mask->capacity(); ++index) {
                    if (!mask->contains(core::LinuxCpuId{static_cast<std::uint32_t>(index)}))
                        continue;
                    if (!ranges.empty())
                        ranges.push_back(',');
                    ranges += std::to_string(index);
                }
                if (ranges.empty())
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_affinity_conversion_failed,
                        PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
                auto converted = core::CpuSet::parse(ranges);
                if (!converted)
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_affinity_conversion_failed,
                        PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
                return std::move(*converted);
            }
            if (result.error_number != EINVAL) {
                if (result.error_number <= 0)
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_affinity_conversion_failed,
                        PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_get_failed,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none,
                    NoPairwiseFailureContext{}, result.error_number)};
            }
            if (requested == dependencies_.affinity_capacity_limit ||
                requested > dependencies_.affinity_capacity_limit / 2U)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_readback_too_large,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
            requested *= 2U;
        }
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::get_affinity,
                                       PairwiseFailureReason::none)};
    }
}

auto PairwisePlatformLeafFacade::prepare_affinity_workspace(
    std::size_t minimum_capacity) const noexcept
    -> std::expected<PairwisePreparedAffinityWorkspace, PairwisePlatformFailure> {
    auto requested = std::max<std::uint64_t>(CPU_SETSIZE, minimum_capacity);
    try {
        before(dependencies_, PairwisePlatformLeafOperation::get_affinity);
        for (;;) {
            if (requested > dependencies_.affinity_capacity_limit ||
                requested > std::numeric_limits<std::size_t>::max())
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_readback_too_large,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
            auto mask = cpu_mask(dependencies_, static_cast<std::size_t>(requested),
                                 PairwiseFailureOperation::get_affinity);
            if (!mask)
                return std::unexpected{std::move(mask.error())};
            const auto result = dependencies_.threads.get_affinity(
                dependencies_.threads.context, mask->native_view());
            if (result.value == 0 && result.error_number == 0) {
                mask->zero();
                return PairwisePreparedAffinityWorkspace{std::move(*mask)};
            }
            if (result.error_number != EINVAL) {
                if (result.error_number <= 0)
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_affinity_conversion_failed,
                        PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_get_failed,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none,
                    NoPairwiseFailureContext{}, result.error_number)};
            }
            if (requested == dependencies_.affinity_capacity_limit ||
                requested > dependencies_.affinity_capacity_limit / 2U)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_readback_too_large,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
            requested *= 2U;
        }
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::get_affinity,
                                       PairwiseFailureReason::none)};
    }
}

auto PairwisePlatformLeafFacade::prepare_affinity_workspace_exact(
    std::size_t capacity) const noexcept
    -> std::expected<PairwisePreparedAffinityWorkspace, PairwisePlatformFailure> {
    if (capacity == 0U || capacity > dependencies_.affinity_capacity_limit)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_affinity_readback_too_large,
            PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
    auto mask = cpu_mask(dependencies_, capacity,
                         PairwiseFailureOperation::get_affinity);
    if (!mask)
        return std::unexpected{std::move(mask.error())};
    if (mask->capacity() != capacity)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_affinity_conversion_failed,
            PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
    mask->zero();
    return PairwisePreparedAffinityWorkspace{std::move(*mask)};
}

auto PairwisePlatformLeafFacade::set_current_thread_affinity_prepared(
    core::LinuxCpuId cpu, PairwisePreparedAffinityWorkspace& workspace) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    if (static_cast<std::uint64_t>(cpu.value()) >= workspace.mask_.capacity())
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::set_affinity);
        workspace.mask_.zero();
        if (!workspace.mask_.set(cpu))
            return std::unexpected{failure(
                PairwiseFailureCode::platform_affinity_conversion_failed,
                PairwiseFailureOperation::set_affinity, PairwiseFailureReason::none)};
        const auto& mask = workspace.mask_;
        const auto result = dependencies_.threads.set_affinity(
            dependencies_.threads.context, mask.native_view());
        if (result.value == 0 && result.error_number == 0)
            return {};
        if (result.value != 0 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_affinity_set_failed,
                                           PairwiseFailureOperation::set_affinity,
                                           PairwiseFailureReason::none,
                                           NoPairwiseFailureContext{}, result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::set_affinity,
                                       PairwiseFailureReason::none)};
    }
}

auto PairwisePlatformLeafFacade::current_thread_affinity_is_singleton_prepared(
    core::LinuxCpuId cpu, PairwisePreparedAffinityWorkspace& workspace) const noexcept
    -> std::expected<bool, PairwisePlatformFailure> {
    try {
        before(dependencies_, PairwisePlatformLeafOperation::get_affinity);
        workspace.mask_.zero();
        const auto result = dependencies_.threads.get_affinity(
            dependencies_.threads.context, workspace.mask_.native_view());
        if (result.value != 0 || result.error_number != 0) {
            if (result.error_number == EINVAL)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_readback_too_large,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
            if (result.error_number > 0)
                return std::unexpected{failure(
                    PairwiseFailureCode::platform_affinity_get_failed,
                    PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none,
                    NoPairwiseFailureContext{}, result.error_number)};
            return std::unexpected{failure(
                PairwiseFailureCode::platform_affinity_conversion_failed,
                PairwiseFailureOperation::get_affinity, PairwiseFailureReason::none)};
        }
        if (static_cast<std::uint64_t>(cpu.value()) >= workspace.mask_.capacity())
            return false;
        if (!workspace.mask_.contains(cpu))
            return false;
        for (std::size_t index = 0; index < workspace.mask_.capacity(); ++index) {
            if (index == cpu.value())
                continue;
            if (workspace.mask_.contains(
                    core::LinuxCpuId{static_cast<std::uint32_t>(index)}))
                return false;
        }
        return true;
    } catch (...) {
        return std::unexpected{failure(PairwiseFailureCode::platform_affinity_conversion_failed,
                                       PairwiseFailureOperation::get_affinity,
                                       PairwiseFailureReason::none)};
    }
}

auto PairwisePlatformLeafFacade::map(std::size_t length) const noexcept
    -> std::expected<platform_linux::AnonymousMapping, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::map_memory;
    if (length == 0U)
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_invalid_length,
                                       operation, PairwiseFailureReason::zero_length)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::map);
        const auto result = dependencies_.mapping.map_anonymous(
            dependencies_.mapping.context, length, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result.address == MAP_FAILED && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_mapping_create_failed,
                                           operation, map_create_reason(result.error_number),
                                           NoPairwiseFailureContext{}, result.error_number)};
        if (result.address == MAP_FAILED || result.address == nullptr || result.error_number != 0) {
            if (result.address != MAP_FAILED && result.address != nullptr)
                static_cast<void>(dependencies_.mapping.unmap(
                    dependencies_.mapping.context, result.address, length));
            return std::unexpected{failure(PairwiseFailureCode::platform_mapping_unexpected_result,
                                           operation,
                                           PairwiseFailureReason::unexpected_native_result)};
        }
        const auto start = reinterpret_cast<std::uintptr_t>(result.address);
        if (length - 1U > std::numeric_limits<std::uintptr_t>::max() - start) {
            static_cast<void>(dependencies_.mapping.unmap(
                dependencies_.mapping.context, result.address, length));
            return std::unexpected{failure(PairwiseFailureCode::platform_mapping_unexpected_result,
                                           operation,
                                           PairwiseFailureReason::address_range_overflow)};
        }
        return platform_linux::detail::AnonymousMappingAccess::adopt(
            result.address, length, dependencies_.mapping);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_create_failed,
                                       operation, PairwiseFailureReason::allocation_failure)};
    } catch (const std::exception&) {
        return std::unexpected{contained_mapping(operation, false)};
    } catch (...) {
        return std::unexpected{contained_mapping(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::advise_no_huge_pages(
    platform_linux::AnonymousMapping& mapping) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::advise_memory;
    if (!mapping.active())
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_invalid_state,
                                       operation, PairwiseFailureReason::inactive_mapping,
                                       mapping_advice_context())};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::advise);
        const auto result = platform_linux::detail::AnonymousMappingAccess::advise(mapping)(
            platform_linux::detail::AnonymousMappingAccess::operation_context(mapping),
            platform_linux::detail::AnonymousMappingAccess::address(mapping), mapping.size(),
            MADV_NOHUGEPAGE);
        if (result.value == 0 && result.error_number == 0)
            return {};
        if (result.value == -1 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_mapping_advice_failed,
                                           operation, mapping_advice_reason(result.error_number),
                                           mapping_advice_context(), result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_unexpected_result,
                                       operation,
                                       PairwiseFailureReason::unexpected_native_result,
                                       mapping_advice_context())};
    } catch (const std::exception&) {
        return std::unexpected{contained_mapping(operation, false)};
    } catch (...) {
        return std::unexpected{contained_mapping(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::protect_read_only(
    platform_linux::AnonymousMapping& mapping) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::protect_memory;
    if (!mapping.active())
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_invalid_state,
                                       operation, PairwiseFailureReason::inactive_mapping,
                                       mapping_protection_context())};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::protect);
        const auto result = platform_linux::detail::AnonymousMappingAccess::protect(mapping)(
            platform_linux::detail::AnonymousMappingAccess::operation_context(mapping),
            platform_linux::detail::AnonymousMappingAccess::address(mapping), mapping.size(),
            PROT_READ);
        if (result.value == 0 && result.error_number == 0) {
            platform_linux::detail::AnonymousMappingAccess::set_protection(
                mapping, platform_linux::MemoryProtection::read_only);
            return {};
        }
        if (result.value == -1 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_mapping_protect_failed,
                                           operation, mapping_protect_reason(result.error_number),
                                           mapping_protection_context(), result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_unexpected_result,
                                       operation,
                                       PairwiseFailureReason::unexpected_native_result,
                                       mapping_protection_context())};
    } catch (const std::exception&) {
        return std::unexpected{contained_mapping(operation, false)};
    } catch (...) {
        return std::unexpected{contained_mapping(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::unmap(platform_linux::AnonymousMapping& mapping) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::unmap_memory;
    if (!mapping.active())
        return {};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::unmap);
        const auto result = platform_linux::detail::AnonymousMappingAccess::unmap(mapping)(
            platform_linux::detail::AnonymousMappingAccess::operation_context(mapping),
            platform_linux::detail::AnonymousMappingAccess::address(mapping), mapping.size());
        if (result.value == 0 && result.error_number == 0) {
            if (dependencies_.retain_mapping_after_successful_unmap == nullptr ||
                !dependencies_.retain_mapping_after_successful_unmap(
                    dependencies_.hook_context))
                platform_linux::detail::AnonymousMappingAccess::clear(mapping);
            return {};
        }
        if (result.value == -1 && result.error_number > 0)
            return std::unexpected{failure(
                PairwiseFailureCode::platform_mapping_unmap_failed, operation,
                result.error_number == EINVAL ? PairwiseFailureReason::invalid_address_range
                                               : PairwiseFailureReason::cleanup_failure,
                NoPairwiseFailureContext{}, result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_mapping_unexpected_result,
                                       operation,
                                       PairwiseFailureReason::unexpected_native_result)};
    } catch (const std::exception&) {
        return std::unexpected{contained_mapping(operation, false)};
    } catch (...) {
        return std::unexpected{contained_mapping(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::numa_mask(const core::LinuxNumaNodeSet& nodes) const noexcept
    -> std::expected<platform_linux::NativeNumaMask, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::numa_mask;
    if (nodes.empty())
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_invalid_argument,
                                       operation, PairwiseFailureReason::empty_node_set)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::numa_mask);
        auto highest = nodes.begin();
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator)
            highest = iterator;
        const auto high = static_cast<std::uint64_t>((*highest).value());
        const auto maxnode = high + 1U;
        const auto native_bind_maxnode = maxnode + 1U;
        constexpr auto word_bits = static_cast<std::uint64_t>(sizeof(unsigned long) * CHAR_BIT);
        const auto word_count = native_bind_maxnode / word_bits +
                                (native_bind_maxnode % word_bits != 0U ? 1U : 0U);
        if (native_bind_maxnode > dependencies_.numa_mask_native_limit ||
            word_count > dependencies_.numa_mask_size_limit ||
            word_count > dependencies_.numa_mask_size_limit / sizeof(unsigned long))
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_mask_overflow,
                                           operation, PairwiseFailureReason::nodemask_overflow)};
        auto words = dependencies_.numa_words(static_cast<std::size_t>(word_count));
        if (words.size() != word_count)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                           operation, PairwiseFailureReason::allocation_failure)};
        std::fill(words.begin(), words.end(), 0UL);
        for (const auto node : nodes) {
            const auto value = node.value();
            words[value / word_bits] |= 1UL << (value % word_bits);
        }
        return platform_linux::detail::NumaAccess::adopt_mask(
            std::move(words), static_cast<unsigned long>(maxnode),
            static_cast<unsigned long>(native_bind_maxnode));
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                       operation, PairwiseFailureReason::allocation_failure)};
    } catch (const std::exception&) {
        return std::unexpected{contained_numa(operation, false)};
    } catch (...) {
        return std::unexpected{contained_numa(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::prepare_numa_mask(
    PairwiseLinuxNumaNodeSelection selection) const noexcept
    -> std::expected<platform_linux::NativeNumaMask, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::numa_mask;
    if (selection.count == 0U)
        return std::unexpected{failure(
            PairwiseFailureCode::platform_numa_invalid_argument, operation,
            PairwiseFailureReason::empty_node_set)};
    if (selection.context == nullptr || selection.at == nullptr)
        std::terminate();
    try {
        core::LinuxNumaNodeSet nodes;
        for (std::size_t index = 0; index < selection.count; ++index) {
            const auto node = selection.at(selection.context, index);
            nodes = nodes.set_union(core::LinuxNumaNodeSet{node});
        }
        return numa_mask(nodes);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(
            PairwiseFailureCode::platform_numa_conversion_failed, operation,
            PairwiseFailureReason::allocation_failure)};
    } catch (const std::exception&) {
        return std::unexpected{contained_numa(operation, false)};
    } catch (...) {
        return std::unexpected{contained_numa(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::bind(const platform_linux::AnonymousMapping& mapping,
                                      const platform_linux::NativeNumaMask& nodes) const noexcept
    -> std::expected<void, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::bind_memory;
    if (!mapping.active() || nodes.word_count() == 0U || nodes.maxnode() == 0U)
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_invalid_argument,
                                       operation, PairwiseFailureReason::invalid_mapping)};
    if (mapping.size() > dependencies_.numa_length_limit)
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                       operation, PairwiseFailureReason::length_out_of_range)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::numa_bind);
        const auto result = dependencies_.numa.bind(
            dependencies_.numa.context, const_cast<std::byte*>(mapping.data()),
            static_cast<unsigned long>(mapping.size()), MPOL_BIND | MPOL_F_STATIC_NODES,
            nodes.data(), platform_linux::detail::NumaAccess::native_bind_maxnode(nodes), 0U);
        if (result.value == 0 && result.error_number == 0)
            return {};
        if (result.value == -1 && result.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_bind_failed,
                                           operation, numa_bind_reason(result.error_number),
                                           NoPairwiseFailureContext{}, result.error_number)};
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_bind_failed, operation,
                                       PairwiseFailureReason::unexpected_native_result)};
    } catch (const std::exception&) {
        return std::unexpected{contained_numa(operation, false)};
    } catch (...) {
        return std::unexpected{contained_numa(operation, true)};
    }
}

auto PairwisePlatformLeafFacade::bind_diagnosed(
    const platform_linux::AnonymousMapping& mapping,
    const platform_linux::NativeNumaMask& nodes,
    platform_linux::detail::NumaDomainMaxNode domain,
    const platform_linux::detail::NumaBindInvocationFacts& facts,
    PairwiseNumaBindWorkspace& workspace) const noexcept
    -> std::expected<void, PairwiseDiagnosedNumaBindFailure> {
    constexpr auto operation = PairwiseFailureOperation::bind_memory;
    const auto failed = [](PairwisePlatformFailure primary,
                           std::optional<platform_linux::detail::NumaBindEinvalDiagnosis>
                               diagnosis = std::nullopt) {
        return std::unexpected{PairwiseDiagnosedNumaBindFailure{
            .primary = std::move(primary), .diagnosis = diagnosis}};
    };
    if (!mapping.active() || mapping.size() == 0U ||
        mapping.size() > dependencies_.numa_length_limit || nodes.maxnode() == 0U ||
        nodes.maxnode() > domain.value() || workspace.words.size() != domain.word_count() ||
        !workspace.einval_available || dependencies_.numa.bind == nullptr ||
        dependencies_.numa.allowed_mems == nullptr) {
        return failed(failure(PairwiseFailureCode::platform_numa_invalid_argument, operation,
                              PairwiseFailureReason::invalid_diagnostic_context));
    }
    try {
        before(dependencies_, PairwisePlatformLeafOperation::numa_bind);
        PairwiseNumaBindInvocation invocation{
            .address = const_cast<std::byte*>(mapping.data()),
            .length = mapping.size(),
            .base_page_bytes = facts.base_page_bytes(),
            .mode = MPOL_BIND | MPOL_F_STATIC_NODES,
            .flags = 0U,
            .selected_mask_nonempty = nodes.maxnode() != 0U,
            .selected_maxnode_within_domain = nodes.maxnode() <= domain.value(),
            .selected_nodes_effective = facts.selected_nodes_effective(),
            .selected_nodes_online = facts.selected_nodes_online(),
            .selected_nodes_memory_backed = facts.selected_nodes_memory_backed(),
        };
        if (dependencies_.adjust_numa_bind_invocation != nullptr)
            dependencies_.adjust_numa_bind_invocation(
                dependencies_.hook_context, invocation);
        const auto bound = dependencies_.numa.bind(
            dependencies_.numa.context, invocation.address,
            static_cast<unsigned long>(invocation.length), invocation.mode,
            nodes.data(), platform_linux::detail::NumaAccess::native_bind_maxnode(nodes),
            invocation.flags);
        if (bound.value == 0 && bound.error_number == 0)
            return {};
        if (bound.value != -1 || bound.error_number != EINVAL) {
            if (bound.value == -1 && bound.error_number > 0)
                return failed(failure(PairwiseFailureCode::platform_numa_bind_failed, operation,
                                      numa_bind_reason(bound.error_number),
                                      NoPairwiseFailureContext{}, bound.error_number));
            return failed(failure(PairwiseFailureCode::platform_numa_bind_failed, operation,
                                  PairwiseFailureReason::unexpected_native_result));
        }

        std::fill(workspace.words.begin(), workspace.words.end(), 0UL);
        const auto observed = dependencies_.numa.allowed_mems(
            dependencies_.numa.context, workspace.words.data(), domain.value(),
            MPOL_F_MEMS_ALLOWED);
        auto diagnosis =
            platform_linux::detail::NumaBindEinvalDiagnosis::allowed_mems_observation_failed;
        if (observed.value == 0 && observed.error_number == 0) {
            auto selected_within_allowed = true;
            constexpr auto word_bits = sizeof(unsigned long) * CHAR_BIT;
            for (unsigned long value = 0; value < nodes.maxnode(); ++value) {
                const auto node = core::LinuxNumaNodeId{static_cast<std::uint32_t>(value)};
                if (nodes.contains(node) &&
                    (workspace.words[value / word_bits] & (1UL << (value % word_bits))) == 0U) {
                    selected_within_allowed = false;
                    break;
                }
            }
            diagnosis = selected_within_allowed
                ? platform_linux::detail::NumaAccess::diagnose_invocation(
                      invocation.address, invocation.length,
                      invocation.base_page_bytes, invocation.mode,
                      invocation.flags, invocation.selected_mask_nonempty,
                      invocation.selected_maxnode_within_domain,
                      invocation.selected_nodes_effective,
                      invocation.selected_nodes_online,
                      invocation.selected_nodes_memory_backed)
                : platform_linux::detail::NumaBindEinvalDiagnosis::
                      selected_nodes_outside_allowed_mems;
        }
        workspace.einval_available = false;
        return failed(failure(PairwiseFailureCode::platform_numa_bind_failed, operation,
                              PairwiseFailureReason::invalid_bind_request,
                              NoPairwiseFailureContext{}, EINVAL), diagnosis);
    } catch (const std::exception&) {
        return failed(contained_numa(operation, false));
    } catch (...) {
        return failed(contained_numa(operation, true));
    }
}

auto PairwisePlatformLeafFacade::query_pages(std::span<void* const> pages) const noexcept
    -> std::expected<std::vector<platform_linux::PageQueryStatus>, PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::query_page_locations;
    if (pages.empty())
        return std::vector<platform_linux::PageQueryStatus>{};
    for (auto* page : pages) {
        if (page == nullptr)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_invalid_argument,
                                           operation,
                                           PairwiseFailureReason::invalid_page_pointer)};
    }
    if (pages.size() > dependencies_.numa_count_limit)
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                       operation, PairwiseFailureReason::count_out_of_range)};
    try {
        before(dependencies_, PairwisePlatformLeafOperation::numa_query);
        auto statuses = dependencies_.numa_statuses(pages.size());
        if (statuses.size() != pages.size())
            throw std::bad_alloc{};
        const auto raw = dependencies_.numa.query_pages(
            dependencies_.numa.context, 0, static_cast<unsigned long>(pages.size()),
            const_cast<void**>(pages.data()), nullptr, statuses.data(), 0);
        if (raw.value == -1 && raw.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_query_failed,
                                           operation, numa_query_reason(raw.error_number),
                                           NoPairwiseFailureContext{}, raw.error_number)};
        if (raw.value != 0 || raw.error_number != 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_query_failed,
                                           operation,
                                           PairwiseFailureReason::unexpected_native_result)};
        auto results = dependencies_.numa_results(pages.size());
        for (const auto status : statuses) {
            if (status >= 0) {
                results.push_back(platform_linux::PageQueryStatus::resident(
                    core::LinuxNumaNodeId{static_cast<std::uint32_t>(status)}));
            } else if (status == -ENOENT) {
                results.push_back(platform_linux::PageQueryStatus::unresolved());
            } else {
                if (status == std::numeric_limits<int>::min())
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_numa_invalid_status, operation,
                        PairwiseFailureReason::invalid_page_status)};
                results.push_back(platform_linux::detail::NumaAccess::page_error_status(-status));
            }
        }
        return results;
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                       operation, PairwiseFailureReason::allocation_failure)};
    } catch (const std::exception&) {
        return std::unexpected{contained_numa(operation, false)};
    } catch (...) {
        return std::unexpected{contained_numa(operation, true)};
    }
}

auto PairwisePreparedNumaQueryBuffers::prepare(std::size_t capacity)
    -> PairwisePreparedNumaQueryBuffers {
    auto raw_statuses = std::vector<int>(capacity);
    auto results = std::vector<platform_linux::PageQueryStatus>{};
    results.reserve(capacity);
    return PairwisePreparedNumaQueryBuffers{std::move(raw_statuses), std::move(results)};
}

auto PairwisePlatformLeafFacade::query_pages_prepared(
    std::span<void* const> pages,
    PairwisePreparedNumaQueryBuffers& buffers) const noexcept
    -> std::expected<std::span<const platform_linux::PageQueryStatus>,
                     PairwisePlatformFailure> {
    constexpr auto operation = PairwiseFailureOperation::query_page_locations;
    if (pages.empty()) {
        buffers.results_.clear();
        return std::span<const platform_linux::PageQueryStatus>{};
    }
    for (auto* page : pages) {
        if (page == nullptr)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_invalid_argument,
                                           operation,
                                           PairwiseFailureReason::invalid_page_pointer)};
    }
    if (pages.size() > dependencies_.numa_count_limit)
        return std::unexpected{failure(PairwiseFailureCode::platform_numa_conversion_failed,
                                       operation, PairwiseFailureReason::count_out_of_range)};
    if (pages.size() > buffers.raw_statuses_.size() ||
        pages.size() > buffers.results_.capacity() ||
        dependencies_.numa.query_pages == nullptr)
        std::terminate();
    try {
        before(dependencies_, PairwisePlatformLeafOperation::numa_query);
        const auto raw = dependencies_.numa.query_pages(
            dependencies_.numa.context, 0, static_cast<unsigned long>(pages.size()),
            const_cast<void**>(pages.data()), nullptr, buffers.raw_statuses_.data(), 0);
        if (raw.value == -1 && raw.error_number > 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_query_failed,
                                           operation, numa_query_reason(raw.error_number),
                                           NoPairwiseFailureContext{}, raw.error_number)};
        if (raw.value != 0 || raw.error_number != 0)
            return std::unexpected{failure(PairwiseFailureCode::platform_numa_query_failed,
                                           operation,
                                           PairwiseFailureReason::unexpected_native_result)};

        buffers.results_.clear();
        for (std::size_t index = 0; index < pages.size(); ++index) {
            const auto status = buffers.raw_statuses_[index];
            if (status >= 0) {
                buffers.results_.push_back(platform_linux::PageQueryStatus::resident(
                    core::LinuxNumaNodeId{static_cast<std::uint32_t>(status)}));
            } else if (status == -ENOENT) {
                buffers.results_.push_back(platform_linux::PageQueryStatus::unresolved());
            } else {
                if (status == std::numeric_limits<int>::min())
                    return std::unexpected{failure(
                        PairwiseFailureCode::platform_numa_invalid_status, operation,
                        PairwiseFailureReason::invalid_page_status)};
                buffers.results_.push_back(
                    platform_linux::detail::NumaAccess::page_error_status(-status));
            }
        }
        return std::span<const platform_linux::PageQueryStatus>{buffers.results_};
    } catch (const std::exception&) {
        return std::unexpected{contained_numa(operation, false)};
    } catch (...) {
        return std::unexpected{contained_numa(operation, true)};
    }
}

} // namespace epycbench::runtime::detail
