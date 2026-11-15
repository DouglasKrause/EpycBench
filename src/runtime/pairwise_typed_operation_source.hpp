#pragma once

#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"

#include <expected>
#include <optional>
#include <span>

namespace epycbench::runtime::detail {

struct PairwiseTypedOperationFailureSource final {
    PairwiseEvidenceFailure primary;
    std::span<const PairwiseEvidenceFailure> secondary;
    std::optional<kernel::DependentLoadLatencyBlockId> active_block_id{};
};

template <class T>
using PairwiseTypedOperationSourceResult =
    std::expected<T, PairwiseTypedOperationFailureSource>;

} // namespace epycbench::runtime::detail
