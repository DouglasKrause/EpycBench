#include "results/pair_lifecycle_admission.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::results::pair_lifecycle {
namespace {
using namespace runtime::detail;
using block_evidence::Error;
using block_evidence::ErrorCode;
auto reject(ErrorCode code) noexcept { return std::unexpected{Error{code, block_evidence::Phase::handoff}}; }
auto add(std::size_t& value, std::size_t increment) noexcept -> bool {
    if (increment > std::numeric_limits<std::size_t>::max() - value) return false;
    value += increment;
    return true;
}
auto within(const Usage& u, const Limits& l) noexcept -> bool {
    return u.records <= l.records && u.marks <= l.marks && u.request_bytes <= l.request_bytes &&
        u.transport_bytes <= l.transport_bytes && u.canonical_bytes <= l.canonical_bytes &&
        u.capacity_elements <= l.capacity_elements && u.retained_bytes <= l.retained_bytes &&
        u.simultaneous_bytes <= l.simultaneous_bytes;
}
auto consistent(const SyntheticLifecycleFacts& facts) noexcept -> bool {
    using M = SyntheticLifecycleMark;
    const auto observed = [&](M value) { return facts.ordinal(value) != 0U; };
    const auto follows = [&](M after, M before) {
        return !observed(after) || (observed(before) && facts.ordinal(before) < facts.ordinal(after));
    };
    if (!follows(M::child_created, M::channels_ready) ||
        !follows(M::request_written, M::child_created) ||
        !follows(M::acknowledgments_validated, M::request_written) ||
        !follows(M::active_roles_bound, M::acknowledgments_validated) ||
        !follows(M::backend_entered, M::active_roles_bound) ||
        !follows(M::backend_returned_evidence, M::backend_entered) ||
        !follows(M::canonical_encoded, M::backend_returned_evidence) ||
        !follows(M::first_success_stream_byte, M::canonical_encoded) ||
        !follows(M::sender_completed, M::first_success_stream_byte) ||
        !follows(M::completion_received, M::first_success_stream_byte) ||
        !follows(M::typed_reconstruction_completed, M::clean_eof) ||
        !follows(M::typed_reconstruction_completed, M::completion_received) ||
        !follows(M::results_projected, M::typed_reconstruction_completed) ||
        !follows(M::cancel_written, M::cancellation_decided) ||
        !follows(M::runner_observed_cancel, M::cancel_written) ||
        !follows(M::cancelled_frame_written, M::runner_observed_cancel) ||
        !follows(M::cancelled_frame_validated, M::cancelled_frame_written) ||
        !follows(M::retirement_completed, M::retirement_begun) ||
        !follows(M::reap_return, M::reap_attempt) ||
        !follows(M::reap_attempt, M::child_created) ||
        !follows(M::terminate_attempt, M::controller_control_close_return) ||
        !follows(M::terminate_attempt, M::controller_event_close_return) ||
        !follows(M::terminate_return, M::terminate_attempt) ||
        !follows(M::kill_attempt, M::terminate_return) ||
        !follows(M::kill_return, M::kill_attempt) ||
        !follows(M::fatal_unreapable_observed, M::reap_return)) return false;
    if (facts.primary.has_value() != observed(M::primary_failure) ||
        facts.identity.has_value() != observed(M::acknowledgments_validated) ||
        facts.cancellation.has_value() != observed(M::cancellation_decided) ||
        facts.cancelled_written_phase.has_value() != observed(M::cancelled_frame_written) ||
        facts.cancelled_validated_phase.has_value() != observed(M::cancelled_frame_validated) ||
        (facts.secondary_count && !facts.primary) || facts.secondary_count > facts.secondary.size() ||
        (observed(M::backend_entered) && !facts.activation_intended) ||
        (observed(M::cancelled_frame_written) && observed(M::first_success_stream_byte))) return false;
    if (facts.cancellation && static_cast<unsigned>(*facts.cancellation) > 2U) return false;
    const auto cancelled_phase = facts.activation_intended ? SyntheticCancelledPhase::execution : SyntheticCancelledPhase::admission;
    if ((facts.cancelled_written_phase && *facts.cancelled_written_phase != cancelled_phase) ||
        (facts.cancelled_validated_phase && *facts.cancelled_validated_phase != cancelled_phase)) return false;
    const std::array durations{facts.config.startup, facts.config.execution, facts.config.cancellation,
        facts.config.completion, facts.config.termination};
    for (std::size_t index{}; index < facts.deadlines.size(); ++index) {
        const auto begin = static_cast<M>(static_cast<unsigned>(M::startup_begin) + 2U * index);
        const auto expiry = static_cast<M>(static_cast<unsigned>(begin) + 1U);
        const auto& time = facts.deadlines[index];
        if (!follows(expiry, begin)) return false;
        if (observed(begin) && (time.begin < 0 || time.duration != durations[index] ||
            time.duration < 0 || time.duration > INT64_MAX - time.begin ||
            time.deadline != time.begin + time.duration)) return false;
        if (observed(expiry) && time.expiry < time.deadline) return false;
    }
    for (std::size_t index{}; index < facts.closes.size(); ++index) {
        const auto attempt = static_cast<M>(static_cast<unsigned>(M::parent_runner_control_close_attempt) + 2U * index);
        const auto returned = static_cast<M>(static_cast<unsigned>(attempt) + 1U);
        if (!follows(returned, attempt) || observed(attempt) != observed(returned) ||
            observed(returned) != (facts.closes[index] != SyntheticCapturedReturn::absent) ||
            facts.closes[index] > SyntheticCapturedReturn::exception) return false;
    }
    std::array<bool, 8U> secondaries{};
    auto previous = facts.ordinal(M::primary_failure);
    for (std::size_t index{}; index < facts.secondary_count; ++index) {
        const auto source = static_cast<std::size_t>(facts.secondary[index].source);
        if (source >= secondaries.size() || secondaries[source]) return false;
        secondaries[source] = true;
        const auto ordinal = facts.ordinal(static_cast<M>(static_cast<unsigned>(M::secondary_backend_retirement) + source));
        if (ordinal <= previous) return false;
        previous = ordinal;
    }
    for (std::size_t index{}; index < secondaries.size(); ++index)
        if (secondaries[index] != observed(static_cast<M>(static_cast<unsigned>(M::secondary_backend_retirement) + index))) return false;
    return true;
}
}

auto Owner::prepare(const runtime::detail::SyntheticLifecycleView& view,
    const journal::RunId& run, const scenario::ScenarioDigestedPlan* plan,
    const block_evidence::PairPrefix* prefix, References references, Limits limits,
    std::size_t prior_stock, std::size_t entry_stock)
    -> std::expected<Owner, block_evidence::Error> {
    LifecycleAdmissionStaging stage{};
    stage.source = &view;
    stage.run = &run;
    stage.plan = plan;
    stage.prefix = prefix;
    stage.references = references;
    stage.limits = limits;
    stage.prior = prior_stock;
    stage.entry = entry_stock;
    stage.usage.simultaneous_bytes = prior_stock;
    if (!add(stage.usage.simultaneous_bytes, entry_stock) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleAdmissionStaging)) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleCopyStaging)) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleContextComparisonStaging)) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleContextHelperStaging)) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleIdentityStaging)) ||
        !add(stage.usage.simultaneous_bytes, sizeof(LifecycleCommitStaging)) ||
        stage.usage.simultaneous_bytes > limits.simultaneous_bytes)
        return reject(ErrorCode::capacity);
    const auto* facts = view.facts();
    if (!facts || facts->capture_failure || !facts->capacity_offer ||
        facts->ordinal(SyntheticLifecycleMark::retirement_completed) == 0U ||
        facts->ordinal(SyntheticLifecycleMark::retirement_begun) == 0U || !consistent(*facts))
        return reject(ErrorCode::state);
    for (const auto ordinal : facts->ordinals) {
        if (!ordinal) continue;
        if (ordinal >= stage.seen.size() || stage.seen[ordinal]) return reject(ErrorCode::malformed);
        stage.seen[ordinal] = true;
        ++stage.usage.marks;
    }
    if (facts->next_ordinal != stage.usage.marks + 1U) return reject(ErrorCode::malformed);
    for (stage.index = 1U; stage.index <= stage.usage.marks; ++stage.index)
        if (!stage.seen[stage.index]) return reject(ErrorCode::malformed);
    const auto& offer = *facts->capacity_offer;
    if (core::sha256(facts->request_body) != offer.capacity.projected().request_body_sha256 ||
        core::sha256(offer.canonical_body) != offer.body_sha256 ||
        offer.capacity.request_id() != facts->pair.first) return reject(ErrorCode::identity);
    if (facts->identity) {
        const auto& id = *facts->identity;
        if (id.pair.first != facts->pair.first || id.pair.second != facts->pair.second ||
            id.request_body_sha256 != offer.capacity.projected().request_body_sha256 ||
            id.capacity_offer_body_sha256 != offer.body_sha256 ||
            id.capacity_body_sha256 != offer.capacity_body_sha256 ||
            id.wire_registry_sha256 != offer.capacity.projected().registry_sha256 ||
            id.source_manifest_sha256 != offer.capacity.projected().source_manifest_sha256 ||
            facts->ordinal(SyntheticLifecycleMark::acknowledgments_validated) == 0U)
            return reject(ErrorCode::identity);
    }
    const bool typed = facts->ordinal(SyntheticLifecycleMark::typed_reconstruction_completed) != 0U;
    const auto bytes = view.canonical_bytes();
    if (typed && (bytes.empty() || !prefix || prefix->canonical_root().empty()))
        return reject(ErrorCode::association);
    if (!typed && prefix && !prefix->canonical_root().empty())
        return reject(ErrorCode::association);
    if (prefix && (!view.admitted() || !facts->identity || !plan ||
        !prefix->lifecycle_outcomes_mode() ||
        prefix->domain() != block_evidence::Domain::synthetic_execution ||
        !prefix->lifecycle_context())) return reject(ErrorCode::association);
    if (typed && (prefix->canonical_root().size() != bytes.size() ||
        !std::equal(bytes.begin(), bytes.end(), std::as_bytes(std::span{prefix->canonical_root()}).begin())))
        return reject(ErrorCode::association);
    stage.source_backing = static_cast<std::size_t>(facts->usage.retained_bytes);
    if (stage.source_backing < sizeof(SyntheticLifecycleFacts) + synthetic_capture_inline_bytes) return reject(ErrorCode::capacity);
    stage.owner_backing = stage.source_backing - sizeof(SyntheticLifecycleFacts) - synthetic_capture_inline_bytes;
    stage.prefix_backing = prefix ? prefix->usage().retained : 0U;
    if (prefix && stage.prefix_backing < sizeof(block_evidence::PairPrefix)) return reject(ErrorCode::capacity);
    stage.usage.records = 1U;
    stage.usage.request_bytes = facts->request_body.size();
    stage.usage.transport_bytes = bytes.size();
    stage.usage.canonical_bytes = facts->request_body.size();
    stage.usage.capacity_elements = static_cast<std::size_t>(facts->usage.capacity_elements);
    stage.usage.retained_bytes = sizeof(Owner);
    if (!add(stage.usage.canonical_bytes, offer.canonical_body.size()) ||
        !add(stage.usage.canonical_bytes, bytes.size()) ||
        !add(stage.usage.retained_bytes, stage.owner_backing) ||
        !add(stage.usage.retained_bytes, bytes.size()) ||
        (prefix && !add(stage.usage.retained_bytes, stage.prefix_backing - sizeof(block_evidence::PairPrefix))))
        return reject(ErrorCode::capacity);
    if (!add(stage.usage.simultaneous_bytes, stage.usage.retained_bytes) ||
        !add(stage.usage.simultaneous_bytes, stage.source_backing) ||
        !add(stage.usage.simultaneous_bytes, bytes.size()) ||
        !within(stage.usage, limits)) return reject(ErrorCode::capacity);
    LifecycleCopyStaging copied;
    copied.facts.emplace(*facts);
    if (!bytes.empty()) copied.body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    LifecycleContextComparisonStaging compared;
    if (prefix) {
        auto context_limits = limits.context;
        context_limits.simultaneous_bytes = std::min(context_limits.simultaneous_bytes,
            limits.simultaneous_bytes - stage.usage.simultaneous_bytes);
        compared.context.emplace(block_evidence::ExpectedPairContext::prepare(run, *plan,
            view.admitted()->inputs(), offer.capacity.projected().capacity, context_limits));
        if (!*compared.context) return std::unexpected{compared.context->error()};
        compared.incoming = prefix->lifecycle_context();
        compared.context_usage = (*compared.context)->usage();
        if (!(*compared.context)->exactly_matches(*compared.incoming)) return reject(ErrorCode::identity);
        if (!add(stage.usage.simultaneous_bytes, compared.context_usage.simultaneous_bytes) ||
            !within(stage.usage, limits)) return reject(ErrorCode::capacity);
    }
    return Owner{std::move(*copied.facts), std::move(copied.body), references, stage.usage};
}
} // namespace epycbench::results::pair_lifecycle
