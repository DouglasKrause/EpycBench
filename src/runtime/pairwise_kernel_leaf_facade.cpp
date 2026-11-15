#include "runtime/pairwise_kernel_leaf_facade.hpp"

#include "core/seed_derivation_access.hpp"
#include "kernel/bounded_index.hpp"
#include "kernel/canonical_latency_permutation.hpp"
#include "kernel/kernel_leaf_access.hpp"
#include "kernel/mix64.hpp"
#include "kernel/region_initialization_access.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] constexpr auto bandwidth_identity(PairwiseKernelPhase phase) noexcept
    -> KernelIdentityContext {
    return {PairwiseKernelFamily::bandwidth,
            PairwiseKernelImplementation::scalar_u64_sequential_v1, phase};
}
[[nodiscard]] constexpr auto latency_identity(PairwiseKernelPhase phase) noexcept
    -> KernelIdentityContext {
    return {PairwiseKernelFamily::latency,
            PairwiseKernelImplementation::scalar_u64_random_cycle_v1, phase};
}

[[nodiscard]] auto kernel_failure(PairwiseFailureCode code,
                                  PairwiseFailureOperation operation,
                                  PairwiseFailureReason reason,
                                  PairwiseFailureContext context) noexcept
    -> PairwiseKernelFailure {
    auto result = PairwiseKernelFailure::create(code, operation, reason, std::move(context),
                                                std::nullopt);
    if (!result)
        std::terminate();
    return std::move(*result);
}

template <class Value>
[[nodiscard]] auto failed(PairwiseFailureCode code, PairwiseFailureOperation operation,
                          PairwiseFailureReason reason,
                          PairwiseFailureContext context) noexcept
    -> std::expected<Value, PairwiseKernelFailure> {
    return std::unexpected{kernel_failure(code, operation, reason, std::move(context))};
}

[[nodiscard]] auto contained_code(PairwiseKernelLeafOperation operation, bool unknown) noexcept
    -> PairwiseFailureCode {
    switch (operation) {
    case PairwiseKernelLeafOperation::bandwidth_pass_count:
        return unknown ? PairwiseFailureCode::kernel_bandwidth_pass_count_contained_unknown_failure
                       : PairwiseFailureCode::kernel_bandwidth_pass_count_contained_exception;
    case PairwiseKernelLeafOperation::bandwidth_accounting:
        return unknown ? PairwiseFailureCode::kernel_bandwidth_accounting_contained_unknown_failure
                       : PairwiseFailureCode::kernel_bandwidth_accounting_contained_exception;
    case PairwiseKernelLeafOperation::bandwidth_validation:
        return unknown ? PairwiseFailureCode::kernel_bandwidth_validation_contained_unknown_failure
                       : PairwiseFailureCode::kernel_bandwidth_validation_contained_exception;
    case PairwiseKernelLeafOperation::latency_validation:
        return unknown ? PairwiseFailureCode::kernel_latency_validation_contained_unknown_failure
                       : PairwiseFailureCode::kernel_latency_validation_contained_exception;
    case PairwiseKernelLeafOperation::latency_step_count:
    case PairwiseKernelLeafOperation::latency_schedule:
        return unknown ? PairwiseFailureCode::kernel_latency_schedule_contained_unknown_failure
                       : PairwiseFailureCode::kernel_latency_schedule_contained_exception;
    case PairwiseKernelLeafOperation::latency_verification:
        return unknown ? PairwiseFailureCode::kernel_latency_verification_contained_unknown_failure
                       : PairwiseFailureCode::kernel_latency_verification_contained_exception;
    case PairwiseKernelLeafOperation::region_initializer:
        return unknown ? PairwiseFailureCode::kernel_region_initializer_contained_unknown_failure
                       : PairwiseFailureCode::kernel_region_initializer_contained_exception;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto operation_id(PairwiseKernelLeafOperation operation) noexcept
    -> PairwiseFailureOperation {
    switch (operation) {
    case PairwiseKernelLeafOperation::bandwidth_pass_count:
        return PairwiseFailureOperation::kernel_bandwidth_pass_count;
    case PairwiseKernelLeafOperation::bandwidth_accounting:
        return PairwiseFailureOperation::kernel_bandwidth_accounting;
    case PairwiseKernelLeafOperation::bandwidth_validation:
        return PairwiseFailureOperation::kernel_bandwidth_validation;
    case PairwiseKernelLeafOperation::latency_validation:
        return PairwiseFailureOperation::kernel_latency_validation;
    case PairwiseKernelLeafOperation::latency_step_count:
    case PairwiseKernelLeafOperation::latency_schedule:
        return PairwiseFailureOperation::kernel_latency_schedule;
    case PairwiseKernelLeafOperation::latency_verification:
        return PairwiseFailureOperation::kernel_latency_verification_prepare;
    case PairwiseKernelLeafOperation::region_initializer:
        return PairwiseFailureOperation::kernel_region_initializer;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto operation_context(PairwiseKernelLeafOperation operation) noexcept
    -> PairwiseFailureContext {
    switch (operation) {
    case PairwiseKernelLeafOperation::bandwidth_pass_count:
        return bandwidth_identity(PairwiseKernelPhase::pass_count_preparation);
    case PairwiseKernelLeafOperation::bandwidth_accounting:
        return bandwidth_identity(PairwiseKernelPhase::work_accounting);
    case PairwiseKernelLeafOperation::bandwidth_validation:
        return bandwidth_identity(PairwiseKernelPhase::execution_input_validation);
    case PairwiseKernelLeafOperation::latency_validation:
        return latency_identity(PairwiseKernelPhase::execution_validation);
    case PairwiseKernelLeafOperation::latency_step_count:
    case PairwiseKernelLeafOperation::latency_schedule:
        return latency_identity(PairwiseKernelPhase::start_state_schedule);
    case PairwiseKernelLeafOperation::latency_verification:
        return latency_identity(PairwiseKernelPhase::terminal_verification_preparation);
    case PairwiseKernelLeafOperation::region_initializer:
        return NoPairwiseFailureContext{};
    }
    std::terminate();
}

template <class Value>
[[nodiscard]] auto contained(PairwiseKernelLeafOperation operation, bool unknown) noexcept
    -> std::expected<Value, PairwiseKernelFailure> {
    return failed<Value>(contained_code(operation, unknown), operation_id(operation),
                         unknown ? PairwiseFailureReason::contained_unknown_failure
                                 : PairwiseFailureReason::contained_exception,
                         operation_context(operation));
}

void before(void* context, PairwiseKernelBeforeOperation hook,
            PairwiseKernelLeafOperation operation) {
    if (hook != nullptr)
        hook(context, operation);
}

[[nodiscard]] auto successors(std::span<const std::uint64_t> permutation)
    -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> result(permutation.size());
    for (std::size_t position{}; position + 1U < permutation.size(); ++position)
        result[static_cast<std::size_t>(permutation[position])] = permutation[position + 1U];
    result[static_cast<std::size_t>(permutation.back())] = permutation.front();
    return result;
}

[[nodiscard]] auto valid_cycle(std::span<const std::uint64_t> values) -> bool {
    if (values.size() < 2U)
        return false;
    std::vector<std::uint8_t> predecessors(values.size(), 0U);
    for (std::size_t node{}; node < values.size(); ++node) {
        const auto successor = values[node];
        if (successor >= values.size() || successor == node)
            return false;
        auto& count = predecessors[static_cast<std::size_t>(successor)];
        if (count != 0U)
            return false;
        count = 1U;
    }
    std::vector<bool> visited(values.size(), false);
    std::size_t node{};
    for (std::size_t visit{}; visit < values.size(); ++visit) {
        if (visited[node])
            return false;
        visited[node] = true;
        node = static_cast<std::size_t>(values[node]);
    }
    return node == 0U && std::ranges::all_of(visited, [](bool value) { return value; });
}

[[nodiscard]] auto draw(void* context, const PairwiseKernelAlgorithms& algorithms,
                        std::uint64_t value, std::uint64_t bound) noexcept
    -> PairwiseKernelDrawResult {
    if (algorithms.draw != nullptr)
        return algorithms.draw(context, value, bound);
    const auto selected = kernel::detail::bounded_index(value, bound);
    return selected ? PairwiseKernelDrawResult{PairwiseKernelDrawState::selected, *selected}
                    : PairwiseKernelDrawResult{PairwiseKernelDrawState::rejected, 0U};
}

[[nodiscard]] auto permutation(void* context,
                               const PairwiseKernelAlgorithms& algorithms,
                               std::size_t count, std::uint64_t seed)
    -> std::expected<std::vector<std::uint64_t>, PairwiseKernelPermutationFailure> {
    if (algorithms.permutation != nullptr)
        return algorithms.permutation(context, count, seed);
    auto result = kernel::detail::canonical_latency_permutation(count, seed);
    if (!result)
        return std::unexpected{PairwiseKernelPermutationFailure::stream_exhausted};
    return std::move(*result);
}

[[nodiscard]] auto checked_size(void* context,
                                const PairwiseKernelAlgorithms& algorithms,
                                std::uint64_t value) noexcept -> std::optional<std::size_t> {
    if (algorithms.checked_size != nullptr)
        return algorithms.checked_size(context, value);
    if (value > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return static_cast<std::size_t>(value);
}

[[nodiscard]] auto cycle_valid(void* context,
                               const PairwiseKernelAlgorithms& algorithms,
                               std::span<const std::uint64_t> values) -> bool {
    return algorithms.validate_cycle != nullptr
               ? algorithms.validate_cycle(context, values)
               : valid_cycle(values);
}

} // namespace

auto PairwiseKernelLeafFacade::bandwidth_configuration(core::ByteCount working_set) const noexcept
    -> std::expected<kernel::SequentialReadBandwidthConfig, PairwiseKernelFailure> {
    if (working_set.value() == 0U || working_set.value() % 64U != 0U)
        return failed<kernel::SequentialReadBandwidthConfig>(
            PairwiseFailureCode::kernel_contract_invalid,
            PairwiseFailureOperation::kernel_contract,
            PairwiseFailureReason::bandwidth_working_set,
            bandwidth_identity(PairwiseKernelPhase::none));
    return kernel::detail::KernelLeafAccess::bandwidth_config(working_set);
}

auto PairwiseKernelLeafFacade::latency_configuration(
    core::ByteCount working_set, kernel::PointerCycleSeedIntent seed) const noexcept
    -> std::expected<kernel::DependentLoadLatencyConfig, PairwiseKernelFailure> {
    if (working_set.value() < 128U || working_set.value() % 64U != 0U)
        return failed<kernel::DependentLoadLatencyConfig>(
            PairwiseFailureCode::kernel_contract_invalid,
            PairwiseFailureOperation::kernel_contract,
            PairwiseFailureReason::latency_working_set,
            latency_identity(PairwiseKernelPhase::none));
    return kernel::detail::KernelLeafAccess::latency_config(working_set, std::move(seed));
}

auto PairwiseKernelLeafFacade::validate_configuration_identity(
    kernel::KernelId id, const kernel::KernelConfiguration& configuration) const noexcept
    -> std::expected<void, PairwiseKernelFailure> {
    if (id == kernel::KernelId::sequential_read_bandwidth_v1 &&
        !std::holds_alternative<kernel::SequentialReadBandwidthConfig>(configuration))
        return failed<void>(PairwiseFailureCode::kernel_contract_invalid,
                            PairwiseFailureOperation::kernel_contract,
                            PairwiseFailureReason::configuration_identity_mismatch,
                            bandwidth_identity(PairwiseKernelPhase::none));
    if (id == kernel::KernelId::dependent_load_latency_v1 &&
        !std::holds_alternative<kernel::DependentLoadLatencyConfig>(configuration))
        return failed<void>(PairwiseFailureCode::kernel_contract_invalid,
                            PairwiseFailureOperation::kernel_contract,
                            PairwiseFailureReason::configuration_identity_mismatch,
                            latency_identity(PairwiseKernelPhase::none));
    if (id != kernel::KernelId::sequential_read_bandwidth_v1 &&
        id != kernel::KernelId::dependent_load_latency_v1)
        return failed<void>(PairwiseFailureCode::kernel_contract_invalid,
                            PairwiseFailureOperation::kernel_contract,
                            PairwiseFailureReason::unknown_kernel_identity,
                            NoPairwiseFailureContext{});
    return {};
}

auto PairwiseKernelLeafFacade::bandwidth_pass_count(std::uint64_t value) const noexcept
    -> std::expected<kernel::SequentialReadBandwidthPassCount, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::bandwidth_pass_count;
    try {
        before(hook_context_, before_operation_, operation);
        if (value == 0U)
            return failed<kernel::SequentialReadBandwidthPassCount>(
                PairwiseFailureCode::kernel_bandwidth_pass_count_invalid,
                operation_id(operation), PairwiseFailureReason::zero_pass_count,
                operation_context(operation));
        return kernel::detail::KernelLeafAccess::pass_count(value);
    } catch (const std::exception&) {
        return contained<kernel::SequentialReadBandwidthPassCount>(operation, false);
    } catch (...) {
        return contained<kernel::SequentialReadBandwidthPassCount>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::bandwidth_input(
    kernel::SequentialReadBandwidthConfig configuration,
    std::span<const std::byte> logical_region) const noexcept
    -> std::expected<kernel::SequentialReadBandwidthInput, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::bandwidth_validation;
    try {
        before(hook_context_, before_operation_, operation);
        if (!std::cmp_equal(logical_region.size_bytes(),
                            configuration.working_set_per_worker().value()))
            return failed<kernel::SequentialReadBandwidthInput>(
                PairwiseFailureCode::kernel_bandwidth_validation_invalid_input,
                operation_id(operation), PairwiseFailureReason::region_size_mismatch,
                operation_context(operation));
        if (reinterpret_cast<std::uintptr_t>(logical_region.data()) % 64U != 0U)
            return failed<kernel::SequentialReadBandwidthInput>(
                PairwiseFailureCode::kernel_bandwidth_validation_invalid_input,
                operation_id(operation), PairwiseFailureReason::region_alignment_invalid,
                operation_context(operation));
        return kernel::detail::KernelLeafAccess::bandwidth_input(std::move(configuration),
                                                                 logical_region);
    } catch (const std::exception&) {
        return contained<kernel::SequentialReadBandwidthInput>(operation, false);
    } catch (...) {
        return contained<kernel::SequentialReadBandwidthInput>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::bandwidth_work(
    const kernel::PreparedSequentialReadBandwidth& prepared,
    kernel::SequentialReadBandwidthPassCount passes) const noexcept
    -> std::expected<kernel::SequentialReadBandwidthWork, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::bandwidth_accounting;
    try {
        before(hook_context_, before_operation_, operation);
        const auto bytes = prepared.input().configuration().working_set_per_worker().checked_multiply(
            passes.value());
        if (!bytes)
            return failed<kernel::SequentialReadBandwidthWork>(
                PairwiseFailureCode::kernel_bandwidth_accounting_overflow,
                operation_id(operation), PairwiseFailureReason::logical_byte_count_overflow,
                operation_context(operation));
        return kernel::detail::KernelLeafAccess::bandwidth_work(*bytes);
    } catch (const std::exception&) {
        return contained<kernel::SequentialReadBandwidthWork>(operation, false);
    } catch (...) {
        return contained<kernel::SequentialReadBandwidthWork>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_input(
    kernel::DependentLoadLatencyConfig configuration,
    std::span<const std::byte> logical_region) const noexcept
    -> std::expected<kernel::DependentLoadLatencyInput, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_validation;
    try {
        before(hook_context_, before_operation_, operation);
        if (!std::cmp_equal(logical_region.size_bytes(), configuration.working_set().value()))
            return failed<kernel::DependentLoadLatencyInput>(
                PairwiseFailureCode::kernel_latency_validation_invalid_input,
                operation_id(operation), PairwiseFailureReason::region_size_mismatch,
                operation_context(operation));
        if (reinterpret_cast<std::uintptr_t>(logical_region.data()) % 64U != 0U)
            return failed<kernel::DependentLoadLatencyInput>(
                PairwiseFailureCode::kernel_latency_validation_invalid_input,
                operation_id(operation), PairwiseFailureReason::region_alignment_invalid,
                operation_context(operation));
        return kernel::detail::KernelLeafAccess::latency_input(std::move(configuration),
                                                               logical_region);
    } catch (const std::exception&) {
        return contained<kernel::DependentLoadLatencyInput>(operation, false);
    } catch (...) {
        return contained<kernel::DependentLoadLatencyInput>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_step_count(std::uint64_t value) const noexcept
    -> std::expected<kernel::DependentLoadLatencyStepCount, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_step_count;
    try {
        before(hook_context_, before_operation_, operation);
        if (value == 0U)
            return failed<kernel::DependentLoadLatencyStepCount>(
                PairwiseFailureCode::kernel_latency_schedule_invalid_step_count,
                operation_id(operation), PairwiseFailureReason::zero_step_count,
                operation_context(operation));
        return kernel::detail::KernelLeafAccess::step_count(value);
    } catch (const std::exception&) {
        return contained<kernel::DependentLoadLatencyStepCount>(operation, false);
    } catch (...) {
        return contained<kernel::DependentLoadLatencyStepCount>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_schedule(
    const kernel::PreparedDependentLoadLatency& prepared,
    PairwisePreparedLatencyScheduleSeed seed,
    kernel::DependentLoadLatencyStepCount step_count) const noexcept
    -> std::expected<kernel::DependentLoadLatencySchedule, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_schedule;
    try {
        before(hook_context_, before_operation_, operation);
        const auto bound = static_cast<std::uint64_t>(prepared.node_count());
        std::uint64_t ordinal{};
        for (;;) {
            if (ordinal == std::numeric_limits<std::uint64_t>::max())
                return failed<kernel::DependentLoadLatencySchedule>(
                    PairwiseFailureCode::kernel_latency_schedule_bounded_index_exhausted,
                    operation_id(operation), PairwiseFailureReason::draw_stream_exhausted,
                    operation_context(operation));
            const auto selected = draw(
                algorithms_.context, algorithms_,
                kernel::detail::mix64_v1(seed.value() + ordinal), bound);
            ++ordinal;
            if (selected.state == PairwiseKernelDrawState::exhausted)
                return failed<kernel::DependentLoadLatencySchedule>(
                    PairwiseFailureCode::kernel_latency_schedule_bounded_index_exhausted,
                    operation_id(operation), PairwiseFailureReason::draw_stream_exhausted,
                    operation_context(operation));
            if (selected.state == PairwiseKernelDrawState::selected)
                return kernel::detail::KernelLeafAccess::schedule(
                    static_cast<std::size_t>(selected.index), step_count);
        }
    } catch (const std::exception&) {
        return contained<kernel::DependentLoadLatencySchedule>(operation, false);
    } catch (...) {
        return contained<kernel::DependentLoadLatencySchedule>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::prepare_latency_schedule_seed(
    std::uint64_t root_seed, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id) const noexcept
    -> std::expected<PairwisePreparedLatencyScheduleSeed, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_schedule;
    try {
        before(hook_context_, before_operation_, operation);
        const auto derived = core::detail::derive_observation_start_seed(
            root_seed, experiment_id.value(), block_id.value(),
            kernel::kernel_id_token(kernel::KernelId::dependent_load_latency_v1));
        return PairwisePreparedLatencyScheduleSeed{derived.value()};
    } catch (const std::exception&) {
        return contained<PairwisePreparedLatencyScheduleSeed>(operation, false);
    } catch (...) {
        return contained<PairwisePreparedLatencyScheduleSeed>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_verification(
    const kernel::PreparedDependentLoadLatency& prepared,
    std::uint64_t resolved_permutation_seed) const noexcept
    -> std::expected<kernel::PreparedDependentLoadLatencyVerification,
                     PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_verification;
    try {
        before(hook_context_, before_operation_, operation);
        auto permutation_result = permutation(algorithms_.context, algorithms_,
                                              prepared.node_count(),
                                              resolved_permutation_seed);
        if (!permutation_result)
            return failed<kernel::PreparedDependentLoadLatencyVerification>(
                PairwiseFailureCode::kernel_latency_verification_preparation_failed,
                operation_id(operation), PairwiseFailureReason::permutation_stream_exhausted,
                operation_context(operation));
        std::vector<std::size_t> inverse(permutation_result->size());
        for (std::size_t position{}; position < permutation_result->size(); ++position)
            inverse[static_cast<std::size_t>((*permutation_result)[position])] = position;
        return kernel::detail::KernelLeafAccess::verification(std::move(*permutation_result),
                                                              std::move(inverse));
    } catch (const std::exception&) {
        return contained<kernel::PreparedDependentLoadLatencyVerification>(operation, false);
    } catch (...) {
        return contained<kernel::PreparedDependentLoadLatencyVerification>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_verification_for_configuration(
    kernel::DependentLoadLatencyConfig configuration,
    std::uint64_t resolved_permutation_seed) const noexcept
    -> std::expected<kernel::PreparedDependentLoadLatencyVerification,
                     PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::latency_verification;
    try {
        before(hook_context_, before_operation_, operation);
        constexpr std::uint64_t node_bytes = 64U;
        const auto bytes = configuration.working_set().value();
        if (bytes == 0U || bytes % node_bytes != 0U ||
            bytes / node_bytes > std::numeric_limits<std::size_t>::max())
            return failed<kernel::PreparedDependentLoadLatencyVerification>(
                PairwiseFailureCode::kernel_latency_verification_preparation_failed,
                operation_id(operation), PairwiseFailureReason::region_size_mismatch,
                operation_context(operation));
        const auto node_count = static_cast<std::size_t>(bytes / node_bytes);
        auto permutation_result = permutation(algorithms_.context, algorithms_,
                                              node_count,
                                              resolved_permutation_seed);
        if (!permutation_result)
            return failed<kernel::PreparedDependentLoadLatencyVerification>(
                PairwiseFailureCode::kernel_latency_verification_preparation_failed,
                operation_id(operation), PairwiseFailureReason::permutation_stream_exhausted,
                operation_context(operation));
        std::vector<std::size_t> inverse(permutation_result->size());
        for (std::size_t position{}; position < permutation_result->size(); ++position)
            inverse[static_cast<std::size_t>((*permutation_result)[position])] = position;
        return kernel::detail::KernelLeafAccess::verification(
            std::move(*permutation_result), std::move(inverse));
    } catch (const std::exception&) {
        return contained<kernel::PreparedDependentLoadLatencyVerification>(operation, false);
    } catch (...) {
        return contained<kernel::PreparedDependentLoadLatencyVerification>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::latency_region_spec(
    kernel::DependentLoadLatencyConfig configuration,
    std::uint64_t resolved_permutation_seed) const noexcept
    -> std::expected<kernel::RegionInitializationSpec, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::region_initializer;
    try {
        before(hook_context_, before_operation_, operation);
        if (configuration.pointer_cycle_seed().mode() ==
                kernel::PointerCycleSeedMode::explicit_value &&
            configuration.pointer_cycle_seed().value() != resolved_permutation_seed)
            return failed<kernel::RegionInitializationSpec>(
                PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                operation_id(operation), PairwiseFailureReason::resolved_seed_mismatch,
                NoPairwiseFailureContext{});
        return kernel::detail::RegionInitializationAccess::unchecked_spec(
            kernel::RegionContentFamily::dependent_load_latency,
            configuration.working_set(), resolved_permutation_seed);
    } catch (const std::exception&) {
        return contained<kernel::RegionInitializationSpec>(operation, false);
    } catch (...) {
        return contained<kernel::RegionInitializationSpec>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::prepare_region_initializer(
    kernel::RegionInitializationSpec specification) const noexcept
    -> std::expected<PairwisePreparedRegionInitializer, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::region_initializer;
    try {
        before(hook_context_, before_operation_, operation);
        const auto logical = specification.logical_bytes().value();
        if (logical == 0U || logical % 64U != 0U)
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                operation_id(operation), PairwiseFailureReason::logical_size_invalid,
                NoPairwiseFailureContext{});
        if (!checked_size(algorithms_.context, algorithms_, logical))
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_arithmetic_failed,
                operation_id(operation), PairwiseFailureReason::logical_size_invalid,
                NoPairwiseFailureContext{});
        if (specification.family() == kernel::RegionContentFamily::sequential_read_bandwidth) {
            if (specification.permutation_seed())
                return failed<PairwisePreparedRegionInitializer>(
                    PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                    operation_id(operation), PairwiseFailureReason::content_profile_invalid,
                    NoPairwiseFailureContext{});
            return PairwisePreparedRegionInitializer{
                kernel::detail::RegionInitializationAccess::evidence(
                    specification.family(), kernel::bandwidth_region_layout_profile,
                    specification.logical_bytes(), logical / 8U, std::nullopt, std::nullopt),
                {}};
        }
        if (specification.family() != kernel::RegionContentFamily::dependent_load_latency ||
            !specification.permutation_seed() || logical < 128U)
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                operation_id(operation), PairwiseFailureReason::content_profile_invalid,
                NoPairwiseFailureContext{});
        const auto nodes64 = logical / 64U;
        const auto nodes = checked_size(algorithms_.context, algorithms_, nodes64);
        if (!nodes || nodes64 < 2U ||
            nodes64 > (std::numeric_limits<std::uint64_t>::max() - 6U) / 7U)
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_arithmetic_failed,
                operation_id(operation), PairwiseFailureReason::node_count_invalid,
                NoPairwiseFailureContext{});
        auto permutation_result = permutation(algorithms_.context, algorithms_, *nodes,
                                              *specification.permutation_seed());
        if (!permutation_result)
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_arithmetic_failed,
                operation_id(operation), PairwiseFailureReason::stream_exhausted,
                NoPairwiseFailureContext{});
        auto next = successors(*permutation_result);
        if (!cycle_valid(algorithms_.context, algorithms_, next))
            return failed<PairwisePreparedRegionInitializer>(
                PairwiseFailureCode::kernel_region_initializer_content_invalid,
                operation_id(operation), PairwiseFailureReason::cycle_invalid,
                NoPairwiseFailureContext{});
        return PairwisePreparedRegionInitializer{
            kernel::detail::RegionInitializationAccess::evidence(
                specification.family(), kernel::latency_region_layout_profile,
                specification.logical_bytes(), std::nullopt, nodes64,
                specification.permutation_seed()),
            std::move(next)};
    } catch (const std::exception&) {
        return contained<PairwisePreparedRegionInitializer>(operation, false);
    } catch (...) {
        return contained<PairwisePreparedRegionInitializer>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::bounded_draw(
    std::span<const std::uint64_t> draws, std::size_t& cursor,
    std::uint64_t bound) const noexcept
    -> std::expected<std::uint64_t, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::region_initializer;
    try {
        before(hook_context_, before_operation_, operation);
        if (bound == 0U)
            return failed<std::uint64_t>(
                PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                operation_id(operation), PairwiseFailureReason::bounded_draw_invalid,
                NoPairwiseFailureContext{});
        while (cursor < draws.size()) {
            const auto result = draw(algorithms_.context, algorithms_, draws[cursor], bound);
            ++cursor;
            if (result.state == PairwiseKernelDrawState::exhausted)
                break;
            if (result.state == PairwiseKernelDrawState::selected)
                return result.index;
        }
        return failed<std::uint64_t>(
            PairwiseFailureCode::kernel_region_initializer_arithmetic_failed,
            operation_id(operation), PairwiseFailureReason::stream_exhausted,
            NoPairwiseFailureContext{});
    } catch (const std::exception&) {
        return contained<std::uint64_t>(operation, false);
    } catch (...) {
        return contained<std::uint64_t>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::canonical_permutation(
    std::size_t node_count, std::uint64_t seed) const noexcept
    -> std::expected<std::vector<std::uint64_t>, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::region_initializer;
    try {
        before(hook_context_, before_operation_, operation);
        if (node_count < 2U || node_count > std::numeric_limits<std::uint64_t>::max())
            return failed<std::vector<std::uint64_t>>(
                PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                operation_id(operation), PairwiseFailureReason::node_count_invalid,
                NoPairwiseFailureContext{});
        auto result = permutation(algorithms_.context, algorithms_, node_count, seed);
        if (!result)
            return failed<std::vector<std::uint64_t>>(
                PairwiseFailureCode::kernel_region_initializer_arithmetic_failed,
                operation_id(operation), PairwiseFailureReason::stream_exhausted,
                NoPairwiseFailureContext{});
        return std::move(*result);
    } catch (const std::exception&) {
        return contained<std::vector<std::uint64_t>>(operation, false);
    } catch (...) {
        return contained<std::vector<std::uint64_t>>(operation, true);
    }
}

auto PairwiseKernelLeafFacade::initialize_region(
    const PairwisePreparedRegionInitializer& initializer,
    std::span<std::byte> logical_region) const noexcept
    -> std::expected<void, PairwiseKernelFailure> {
    constexpr auto operation = PairwiseKernelLeafOperation::region_initializer;
    try {
        before(hook_context_, before_operation_, operation);
        if (logical_region.size() != initializer.evidence_.logical_bytes().value())
            return failed<void>(PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                                operation_id(operation),
                                PairwiseFailureReason::span_size_mismatch,
                                NoPairwiseFailureContext{});
        if (reinterpret_cast<std::uintptr_t>(logical_region.data()) % 64U != 0U)
            return failed<void>(PairwiseFailureCode::kernel_region_initializer_invalid_argument,
                                operation_id(operation),
                                PairwiseFailureReason::alignment_invalid,
                                NoPairwiseFailureContext{});
        if (initializer.evidence_.family() ==
            kernel::RegionContentFamily::sequential_read_bandwidth) {
            for (std::uint64_t word{}; word < *initializer.evidence_.word_count(); ++word) {
                const auto offset = static_cast<std::size_t>(word * 8U);
                kernel::detail::RegionInitializationAccess::store_little_endian(
                    std::span<std::byte, 8>{logical_region.data() + offset, 8U},
                    kernel::detail::RegionInitializationAccess::mix64(word));
            }
            return {};
        }
        if (initializer.evidence_.family() !=
                kernel::RegionContentFamily::dependent_load_latency ||
            !initializer.evidence_.node_count() ||
            initializer.successors_.size() != *initializer.evidence_.node_count())
            return failed<void>(PairwiseFailureCode::kernel_region_initializer_content_invalid,
                                operation_id(operation),
                                PairwiseFailureReason::prepared_content_invalid,
                                NoPairwiseFailureContext{});
        for (std::size_t node{}; node < initializer.successors_.size(); ++node) {
            const auto node_offset = node * 64U;
            kernel::detail::RegionInitializationAccess::store_little_endian(
                std::span<std::byte, 8>{logical_region.data() + node_offset, 8U},
                initializer.successors_[node]);
            for (std::size_t slot{}; slot < 7U; ++slot) {
                const auto ordinal = static_cast<std::uint64_t>(node) * 7U + slot;
                kernel::detail::RegionInitializationAccess::store_little_endian(
                    std::span<std::byte, 8>{logical_region.data() + node_offset +
                                                (slot + 1U) * 8U,
                                            8U},
                    kernel::detail::RegionInitializationAccess::mix64(ordinal));
            }
        }
        return {};
    } catch (const std::exception&) {
        return contained<void>(operation, false);
    } catch (...) {
        return contained<void>(operation, true);
    }
}

} // namespace epycbench::runtime::detail
