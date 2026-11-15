#pragma once
#include "runtime/finalized_evidence_wire_component.hpp"
#include "runtime/finalized_evidence_wire_logical_codec.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
namespace epycbench::runtime::wire_v1::detail {
struct TypedStockSum final {
    std::size_t value{};
    bool valid{true};
    void add(std::size_t n, std::size_t size = 1U) noexcept {
        const auto product = multiply_component_stock(n, size);
        if (!product) { valid = false; return; }
        const auto sum = add_wire_json_count(value, *product);
        if (!sum) valid = false; else value = *sum;
    }
};
struct PreparationOutcomeStocks final {
    TypedStockSum owned, scratch;
    std::size_t candidates{}, trials{};
};
// Admitted nonnull outcome shape; shared with the complete-root counter.
[[nodiscard]] auto typed_preparation_outcome_stocks(EvidenceComponentKind,
    const nlohmann::json&) -> PreparationOutcomeStocks;
struct FinalizationStocks final { TypedStockSum owned, scratch, workers, targets, pages; };
// Requires the generated nonnull finalization registry shape.
[[nodiscard]] auto typed_finalization_stocks(const nlohmann::json&) -> FinalizationStocks;
struct TypedStocks final { TypedStockSum owned, scratch, memberships; std::size_t workers{}; };
// Requires the admitted component shape; does not replace typed validation.
[[nodiscard]] auto typed_component_stocks(EvidenceComponentKind, const nlohmann::json&) -> TypedStocks;
struct RootReconstructionUsage final {
    std::size_t typed{}, scratch{}, ledger{}, peak{};
    std::size_t blocks{}, workers{}, memberships{}, candidates{}, trials{}, targets{}, pages{};
};
struct RootReconstructionLimits final {
    std::size_t typed{SIZE_MAX}, scratch{SIZE_MAX}, ledger{SIZE_MAX}, peak{SIZE_MAX};
    std::size_t blocks{SIZE_MAX}, workers{SIZE_MAX}, memberships{SIZE_MAX};
    std::size_t candidates{SIZE_MAX}, trials{SIZE_MAX}, targets{SIZE_MAX}, pages{SIZE_MAX};
};
enum class RootReconstructionFailureCode : std::uint8_t {
    shape, diagnostic_overflow, arithmetic, capacity, allocation_failure, reconstruction
};
struct RootReconstructionFailure final {
    RootReconstructionFailureCode code;
    FixedTypedReconstructionError detail{TypedReconstructionErrorCode::invalid_scalar, {}};
    std::size_t limit{}, observed{};
};
[[nodiscard]] auto check_root_reconstruction_shape(const nlohmann::json&) noexcept
    -> std::expected<void, RootReconstructionFailure>;
[[nodiscard]] auto count_root_reconstruction(const nlohmann::json&)
    -> std::expected<RootReconstructionUsage, RootReconstructionFailure>;
[[nodiscard]] auto check_root_reconstruction_limits(const RootReconstructionUsage&,
    const RootReconstructionLimits&) noexcept -> std::expected<void, RootReconstructionFailure>;
[[nodiscard]] auto reconstruct_finalized_evidence_bounded(const LogicalBody&,
    std::span<const core::LinuxCpuId>, const RootReconstructionLimits&)
    -> std::expected<FinalizedPairwiseRunEvidence, RootReconstructionFailure>;
} // namespace epycbench::runtime::wire_v1::detail
