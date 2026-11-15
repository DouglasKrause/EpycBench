#include "results/run_creation_admission.hpp"
#include <algorithm>
#include <numeric>
#include <limits>
#include <type_traits>

namespace epycbench::results::run_creation {
namespace p = process_subject;
namespace {
struct Cells {
    const Input* input;
    const Limits* limits;
    Usage measured, returned;
    std::size_t prior, session_stock, input_stock, candidate_stock, peak;
    std::unique_ptr<detail::State> candidate, constructor_parameter;
    std::optional<Owner> constructed;
    std::optional<std::expected<Owner, Error>> result, result_return;
    authored_source::Input source, left, right;
    host_assembly::Usage host, host_return;
    build_manifest::Usage manifest, manifest_return;
    private_error::Usage error, error_return;
    const FailureFact *fact, *primary, *selected;
    const Binding* binding;
    std::string_view run, left_text, right_text;
    Target target;
    Binding binding_value;
    TargetPosition position, position_parameter;
    std::size_t failure_index, target_index, binding_index, token_index, target_count;
    std::optional<std::uint64_t> process, primary_ordinal;
    std::optional<Error> failure;
    // Each sorting closure borrows the input; lookup closures borrow spans.
    const Input *target_comparator, *binding_comparator;
    std::span<const FailureFact> lookup_facts;
    std::span<const Binding> lookup_bindings;
    std::span<const std::size_t> lookup_index;
    const Target* lookup_target;
    std::span<const std::size_t>::iterator found;
};
auto live(const Input& input) noexcept -> bool {
    return input.source.logical_bytes() >= sizeof(authored_source::SourceBundle) &&
        input.host.categories() && !input.run.text().empty();
}
auto bound(const Input& input, const detail::State& state, const Target& target) noexcept
    -> const FailureFact* {
    return detail::bound_failure(input.failures, input.bindings, state.binding_index, target);
}
}
auto detail::fixed_stock() noexcept -> std::size_t { return sizeof(Cells); }
auto detail::add(std::size_t& value, std::size_t amount) noexcept -> bool {
    if (amount > SIZE_MAX - value) return false;
    value += amount;
    return true;
}
auto detail::elements(std::size_t& value, std::size_t count, std::size_t size) noexcept -> bool {
    return (size == 0U || count <= SIZE_MAX / size) && add(value, count * size);
}
auto detail::fits(const Usage& u, const Limits& l) noexcept -> bool {
    return u.records <= l.records && u.artifact_slots <= l.artifact_slots &&
        u.source_bytes <= l.source_bytes && u.source_name_bytes <= l.source_name_bytes &&
        u.override_count <= l.override_count && u.override_bytes <= l.override_bytes &&
        u.failure_facts <= l.failure_facts && u.targets <= l.targets && u.bindings <= l.bindings &&
        u.error_context_nodes <= l.error_context_nodes && u.error_string_bytes <= l.error_string_bytes &&
        u.error_binary_bytes <= l.error_binary_bytes && u.canonical_child_bytes <= l.canonical_child_bytes &&
        u.retained_bytes <= l.retained_bytes && u.consumer_simultaneous_bytes <= l.consumer_simultaneous_bytes;
}
auto detail::same_source(const authored_source::SourceBundle& a,
                         const authored_source::SourceBundle& b) noexcept -> bool {
    if (a.logical_bytes() == 0U || b.logical_bytes() == 0U) return false;
    const auto left = a.input(), right = b.input();
    return left.availability == right.availability && left.bytes == right.bytes &&
        left.name == right.name && left.format == right.format &&
        left.acquisition_failure == right.acquisition_failure &&
        std::ranges::equal(left.rejected_overrides, right.rejected_overrides);
}
auto detail::bound_failure(std::span<const FailureFact> facts, std::span<const Binding> bindings,
    std::span<const std::size_t> index, const Target& target) noexcept -> const FailureFact* {
    const auto found = std::lower_bound(index.begin(), index.end(), target,
        [&](std::size_t i, const Target& t) { return bindings[i].target < t; });
    if (found == index.end() || bindings[*found].target != target ||
        bindings[*found].failure_ordinal >= facts.size()) return nullptr;
    return &facts[static_cast<std::size_t>(bindings[*found].failure_ordinal)];
}
Owner::Owner(std::unique_ptr<detail::State> state) noexcept : state_(std::move(state)) {}
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
Owner::~Owner() = default;
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage : Usage{}; }
auto Owner::failure_for(const Target& target) const noexcept -> const FailureFact* {
    return state_ ? detail::bound_failure(state_->failures, state_->bindings,
                                        state_->binding_index, target) : nullptr;
}
auto Owner::failure_at(std::uint64_t ordinal) const noexcept -> const FailureFact* {
    return state_ && ordinal < state_->failures.size() ?
        &state_->failures[static_cast<std::size_t>(ordinal)] : nullptr;
}
auto Owner::prepare(const Input& input, const Limits& limits, std::size_t prior,
    std::size_t session_stock) -> std::expected<Owner, Error> {
    using enum Error;
    if (!live(input)) return std::unexpected{invalid_input};
    if (input.host.run_id() != input.run.text()) return std::unexpected{identity};
    if (limits.records > 1U || limits.failure_facts > 1024U || limits.targets > 65536U ||
        limits.bindings > 65536U || input.failures.size() > 1024U || input.bindings.size() > 65536U)
        return std::unexpected{capacity};
    const auto source = input.source.input();
    const auto& build = input.build;
    if ((build.state != p::BuildState::available && build.state != p::BuildState::not_attempted &&
         build.state != p::BuildState::not_obtained) ||
        ((build.state == p::BuildState::available) != build.manifest.has_value()) ||
        ((build.state == p::BuildState::not_obtained) != build.acquisition_failure.has_value()) ||
        (build.manifest && (!build.manifest->input() || !build.manifest->digest())))
        return std::unexpected{invalid_input};
    Usage measured{.records = 1, .artifact_slots = 9, .source_bytes = source.bytes.size(),
        .source_name_bytes = source.name.size(), .override_count = source.rejected_overrides.size(),
        .failure_facts = input.failures.size(), .bindings = input.bindings.size()};
    for (const auto token : source.rejected_overrides)
        if (!detail::add(measured.override_bytes, token.size())) return std::unexpected{capacity};
    measured.retained_bytes = sizeof(Owner) + sizeof(detail::State);
    if (input.host.usage().retained_bytes < sizeof(host_assembly::Owner) ||
        !detail::add(measured.retained_bytes, input.run.text().size()) ||
        !detail::add(measured.retained_bytes, input.source.logical_bytes() - sizeof(authored_source::SourceBundle)) ||
        !detail::add(measured.retained_bytes, input.host.usage().retained_bytes - sizeof(host_assembly::Owner)))
        return std::unexpected{capacity};
    measured.canonical_child_bytes = input.host.usage().canonical_bytes;
    if (build.manifest) {
        const auto stock = build.manifest->usage();
        if (stock.total_bytes < sizeof(build_manifest::Owner) ||
            !detail::add(measured.retained_bytes, stock.total_bytes - sizeof(build_manifest::Owner)) ||
            !detail::add(measured.canonical_child_bytes, stock.canonical_bytes)) return std::unexpected{capacity};
    }
    for (std::size_t i = 0; i < input.failures.size(); ++i) {
        const auto& fact = input.failures[i];
        if (!detail::add(measured.targets, fact.targets.size()) || measured.targets > 65536U)
            return std::unexpected{capacity};
        if (!fact.error.source() || fact.targets.empty() ||
            (fact.producer != p::Producer::controller && fact.producer != p::Producer::runner && fact.producer != p::Producer::results) ||
            (fact.process_sequence && *fact.process_sequence != 1U)) return std::unexpected{reference};
        if (fact.secondary_of) {
            if (*fact.secondary_of != 1U || !fact.secondary_creation_ordinal ||
                *fact.secondary_creation_ordinal >= i) return std::unexpected{reference};
            const auto& primary = input.failures[static_cast<std::size_t>(*fact.secondary_creation_ordinal)];
            if (primary.secondary_of || primary.process_sequence != fact.process_sequence)
                return std::unexpected{reference};
        } else if (fact.secondary_creation_ordinal) return std::unexpected{reference};
        for (const auto& target : fact.targets)
            if (!p::detail::valid_target(target, fact)) return std::unexpected{reference};
        const auto stock = fact.error.usage();
        if (stock.retained_bytes < sizeof(private_error::Owner) ||
            !detail::add(measured.error_context_nodes, stock.nodes) ||
            !detail::add(measured.error_string_bytes, stock.string_bytes) ||
            !detail::add(measured.error_binary_bytes, stock.binary_bytes) ||
            !detail::add(measured.retained_bytes, stock.retained_bytes - sizeof(private_error::Owner)))
            return std::unexpected{capacity};
    }
    if (!detail::elements(measured.retained_bytes, measured.failure_facts, sizeof(FailureFact)) ||
        !detail::elements(measured.retained_bytes, measured.targets, sizeof(Target) + sizeof(TargetPosition)) ||
        !detail::elements(measured.retained_bytes, measured.bindings, sizeof(Binding) + sizeof(std::size_t)))
        return std::unexpected{capacity};
    // The input owns children, while the candidate initially owns only empty cells
    // and indices. Count each actual allocation once through the transfer boundary.
    std::size_t input_stock = sizeof(Input) + sizeof(journal::RunId);
    if (!detail::add(input_stock, measured.retained_bytes - sizeof(Owner) - sizeof(detail::State)) ||
        measured.targets > SIZE_MAX / sizeof(TargetPosition) || measured.bindings > SIZE_MAX / sizeof(std::size_t))
        return std::unexpected{capacity};
    input_stock -= measured.targets * sizeof(TargetPosition) + measured.bindings * sizeof(std::size_t);
    std::size_t candidate_stock = sizeof(detail::State);
    if (!detail::add(candidate_stock, input.run.text().size()) ||
        !detail::elements(candidate_stock, measured.targets, sizeof(TargetPosition)) ||
        !detail::elements(candidate_stock, measured.bindings, sizeof(std::size_t))) return std::unexpected{capacity};
    auto peak = prior;
    if (!detail::add(peak, session_stock) || !detail::add(peak, detail::fixed_stock()) ||
        !detail::add(peak, input_stock) || !detail::add(peak, candidate_stock)) return std::unexpected{capacity};
    measured.consumer_simultaneous_bytes = peak;
    if (!detail::fits(measured, limits)) return std::unexpected{capacity};

    auto candidate = std::make_unique<detail::State>();
    candidate->run = input.run.text();
    candidate->domain = input.domain;
    candidate->usage = measured;
    candidate->targets.reserve(measured.targets);
    for (std::size_t i = 0; i < input.failures.size(); ++i)
        for (std::size_t j = 0; j < input.failures[i].targets.size(); ++j) candidate->targets.push_back({i, j});
    const auto less = [&](TargetPosition a, TargetPosition b) {
        const auto& left = input.failures[a.failure].targets[a.target];
        const auto& right = input.failures[b.failure].targets[b.target];
        return left != right ? left < right : a.failure < b.failure;
    };
    std::sort(candidate->targets.begin(), candidate->targets.end(), less);
    for (std::size_t i = 1; i < candidate->targets.size(); ++i)
        if (!less(candidate->targets[i - 1], candidate->targets[i])) return std::unexpected{reference};
    candidate->binding_index.resize(measured.bindings);
    std::iota(candidate->binding_index.begin(), candidate->binding_index.end(), 0U);
    std::sort(candidate->binding_index.begin(), candidate->binding_index.end(),
        [&](std::size_t a, std::size_t b) { return input.bindings[a].target < input.bindings[b].target; });
    for (std::size_t i = 0; i < candidate->binding_index.size(); ++i) {
        const auto& binding = input.bindings[candidate->binding_index[i]];
        if (binding.failure_ordinal >= input.failures.size() ||
            (i != 0U && input.bindings[candidate->binding_index[i - 1]].target == binding.target))
            return std::unexpected{reference};
        const auto found = std::lower_bound(candidate->targets.begin(), candidate->targets.end(), binding,
            [&](TargetPosition pos, const Binding& b) {
                const auto& target = input.failures[pos.failure].targets[pos.target];
                return target != b.target ? target < b.target : pos.failure < b.failure_ordinal;
            });
        if (found == candidate->targets.end() || found->failure != binding.failure_ordinal ||
            input.failures[found->failure].targets[found->target] != binding.target) return std::unexpected{reference};
    }
    if (source.availability == authored_source::Availability::not_obtained) {
        const auto* fact = bound(input, *candidate, {p::Category::authored_source, p::Selector::source_acquisition});
        if (!source.acquisition_failure || source.acquisition_failure->value != 1U || !fact ||
            fact->producer != p::Producer::controller || fact->process_sequence) return std::unexpected{reference};
    }
    if (build.state == p::BuildState::not_obtained) {
        const auto* fact = bound(input, *candidate, {p::Category::base_build, p::Selector::base_build_acquisition});
        if (*build.acquisition_failure != 1U || !fact || fact->producer != p::Producer::controller ||
            fact->process_sequence) return std::unexpected{reference};
    }
    return Owner{std::move(candidate)};
}
void Owner::commit(Input& input) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<authored_source::SourceBundle>);
    static_assert(std::is_nothrow_move_constructible_v<p::BaseBuild>);
    static_assert(std::is_nothrow_move_constructible_v<host_assembly::Owner>);
    state_->source.emplace(std::move(input.source));
    state_->build.emplace(std::move(input.build));
    state_->host.emplace(std::move(input.host));
    state_->failures = std::move(input.failures);
    state_->bindings = std::move(input.bindings);
}
} // namespace epycbench::results::run_creation
