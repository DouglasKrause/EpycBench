#include "results/observation_timing_admission.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace epycbench::results::observation_timing {
namespace block = block_evidence;
Input::Input(IdentityView source, Boundary point, Disposition state) noexcept
    : identity{source}, boundary{point}, disposition{state} {}
Input::Input(Input&& from) noexcept
    : identity{from.identity}, boundary{from.boundary}, disposition{from.disposition},
      clock{from.clock}, raw_start{from.raw_start}, utc{from.utc}, error{std::move(from.error)},
      live_{std::exchange(from.live_, false)} {}
auto Input::operator=(Input&& from) noexcept -> Input& {
    if (this != &from) {
        std::destroy_at(this);
        std::construct_at(this, std::move(from));
    }
    return *this;
}
struct Owner::State final {
    State(block::PairFactsLease&& lease, const Input& input, const Usage& measured,
          std::uint64_t calibration) noexcept
        : context{std::move(lease)}, domain{input.identity.domain}, family{input.identity.family},
          block{input.identity.block}, slot{input.identity.slot}, variant{input.identity.variant},
          observation{input.identity.observation}, boundary{input.boundary}, disposition{input.disposition},
          clock{input.clock}, raw_start{input.raw_start}, calibration_sequence{calibration}, usage{measured} {}
    block::PairFactsLease context;
    block::Domain domain;
    Family family;
    kernel::DependentLoadLatencyBlockId block;
    std::uint64_t slot;
    measurement::ComparisonVariantSlot variant;
    core::ObservationId observation;
    Boundary boundary;
    Disposition disposition;
    std::optional<Clock> clock;
    std::optional<platform_linux::RawMonotonicTimestamp> raw_start;
    std::optional<journal::Timestamp> utc;
    std::optional<private_error::Owner> error;
    std::uint64_t calibration_sequence;
    Usage usage;
};
namespace {
auto reject(block::ErrorCode code) noexcept {
    return std::unexpected{block::Error{code, block::Phase::event}};
}
struct MeasureCells final {
    Usage measured;
    private_error::Usage error_usage;
    const Input* input;
    const block::PairFactsLease* context;
    const runtime::PairwiseRunProvenance* provenance;
    block::Domain domain;
    const IdentityView* identity;
    std::expected<Usage, block::Error> returned;
};
struct MatchCells final {
    const Input* input;
    const runtime::FactualObservationEvidence* factual;
    const runtime::FailedObservationEvidence* failed;
    std::optional<measurement::ScheduledObservationSlot> slot;
    core::ObservationId id;
    std::optional<platform_linux::RawMonotonicTimestamp> actual_start;
    const Input* visitor_capture;
    const void* visited_observation;
    platform_linux::RawMonotonicTimestamp returned_start;
    bool result;
};
struct AccumulateCells final {
    const Usage *before, *incoming;
    Usage result;
    std::optional<Usage> returned;
};
struct PrepareCells final {
    block::PairFactsLease* lease;
    const Input* input;
    const Usage* usage;
    std::uint64_t sequence;
    std::unique_ptr<Owner::State> state, constructor_argument;
    Owner returned_owner;
    std::expected<Owner, block::Error> returned_result;
    block::PairFactsLease* constructor_lease;
    const Input* constructor_input;
    const Usage* constructor_usage;
    std::uint64_t constructor_sequence;
};
struct CommonCells final {
    Input caller_input;
    private_error::Usage returned_error_usage;
    block::ContextUsage returned_context_usage;
    std::size_t *sum_parameter, add_parameter;
    const Usage* fits_usage;
    const Limits* fits_limits;
    block::ErrorCode reject_parameter;
    block::Error error;
    std::unexpected<block::Error> unexpected, returned_unexpected;
    Owner* commit_this;
    Input* commit_input;
};
} // namespace
Owner::Owner(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
Owner::~Owner() = default;
auto Owner::state_bytes() noexcept -> std::size_t { return sizeof(State); }
auto Owner::staging_bytes() noexcept -> std::size_t {
    return sizeof(MeasureCells) + sizeof(MatchCells) + sizeof(AccumulateCells) +
        sizeof(PrepareCells) + sizeof(CommonCells);
}
auto Owner::identity() const noexcept -> std::optional<IdentityView> {
    if (!state_) return std::nullopt;
    const auto& p = state_->context.provenance();
    return IdentityView{&state_->context.run(), state_->domain, p.experiment_id, p.scenario_profile,
        p.scenario_digest, p.resolved_plan_digest, p.workload_id, p.first_variant_id, p.second_variant_id,
        state_->family, state_->block, state_->slot, state_->variant, state_->observation};
}
auto Owner::boundary() const noexcept -> std::optional<Boundary> { return state_ ? std::optional{state_->boundary} : std::nullopt; }
auto Owner::disposition() const noexcept -> std::optional<Disposition> { return state_ ? std::optional{state_->disposition} : std::nullopt; }
auto Owner::clock() const noexcept -> std::optional<Clock> { return state_ ? state_->clock : std::nullopt; }
auto Owner::raw_start() const noexcept -> std::optional<platform_linux::RawMonotonicTimestamp> { return state_ ? state_->raw_start : std::nullopt; }
auto Owner::utc() const noexcept -> const journal::Timestamp* { return state_ && state_->utc ? &*state_->utc : nullptr; }
auto Owner::error() const noexcept -> const private_error::Owner* { return state_ && state_->error ? &*state_->error : nullptr; }
auto Owner::calibration_sequence() const noexcept -> std::optional<std::uint64_t> { return state_ ? std::optional{state_->calibration_sequence} : std::nullopt; }
auto Owner::usage() const noexcept -> const Usage* { return state_ ? &state_->usage : nullptr; }
auto Owner::shared_context_bytes() const noexcept -> std::size_t {
    return state_ ? state_->context.usage().retained_bytes - sizeof(block::PairFactsLease) : 0U;
}
auto detail::measure(const Input& input, const block::PairFactsLease& context, block::Domain domain) noexcept
    -> std::expected<Usage, block::Error> {
    if (!input.available()) return reject(block::ErrorCode::state);
    if (static_cast<unsigned>(input.boundary) > 1U || static_cast<unsigned>(input.disposition) > 2U ||
        static_cast<unsigned>(input.identity.family) > 1U || static_cast<unsigned>(input.identity.domain) > 1U ||
        static_cast<unsigned>(input.identity.variant) > 1U || (input.clock && *input.clock != Clock::monotonic_raw))
        return reject(block::ErrorCode::malformed);
    if ((input.disposition == Disposition::available) != bool(input.utc) ||
        (input.disposition == Disposition::acquisition_failed) != bool(input.error) ||
        (input.utc && input.utc->text().empty()) || (input.error && !input.error->source()) ||
        (input.boundary == Boundary::completed_enclosing_start ? (!input.clock || !input.raw_start) : bool(input.clock) || bool(input.raw_start)))
        return reject(block::ErrorCode::malformed);
    const auto& p = context.provenance();
    const auto& i = input.identity;
    if (!i.run || i.run->text().empty() || i.run->text() != context.run().text() || i.domain != domain ||
        i.experiment != p.experiment_id || i.scenario_profile != p.scenario_profile ||
        i.scenario_digest != p.scenario_digest || i.resolved_plan_digest != p.resolved_plan_digest ||
        i.workload != p.workload_id || i.first_variant != p.first_variant_id || i.second_variant != p.second_variant_id ||
        static_cast<std::size_t>(i.family) != context.kernel(false).index())
        return reject(block::ErrorCode::identity);
    Usage measured{};
    measured.records = 1U;
    measured.utc_bytes = input.utc ? input.utc->text().size() : 0U;
    measured.retained_bytes = Owner::state_bytes();
    if (input.error) {
        const auto usage = input.error->usage();
        if (usage.retained_bytes < sizeof(private_error::Owner)) return reject(block::ErrorCode::capacity);
        measured.error_nodes = usage.nodes; measured.error_depth = usage.depth;
        measured.error_string_bytes = usage.string_bytes; measured.error_binary_bytes = usage.binary_bytes;
        measured.error_retained_bytes = usage.retained_bytes - sizeof(private_error::Owner);
    }
    if (!add(measured.retained_bytes, measured.utc_bytes) ||
        !add(measured.retained_bytes, measured.error_retained_bytes)) return reject(block::ErrorCode::capacity);
    return measured;
}
auto detail::matches(const Input& input, const runtime::FactualObservationEvidence* factual,
    const runtime::FailedObservationEvidence* failed) noexcept -> bool {
    if ((!factual && !failed) || (factual && failed)) return false;
    const auto slot = factual ? factual->slot : failed->slot;
    const auto id = factual ? factual->observation_id : failed->observation_id;
    if (slot.block_id() != input.identity.block || slot.slot_index() != input.identity.slot ||
        slot.comparison_variant() != input.identity.variant || id != input.identity.observation) return false;
    if (!factual) return input.boundary == Boundary::failed_attempt_start;
    return input.boundary == Boundary::completed_enclosing_start &&
        factual->observation.index() == static_cast<std::size_t>(input.identity.family) &&
        std::visit([&](const auto& observation) {
            return input.raw_start == observation.enclosing_interval().start();
        }, factual->observation);
}
auto detail::accumulate(const Usage& before, const Usage& incoming) noexcept -> std::optional<Usage> {
    Usage result = before;
    if (!add(result.records, incoming.records) || !add(result.utc_bytes, incoming.utc_bytes) ||
        !add(result.error_nodes, incoming.error_nodes) || !add(result.error_string_bytes, incoming.error_string_bytes) ||
        !add(result.error_binary_bytes, incoming.error_binary_bytes) || !add(result.error_retained_bytes, incoming.error_retained_bytes) ||
        !add(result.retained_bytes, incoming.retained_bytes)) return std::nullopt;
    result.error_depth = std::max(before.error_depth, incoming.error_depth);
    result.simultaneous_bytes = std::max(before.simultaneous_bytes, incoming.simultaneous_bytes);
    return result;
}
auto detail::fits(const Usage& usage, const Limits& limits) noexcept -> bool {
    return usage.records <= limits.records && usage.utc_bytes <= limits.utc_bytes && usage.error_nodes <= limits.error_nodes &&
        usage.error_depth <= limits.error_depth && usage.error_string_bytes <= limits.error_string_bytes &&
        usage.error_binary_bytes <= limits.error_binary_bytes && usage.error_retained_bytes <= limits.error_retained_bytes &&
        usage.retained_bytes <= limits.retained_bytes && usage.simultaneous_bytes <= limits.simultaneous_bytes;
}
auto Owner::prepare(block::PairFactsLease&& lease, const Input& input, const Usage& usage,
    std::uint64_t sequence) -> std::expected<Owner, block::Error> {
    try {
        auto state = std::make_unique<State>(std::move(lease), input, usage, sequence);
        if (input.utc) state->utc.emplace(*input.utc);
        return Owner{std::move(state)};
    } catch (const std::bad_alloc&) { return reject(block::ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(block::ErrorCode::capacity); }
}
void Owner::commit(Input& input) noexcept {
    state_->error = std::move(input.error);
    input.error.reset();
    input.live_ = false;
}
} // namespace epycbench::results::observation_timing
