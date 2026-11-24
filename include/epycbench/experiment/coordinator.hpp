#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/scenario/configuration_space.hpp"

#include <span>
#include <variant>

namespace epycbench::experiment {

inline constexpr std::size_t maximum_attempts = 4096U;

// Identities refer to resolved sweep candidates. Occurrences of the same
// candidate have separate trial identities and independently derived order seeds.
struct CandidateIdentity {
    core::Sha256Digest candidate;
    core::Sha256Digest problem;
    auto operator==(const CandidateIdentity&) const -> bool = default;
};

struct ScheduledTrial {
    std::size_t ordinal;
    CandidateIdentity configuration;
    core::Sha256Digest id;
    std::uint64_t comparison_order_seed;
};

class Schedule {
  public:
    [[nodiscard]] static auto create(scenario::TaskObjective, std::uint64_t seed,
        std::size_t attempt_budget, std::span<const CandidateIdentity> ordered)
        -> std::expected<Schedule, core::Error>;
    [[nodiscard]] auto trials() const noexcept -> std::span<const ScheduledTrial> { return trials_; }
    [[nodiscard]] auto id() const noexcept -> const core::Sha256Digest& { return id_; }
    [[nodiscard]] auto attempt_budget() const noexcept -> std::size_t { return attempt_budget_; }
    [[nodiscard]] auto seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto objective() const noexcept -> scenario::TaskObjective { return objective_; }

  private:
    Schedule(core::Sha256Digest id, std::vector<ScheduledTrial> trials,
             scenario::TaskObjective objective, std::uint64_t seed, std::size_t budget)
        : id_{id}, trials_{std::move(trials)}, objective_{objective}, seed_{seed},
          attempt_budget_{budget} {}
    core::Sha256Digest id_;
    std::vector<ScheduledTrial> trials_;
    scenario::TaskObjective objective_;
    std::uint64_t seed_;
    std::size_t attempt_budget_;
};

struct Reservation {
    core::Sha256Digest trial;
};

enum class Disposition { completed, invalid, failed, interrupted };

struct ResultAssociation {
    std::string run_id;
    core::Sha256Digest content_digest;
    std::string artifact;
};

enum class UnavailableResult { run_failed, invalid_result, result_missing, interrupted };

struct Outcome {
    core::Sha256Digest trial;
    Disposition disposition;
    std::optional<ResultAssociation> result;
    std::optional<UnavailableResult> unavailable{};
    std::optional<int> exit_code{};
    std::optional<int> signal{};
    bool scientific_stop{};
};

enum class StopReason { scientific, user_interrupted };

struct Stop {
    StopReason reason;
};

using Event = std::variant<Reservation, Outcome, Stop>;

struct Entry {
    std::uint64_t sequence;
    Event event;
};

class Ledger {
  public:
    explicit Ledger(Schedule schedule) : schedule_{std::move(schedule)} {}
    [[nodiscard]] auto schedule() const noexcept -> const Schedule& { return schedule_; }
    [[nodiscard]] auto entries() const noexcept -> std::span<const Entry> { return entries_; }
    [[nodiscard]] auto next() const noexcept -> const ScheduledTrial*;
    [[nodiscard]] auto active() const noexcept -> const ScheduledTrial*;
    [[nodiscard]] auto charged_attempts() const noexcept -> std::size_t { return charged_; }
    [[nodiscard]] auto scientifically_stopped() const noexcept -> bool { return scientific_stop_; }
    [[nodiscard]] auto validate(const Entry&) const -> std::expected<void, core::Error>;
    [[nodiscard]] auto append(Entry) -> std::expected<void, core::Error>;

  private:
    Schedule schedule_;
    std::vector<Entry> entries_;
    std::size_t charged_{};
    bool active_{};
    bool scientific_stop_{};
};

struct Operations {
    void* context{};
    // A successful persist commits one entry durably. Replay after an ambiguous
    // storage error before making any further attempt.
    std::expected<void, core::Error> (*persist)(void*, const Entry&){};
    // Called only after persistence of a charged reservation. Return after the
    // launched Run and its resources retire. An error leaves the reservation
    // unresolved until retirement is confirmed.
    std::expected<Outcome, core::Error> (*evaluate)(void*, const ScheduledTrial&){};
    bool (*cancelled)(void*) noexcept{};
};

// Caller holds exclusive experiment ownership throughout each invocation.
// Recovery requires external confirmation that the prior evaluator retired.
enum class RunExtent { remaining, one_attempt };
[[nodiscard]] auto run(Ledger&, const Operations&, RunExtent = RunExtent::remaining) -> std::expected<void, core::Error>;
[[nodiscard]] auto recover_retired_trial(Ledger&, const Operations&) -> std::expected<void, core::Error>;
[[nodiscard]] auto record_scientific_stop(Ledger&, const Operations&) -> std::expected<void, core::Error>;

} // namespace epycbench::experiment
