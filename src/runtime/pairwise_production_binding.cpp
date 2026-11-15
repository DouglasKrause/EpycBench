#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include "runtime/pairwise_production_run_core.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"

#include <atomic>
#include <memory>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto binding_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseOperationFailure {
    auto failure = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!failure)
        std::terminate();
    return {PairwiseEvidenceFailure{std::move(*failure)}, {}, std::nullopt};
}

} // namespace

class PairwiseTypedProductionBinding final {
  public:
    explicit PairwiseTypedProductionBinding(
        PairwiseTypedProductionBindingDependencies dependencies,
        std::optional<PreparationGraphLimits> limits = std::nullopt) noexcept
        : limits_{limits}, dependencies_{std::move(dependencies)} {}

    [[nodiscard]] auto prepare(
        const AdmittedPairwiseProductionInputs& inputs) noexcept
        -> PairwiseOperationResult<PairwiseProductionRun> {
        if (consumed_.exchange(true, std::memory_order_acq_rel))
            return std::unexpected{
                binding_failure(PairwiseFailureReason::contained_exception)};
        const PairwiseTypedPreOwnerDependencies borrowed{
            &dependencies_.capacity_producer,
            dependencies_.platform,
            dependencies_.temperature_operations,
            dependencies_.frequency_operations,
            dependencies_.kernel,
            dependencies_.worker, dependencies_.shared_cancellation};
        auto prepared = prepare_pairwise_typed_pre_owner(inputs, borrowed, limits_);
        if (!prepared)
            return std::unexpected{std::move(prepared.error())};
        if (prepared->preparation_graph_)
            graph.emplace(prepared->preparation_graph_->retain());
        // All failure-local owners leave this scope before partial publication.
        auto result = [&]() noexcept -> PairwiseOperationResult<PairwiseProductionRun> {
            auto owners = prepare_pairwise_typed_owner_transaction(std::move(*prepared));
            if (!owners) return std::unexpected{std::move(owners.error())};
            auto core = bind_pairwise_typed_owner_run(std::move(*owners));
            if (!core) return std::unexpected{std::move(core.error())};
            if (graph && !graph->transition(PreparationGraphStage::bound)) std::terminate();
            auto wrapper = make_pairwise_production_run(std::move(*core));
            if (wrapper && graph && !graph->transition(PreparationGraphStage::published)) std::terminate();
            return wrapper;
        }();
        if (!result && graph) graph->retire();
        return result;
    }

    std::optional<PairwisePreparationGraph> graph;

  private:
    std::optional<PreparationGraphLimits> limits_;
    PairwiseTypedProductionBindingDependencies dependencies_;
    std::atomic_bool consumed_{};
};

namespace {
auto prepare_run(void* opaque,
                 const AdmittedPairwiseProductionInputs& inputs) noexcept
    -> PairwiseOperationResult<PairwiseProductionRun> {
    return static_cast<PairwiseTypedProductionBinding*>(opaque)->prepare(inputs);
}

} // namespace

auto make_pairwise_production_operations(
    PairwiseTypedProductionBindingDependencies dependencies) noexcept
    -> PairwiseOperationResult<PairwiseProductionOperations> {
    try {
        auto context = std::make_shared<PairwiseTypedProductionBinding>(
            std::move(dependencies));
        return PairwiseProductionOperations{
            std::move(context),
            prepare_run};
    } catch (...) {
        return std::unexpected{
            binding_failure(PairwiseFailureReason::contained_exception)};
    }
}

auto make_pairwise_preparation_binding(
    PairwiseTypedProductionBindingDependencies dependencies, PreparationGraphLimits limits) noexcept
    -> PairwiseOperationResult<PairwisePreparationBindingBundle> {
    try {
        auto context = std::make_shared<PairwiseTypedProductionBinding>(std::move(dependencies), limits);
        std::shared_ptr<const std::optional<PairwisePreparationGraph>> observer{context, &context->graph};
        return PairwisePreparationBindingBundle{{std::move(context), prepare_run}, std::move(observer)};
    } catch (...) {
        return std::unexpected{binding_failure(PairwiseFailureReason::contained_exception)};
    }
}

auto make_pairwise_runner_execution_backend(
    PairwiseTypedProductionBindingDependencies dependencies) noexcept
    -> PairwiseOperationResult<PairwiseRunnerExecutionBackend> {
    auto operations = make_pairwise_production_operations(
        std::move(dependencies));
    if (!operations)
        return std::unexpected{std::move(operations.error())};
    return PairwiseRunnerExecutionBackend{std::move(*operations)};
}

} // namespace epycbench::runtime::detail

namespace epycbench::runtime {

auto make_pairwise_production_backend() noexcept
    -> PairwiseOperationResult<PairwiseRunnerExecutionBackend> {
    const auto sysfs =
        platform_linux::detail::production_bounded_sysfs_operations();
    return detail::make_pairwise_runner_execution_backend({
        detail::PairwiseTypedLocalMonitoringCapacityProducer{},
        detail::PairwisePlatformLeafFacade::production(), sysfs, sysfs,
        detail::PairwiseKernelLeafFacade{}, std::nullopt});
}

} // namespace epycbench::runtime
