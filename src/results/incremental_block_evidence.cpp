#include "results/incremental_block_evidence.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
#include "runtime/pairwise_block_association.hpp"
#include "runtime/finalized_evidence_wire_typed_stock.hpp"

#include <algorithm>
#include <charconv>
#include <new>

namespace epycbench::results::block_evidence {
namespace wire = runtime::wire_v1;
namespace d = wire::detail;
using Kind = wire::EvidenceComponentKind;
using Value = std::variant<measurement::MeasurementBlockPlan,
    runtime::FactualObservationEvidence, runtime::FailedObservationEvidence>;
namespace {
struct Sum final {
    std::size_t value{};
    bool valid{true};
    void add(std::size_t n, std::size_t size = 1U) noexcept {
        const auto product = d::multiply_component_stock(n, size);
        if (!product) { valid = false; return; }
        const auto sum = d::add_wire_json_count(value, *product);
        if (!sum) valid = false; else value = *sum;
    }
};
auto failure(ErrorCode code, Phase phase, std::size_t limit = 0, std::size_t observed = 0) noexcept {
    return std::unexpected{Error{code, phase, limit, observed}};
}
auto typed_failure(const d::FixedTypedReconstructionError& from) noexcept {
    Error result{ErrorCode::reconstruction, Phase::reconstruct};
    result.detail_code = static_cast<std::uint8_t>(from.code);
    if (!from.path.fixed() || from.path.overflowed()) {
        result.code = ErrorCode::diagnostic_overflow;
        return std::unexpected{result};
    }
    const auto text = from.path.view();
    auto& out = result.diagnostic;
    for (std::size_t i{}; i < text.size();) {
        if (text[i] == '[') {
            const auto end = text.find(']', i + 1U);
            std::uint64_t ordinal{};
            const auto parsed = end == std::string_view::npos ? std::from_chars_result{}
                : std::from_chars(text.data() + i + 1U, text.data() + end, ordinal);
            if (end == std::string_view::npos || parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + end || out.ordinal_count == out.ordinals.size() ||
                out.identity_size + 2U > out.identity.size()) {
                result.code = ErrorCode::diagnostic_overflow; return std::unexpected{result};
            }
            out.identity[out.identity_size++] = '[';
            out.identity[out.identity_size++] = ']';
            out.ordinals[out.ordinal_count++] = ordinal;
            i = end + 1U;
        } else {
            if (out.identity_size == out.identity.size()) {
                result.code = ErrorCode::diagnostic_overflow; return std::unexpected{result};
            }
            out.identity[out.identity_size++] = text[i++];
        }
    }
    return std::unexpected{result};
}
auto matches(const runtime::FactualObservationEvidence& evidence, const PairFactsLease& context) noexcept -> bool {
    const auto& expected = context.kernel(evidence.slot.comparison_variant() == measurement::ComparisonVariantSlot::second);
    return std::visit([&](const auto& measured) {
        using Measured = std::remove_cvref_t<decltype(measured)>;
        constexpr bool bandwidth = std::same_as<Measured, measurement::SequentialReadBandwidthObservation>;
        using Inputs = std::conditional_t<bandwidth, runtime::AdmittedBandwidthExecutionInputs,
                                         runtime::AdmittedLatencyExecutionInputs>;
        const auto* input = std::get_if<Inputs>(&expected);
        if (!input || measured.experiment_id() != context.provenance().experiment_id ||
            !(measured.configuration() == input->configuration()) ||
            measured.workers().size() != context.workers().size()) return false;
        for (std::size_t i{}; i < measured.workers().size(); ++i)
            if (measured.workers()[i].worker_id() != context.workers()[i]) return false;
        return true;
    }, evidence.observation);
}
}
struct Component::State final {
    State(PairFactsLease expected, Kind k, Value supplied_value, std::string text, ComponentUsage count)
        : context{std::move(expected)}, kind{k}, value{std::move(supplied_value)}, canonical{std::move(text)}, usage{count} {}
    PairFactsLease context;
    Kind kind;
    Value value;
    std::string canonical;
    ComponentUsage usage;
};
Component::Component() noexcept = default;
Component::Component(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Component::Component(Component&&) noexcept = default;
auto Component::operator=(Component&&) noexcept -> Component& = default;
Component::~Component() = default;
auto Component::kind() const noexcept -> Kind { return state_->kind; }
auto Component::canonical() const noexcept -> std::string_view { return state_->canonical; }
auto Component::usage() const noexcept -> ComponentUsage { return state_->usage; }
auto Component::plan() const noexcept -> const measurement::MeasurementBlockPlan* { return state_ ? std::get_if<measurement::MeasurementBlockPlan>(&state_->value) : nullptr; }
auto Component::factual() const noexcept -> const runtime::FactualObservationEvidence* { return state_ ? std::get_if<runtime::FactualObservationEvidence>(&state_->value) : nullptr; }
auto Component::failed() const noexcept -> const runtime::FailedObservationEvidence* { return state_ ? std::get_if<runtime::FailedObservationEvidence>(&state_->value) : nullptr; }

auto Component::admit(const PairFactsLease& context, Kind kind, const Component* plan,
    std::size_t slot, std::string_view bytes, Limits limits, std::size_t owned)
    -> std::expected<Component, Error> {
    try {
        if (!context.available() || limits.depth > 64U || bytes.size() > limits.canonical_bytes)
            return failure(ErrorCode::capacity, Phase::parse);
        if (kind != Kind::block_plan && (!plan || !plan->plan() || slot >= 4U ||
            !plan->state_->context.same_owner(context))) return failure(ErrorCode::association, Phase::parse);
        const auto shape = d::count_component_wire_shape(bytes, limits.depth);
        if (!shape) return failure(shape.error().code == d::WireJsonFailure::malformed
                                     ? ErrorCode::malformed : ErrorCode::capacity, Phase::parse);
        if (shape->nodes > limits.nodes || shape->strings > limits.strings ||
            shape->elements > limits.array_elements) return failure(ErrorCode::capacity, Phase::parse);
        const auto j = d::component_dom_stock(shape->nodes, shape->strings);
        const auto p = d::component_parser_stock(shape->bytes, shape->depth);
        const auto q = d::component_renderer_stock(shape->depth, shape->members, shape->bytes);
        if (!j || !p || !q) return failure(ErrorCode::capacity, Phase::parse);
        Sum phase1{owned}; phase1.add(sizeof(State)); phase1.add(bytes.size());
        phase1.add(2U, *j); phase1.add(*p); phase1.add(*q);
        if (!phase1.valid || phase1.value > limits.simultaneous_bytes)
            return failure(ErrorCode::capacity, Phase::parse, limits.simultaneous_bytes, phase1.value);
        const auto side = kind == Kind::block_plan ||
            plan->plan()->observations()[slot].comparison_variant() == measurement::ComparisonVariantSlot::first
            ? wire::EvidenceComponentSide::first : wire::EvidenceComponentSide::second;
        auto logical = wire::decode_evidence_component(kind, side, bytes, context.capacity());
        if (!logical) return failure(logical.error().code == wire::LogicalCodecErrorCode::allocation_failure
                                         ? ErrorCode::allocation_failure : ErrorCode::malformed, Phase::parse);
        const auto stocks = d::typed_component_stocks(kind, logical->value());
        if (!stocks.owned.valid || !stocks.scratch.valid || !stocks.memberships.valid ||
            stocks.workers > limits.workers || stocks.memberships.value > limits.frequency_cpu_memberships)
            return failure(ErrorCode::capacity, Phase::reconstruct);
        Sum retained{sizeof(State)}; retained.add(bytes.size()); retained.add(stocks.owned.value);
        Sum phase2a{owned}; phase2a.add(retained.value); phase2a.add(*j);
        phase2a.add(shape->depth, sizeof(d::JsonRetirementFrame)); phase2a.add(stocks.scratch.value);
        if (!retained.valid || !phase2a.valid || retained.value > limits.typed_bytes ||
            phase2a.value > limits.simultaneous_bytes)
            return failure(ErrorCode::capacity, Phase::reconstruct, limits.simultaneous_bytes, phase2a.value);
        auto reconstruct = [&]() -> std::expected<Value, Error> {
            if (kind == Kind::block_plan) {
                auto made = d::fixed_reconstruct_block_plan_component(logical->value());
                if (!made) return typed_failure(made.error());
                return Value{std::in_place_type<measurement::MeasurementBlockPlan>, std::move(*made)};
            }
            if (kind == Kind::factual_observation) {
                auto made = d::fixed_reconstruct_factual_component(logical->value(), *plan->plan(), slot, context.cpus());
                if (!made) return typed_failure(made.error());
                if (!matches(*made, context)) return failure(ErrorCode::association, Phase::reconstruct);
                return Value{std::in_place_type<runtime::FactualObservationEvidence>, std::move(*made)};
            }
            auto header = d::fixed_reconstruct_failed_component_header(logical->value(), *plan->plan(), "failed_observation");
            if (!header) return typed_failure(header.error());
            if (header->slot.slot_index() != slot) return failure(ErrorCode::association, Phase::reconstruct);
            auto made = d::fixed_reconstruct_failed_component_workers(logical->value(), std::move(*header), "failed_observation");
            if (!made) return typed_failure(made.error());
            return Value{std::in_place_type<runtime::FailedObservationEvidence>, std::move(*made)};
        };
        auto typed = reconstruct();
        if (!typed) return std::unexpected{typed.error()};
        const auto count = std::visit([](const auto& item) {
            using T = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::same_as<T, measurement::MeasurementBlockPlan>) return d::count_block_plan_component(item);
            else if constexpr (std::same_as<T, runtime::FactualObservationEvidence>) return d::count_factual_component(item);
            else return d::count_failed_component(item);
        }, *typed);
        if (count.overflow || count.invalid || count.nodes > limits.nodes || count.strings > limits.strings ||
            count.elements > limits.array_elements || count.depth > limits.depth)
            return failure(ErrorCode::capacity, Phase::project);
        if (count.bytes != bytes.size()) return failure(ErrorCode::projection, Phase::project);
        const auto jp = d::component_dom_stock(count.nodes, count.strings);
        const auto qp = d::component_renderer_stock(count.depth, count.members, count.bytes);
        if (!jp || !qp) return failure(ErrorCode::capacity, Phase::project);
        Sum phase2b{phase2a.value}; phase2b.add(*jp); phase2b.add(count.depth, sizeof(d::JsonRetirementFrame)); phase2b.add(*qp);
        if (!phase2b.valid || phase2b.value > limits.simultaneous_bytes)
            return failure(ErrorCode::capacity, Phase::project, limits.simultaneous_bytes, phase2b.value);
        d::WireJson projected{count.depth};
        projected.value = std::visit([](const auto& item) {
            using T = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::same_as<T, measurement::MeasurementBlockPlan>) return d::project_block_plan_component(item);
            else if constexpr (std::same_as<T, runtime::FactualObservationEvidence>) return d::project_factual_component(item);
            else return d::project_failed_component(item);
        }, *typed);
        auto encoded = d::render_wire_json(projected.value, count.depth, count.nodes, count.bytes);
        if (!encoded || *encoded != bytes) return failure(ErrorCode::projection, Phase::project);
        ComponentUsage usage{*shape, stocks.owned.value, stocks.scratch.value, *jp, retained.value,
                             phase1.value, phase2a.value, phase2b.value};
        return Component{std::make_unique<State>(context.retain(), kind,
            std::move(*typed), std::move(*encoded), usage)};
    } catch (const std::bad_alloc&) { return failure(ErrorCode::allocation_failure, Phase::reconstruct); }
      catch (const std::length_error&) { return failure(ErrorCode::capacity, Phase::reconstruct); }
      catch (const nlohmann::json::exception&) { return failure(ErrorCode::malformed, Phase::reconstruct); }
}
struct Accumulator::State final {
    State(PairFactsLease&& c, Domain d, Limits l, std::size_t retained_stock) noexcept
        : context{std::move(c)}, domain{d}, limits{l}, retained{retained_stock} {}
    PairFactsLease context;
    Domain domain;
    Limits limits;
    std::array<Event, 6> events;
    std::size_t event_count{};
    std::size_t factual_count{};
    std::size_t retained;
    bool failed{}, closed{}, consumed{}, copied{};
};
Accumulator::Accumulator(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Accumulator::Accumulator(Accumulator&&) noexcept = default;
auto Accumulator::operator=(Accumulator&&) noexcept -> Accumulator& = default;
Accumulator::~Accumulator() = default;
auto Accumulator::events() const noexcept -> std::span<const Event> { return state_ ? std::span<const Event>{state_->events.data(), state_->event_count} : std::span<const Event>{}; }
auto Accumulator::consumed() const noexcept -> bool { return !state_ || state_->consumed; }
auto Accumulator::closed() const noexcept -> bool { return state_ && state_->closed; }
auto Accumulator::domain() const noexcept -> Domain { return state_->domain; }
auto Accumulator::retained_bytes() const noexcept -> std::size_t { return state_ ? state_->retained : 0U; }
auto Accumulator::base_stock(const PairFactsLease& context) noexcept -> std::optional<std::size_t> {
    Sum sum{sizeof(Accumulator) + sizeof(State)};
    sum.add(context.usage().retained_bytes - sizeof(PairFactsLease));
    if (!sum.valid) return std::nullopt;
    return sum.value;
}
void Accumulator::residual_limits(std::size_t retained, std::size_t simultaneous) noexcept {
    state_->limits.typed_bytes = retained;
    state_->limits.simultaneous_bytes = simultaneous;
}
auto Accumulator::prepare(ExpectedPairContext&& context, Domain domain, Limits limits)
    -> std::expected<Accumulator, Error> {
    auto result=prepare(PairFactsLease{context},domain,limits);
    if(result)context.state_.reset();
    return result;
}
auto Accumulator::prepare(PairFactsLease&& context, Domain domain, Limits limits)
    -> std::expected<Accumulator, Error> {
    if (!context.available() || (domain != Domain::supplied_observation && domain != Domain::synthetic_execution) ||
        (context.origin()==PairFactsLease::Origin::decoded_evidence &&
            context.decoded_domain()!=std::optional{domain==Domain::supplied_observation?process_subject::Domain::supplied_observation:process_subject::Domain::synthetic_execution}))
        return failure(ErrorCode::identity, Phase::context);
    if (limits.events > 6U || limits.depth > 64U) return failure(ErrorCode::capacity, Phase::context);
    Sum retained{sizeof(Accumulator) + sizeof(State)};
    retained.add(context.usage().retained_bytes - sizeof(PairFactsLease));
    if (!retained.valid || retained.value > limits.typed_bytes || retained.value > limits.simultaneous_bytes)
        return failure(ErrorCode::capacity, Phase::context);
    try { return Accumulator{std::make_unique<State>(std::move(context), domain, limits, retained.value)}; }
    catch (const std::bad_alloc&) { return failure(ErrorCode::allocation_failure, Phase::context); }
}
auto Accumulator::admit_plan(std::string_view bytes) -> std::expected<Component, Error> { return admit(Kind::block_plan, bytes); }
auto Accumulator::admit_factual(std::string_view bytes) -> std::expected<Component, Error> { return admit(Kind::factual_observation, bytes); }
auto Accumulator::admit_failed(std::string_view bytes) -> std::expected<Component, Error> { return admit(Kind::failed_observation, bytes); }
auto Accumulator::admit(Kind kind, std::string_view bytes) -> std::expected<Component, Error> {
    if (consumed()) return failure(ErrorCode::state, Phase::event);
    auto& state = *state_;
    if (state.event_count == state.limits.events) {
        state.consumed = true; return failure(ErrorCode::capacity, Phase::event);
    }
    if (state.closed || state.failed || (kind == Kind::block_plan) != (state.event_count == 0U) ||
        (kind != Kind::block_plan && state.factual_count == 4U)) {
        state.consumed = true; return failure(ErrorCode::state, Phase::event);
    }
    auto result = Component::admit(state.context, kind,
        (state.event_count == 0U) ? nullptr : &state.events.front().component,
        state.factual_count, bytes, state.limits, state.retained);
    if (!result) state.consumed = true;
    return result;
}
auto Accumulator::append(std::uint64_t sequence, const journal::Timestamp& timestamp, Component&& component)
    -> std::expected<void, Error> {
    return append_impl(sequence, timestamp, std::move(component), nullptr, nullptr, 0U);
}
namespace {
struct TimingArithmeticCells final {
    Sum* self;
    std::size_t n, size, multiply_count, multiply_size, add_current, add_additional;
    std::expected<std::size_t, d::WireJsonError> product, sum, multiply_return, add_return;
    d::WireJsonError multiply_error, add_error;
    std::unexpected<d::WireJsonError> multiply_unexpected, add_unexpected;
};
struct TimingAppendCells final {
    std::uint64_t sequence, calibration;
    const journal::Timestamp* timestamp;
    Component* component;
    observation_timing::Input* timing;
    const observation_timing::Usage* timing_usage;
    Accumulator::State *state, *reject_capture;
    Kind kind;
    const measurement::ScheduledObservationSlot* planned;
    measurement::ScheduledObservationSlot current_slot;
    core::ObservationId id;
    const Event* previous;
    ComponentUsage component_usage;
    Sum retained, peak;
    journal::Timestamp copied_time;
    EventKind event_kind;
    Event published;
    std::optional<observation_timing::Owner> metadata;
    std::expected<observation_timing::Owner, Error> prepared;
    PairFactsLease lease;
    PairFactsLease lease_constructor_parameter;
    ExpectedPairContext actual_lease_constructor;
    std::shared_ptr<const ExpectedPairContext::State> actual_shared_parameter;
    std::expected<void, Error> returned;
    ErrorCode rejection_code;
    ErrorCode failure_code;
    Phase failure_phase;
    std::size_t failure_limit, failure_observed;
    Error rejection;
    std::unexpected<Error> returned_rejection;
};
struct LegacyTimingAppendCells final {
    std::uint64_t wrapper_sequence;
    const journal::Timestamp* wrapper_timestamp;
    Component* wrapper_component;
    observation_timing::Input* timing;
    const observation_timing::Usage* timing_usage;
    std::uint64_t calibration;
    std::optional<observation_timing::Owner> metadata, event_metadata;
    Sum peak;
    std::expected<void, Error> wrapper_return;
};
}
auto Accumulator::timing_staging_bytes() noexcept -> std::size_t { return sizeof(TimingAppendCells) + sizeof(TimingArithmeticCells); }
auto Accumulator::legacy_timing_staging_bytes() noexcept -> std::size_t { return sizeof(LegacyTimingAppendCells) + sizeof(TimingArithmeticCells); }
auto Accumulator::append_impl(std::uint64_t sequence, const journal::Timestamp& timestamp, Component&& component,
    observation_timing::Input* timing, const observation_timing::Usage* timing_usage, std::uint64_t calibration)
    -> std::expected<void, Error> {
    if (consumed()) return failure(ErrorCode::state, Phase::event);
    auto& state = *state_;
    auto reject = [&](ErrorCode code) { state.consumed = true; return failure(code, Phase::event); };
    if (state.closed || !component.available() || !component.state_->context.same_owner(state.context))
        return reject(ErrorCode::association);
    if (!sequence || (!(state.event_count == 0U) && sequence <= state.events[state.event_count - 1U].sequence) || timestamp.text().empty())
        return reject(ErrorCode::sequence);
    const auto kind = component.kind();
    if ((kind == Kind::block_plan) != (state.event_count == 0U) || state.failed ||
        (kind != Kind::block_plan && state.factual_count == 4U)) return reject(ErrorCode::state);
    if (kind != Kind::block_plan) {
        const auto& planned = state.events.front().component.plan()->observations()[state.factual_count];
        const auto current_slot = component.factual() ? component.factual()->slot : component.failed()->slot;
        const auto id = component.factual() ? component.factual()->observation_id : component.failed()->observation_id;
        if (!(current_slot == planned)) return reject(ErrorCode::association);
        for (const auto& previous : events())
            if (const auto* factual = previous.component.factual(); factual && factual->observation_id == id)
                return reject(ErrorCode::association);
    }
    Sum retained{state.retained}; retained.add(timestamp.text().size());
    retained.add(component.usage().retained_bytes);
    if (timing) {
        if (!timing_usage || !calibration ||
            !observation_timing::detail::matches(*timing, component.factual(), component.failed()))
            return reject(ErrorCode::association);
        retained.add(timing_usage->retained_bytes);
    }
    Sum peak{retained.value}; peak.add(timing ? timing_staging_bytes() : legacy_timing_staging_bytes());
    if (!retained.valid || !peak.valid || state.event_count == state.limits.events || retained.value > state.limits.typed_bytes ||
        peak.value > state.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        // The timestamp copy precedes assignment into the already owned fixed slot.
        auto copied_time = timestamp;
        std::optional<observation_timing::Owner> metadata;
        if (timing) {
            auto prepared = observation_timing::Owner::prepare(state.context.retain(),
                *timing, *timing_usage, calibration);
            if (!prepared) { state.consumed = true; return std::unexpected{prepared.error()}; }
            metadata.emplace(std::move(*prepared));
        }
        const auto event_kind = kind == Kind::block_plan ? EventKind::opened :
            kind == Kind::factual_observation ? EventKind::factual : EventKind::failed;
        if (metadata) metadata->commit(*timing);
        state.events[state.event_count++] = Event{sequence, std::move(copied_time), event_kind,
            std::move(component), std::nullopt, std::move(metadata)};
        if (kind == Kind::factual_observation) ++state.factual_count;
        if (kind == Kind::failed_observation) state.failed = true;
        state.retained = retained.value;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto Accumulator::close(std::uint64_t sequence, const journal::Timestamp& timestamp,
                        runtime::BlockAttemptCompletion completion) -> std::expected<void, Error> {
    if (consumed()) return failure(ErrorCode::state, Phase::event);
    auto& state = *state_;
    auto reject = [&](ErrorCode code) { state.consumed = true; return failure(code, Phase::event); };
    if ((state.event_count == 0U) || state.closed) return reject(ErrorCode::state);
    if (!sequence || sequence <= state.events[state.event_count - 1U].sequence || timestamp.text().empty()) return reject(ErrorCode::sequence);
    if (completion != runtime::BlockAttemptCompletion::complete_valid &&
        completion != runtime::BlockAttemptCompletion::complete_invalid &&
        completion != runtime::BlockAttemptCompletion::operationally_incomplete) return reject(ErrorCode::association);
    const auto relation = runtime::detail::validate_pairwise_block_association(
        runtime::detail::PairwiseBlockAssociationView{
            *state.events.front().component.plan(), state.factual_count, &state,
            [](const void* owner, std::size_t index) noexcept -> const runtime::FactualObservationEvidence& {
                return *static_cast<const State*>(owner)->events[index + 1U].component.factual();
            }, state.failed ? state.events[state.event_count - 1U].component.failed() : nullptr, completion});
    if (!relation) return reject(ErrorCode::association);
    Sum retained{state.retained}; retained.add(timestamp.text().size());
    // New close working cells: the timing field in the temporary Event and this sum.
    Sum peak{retained.value}; peak.add(sizeof(std::optional<observation_timing::Owner>) + sizeof(Sum) + sizeof(TimingArithmeticCells));
    if (!retained.valid || state.event_count == state.limits.events || retained.value > state.limits.typed_bytes ||
        !peak.valid || peak.value > state.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        auto copied_time = timestamp;
        state.events[state.event_count++] = Event{sequence, std::move(copied_time), EventKind::closed, Component{}, completion, std::nullopt};
        state.closed = true; state.retained = retained.value;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto Accumulator::copy_closed_block() -> std::expected<runtime::PairwiseBlockAttemptEvidence, Error> {
    if (consumed()) return failure(ErrorCode::state, Phase::handoff);
    auto& state = *state_;
    auto reject = [&](ErrorCode code) { state.consumed = true; return failure(code, Phase::handoff); };
    if (!state.closed || state.copied) return reject(ErrorCode::state);
    Sum copy{sizeof(runtime::PairwiseBlockAttemptEvidence)};
    copy.add(state.factual_count, sizeof(runtime::FactualObservationEvidence));
    for (const auto& event : events())
        if (event.component.available()) copy.add(event.component.usage().typed_payload);
    if (!copy.valid || !detail::closed_copy_fits(state.retained, copy.value, state.limits.simultaneous_bytes))
        return reject(ErrorCode::capacity);
    try {
        std::vector<runtime::FactualObservationEvidence> facts;
        facts.reserve(state.factual_count);
        for (std::size_t i{}; i < state.factual_count; ++i) facts.push_back(*state.events[i + 1U].component.factual());
        std::optional<runtime::FailedObservationEvidence> failed;
        if (state.failed) failed.emplace(*state.events[state.event_count - 2U].component.failed());
        runtime::PairwiseBlockAttemptEvidence result{*state.events.front().component.plan(), std::move(facts),
            std::move(failed), *state.events[state.event_count - 1U].completion};
        state.copied = true; state.consumed = true;
        return result;
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
static_assert(std::is_nothrow_move_constructible_v<Event>);
static_assert(std::is_nothrow_move_assignable_v<Event>);
static_assert(std::is_nothrow_move_constructible_v<Component>);
} // namespace epycbench::results::block_evidence
