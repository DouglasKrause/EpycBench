#pragma once

#include "epycbench/results/pairwise_run_result.hpp"
#include "runtime/finalized_evidence_prepared_direct_codec.hpp"

#include <expected>
#include <memory>
#include <span>

namespace epycbench::runtime::detail { struct FinalizedEvidenceComponents; }

namespace epycbench::results::detail {

class PreparedPairwiseRunResultProjector final {
  public:
    PreparedPairwiseRunResultProjector(const PreparedPairwiseRunResultProjector&) = delete;
    PreparedPairwiseRunResultProjector(PreparedPairwiseRunResultProjector&&) noexcept;
    auto operator=(const PreparedPairwiseRunResultProjector&)
        -> PreparedPairwiseRunResultProjector& = delete;
    auto operator=(PreparedPairwiseRunResultProjector&&)
        -> PreparedPairwiseRunResultProjector& = delete;
    ~PreparedPairwiseRunResultProjector() noexcept;

    [[nodiscard]] static auto prepare(
        const runtime::prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedPairwiseRunResultProjector,
                         runtime::prepared_resources_v1::PreparedResourceError>;
    // The caller consumes the authentic owner only after the outer decoder has
    // checked canonical bytes, semantic invariants and authenticated CPUs.
    // Reserve only numerical/stopping scratch; no duplicate raw clone pipeline.
    [[nodiscard]] static auto prepare_owned(
        const runtime::prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedPairwiseRunResultProjector,
                         runtime::prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto project(
        const runtime::FinalizedPairwiseRunEvidence&,
        std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
        -> std::expected<PairwiseRunResult,
                         runtime::prepared_codec_v1::ActiveCodecFailure>;
    [[nodiscard]] auto project_owned(runtime::FinalizedPairwiseRunEvidence&&) noexcept
        -> std::expected<PairwiseRunResult,
                         runtime::prepared_codec_v1::ActiveCodecFailure>;

  private:
    struct Storage;
    explicit PreparedPairwiseRunResultProjector(std::unique_ptr<Storage>) noexcept;
    [[nodiscard]] static auto prepare_impl(
        const runtime::prepared_resources_v1::PreparedResourceValue&,bool clone) noexcept
        -> std::expected<PreparedPairwiseRunResultProjector,
                         runtime::prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto project_components(runtime::detail::FinalizedEvidenceComponents) noexcept
        -> std::expected<PairwiseRunResult,runtime::prepared_codec_v1::ActiveCodecFailure>;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::results::detail
