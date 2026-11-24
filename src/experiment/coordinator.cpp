#include "epycbench/experiment/coordinator.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/core/seed.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>

namespace epycbench::experiment {
namespace {
auto failure(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"experiment.coordinator.invalid"},
            .message = "Experiment coordinator could not continue",
            .operation = "experiment.coordinator",
            .os_error = {},
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto commit(Ledger& ledger, const Operations& operations, Event event)
    -> std::expected<void, core::Error> {
    Entry entry{ledger.entries().size() + 1U, std::move(event)};
    if (auto valid = ledger.validate(entry); !valid) return valid;
    if (auto persisted = operations.persist(operations.context, entry); !persisted) return persisted;
    return ledger.append(std::move(entry));
}
} // namespace

auto Schedule::create(scenario::TaskObjective objective, std::uint64_t seed,
                      std::size_t budget, std::span<const CandidateIdentity> ordered)
    -> std::expected<Schedule, core::Error> {
    if (budget == 0U || budget > maximum_attempts || ordered.size() > budget)
        return std::unexpected{failure("attempt_budget")};
    if (objective != scenario::TaskObjective::throughput && objective != scenario::TaskObjective::time_per_task)
        return std::unexpected{failure("objective")};

    auto candidates = nlohmann::json::array();
    for (const auto& identity : ordered)
        candidates.push_back({{"candidate", identity.candidate.to_hex()}, {"problem", identity.problem.to_hex()}});
    auto digest = core::hash_canonical_json(nlohmann::json{
        {"profile", "epycbench.experiment-schedule/v1"}, {"objective", scenario::objective_token(objective)},
        {"root_seed", std::to_string(seed)}, {"attempt_budget", budget}, {"candidates", std::move(candidates)}});
    if (!digest) return std::unexpected{std::move(digest.error())};

    const auto schedule_id = digest->to_hex();
    std::vector<ScheduledTrial> trials;
    trials.reserve(ordered.size());
    for (std::size_t ordinal{}; ordinal < ordered.size(); ++ordinal) {
        auto trial = core::hash_canonical_json(nlohmann::json{
            {"profile", "epycbench.experiment-trial/v1"}, {"schedule", schedule_id}, {"ordinal", ordinal}});
        if (!trial) return std::unexpected{std::move(trial.error())};
        const auto derived = core::derive_seed(seed, {core::SeedDomain::block_order,
            {{"experiment_schedule", schedule_id}, {"trial", trial->to_hex()}}});
        if (!derived) return std::unexpected{derived.error()};
        trials.push_back({ordinal, ordered[ordinal], *trial, derived->value()});
    }
    return Schedule{*digest, std::move(trials), objective, seed, budget};
}

auto Ledger::next() const noexcept -> const ScheduledTrial* {
    if (active_ || scientific_stop_ || charged_ >= schedule_.trials().size()) return nullptr;
    return &schedule_.trials()[charged_];
}

auto Ledger::active() const noexcept -> const ScheduledTrial* {
    return active_ ? &schedule_.trials()[charged_ - 1U] : nullptr;
}

auto Ledger::validate(const Entry& entry) const -> std::expected<void, core::Error> {
    if (entries_.size() == std::numeric_limits<std::uint64_t>::max() ||
        entry.sequence != entries_.size() + 1U || scientific_stop_)
        return std::unexpected{failure("event_sequence")};
    if (const auto* reservation = std::get_if<Reservation>(&entry.event)) {
        const auto* trial = next();
        if (!trial || trial->id != reservation->trial) return std::unexpected{failure("reservation")};
    } else if (const auto* outcome = std::get_if<Outcome>(&entry.event)) {
        const auto* trial = active();
        if (!trial || trial->id != outcome->trial) return std::unexpected{failure("outcome")};
        if (outcome->disposition != Disposition::completed && outcome->disposition != Disposition::invalid &&
            outcome->disposition != Disposition::failed && outcome->disposition != Disposition::interrupted)
            return std::unexpected{failure("disposition")};
        if (static_cast<bool>(outcome->result) == static_cast<bool>(outcome->unavailable) ||
            ((outcome->disposition == Disposition::completed || outcome->disposition == Disposition::invalid) &&
             !outcome->result))
            return std::unexpected{failure("outcome_result_association")};
        if (outcome->result && (outcome->result->run_id.empty() || outcome->result->artifact.empty()))
            return std::unexpected{failure("result_identity")};
        if (outcome->result && std::ranges::any_of(entries_, [&](const auto& prior) {
                const auto* recorded = std::get_if<Outcome>(&prior.event);
                return recorded && recorded->result && recorded->result->run_id == outcome->result->run_id;
            }))
            return std::unexpected{failure("duplicate_run_identity")};
        if (outcome->exit_code && outcome->signal)
            return std::unexpected{failure("incompatible_exit_facts")};
    } else {
        if (active_) return std::unexpected{failure("unretired_trial")};
        const auto reason = std::get<Stop>(entry.event).reason;
        if (reason != StopReason::scientific && reason != StopReason::user_interrupted)
            return std::unexpected{failure("stop_reason")};
        if (reason == StopReason::user_interrupted && !entries_.empty() &&
            std::holds_alternative<Stop>(entries_.back().event))
            return std::unexpected{failure("duplicate_user_stop")};
    }
    return {};
}

auto Ledger::append(Entry entry) -> std::expected<void, core::Error> {
    if (auto valid = validate(entry); !valid) return valid;
    entries_.push_back(std::move(entry));
    const auto& event = entries_.back().event;
    if (std::holds_alternative<Reservation>(event)) {
        ++charged_;
        active_ = true;
    } else if (std::holds_alternative<Outcome>(event)) {
        active_ = false;
        scientific_stop_ = std::get<Outcome>(event).scientific_stop;
    } else {
        scientific_stop_ = std::get<Stop>(event).reason == StopReason::scientific;
    }
    return {};
}

auto run(Ledger& ledger, const Operations& operations, RunExtent extent) -> std::expected<void, core::Error> {
    if (!operations.persist || !operations.evaluate || ledger.active())
        return std::unexpected{failure("evaluator_or_recovery_required")};
    while (const auto* trial = ledger.next()) {
        if (operations.cancelled && operations.cancelled(operations.context)) {
            if (!ledger.entries().empty() && std::holds_alternative<Stop>(ledger.entries().back().event))
                return {};
            return commit(ledger, operations, Stop{StopReason::user_interrupted});
        }
        if (auto charged = commit(ledger, operations, Reservation{trial->id}); !charged) return charged;
        auto outcome = operations.evaluate(operations.context, *trial);
        if (!outcome) return std::unexpected{std::move(outcome.error())};
        if (auto recorded = commit(ledger, operations, std::move(*outcome)); !recorded) return recorded;
        if (!ledger.scientifically_stopped() && operations.cancelled &&
            operations.cancelled(operations.context))
            return commit(ledger, operations, Stop{StopReason::user_interrupted});
        if (extent == RunExtent::one_attempt) break;
    }
    return {};
}

auto recover_retired_trial(Ledger& ledger, const Operations& operations)
    -> std::expected<void, core::Error> {
    const auto* trial = ledger.active();
    if (!trial || !operations.persist) return std::unexpected{failure("no_recoverable_reservation")};
    return commit(ledger, operations, Outcome{trial->id, Disposition::interrupted, {},
        UnavailableResult::interrupted});
}

auto record_scientific_stop(Ledger& ledger, const Operations& operations)
    -> std::expected<void, core::Error> {
    if (!operations.persist) return std::unexpected{failure("storage_required")};
    return commit(ledger, operations, Stop{StopReason::scientific});
}

} // namespace epycbench::experiment
