#include "results/pair_analysis_admission.hpp"
#include "results/pairwise_analysis_serialization.hpp"
#include "results/calculation_failure.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"
#include <algorithm>
#include <limits>

namespace epycbench::results::pair_analysis {
namespace {
using Sum=detail::CheckedSum;
constexpr auto quantities = std::to_array<std::size_t Quantities::*>({
    &Quantities::records, &Quantities::canonical_bytes, &Quantities::input_bytes,
    &Quantities::root_bytes, &Quantities::root_nodes, &Quantities::root_strings,
    &Quantities::root_elements, &Quantities::root_depth, &Quantities::blocks,
    &Quantities::observations, &Quantities::historical, &Quantities::comparison_replicates,
    &Quantities::stopping_replicates, &Quantities::result_bytes, &Quantities::workspace_bytes,
    &Quantities::retained_bytes, &Quantities::simultaneous_bytes});
auto fits(const Quantities& usage, const Quantities& limits) noexcept -> bool {
    for (const auto member : quantities) if (usage.*member > limits.*member) return false;
    return true;
}
auto reject(Error error) noexcept -> std::unexpected<Error> { return std::unexpected{error}; }
}
struct Owner::State final {
    PairwiseRunResult result;
    std::string canonical;
    References references;
    Usage usage;
    State(PairwiseRunResult value, std::string bytes, References refs, Usage stock) noexcept
        : result{std::move(value)}, canonical{std::move(bytes)}, references{refs}, usage{stock} {}
};
Owner::~Owner() = default;
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
Owner::Owner(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
auto Owner::result() const noexcept -> const PairwiseRunResult* { return state_ ? &state_->result : nullptr; }
auto Owner::canonical() const noexcept -> std::string_view { return state_ ? state_->canonical : std::string_view{}; }
auto Owner::references() const noexcept -> const References* { return state_ ? &state_->references : nullptr; }
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage : Usage{}; }
auto Usage::operator==(const Usage& other) const noexcept -> bool {
    const auto& a=root; const auto& b=other.root;
    return values==other.values && a.typed==b.typed && a.scratch==b.scratch && a.ledger==b.ledger &&
        a.peak==b.peak && a.blocks==b.blocks && a.workers==b.workers && a.memberships==b.memberships &&
        a.candidates==b.candidates && a.trials==b.trials && a.targets==b.targets && a.pages==b.pages;
}
auto detail::fixed_stock() noexcept -> std::size_t { return sizeof(AdmissionStaging)+sizeof(HelperStaging); }
auto detail::projection_stock(const ProjectionDimensions& d, const stock::RootReconstructionUsage& root)
    noexcept -> std::expected<ProjectionStock, Error> {
    if (d.valid > std::vector<core::ObservationId>{}.max_size()/2U ||
        d.valid > std::vector<statistics::OutlierFlagState>{}.max_size()/2U ||
        d.valid > std::vector<statistics::Bandwidth>{}.max_size()/2U ||
        d.valid > std::vector<statistics::DependentLoadLatency>{}.max_size()/2U ||
        d.valid > std::vector<kernel::DependentLoadLatencyBlockId>{}.max_size() ||
        d.valid > std::vector<statistics::PairedLogEffect>{}.max_size() ||
        d.blocks > std::vector<runtime::PairwiseBlockAttemptEvidence>{}.max_size() ||
        d.blocks > std::vector<statistics::PairedLogEffect>{}.max_size() ||
        d.historical > std::vector<ControllerStoppingReplay>{}.max_size() ||
        d.historical > std::vector<RecalculationMismatchDiagnostic>{}.max_size() ||
        d.comparison_replicates > std::vector<double>{}.max_size() ||
        d.stopping_replicates > std::vector<double>{}.max_size() ||
        d.workers > std::vector<std::uint64_t>{}.max_size()/2U)
        return reject(Error::capacity);
    Sum result{sizeof(PairwiseRunResult)}, workspace;
    result.add(root.typed); result.add(root.ledger);
    result.add(d.valid, 4U * (sizeof(core::ObservationId) + sizeof(statistics::OutlierFlagState)));
    result.add(d.valid, sizeof(kernel::DependentLoadLatencyBlockId) + sizeof(statistics::PairedLogEffect));
    result.add(d.historical, sizeof(ControllerStoppingReplay) + sizeof(RecalculationMismatchDiagnostic) +
        results::detail::DiagnosticTextLimits::code + results::detail::DiagnosticTextLimits::stage +
        results::detail::DiagnosticTextLimits::field);
    // Sum disjoint math phases conservatively: ABBA has two inputs per role.
    workspace.add(d.valid, d.metric_size); workspace.add(d.valid, d.metric_size);
    workspace.add(d.valid, d.metric_size); workspace.add(d.valid, d.metric_size);
    workspace.add(d.valid, 4U * (sizeof(core::ObservationId) + sizeof(statistics::OutlierFlagState)));
    workspace.add(d.valid, sizeof(kernel::DependentLoadLatencyBlockId) + sizeof(statistics::PairedLogEffect));
    workspace.add(d.valid, 8U * sizeof(double)); // two descriptive + two outlier arrays, each 2V
    workspace.add(d.valid, 2U * sizeof(double) + sizeof(statistics::PairedLogEffect));
    workspace.add(std::max(d.comparison_replicates, d.stopping_replicates), sizeof(double));
    workspace.add(d.blocks, sizeof(statistics::PairedLogEffect));
    workspace.add(d.workers, 2U * sizeof(std::uint64_t));
    if (!result.valid || !workspace.valid) return reject(Error::capacity);
    return ProjectionStock{result.value, workspace.value};
}
auto detail::combine(const Usage& prior, const Usage& child) noexcept -> std::expected<Usage, Error> {
    auto result = prior;
    for (const auto member : quantities) {
        if (member == &Quantities::records || member == &Quantities::canonical_bytes ||
            member == &Quantities::retained_bytes) {
            Sum sum{prior.values.*member}; sum.add(child.values.*member);
            if (!sum.valid) return reject(Error::capacity);
            result.values.*member = sum.value;
        } else result.values.*member = std::max(prior.values.*member, child.values.*member);
    }
    const auto merge = [](auto a, auto b) { return std::max(a, b); };
    result.root = {merge(prior.root.typed,child.root.typed), merge(prior.root.scratch,child.root.scratch),
        merge(prior.root.ledger,child.root.ledger), merge(prior.root.peak,child.root.peak),
        merge(prior.root.blocks,child.root.blocks), merge(prior.root.workers,child.root.workers),
        merge(prior.root.memberships,child.root.memberships), merge(prior.root.candidates,child.root.candidates),
        merge(prior.root.trials,child.root.trials), merge(prior.root.targets,child.root.targets), merge(prior.root.pages,child.root.pages)};
    return result;
}
auto Owner::prepare(const pair_lifecycle::Owner& parent, References references,
    std::string_view supplied, const Limits& limits, std::size_t prior, std::size_t session_stock)
    -> std::expected<Owner, Error> {
    const auto* prefix = parent.prefix();
    const auto* context = prefix ? prefix->lifecycle_context() : nullptr;
    if (!prefix || !context || !prefix->lifecycle_outcomes_mode() || prefix->canonical_root().empty() ||
        !parent.facts().ordinal(runtime::detail::SyntheticLifecycleMark::typed_reconstruction_completed))
        return reject(Error::reference);
    const auto bytes = prefix->canonical_root();
    Usage usage{};
    auto& u = usage.values;
    u.records = 1U; u.canonical_bytes = supplied.size(); u.input_bytes = supplied.size(); u.root_bytes = bytes.size();
    if (supplied.empty() || !fits(u, limits.values) || limits.serialization.depth > 64U ||
        limits.values.root_depth > 64U || !limits.serialization.nodes || !limits.serialization.output_bytes)
        return reject(Error::capacity);
    Sum common{prior}; common.add(detail::fixed_stock()); common.add(session_stock); common.add(supplied.size());
    if (!common.valid) return reject(Error::capacity);
    auto phase = [&](std::initializer_list<std::size_t> stocks) {
        Sum sum{common.value}; for (auto value : stocks) sum.add(value);
        u.simultaneous_bytes = std::max(u.simultaneous_bytes, sum.value);
        return sum.valid && u.simultaneous_bytes <= limits.values.simultaneous_bytes;
    };
    auto projected = [&]() -> std::expected<PairwiseRunResult, Error> {
        auto reconstructed = [&]() -> std::expected<runtime::FinalizedPairwiseRunEvidence, Error> {
            const auto shape = stock::count_component_wire_shape(bytes, limits.values.root_depth);
            if (!shape) return reject(Error::reconstruction);
            u.root_nodes = shape->nodes; u.root_strings = shape->strings;
            u.root_elements = shape->elements; u.root_depth = shape->depth;
            const auto j = stock::component_dom_stock(shape->nodes, shape->strings);
            const auto p = stock::component_parser_stock(shape->bytes, shape->depth);
            const auto q = stock::component_renderer_stock(shape->depth, shape->members, shape->bytes);
            const auto retirement = stock::multiply_component_stock(shape->depth, sizeof(stock::JsonRetirementFrame));
            if (!fits(u, limits.values) || !j || !p || !q || !retirement || !phase({*j,*j,*p,*q}))
                return reject(Error::capacity);
            auto logical = runtime::wire_v1::decode_evidence_body(bytes, context->capacity());
            if (!logical) {
                if (logical.error().code==runtime::wire_v1::LogicalCodecErrorCode::allocation_failure) throw std::bad_alloc{};
                return reject(Error::reconstruction);
            }
            auto counted = stock::count_root_reconstruction(logical->value());
            if (!counted || !stock::check_root_reconstruction_limits(*counted, limits.root))
                return reject(Error::capacity);
            usage.root = *counted;
            if (!phase({*j,*retirement,counted->typed,counted->scratch,counted->ledger})) return reject(Error::capacity);
            auto root = stock::reconstruct_finalized_evidence_bounded(*logical, context->cpus(), limits.root);
            if (!root) {
                if (root.error().code==stock::RootReconstructionFailureCode::allocation_failure) throw std::bad_alloc{};
                return reject(Error::reconstruction);
            }
            return std::move(*root);
        }(); // Logical DOM and reconstruction scratch are retired here.
        if (!reconstructed) return reject(reconstructed.error());
        detail::ProjectionDimensions d{};
        d.blocks = reconstructed->blocks().size(); d.historical = reconstructed->stopping_evaluations().size();
        for (const auto& block : reconstructed->blocks())
            if (block.completion == runtime::BlockAttemptCompletion::complete_valid) ++d.valid;
        if (reconstructed->finalization()) d.workers = reconstructed->finalization()->first.workers.size() +
            reconstructed->finalization()->second.workers.size();
        d.metric_size = std::max(sizeof(statistics::Bandwidth), sizeof(statistics::DependentLoadLatency));
        if (d.valid && std::holds_alternative<runtime::NormalRunTerminalEvidence>(reconstructed->terminal()))
            d.comparison_replicates = reconstructed->statistics_policy().comparison_bootstrap_policy().replicate_count().value();
        if (d.historical) d.stopping_replicates = reconstructed->statistics_policy().stopping_bootstrap_policy().replicate_count().value();
        if (d.valid > SIZE_MAX / 2U) return reject(Error::capacity);
        u.blocks=d.blocks; u.observations=d.valid*2U; u.historical=d.historical;
        u.comparison_replicates=d.comparison_replicates; u.stopping_replicates=d.stopping_replicates;
        auto stock = detail::projection_stock(d, usage.root);
        if (!stock) return reject(stock.error());
        u.result_bytes=stock->result; u.workspace_bytes=stock->workspace;
        if (!fits(u, limits.values) || !phase({usage.root.typed,usage.root.ledger,u.result_bytes,u.workspace_bytes}))
            return reject(Error::capacity);
        auto result = project_pairwise_run_result(*reconstructed);
        if (!result) return reject(Error::projection);
        return std::move(*result);
    }(); // The reconstructed raw ledger is retired before serialization.
    if (!projected) return reject(projected.error());
    const auto analysis_dom = stock::component_dom_stock(limits.serialization.nodes, limits.serialization.output_bytes);
    Sum scratch; scratch.add(limits.serialization.nodes,sizeof(std::uint64_t));
    if (!analysis_dom || !scratch.valid || !phase({u.result_bytes,*analysis_dom,scratch.value,limits.serialization.output_bytes}))
        return reject(Error::capacity);
    auto serialized = analysis::serialize_pairwise_analysis(*projected, limits.serialization);
    if (!serialized) return reject(Error::serialization);
    if (*serialized != supplied) return reject(Error::mismatch);
    u.canonical_bytes = serialized->size();
    Sum retained{sizeof(Owner)+sizeof(State)};
    retained.add(u.result_bytes); retained.add(u.canonical_bytes);
    u.retained_bytes=retained.value;
    if (!retained.valid || !fits(u,limits.values) || !phase({u.retained_bytes})) return reject(Error::capacity);
    static_assert(sizeof(std::unique_ptr<State>)==sizeof(std::unique_ptr<std::byte>));
    return Owner{std::make_unique<State>(std::move(*projected),std::move(*serialized),references,usage)};
}
} // namespace epycbench::results::pair_analysis
