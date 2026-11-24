#pragma once

#include "epycbench/experiment/coordinator.hpp"
#include "epycbench/experiment/genetic.hpp"
#include "epycbench/experiment/local_search.hpp"
#include "experiment/annealing.hpp"
#include "experiment/bayesian_search.hpp"

namespace epycbench::platform_linux::experiment_operations {
class Directory;
}

namespace epycbench::experiment {

enum class StudyHistoryStrategy { genetic, hill_climb, annealing, bayesian, hybrid };
[[nodiscard]] constexpr auto uses_bayesian_proposals(StudyHistoryStrategy strategy) noexcept -> bool {
    return strategy == StudyHistoryStrategy::bayesian || strategy == StudyHistoryStrategy::hybrid;
}

struct GeneticHistoryConfig {
    // Digest of the immutable outer plan from which the other fields were read.
    core::Sha256Digest plan_digest;
    GeneticSpace space;
    std::uint64_t root_seed;
    std::size_t attempt_budget;
    GeneticProposalOptions options;
    std::size_t maximum_generations{maximum_attempts};
    StudyHistoryStrategy strategy{StudyHistoryStrategy::genetic};
    std::optional<AnnealingCooling> annealing_cooling{};
};

struct GeneticGenerationIntent {
    GeneticGeneration generation;
    core::Sha256Digest child_plan;
    // A subsequence of this generation's proposals, in child schedule order.
    std::vector<std::size_t> scheduled_ordinals;
    std::optional<HillClimbBatch> hill_climb{};
    std::optional<AnnealingStep> annealing{};
    std::optional<BayesianStep> bayesian{};

    [[nodiscard]] static auto for_hill_climb(HillClimbBatch, core::Sha256Digest,
                                             std::vector<std::size_t>) -> GeneticGenerationIntent;
    [[nodiscard]] static auto for_annealing(AnnealingStep, core::Sha256Digest,
                                            std::vector<std::size_t>) -> GeneticGenerationIntent;
    [[nodiscard]] static auto for_bayesian(BayesianStep, core::Sha256Digest,
                                           std::vector<std::size_t>) -> GeneticGenerationIntent;
    [[nodiscard]] auto index() const noexcept -> std::uint64_t {
        if (bayesian) return bayesian->step;
        if (annealing) return annealing->step;
        return hill_climb ? hill_climb->step : generation.generation;
    }
    [[nodiscard]] auto proposal_count() const noexcept -> std::size_t {
        if (bayesian) return bayesian->proposal ? 1U : 0U;
        if (annealing) return annealing->proposal ? 1U : 0U;
        return hill_climb ? hill_climb->proposals.size() : generation.proposals.size();
    }
    [[nodiscard]] auto proposal_ordinal(std::size_t index) const -> std::size_t {
        if (bayesian && index == 0U) return bayesian->proposal.value().ordinal;
        if (annealing && index == 0U) return annealing->proposal.value().ordinal;
        return hill_climb ? hill_climb->proposals.at(index).ordinal : generation.proposals.at(index).ordinal;
    }
};

struct GeneticParentResult {
    std::size_t ordinal;
    std::size_t rank;
    core::Sha256Digest trial;
    ResultAssociation result;
    // Exact median tasks per second from independently verified matching evidence.
    std::optional<double> throughput{};
};

struct HillClimbIncumbentScore {
    std::size_t ordinal;
    double throughput;
    auto operator==(const HillClimbIncumbentScore&) const -> bool = default;
};

struct GeneticGenerationCompletion {
    std::uint64_t generation;
    core::Sha256Digest child_plan;
    std::optional<core::Sha256Digest> child_ledger_head;
    std::size_t charged_attempts;
    // Retain eligible verified Results in child schedule order. Rank is an
    // zero-based ordinal within this completed generation and may be tied.
    std::vector<GeneticParentResult> eligible_parents;
    bool scientific_stop{};
    // The history derives this from verified factual scores when left empty.
    std::optional<AnnealingDecision> annealing_decision{};
};

struct GeneticCompletedGeneration {
    GeneticGenerationIntent intent;
    GeneticGenerationCompletion completion;
};

struct GeneticChildVerification {
    void* context{};
    // Verify the exact child plan bytes, terminal ledger head, charges, stop
    // and eligible Result associations under the child ownership lock.
    // Scored-strategy verification includes each retained binary64 throughput
    // and requires one fixed problem, Build and topology cohort across children.
    // The history independently rederives the annealing decision.
    std::expected<void, core::Error> (*verify)(void*, const GeneticGenerationIntent&,
                                               const GeneticGenerationCompletion&){};
};

class GeneticHistory {
  public:
    // The borrowed directory owner and verifier must outlive the history. Each
    // retained completion is verified again on replay before use as parents.
    [[nodiscard]] static auto open(const platform_linux::experiment_operations::Directory&,
                                   GeneticHistoryConfig, GeneticChildVerification)
        -> std::expected<GeneticHistory, core::Error>;
    GeneticHistory(GeneticHistory&&) noexcept;
    GeneticHistory(const GeneticHistory&) = delete;
    auto operator=(GeneticHistory&&) -> GeneticHistory& = delete;
    auto operator=(const GeneticHistory&) -> GeneticHistory& = delete;

    [[nodiscard]] auto next() const -> std::expected<std::optional<GeneticGeneration>, core::Error>;
    [[nodiscard]] auto next_hill_climb() const -> std::expected<std::optional<HillClimbBatch>, core::Error>;
    [[nodiscard]] auto next_annealing() const -> std::expected<std::optional<AnnealingStep>, core::Error>;
    [[nodiscard]] auto next_bayesian() const -> std::expected<std::optional<BayesianStep>, core::Error>;
    [[nodiscard]] auto bayesian_best() const noexcept -> std::optional<BayesianScore> {
        return bayesian_best_;
    }
    [[nodiscard]] auto hill_climb_incumbent() const noexcept -> std::optional<HillClimbIncumbentScore> {
        return incumbent_;
    }
    [[nodiscard]] auto annealing_incumbent() const noexcept -> std::optional<AnnealingScore> {
        return annealing_incumbent_;
    }
    [[nodiscard]] auto annealing_best() const noexcept -> std::optional<AnnealingScore> {
        return annealing_best_;
    }
    // Freeze the child plan digest before publishing the intent. Execution or
    // resume may use only the matching ordinary child plan after publication.
    [[nodiscard]] auto begin(GeneticGenerationIntent) -> std::expected<void, core::Error>;
    [[nodiscard]] auto complete(GeneticGenerationCompletion) -> std::expected<void, core::Error>;
    // A pending child may stop only through its verified scientific completion.
    [[nodiscard]] auto stop_scientifically() -> std::expected<void, core::Error>;
    [[nodiscard]] auto pending() const noexcept -> const GeneticGenerationIntent* {
        return pending_ ? &*pending_ : nullptr;
    }
    [[nodiscard]] auto completed() const noexcept -> std::span<const GeneticCompletedGeneration> {
        return completed_;
    }
    // Active child reservations belong to the pending child's own ledger.
    [[nodiscard]] auto head() const noexcept -> std::optional<core::Sha256Digest> { return head_; }
    [[nodiscard]] auto charged_attempts() const noexcept -> std::size_t { return charged_; }
    [[nodiscard]] auto scientifically_stopped() const noexcept -> bool { return stopped_; }

  private:
    using Event = std::variant<GeneticGenerationIntent, GeneticGenerationCompletion, std::monostate>;
    GeneticHistory(const platform_linux::experiment_operations::Directory&,
                   GeneticHistoryConfig, GeneticChildVerification);
    [[nodiscard]] auto accept(Event, std::optional<std::string_view> replay = {})
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto proposal_limit() const noexcept -> std::size_t;

    const platform_linux::experiment_operations::Directory* directory_;
    GeneticHistoryConfig config_;
    GeneticChildVerification verification_;
    std::vector<std::size_t> visited_;
    std::optional<GeneticGenerationIntent> pending_;
    std::vector<GeneticCompletedGeneration> completed_;
    std::optional<HillClimbIncumbentScore> incumbent_;
    std::optional<AnnealingScore> annealing_incumbent_;
    std::optional<AnnealingScore> annealing_best_;
    std::optional<BayesianScore> bayesian_best_;
    std::optional<core::Sha256Digest> head_;
    std::uint64_t next_sequence_{1};
    std::size_t charged_{};
    bool stopped_{};
};

} // namespace epycbench::experiment
