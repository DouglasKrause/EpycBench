#pragma once
#include "results/pair_lifecycle_admission.hpp"
#include "epycbench/results/pairwise_run_result.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <memory>

namespace epycbench::results::pair_analysis {
namespace stock = runtime::wire_v1::detail;
struct Quantities final {
    std::size_t records{}, canonical_bytes{}, input_bytes{}, root_bytes{}, root_nodes{},
        root_strings{}, root_elements{}, root_depth{}, blocks{}, observations{}, historical{},
        comparison_replicates{}, stopping_replicates{}, result_bytes{}, workspace_bytes{},
        retained_bytes{}, simultaneous_bytes{};
    auto operator==(const Quantities&) const noexcept -> bool = default;
};
struct Limits final {
    Quantities values;
    stock::RootReconstructionLimits root;
    core::JcsLimits serialization;
};
struct Usage final {
    Quantities values;
    stock::RootReconstructionUsage root;
    auto operator==(const Usage&) const noexcept -> bool;
};
struct References final {
    std::uint64_t lifecycle_sequence{}, controller_sequence{}, runner_sequence{},
        pair_ordinal{}, normalization_sequence{};
    auto operator==(const References&) const noexcept -> bool = default;
};
enum class Error { state, reference, capacity, reconstruction, projection, serialization, mismatch };
class Owner final {
  public:
    ~Owner();
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto result() const noexcept -> const PairwiseRunResult*;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto references() const noexcept -> const References*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    struct State;
    explicit Owner(std::unique_ptr<State>) noexcept;
    [[nodiscard]] static auto prepare(const pair_lifecycle::Owner&, References,
        std::string_view, const Limits&, std::size_t prior, std::size_t session_stock)
        -> std::expected<Owner, Error>;
    std::unique_ptr<State> state_;
    friend class process_subject::Session;
};
namespace detail {
struct CheckedSum final {
    std::size_t value{};
    bool valid{true};
    void add(std::size_t count, std::size_t width=1U) noexcept {
        if (width && count>(SIZE_MAX-value)/width) { valid=false; return; }
        value+=count*width;
    }
};
struct ProjectionDimensions final {
    std::size_t blocks{}, valid{}, historical{}, workers{}, comparison_replicates{},
        stopping_replicates{}, metric_size{};
};
struct ProjectionStock final { std::size_t result{}, workspace{}; };
[[nodiscard]] auto projection_stock(const ProjectionDimensions&, const stock::RootReconstructionUsage&)
    noexcept -> std::expected<ProjectionStock, Error>;
[[nodiscard]] auto combine(const Usage&, const Usage&) noexcept -> std::expected<Usage, Error>;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
// Named fixed cells for this new admission chain. Accepted library stack internals
// remain separately bounded; this is conservative logical stock, not stack size.
struct AdmissionStaging final {
    const pair_lifecycle::Owner* parent;
    const block_evidence::PairPrefix* prefix;
    const block_evidence::ExpectedPairContext* context;
    const Limits* limits;
    References parameter_references, constructor_references;
    Usage usage, constructor_usage, returned_usage;
    std::string_view supplied, root;
    std::size_t prior, session_stock, common, parse, reconstruct, project, serialize,
        publication;
    std::expected<std::size_t, stock::WireJsonError> dom, parser, renderer, retirement, analysis_dom;
    CheckedSum common_sum, retained_sum, serializer_identity_sum;
    ProjectionDimensions dimensions;
    std::expected<ProjectionStock, Error> projection;
    std::optional<std::expected<stock::ComponentWireShape, stock::WireJsonError>> shape;
    std::optional<std::expected<stock::RootReconstructionUsage, stock::RootReconstructionFailure>> root_stock;
    std::optional<std::expected<runtime::wire_v1::LogicalBody, runtime::wire_v1::LogicalCodecError>> logical;
    std::optional<std::expected<runtime::FinalizedPairwiseRunEvidence, stock::RootReconstructionFailure>> reconstructed;
    std::optional<std::expected<runtime::FinalizedPairwiseRunEvidence, Error>> reconstruction_return;
    std::optional<std::expected<PairwiseRunResult, core::Error>> projected;
    std::optional<std::expected<PairwiseRunResult, Error>> projection_return;
    std::optional<std::expected<std::string, core::Error>> serialized;
    std::optional<PairwiseRunResult> constructor_result;
    std::string constructor_text;
    std::unique_ptr<std::byte> candidate_state;
    std::optional<std::expected<Owner, Error>> local_return, caller_return;
};
struct HelperStaging final {
    struct RootClosure {
        const std::string_view* bytes;
        const Limits* limits;
        Quantities* quantities;
        const block_evidence::ExpectedPairContext* const* context;
        Usage* usage;
        const void* phase;
    } reconstruct_closure, project_closure;
    struct PhaseClosure { const CheckedSum* common; Quantities* usage; const Limits* limits; } phase_closure;
    std::array<std::size_t, 5U> phase_arguments;
    std::initializer_list<std::size_t> phase_parameter;
    const std::size_t *phase_begin, *phase_end;
    std::size_t phase_value;
    CheckedSum phase_sum, result_sum, workspace_sum, combine_sum;
    const ProjectionDimensions* dimensions;
    const stock::RootReconstructionUsage* root_usage;
    const Quantities *fit_usage, *fit_limits;
    std::size_t Quantities::*fit_member;
    const std::size_t Quantities::*const* fit_begin;
    const std::size_t Quantities::*const* fit_end;
    const Usage *combine_prior, *combine_child;
    Usage combine_result, combine_return;
    std::size_t Quantities::*combine_member;
    std::size_t count_parameter, width_parameter;
    CheckedSum* sum_this;
    std::expected<ProjectionStock, Error> projection_return;
    std::unexpected<Error> rejection_return;
    Error rejection_parameter;
    std::array<bool,4U> fit_phase_sum_and_combine_returns;
    // Each max_size temporary below is a distinct vector in one full expression.
    std::vector<core::ObservationId> ids;
    std::vector<statistics::OutlierFlagState> flags;
    std::vector<statistics::Bandwidth> bandwidth;
    std::vector<statistics::DependentLoadLatency> latency;
    std::vector<kernel::DependentLoadLatencyBlockId> blocks;
    std::vector<statistics::PairedLogEffect> effects, prefix;
    std::vector<runtime::PairwiseBlockAttemptEvidence> raw;
    std::vector<ControllerStoppingReplay> replays;
    std::vector<RecalculationMismatchDiagnostic> diagnostics;
    std::vector<double> comparison_replicates, stopping_replicates;
    std::vector<std::uint64_t> ordinals;
};

}
} // namespace epycbench::results::pair_analysis
