#pragma once

#include "epycbench/runtime/pairwise_run_evidence.hpp"

namespace epycbench::runtime::detail {

struct PairwiseBlockAssociationView final {
    const measurement::MeasurementBlockPlan& plan;
    std::size_t count;
    const void* owner;
    const FactualObservationEvidence& (*observation)(const void*, std::size_t) noexcept;
    const FailedObservationEvidence* failed;
    BlockAttemptCompletion completion;
};
[[nodiscard]] auto validate_pairwise_block_association(PairwiseBlockAssociationView) noexcept
    -> std::expected<void, PairwiseRuntimeFailure>;

// Shared outer-block relation only. Cross-block ordering, ledger lifecycle and
// capacity remain the ledger's responsibility; no root admission is implied.
[[nodiscard]] auto validate_pairwise_block_association(
    const PairwiseBlockAttemptEvidence&) noexcept
    -> std::expected<void, PairwiseRuntimeFailure>;

} // namespace epycbench::runtime::detail
