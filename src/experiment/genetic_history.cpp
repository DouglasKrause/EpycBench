#include "experiment/genetic_history.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "platform_linux/experiment_operations.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace epycbench::experiment {
namespace {

using Json = nlohmann::json;
using platform_linux::experiment_operations::Directory;
using namespace std::string_view_literals;

constexpr auto genetic_profile = "epycbench.genetic-history-event/v1"sv;
constexpr auto hill_climb_profile = "epycbench.hill-climb-history-event/v1"sv;
constexpr auto annealing_history_profile = "epycbench.annealing-history-event/v1"sv;
constexpr auto hybrid_history_profile = "epycbench.hybrid-history-event/v1"sv;
constexpr auto bayesian_history_profile = "epycbench.bayesian-history-event/v1"sv;
constexpr auto event_prefix = "genetic-event-"sv;
constexpr std::size_t record_bytes = 4U << 20U;
constexpr std::size_t sequence_digits = 8U;

auto failure(std::string_view classification, std::uint64_t sequence)
    -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"experiment.genetic_history.invalid"},
        .message = "Genetic generation history could not be validated",
        .operation = "experiment.genetic_history",
        .os_error = {},
        .context = {{"classification", classification}, {"sequence", sequence}},
    }};
}

auto filename(std::uint64_t sequence) -> std::string {
    const auto digits = std::to_string(sequence);
    return std::string{event_prefix} + std::string(sequence_digits - digits.size(), '0') + digits +
           ".json";
}

constexpr std::array origins{
    std::pair{GeneticProposalOrigin::initial, "initial"sv},
    std::pair{GeneticProposalOrigin::offspring, "offspring"sv},
    std::pair{GeneticProposalOrigin::exploration, "exploration"sv},
};

constexpr std::array hill_climb_origins{
    std::pair{HillClimbProposalOrigin::initial, "initial"sv},
    std::pair{HillClimbProposalOrigin::neighbor, "neighbor"sv},
    std::pair{HillClimbProposalOrigin::restart, "restart"sv},
};

auto local_proposal(const HillClimbProposal& proposal) -> Json {
    const auto& change = proposal.change;
    return {{"ordinal", proposal.ordinal}, {"origin", core::detail::enum_name(proposal.origin, hill_climb_origins)},
            {"axis_change", change ? Json{{"axis", change->axis}, {"before", change->before},
                 {"after", change->after}} : Json(nullptr)}};
}

auto hill_climb_payload(const GeneticGenerationIntent& intent, const HillClimbBatch& batch) -> Json {
    auto proposals = Json::array();
    for (const auto& proposal : batch.proposals) proposals.push_back(local_proposal(proposal));
    return {{"kind", "generation_open"}, {"generation", batch.step},
            {"proposal_profile", hill_climb_proposal_profile}, {"proposals", std::move(proposals)},
            {"incumbent_ordinal", batch.incumbent ? Json(batch.incumbent->ordinal) : Json(nullptr)},
            {"unseen_neighbors", batch.unseen_neighbors}, {"exhausted", batch.exhausted},
            {"child_plan_sha256", intent.child_plan.to_hex()}, {"scheduled_ordinals", intent.scheduled_ordinals}};
}

auto annealing_payload(const GeneticGenerationIntent& intent, const AnnealingStep& step) -> Json {
    return {{"kind", "generation_open"}, {"generation", step.step},
            {"proposal_profile", annealing_profile}, {"proposals", Json::array({local_proposal(*step.proposal)})},
            {"incumbent_ordinal", step.incumbent_ordinal ? Json(*step.incumbent_ordinal) : Json(nullptr)},
            {"temperature", step.temperature}, {"exhausted", step.exhausted},
            {"child_plan_sha256", intent.child_plan.to_hex()}, {"scheduled_ordinals", intent.scheduled_ordinals}};
}

auto bayesian_payload(const GeneticGenerationIntent& intent, const BayesianStep& step,
                      StudyHistoryStrategy strategy) -> Json {
    const auto& proposal = *step.proposal;
    const auto origin = proposal.origin == BayesianProposalOrigin::local ? "local" :
        proposal.origin == BayesianProposalOrigin::surrogate ? "surrogate" : "exploration";
    Json lineage{{"ordinal", proposal.ordinal}, {"origin", origin},
        {"elite_probability_units", proposal.elite_probability_units ? Json(*proposal.elite_probability_units) : Json(nullptr)}};
    const bool hybrid = strategy == StudyHistoryStrategy::hybrid;
    if (hybrid) {
        lineage["parent_ordinal"] = proposal.parent ? Json(*proposal.parent) : Json(nullptr);
        const auto& change = proposal.change;
        lineage["axis_change"] = change ? Json{{"axis", change->axis}, {"before", change->before}, {"after", change->after}}
                                        : Json(nullptr);
    }
    return {{"kind", "generation_open"}, {"generation", step.step},
            {"proposal_profile", hybrid ? hybrid_proposal_profile : bayesian_proposal_profile},
            {"proposals", Json::array({std::move(lineage)})},
            {"verified_count", step.verified_count}, {"elite_count", step.elite_count},
            {"exhausted", step.exhausted}, {"child_plan_sha256", intent.child_plan.to_hex()},
            {"scheduled_ordinals", intent.scheduled_ordinals}};
}

auto payload(const GeneticGenerationIntent& intent, StudyHistoryStrategy strategy) -> Json {
    if (intent.bayesian) return bayesian_payload(intent, *intent.bayesian, strategy);
    if (intent.annealing) return annealing_payload(intent, *intent.annealing);
    if (intent.hill_climb) return hill_climb_payload(intent, *intent.hill_climb);
    auto proposals = Json::array();
    for (const auto& proposal : intent.generation.proposals) {
        const auto& crossover = proposal.crossover;
        const auto& mutation = proposal.mutation;
        proposals.push_back({{"ordinal", proposal.ordinal},
            {"origin", core::detail::enum_name(proposal.origin, origins)},
            {"crossover", crossover ? Json{{"first_parent", crossover->first_parent},
                 {"second_parent", crossover->second_parent},
                 {"second_parent_axes", crossover->second_parent_axes},
                 {"ordinal", crossover->ordinal}} : Json(nullptr)},
            {"mutation", mutation ? Json{{"axis", mutation->axis}, {"before", mutation->before},
                 {"after", mutation->after}} : Json(nullptr)}});
    }
    return {{"kind", "generation_open"}, {"generation", intent.generation.generation},
            {"proposal_profile", genetic_proposal_profile}, {"proposals", std::move(proposals)},
            {"offspring_draws", intent.generation.offspring_draws},
            {"duplicate_offspring", intent.generation.duplicate_offspring},
            {"exhausted", intent.generation.exhausted},
            {"child_plan_sha256", intent.child_plan.to_hex()},
            {"scheduled_ordinals", intent.scheduled_ordinals}};
}

constexpr std::size_t throughput_hex_digits = 16U;

auto encode_throughput(double value) -> std::string {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
    auto bits = std::bit_cast<std::uint64_t>(value);
    std::string digits(throughput_hex_digits, '0');
    constexpr std::string_view hex{"0123456789abcdef"};
    for (auto& digit : digits | std::views::reverse) {
        digit = hex[static_cast<std::size_t>(bits & 0xfU)];
        bits >>= 4U;
    }
    return digits;
}

auto payload(const GeneticGenerationCompletion& completion) -> Json {
    auto parents = Json::array();
    for (const auto& parent : completion.eligible_parents) {
        auto row = Json{{"ordinal", parent.ordinal}, {"rank", parent.rank},
            {"trial_sha256", parent.trial.to_hex()}, {"run_id", parent.result.run_id},
            {"result_sha256", parent.result.content_digest.to_hex()}, {"artifact", parent.result.artifact}};
        if (parent.throughput) row["throughput_binary64"] = encode_throughput(*parent.throughput);
        parents.push_back(std::move(row));
    }
    auto document = Json{{"kind", "generation_complete"}, {"generation", completion.generation},
            {"child_plan_sha256", completion.child_plan.to_hex()},
            {"child_ledger_head_sha256", completion.child_ledger_head
                ? Json(completion.child_ledger_head->to_hex()) : Json(nullptr)},
            {"charged_attempts", completion.charged_attempts},
            {"eligible_parents", std::move(parents)}, {"scientific_stop", completion.scientific_stop}};
    if (const auto& decision = completion.annealing_decision) {
        document["annealing_decision"] = {
            {"accepted", decision->accepted}, {"loss_units", decision->loss_units},
            {"threshold", decision->threshold}, {"draw", decision->draw ? Json(*decision->draw) : Json(nullptr)}};
    }
    return document;
}

auto payload(std::monostate) -> Json { return {{"kind", "scientific_stop"}}; }

struct EncodedEntry {
    std::string bytes;
    core::Sha256Digest digest;
};

auto encode(Json event, StudyHistoryStrategy strategy, const core::Sha256Digest& plan, std::uint64_t sequence,
            const std::optional<core::Sha256Digest>& previous)
    -> std::expected<EncodedEntry, core::Error> {
    const auto profile = strategy == StudyHistoryStrategy::hybrid ? hybrid_history_profile
        : strategy == StudyHistoryStrategy::bayesian ? bayesian_history_profile
        : strategy == StudyHistoryStrategy::annealing ? annealing_history_profile
        : strategy == StudyHistoryStrategy::hill_climb ? hill_climb_profile : genetic_profile;
    Json value{{"profile", profile}, {"plan_sha256", plan.to_hex()}, {"sequence", sequence},
               {"previous_event_sha256", previous ? Json(previous->to_hex()) : Json(nullptr)},
               {"event", std::move(event)}};
    auto digest = core::hash_canonical_json(value);
    if (!digest) return std::unexpected{std::move(digest.error())};
    value["event_sha256"] = digest->to_hex();
    auto bytes = core::canonicalize_json(value);
    if (!bytes) return std::unexpected{std::move(bytes.error())};
    return EncodedEntry{std::move(*bytes), *digest};
}

struct Malformed {};

auto number(const Json& value) -> std::uint64_t {
    if (!value.is_number_unsigned()) throw Malformed{};
    return value.get<std::uint64_t>();
}

auto index_value(const Json& value) -> std::size_t {
    const auto index = number(value);
    if (index > std::numeric_limits<std::size_t>::max()) throw Malformed{};
    return static_cast<std::size_t>(index);
}

auto text(const Json& value) -> const std::string& { return value.get_ref<const std::string&>(); }

auto digest_value(const Json& value) -> core::Sha256Digest {
    const auto digest = core::Sha256Digest::parse(text(value));
    if (!digest) throw Malformed{};
    return *digest;
}

auto decode_scheduled(const Json& event, std::size_t proposals) -> std::vector<std::size_t> {
    const auto& values = event.at("scheduled_ordinals");
    if (!values.is_array() || values.size() > proposals) throw Malformed{};
    std::vector<std::size_t> scheduled;
    scheduled.reserve(values.size());
    for (const auto& value : values) scheduled.push_back(index_value(value));
    return scheduled;
}

auto decode_throughput(const Json& value) -> double {
    const auto& digits = text(value);
    if (digits.size() != throughput_hex_digits) throw Malformed{};
    std::uint64_t bits{};
    const auto* last = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), last, bits, 16);
    if (parsed.ptr != last || parsed.ec != std::errc{}) throw Malformed{};
    return std::bit_cast<double>(bits);
}

auto decode_completion(const Json& event, bool retain_scores) -> GeneticGenerationCompletion {
    const auto charged = index_value(event.at("charged_attempts"));
    const auto& values = event.at("eligible_parents");
    if (!values.is_array() || charged > maximum_attempts || values.size() > charged)
        throw Malformed{};
    std::vector<GeneticParentResult> parents;
    parents.reserve(values.size());
    for (const auto& value : values) {
        const auto throughput = retain_scores ? std::optional{decode_throughput(value.at("throughput_binary64"))}
                                              : std::nullopt;
        parents.push_back({index_value(value.at("ordinal")), index_value(value.at("rank")),
            digest_value(value.at("trial_sha256")),
            {text(value.at("run_id")), digest_value(value.at("result_sha256")), text(value.at("artifact"))},
            throughput});
    }
    std::optional<core::Sha256Digest> head;
    if (const auto& encoded = event.at("child_ledger_head_sha256"); !encoded.is_null())
        head = digest_value(encoded);
    return {number(event.at("generation")), digest_value(event.at("child_plan_sha256")), head,
            charged, std::move(parents), event.at("scientific_stop").get<bool>()};
}

auto valid_reference(const ResultAssociation& association) noexcept -> bool {
    constexpr auto run_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"sv;
    constexpr auto artifact_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:"sv;
    return !association.run_id.empty() && association.run_id.size() <= 64U &&
           association.run_id.find_first_not_of(run_chars) == std::string::npos &&
           !association.artifact.empty() && association.artifact.size() <= 255U &&
           association.artifact != "." && association.artifact != ".." &&
           association.artifact.find_first_not_of(artifact_chars) == std::string::npos;
}

auto valid_intent_schedule(const GeneticGenerationIntent& intent) noexcept -> bool {
    const auto count = intent.proposal_count();
    std::size_t next{};
    for (const auto ordinal : intent.scheduled_ordinals) {
        while (next < count && intent.proposal_ordinal(next) != ordinal) ++next;
        if (next == count) return false;
        ++next;
    }
    return true;
}

auto valid_completion(const GeneticGenerationIntent& intent,
                      const GeneticGenerationCompletion& completion) noexcept -> bool {
    const bool retain_scores = intent.hill_climb.has_value() || intent.annealing.has_value() || intent.bayesian.has_value();
    if (completion.generation != intent.index() || completion.child_plan != intent.child_plan ||
        completion.charged_attempts > intent.scheduled_ordinals.size() ||
        (completion.charged_attempts != intent.scheduled_ordinals.size() && !completion.scientific_stop) ||
        (!completion.child_ledger_head && (completion.charged_attempts != 0 || completion.scientific_stop)) ||
        (retain_scores && completion.scientific_stop && !completion.eligible_parents.empty()) ||
        (!intent.annealing && completion.annealing_decision))
        return false;
    const auto charged = std::span{intent.scheduled_ordinals}.first(completion.charged_attempts);
    auto next = charged.begin();
    for (const auto& parent : completion.eligible_parents) {
        if (parent.rank >= charged.size() || !valid_reference(parent.result) ||
            parent.throughput.has_value() != retain_scores ||
            (parent.throughput && (!std::isfinite(*parent.throughput) || *parent.throughput <= 0.0)))
            return false;
        next = std::ranges::find(next, charged.end(), parent.ordinal);
        if (next == charged.end()) return false;
        ++next;
    }
    return true;
}

void select_hill_climb_incumbent(const HillClimbBatch& batch, const GeneticGenerationCompletion& completion,
                                std::optional<HillClimbIncumbentScore>& incumbent) {
    if (completion.eligible_parents.empty()) return;
    const auto best = std::ranges::max_element(completion.eligible_parents, [](const auto& a, const auto& b) {
        return a.throughput == b.throughput ? a.ordinal > b.ordinal : a.throughput < b.throughput;
    });
    const auto rate = *best->throughput;
    const auto phase = batch.proposals.front().origin;
    if (!incumbent || phase != HillClimbProposalOrigin::neighbor || rate > incumbent->throughput)
        incumbent.emplace(best->ordinal, rate);
}

} // namespace

auto GeneticGenerationIntent::for_hill_climb(HillClimbBatch batch, core::Sha256Digest child_plan,
                                            std::vector<std::size_t> scheduled) -> GeneticGenerationIntent {
    return {{}, child_plan, std::move(scheduled), std::move(batch)};
}

auto GeneticGenerationIntent::for_annealing(AnnealingStep step, core::Sha256Digest child_plan,
                                            std::vector<std::size_t> scheduled) -> GeneticGenerationIntent {
    return {{}, child_plan, std::move(scheduled), {}, std::move(step)};
}

auto GeneticGenerationIntent::for_bayesian(BayesianStep step, core::Sha256Digest child_plan,
                                           std::vector<std::size_t> scheduled) -> GeneticGenerationIntent {
    return {{}, child_plan, std::move(scheduled), {}, {}, std::move(step)};
}

GeneticHistory::GeneticHistory(const Directory& directory, GeneticHistoryConfig config,
                               GeneticChildVerification verification)
    : directory_{&directory}, config_{std::move(config)}, verification_{verification} {
    visited_.reserve(config_.space.size());
    completed_.reserve(std::min(config_.maximum_generations, config_.space.size()));
}

GeneticHistory::GeneticHistory(GeneticHistory&& other) noexcept
    : directory_{std::exchange(other.directory_, nullptr)}, config_{std::move(other.config_)},
      verification_{other.verification_}, visited_{std::move(other.visited_)},
      pending_{std::move(other.pending_)}, completed_{std::move(other.completed_)}, incumbent_{other.incumbent_},
      annealing_incumbent_{other.annealing_incumbent_}, annealing_best_{other.annealing_best_},
      bayesian_best_{other.bayesian_best_}, head_{other.head_},
      next_sequence_{other.next_sequence_}, charged_{other.charged_}, stopped_{other.stopped_} {}

auto GeneticHistory::open(const Directory& directory, GeneticHistoryConfig config,
                         GeneticChildVerification verification)
    -> std::expected<GeneticHistory, core::Error> {
    const bool genetic = config.strategy == StudyHistoryStrategy::genetic;
    const bool hill_climb = config.strategy == StudyHistoryStrategy::hill_climb;
    const bool annealing = config.strategy == StudyHistoryStrategy::annealing;
    const bool bayesian = uses_bayesian_proposals(config.strategy);
    if (!verification.verify || config.attempt_budget == 0U || config.attempt_budget > maximum_attempts ||
        config.maximum_generations == 0U || config.maximum_generations > maximum_attempts ||
        config.options.population == 0U || config.options.population > maximum_attempts ||
        (!genetic && !hill_climb && !annealing && !bayesian) ||
        config.options.mutation_per_mille > (genetic ? 1000U : 0U) ||
        config.annealing_cooling.has_value() != annealing || ((annealing || bayesian) && config.options.population != 1U))
        return failure("generation_options", 0);
    if (const auto& cooling = config.annealing_cooling) {
        if (cooling->minimum_temperature == 0U || cooling->minimum_temperature > cooling->initial_temperature ||
            cooling->initial_temperature > annealing_loss_scale || cooling->steps_per_halving == 0U ||
            cooling->steps_per_halving > maximum_attempts)
            return failure("annealing_cooling", 0);
    }
    const auto generations = std::min(config.maximum_generations, config.space.size());
    const auto maximum_records = 2U * generations + 1U;
    auto names = directory.list(maximum_records + generations + 16U);
    if (!names) return std::unexpected{std::move(names.error())};
    GeneticHistory history{directory, std::move(config), verification};
    for (const auto& name : *names) {
        if (!name.starts_with(event_prefix)) continue;
        const auto sequence = history.next_sequence_;
        if (sequence > maximum_records || name != filename(sequence))
            return failure("record_sequence", sequence);
        auto bytes = directory.read(name, record_bytes);
        if (!bytes) return std::unexpected{std::move(bytes.error())};
        try {
            std::size_t events{};
            const auto bounded = [&events](int depth, Json::parse_event_t, Json&) {
                if (depth > 8 || ++events > 80U * maximum_attempts + 128U) throw Malformed{};
                return true;
            };
            const auto value = Json::parse(*bytes, bounded);
            const auto& event = value.at("event");
            const auto& kind = text(event.at("kind"));
            Event decoded{std::monostate{}};
            if (kind == "generation_open") {
                const auto child_plan = digest_value(event.at("child_plan_sha256"));
                if (bayesian) {
                    auto planned = history.next_bayesian();
                    if (!planned) return std::unexpected{std::move(planned.error())};
                    if (!*planned) return failure("unexpected_generation", sequence);
                    auto scheduled = decode_scheduled(event, 1U);
                    decoded = GeneticGenerationIntent::for_bayesian(std::move(**planned), child_plan,
                                                                    std::move(scheduled));
                } else if (annealing) {
                    auto planned = history.next_annealing();
                    if (!planned) return std::unexpected{std::move(planned.error())};
                    if (!*planned) return failure("unexpected_generation", sequence);
                    auto scheduled = decode_scheduled(event, 1U);
                    decoded = GeneticGenerationIntent::for_annealing(std::move(**planned), child_plan,
                                                                      std::move(scheduled));
                } else if (hill_climb) {
                    auto planned = history.next_hill_climb();
                    if (!planned) return std::unexpected{std::move(planned.error())};
                    if (!*planned) return failure("unexpected_generation", sequence);
                    auto scheduled = decode_scheduled(event, (**planned).proposals.size());
                    decoded = GeneticGenerationIntent::for_hill_climb(std::move(**planned), child_plan,
                                                                      std::move(scheduled));
                } else {
                    auto planned = history.next();
                    if (!planned) return std::unexpected{std::move(planned.error())};
                    if (!*planned) return failure("unexpected_generation", sequence);
                    auto scheduled = decode_scheduled(event, (**planned).proposals.size());
                    decoded = GeneticGenerationIntent{std::move(**planned), child_plan, std::move(scheduled)};
                }
            } else if (kind == "generation_complete") {
                decoded = decode_completion(event, !genetic);
            } else if (kind != "scientific_stop") {
                return failure("record_kind", sequence);
            }
            auto accepted = history.accept(std::move(decoded), std::string_view{*bytes});
            if (!accepted) return std::unexpected{std::move(accepted.error())};
        } catch (const Malformed&) {
            return failure("record_shape", sequence);
        } catch (const Json::exception&) {
            return failure("record_shape", sequence);
        }
    }
    return history;
}

auto GeneticHistory::proposal_limit() const noexcept -> std::size_t {
    if (pending_ || stopped_ || completed_.size() == config_.maximum_generations) return 0;
    return std::min(config_.options.population, config_.attempt_budget - charged_);
}

auto GeneticHistory::next() const -> std::expected<std::optional<GeneticGeneration>, core::Error> {
    if (!directory_) return failure("closed_history", next_sequence_);
    if (config_.strategy != StudyHistoryStrategy::genetic) return failure("proposal_strategy", next_sequence_);
    const auto limit = proposal_limit();
    if (limit == 0U) return std::optional<GeneticGeneration>{};
    const GeneticProposalOptions options{limit, config_.options.mutation_per_mille};
    std::vector<RankedGeneticParent> parents;
    if (!completed_.empty()) {
        const auto& eligible = completed_.back().completion.eligible_parents;
        parents.reserve(eligible.size());
        for (const auto& parent : eligible) parents.push_back({parent.ordinal, parent.rank});
    }
    auto planned = propose_genetic_generation(config_.space, config_.root_seed, completed_.size(),
                                               options, parents, visited_);
    if (!planned) return std::unexpected{std::move(planned.error())};
    if (planned->proposals.empty()) return std::optional<GeneticGeneration>{};
    return std::optional<GeneticGeneration>{std::move(*planned)};
}

auto GeneticHistory::next_hill_climb() const -> std::expected<std::optional<HillClimbBatch>, core::Error> {
    if (!directory_) return failure("closed_history", next_sequence_);
    if (config_.strategy != StudyHistoryStrategy::hill_climb) return failure("proposal_strategy", next_sequence_);
    const auto limit = proposal_limit();
    if (limit == 0U) return std::optional<HillClimbBatch>{};
    const auto incumbent = incumbent_ ? std::optional{HillClimbIncumbent{incumbent_->ordinal}} : std::nullopt;
    auto planned = propose_hill_climb_batch(config_.space, config_.root_seed, completed_.size(),
                                           limit, incumbent, visited_);
    if (!planned) return std::unexpected{std::move(planned.error())};
    if (planned->proposals.empty()) return std::optional<HillClimbBatch>{};
    return std::optional<HillClimbBatch>{std::move(*planned)};
}

auto GeneticHistory::next_annealing() const -> std::expected<std::optional<AnnealingStep>, core::Error> {
    if (!directory_) return failure("closed_history", next_sequence_);
    if (config_.strategy != StudyHistoryStrategy::annealing) return failure("proposal_strategy", next_sequence_);
    if (proposal_limit() == 0U) return std::optional<AnnealingStep>{};
    const auto incumbent = annealing_incumbent_ ? std::optional{annealing_incumbent_->ordinal} : std::nullopt;
    auto planned = propose_annealing_step(config_.space, config_.root_seed, completed_.size(),
                                          *config_.annealing_cooling, incumbent, visited_);
    if (!planned) return std::unexpected{std::move(planned.error())};
    if (!planned->proposal) return std::optional<AnnealingStep>{};
    return std::optional<AnnealingStep>{std::move(*planned)};
}

auto GeneticHistory::next_bayesian() const -> std::expected<std::optional<BayesianStep>, core::Error> {
    if (!directory_) return failure("closed_history", next_sequence_);
    if (!uses_bayesian_proposals(config_.strategy)) return failure("proposal_strategy", next_sequence_);
    if (proposal_limit() == 0U) return std::optional<BayesianStep>{};
    std::vector<BayesianScore> scores;
    scores.reserve(charged_);
    for (const auto& completed : completed_)
        for (const auto& result : completed.completion.eligible_parents)
            scores.push_back({result.ordinal, *result.throughput});
    const auto policy = config_.strategy == StudyHistoryStrategy::hybrid
        ? BayesianProposalPolicy::hybrid : BayesianProposalPolicy::categorical;
    auto planned = propose_bayesian_step(config_.space, config_.root_seed, completed_.size(), scores, visited_, policy);
    if (!planned) return std::unexpected{std::move(planned.error())};
    if (!planned->proposal) return std::optional<BayesianStep>{};
    return std::optional<BayesianStep>{std::move(*planned)};
}

auto GeneticHistory::begin(GeneticGenerationIntent intent) -> std::expected<void, core::Error> {
    return accept(std::move(intent));
}

auto GeneticHistory::complete(GeneticGenerationCompletion completion) -> std::expected<void, core::Error> {
    return accept(std::move(completion));
}

auto GeneticHistory::stop_scientifically() -> std::expected<void, core::Error> {
    return accept(std::monostate{});
}

auto GeneticHistory::accept(Event event, std::optional<std::string_view> replay)
    -> std::expected<void, core::Error> {
    const auto sequence = next_sequence_;
    if (!directory_ || stopped_) return failure("closed_history", sequence);
    auto* intent = std::get_if<GeneticGenerationIntent>(&event);
    auto* completion = std::get_if<GeneticGenerationCompletion>(&event);
    std::optional<AnnealingScore> candidate_score;
    if (intent) {
        const bool hill_climb = config_.strategy == StudyHistoryStrategy::hill_climb;
        const bool annealing = config_.strategy == StudyHistoryStrategy::annealing;
        const bool bayesian = uses_bayesian_proposals(config_.strategy);
        if (intent->hill_climb.has_value() != hill_climb || intent->annealing.has_value() != annealing ||
            intent->bayesian.has_value() != bayesian ||
            ((hill_climb || annealing || bayesian) && intent->generation != GeneticGeneration{}))
            return failure("proposal_strategy", sequence);
        if (!replay) {
            if (bayesian) {
                auto planned = next_bayesian();
                if (!planned) return std::unexpected{std::move(planned.error())};
                if (*planned != intent->bayesian) return failure("generation_lineage", sequence);
            } else if (annealing) {
                auto planned = next_annealing();
                if (!planned) return std::unexpected{std::move(planned.error())};
                if (*planned != intent->annealing) return failure("generation_lineage", sequence);
            } else if (hill_climb) {
                auto planned = next_hill_climb();
                if (!planned) return std::unexpected{std::move(planned.error())};
                if (*planned != intent->hill_climb) return failure("generation_lineage", sequence);
            } else {
                auto planned = next();
                if (!planned) return std::unexpected{std::move(planned.error())};
                if (!*planned || **planned != intent->generation) return failure("generation_lineage", sequence);
            }
        }
        if (!valid_intent_schedule(*intent)) return failure("child_schedule", sequence);
    } else if (completion) {
        if (!pending_ || !valid_completion(*pending_, *completion))
            return failure("child_completion", sequence);
        if (pending_->annealing) {
            if (!completion->eligible_parents.empty()) {
                const auto& eligible = completion->eligible_parents.front();
                candidate_score.emplace(eligible.ordinal, *eligible.throughput);
            }
            // Only verified factual scores reach durable storage or change the
            // incumbent, after the canonical frame and verifier checks below.
            auto decided = decide_annealing_move(*pending_->annealing, config_.root_seed,
                                                 annealing_incumbent_, candidate_score);
            if (!decided || (completion->annealing_decision && *completion->annealing_decision != *decided))
                return failure("annealing_decision", sequence);
            completion->annealing_decision = *decided;
        }
    } else if (pending_) {
        return failure("pending_child", sequence);
    }

    auto encoded = encode(std::visit([this](const auto& item) {
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>, GeneticGenerationIntent>)
            return payload(item, config_.strategy);
        else return payload(item);
    }, event),
                          config_.strategy, config_.plan_digest, sequence, head_);
    if (!encoded) return std::unexpected{std::move(encoded.error())};
    if (replay && encoded->bytes != *replay) return failure("record_encoding_or_binding", sequence);
    if (completion) {
        auto verified = verification_.verify(verification_.context, *pending_, *completion);
        if (!verified) return verified;
    }
    if (!replay) {
        auto persisted = directory_->write_new(filename(sequence), encoded->bytes, record_bytes);
        if (!persisted) {
            directory_ = nullptr;
            return persisted;
        }
    }

    if (intent) {
        for (std::size_t i{}; i < intent->proposal_count(); ++i) visited_.push_back(intent->proposal_ordinal(i));
        pending_.emplace(std::move(*intent));
    } else if (completion) {
        charged_ += completion->charged_attempts;
        stopped_ = completion->scientific_stop;
        if (pending_->hill_climb) select_hill_climb_incumbent(*pending_->hill_climb, *completion, incumbent_);
        if (candidate_score) {
            if (completion->annealing_decision->accepted) annealing_incumbent_ = candidate_score;
            if (!annealing_best_ || candidate_score->throughput > annealing_best_->throughput ||
                (candidate_score->throughput == annealing_best_->throughput &&
                 candidate_score->ordinal < annealing_best_->ordinal))
                annealing_best_ = candidate_score;
        }
        if (pending_->bayesian) {
            for (const auto& score : completion->eligible_parents)
                if (!bayesian_best_ || *score.throughput > bayesian_best_->throughput ||
                    (*score.throughput == bayesian_best_->throughput && score.ordinal < bayesian_best_->ordinal))
                    bayesian_best_ = BayesianScore{score.ordinal, *score.throughput};
        }
        completed_.push_back({std::move(*pending_), std::move(*completion)});
        pending_.reset();
    } else {
        stopped_ = true;
    }
    head_ = encoded->digest;
    ++next_sequence_;
    return {};
}

} // namespace epycbench::experiment
