#pragma once

#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include "runtime/pairwise_v2_progress.hpp"

namespace epycbench::runtime::detail {

enum class PreparedPairwiseExecutionStage : unsigned char {
    storage, acquired, finalized, failed, transferred, interrupted
};

// Private, inactive composition. No route, source acquisition or run is selected
// by construction. The borrowed-input ledger view includes rejected pending facts.
class PreparedPairwiseRunnerExecution final {
  public:
    PreparedPairwiseRunnerExecution(PreparedPairwiseRunnerExecution&&) noexcept;
    PreparedPairwiseRunnerExecution(const PreparedPairwiseRunnerExecution&) = delete;
    auto operator=(PreparedPairwiseRunnerExecution&&) -> PreparedPairwiseRunnerExecution& = delete;
    ~PreparedPairwiseRunnerExecution() noexcept;

    [[nodiscard]] static auto auxiliary_bytes(bool progress = false) noexcept -> std::size_t;
    [[nodiscard]] static auto prepare(
        const AdmittedPairwiseProductionInputs&,
        PairwiseTypedProductionBindingDependencies, std::size_t auxiliary_limit, bool progress = false,
        std::optional<PreparationGraphLimits> graph_limits = std::nullopt) noexcept
        -> PairwiseOperationResult<PreparedPairwiseRunnerExecution>;
    [[nodiscard]] auto acquire() noexcept -> std::expected<void, PairwiseEvidenceFailure>;
    [[nodiscard]] auto execute(void* finalization_context = nullptr,
        void (*before_finalization)(void*) noexcept = nullptr) noexcept -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto execute_progress(progress::Callback, void* finalization_context = nullptr,
        void (*before_finalization)(void*) noexcept = nullptr) noexcept
        -> std::expected<bool, PairwiseRuntimeFailure>;
    [[nodiscard]] auto progress_failure() const noexcept -> std::optional<progress::Error>;
    [[nodiscard]] auto finalize_without_execution() noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto preparation_graph() const noexcept -> const PairwisePreparationGraph*;
    [[nodiscard]] auto progress_state() noexcept -> PairwiseProgressState*;
    [[nodiscard]] auto stage() const noexcept -> PreparedPairwiseExecutionStage;
    [[nodiscard]] auto matches(const AdmittedPairwiseProductionInputs&) const noexcept -> bool;
    [[nodiscard]] auto cancellation() const noexcept -> const std::shared_ptr<RunnerCancellation>&;
    [[nodiscard]] auto finalized_evidence() const noexcept -> const FinalizedPairwiseRunEvidence*;
    [[nodiscard]] auto take_finalized_evidence() noexcept
        -> std::expected<FinalizedPairwiseRunEvidence, PairwiseRuntimeFailure>;
    [[nodiscard]] auto ledger() const noexcept -> const PairwiseRunEvidenceLedger*;
    [[nodiscard]] auto pending_block() const noexcept -> const PairwiseBlockAttemptEvidence*;
    [[nodiscard]] auto pending_finalization() const noexcept -> const PairwiseFinalizationOutcome*;
    [[nodiscard]] auto pending_terminal() const noexcept -> const OperationalRunTerminalEvidence*;
    [[nodiscard]] auto preparation_failure() const noexcept -> const PairwiseOperationFailure*;
    [[nodiscard]] auto fatal_failure() const noexcept -> const PairwiseRuntimeFailure*;

  private:
    [[nodiscard]] auto execute_impl(progress::Callback, void*, void (*)(void*) noexcept) noexcept
        -> std::expected<bool, PairwiseRuntimeFailure>;
    struct Storage;
    explicit PreparedPairwiseRunnerExecution(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::runtime::detail
