#include "cli/experiment_command.hpp"

#include "cli/package_run.hpp"
#include "cli/results_command.hpp"
#include "cli/scenario_command.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/core/seed.hpp"
#include "epycbench/experiment/coordinator.hpp"
#include "epycbench/experiment/search.hpp"
#include "epycbench/experiment/tradeoffs.hpp"
#include "epycbench/results/journal_records.hpp"
#include "experiment/analysis.hpp"
#include "experiment/resolved_population.hpp"
#include "experiment/genetic_history.hpp"
#include "experiment/ledger_store.hpp"
#include "experiment/package_confirmation.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/experiment_operations.hpp"
#include "results/package_result.hpp"
#include "runtime/package_catalog.hpp"
#include "scenario/package_scenario.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <ostream>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace epycbench::cli {

auto plan_package_digest(const scenario::ConfigurationSpace& space,
                         const kernel::packages::Catalog& catalog) -> core::Sha256Digest {
    auto identities = nlohmann::json::array();
    for (const auto& [task, implementation] : space.package_selections()) {
        const auto* package = catalog.find(task, implementation);
        identities.push_back(package ? kernel::packages::package_identity(*package)
            : nlohmann::json{{"task", task}, {"implementation", implementation}, {"available", false}});
    }
    return core::sha256(identities.dump());
}

namespace {
namespace exp = epycbench::experiment;
namespace ops = platform_linux::experiment_operations;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Digest = core::Sha256Digest;
using Error = core::Error;
using SignalScope = platform_linux::detail::ControllerSignalScope;
using ControllerSignal = platform_linux::detail::ControllerSignal;
// An interactive caller owns the coordinator's signals across terminal setup
// and restoration. Ordinary commands own them for the duration of dispatch.
class ExperimentSignals {
  public:
    explicit ExperimentSignals(SignalScope* borrowed) : borrowed_(borrowed) {}
    auto acquire() { return borrowed_ ? borrowed_->validate() : local_.acquire_for_package(); }
    auto& scope() { return borrowed_ ? *borrowed_ : local_; }
    auto finish() -> std::optional<ControllerSignal> {
        if (borrowed_) return borrowed_->observe();
        const auto closed = local_.close();
        if (!closed) return std::nullopt;
        return closed->observed;
    }
  private:
    SignalScope* borrowed_;
    SignalScope local_;
};
using Objective = scenario::TaskObjective;
using StudyStrategy = exp::StudyHistoryStrategy;
constexpr std::string_view plan_file = "plan.json";
constexpr std::string_view plan_profile = "epycbench.experiment-plan/v1";
constexpr std::string_view exploration_profile = "epycbench.experiment-plan/v2";
constexpr std::string_view confirmation_profile = "epycbench.experiment-plan/v3";
struct ManagedStudyProfile {
    StudyStrategy strategy;
    std::string_view child;
    std::string_view confirmation;
    std::string_view child_field;
    std::string_view discovery_field;
};
constexpr std::array managed_study_profiles{
    ManagedStudyProfile{StudyStrategy::genetic, "epycbench.experiment-plan/v4", "epycbench.experiment-plan/v5",
        "genetic_child", "genetic_discovery"},
    ManagedStudyProfile{StudyStrategy::hill_climb, "epycbench.experiment-plan/v6", "epycbench.experiment-plan/v7",
        "hill_climb_child", "hill_climb_discovery"},
    ManagedStudyProfile{StudyStrategy::annealing, "epycbench.experiment-plan/v8", "epycbench.experiment-plan/v9",
        "annealing_child", "annealing_discovery"},
    ManagedStudyProfile{StudyStrategy::bayesian, "epycbench.experiment-plan/v10", "epycbench.experiment-plan/v11",
        "bayesian_child", "bayesian_discovery"},
    ManagedStudyProfile{StudyStrategy::hybrid, "epycbench.experiment-plan/v12", "epycbench.experiment-plan/v13",
        "hybrid_child", "hybrid_discovery"},
};
auto managed_profile(StudyStrategy strategy) -> const ManagedStudyProfile& {
    return *std::ranges::find(managed_study_profiles, strategy, &ManagedStudyProfile::strategy);
}
auto managed_strategy(const Json& profile, std::string_view ManagedStudyProfile::* field)
    -> std::optional<StudyStrategy> {
    for (const auto& entry : managed_study_profiles)
        if (profile == entry.*field) return entry.strategy;
    return {};
}
constexpr std::string_view package_confirmation_protocol = "epycbench.package-fixed-confirmation/v1";
constexpr std::string_view confirmation_leaf = "confirmation";
constexpr std::string_view result_suffix = ".epycbench.json";
constexpr std::string_view journal_suffix = ".epycbench.journal.jsonseq";
constexpr std::size_t mib = 1024U * 1024U;
constexpr std::size_t maximum_trials = exp::maximum_attempts;
constexpr std::size_t maximum_plan_bytes = 16U * mib;
constexpr std::array disposition_names{"completed", "invalid", "failed", "interrupted"};

auto problem(std::string_view key, std::string_view message) -> Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"experiment." + std::string{key}},
            .message = std::string{message},
            .operation = "experiment", .os_error = {}, .context = {}};
}

auto current_study_build() -> std::expected<Digest, Error> {
    const auto& build = current_package_build();
    if (!build) return std::unexpected{build.error()};
    return build->digest;
}

auto admit_study_build(const Digest& required) -> std::expected<void, Error> {
    const auto current = current_study_build();
    if (!current) return std::unexpected{current.error()};
    if (*current != required)
        return std::unexpected{problem("plan.build", "The executing Build differs from the retained study Build")};
    return {};
}

struct FrozenTrial {
    Json scenario;
    std::string scenario_digest;
    std::string resolved_plan_digest;
    Digest build;
    Json controller;
};

struct Proposal {
    std::size_t ordinal;
    std::optional<std::pair<std::string, std::string>> rejection;
};

struct Exploration {
    exp::SearchStrategy strategy;
    scenario::ScenarioFormat format;
    std::string authored_space;
    std::vector<Proposal> proposals;
};

struct GeneticConfirmation {
    Digest study_plan;
    std::uint64_t generation;
    Digest generation_ledger_head;
    StudyStrategy strategy{StudyStrategy::genetic};
};

struct Confirmation {
    Digest discovery_plan;
    Digest discovery_trial;
    std::size_t discovery_trial_ordinal;
    std::size_t candidate_ordinal;
    std::uint64_t discovery_seed;
    std::uint64_t discovery_order_seed;
    exp::ResultAssociation discovery_result;
    Digest discovery_build;
    std::optional<GeneticConfirmation> genetic{};
    std::optional<std::uint64_t> discovery_calibrated_repetitions;
    Digest discovery_observed_inputs;
};

struct GeneticChild {
    Digest study_plan;
    std::uint64_t generation;
    std::vector<Proposal> proposals;
    StudyStrategy strategy{StudyStrategy::genetic};
};

struct Plan {
    exp::Schedule schedule;
    std::vector<FrozenTrial> frozen;
    std::optional<Exploration> exploration;
    std::optional<Confirmation> confirmation;
    Digest build;
    std::optional<GeneticChild> genetic_child{};
};

auto genetic_child_document(const GeneticChild&) -> Json;
auto decode_genetic_child(const Json&, const Plan&, StudyStrategy) -> std::expected<GeneticChild, Error>;

auto confirmation_document(const Confirmation& origin) -> Json {
    auto result = Json{{"purpose", "confirmation"},
            {"protocol", package_confirmation_protocol},
            {"discovery_plan_sha256", origin.discovery_plan.to_hex()},
            {"discovery_trial_sha256", origin.discovery_trial.to_hex()},
            {"discovery_trial_ordinal", origin.discovery_trial_ordinal},
            {"candidate_ordinal", origin.candidate_ordinal},
            {"discovery_seed", origin.discovery_seed},
            {"discovery_order_seed", origin.discovery_order_seed},
            {"discovery_build_sha256", origin.discovery_build.to_hex()},
            {"discovery_observed_inputs_sha256", origin.discovery_observed_inputs.to_hex()},
            {"discovery_result", {{"run_id", origin.discovery_result.run_id},
                                  {"content_sha256", origin.discovery_result.content_digest.to_hex()},
                                  {"artifact", origin.discovery_result.artifact}}}};
    if (origin.discovery_calibrated_repetitions)
        result["discovery_calibrated_repetitions"] = *origin.discovery_calibrated_repetitions;
    if (const auto& genetic = origin.genetic)
        result[managed_profile(genetic->strategy).discovery_field] = {
            {"study_plan_sha256", genetic->study_plan.to_hex()}, {"generation", genetic->generation},
            {"generation_ledger_head_sha256", genetic->generation_ledger_head.to_hex()}};
    return result;
}

auto exploration_document(const Exploration& exploration) -> Json {
    auto proposals = Json::array();
    std::size_t next_trial{};
    for (const auto& proposal : exploration.proposals) {
        auto item = Json{{"ordinal", proposal.ordinal}, {"trial_ordinal", nullptr}, {"rejection", nullptr}};
        if (proposal.rejection) {
            const auto& [code, pointer] = *proposal.rejection;
            item["rejection"] = {{"code", code}, {"pointer", pointer}};
        } else {
            item["trial_ordinal"] = next_trial++;
        }
        proposals.push_back(std::move(item));
    }
    return {{"purpose", "exploration"}, {"strategy", exp::strategy_token(exploration.strategy)},
            {"format", exploration.format == scenario::ScenarioFormat::yaml ? "yaml" : "json"},
            {"configuration_space", exploration.authored_space},
            {"space_sha256", core::sha256(exploration.authored_space).to_hex()},
            {"proposals", std::move(proposals)}};
}

auto document(const Plan& plan) -> Json {
    auto trials = Json::array();
    for (const auto& trial : plan.schedule.trials()) {
        const auto& frozen = plan.frozen.at(trial.ordinal);
        trials.push_back({{"trial", trial.id.to_hex()},
                          {"candidate", trial.configuration.candidate.to_hex()},
                          {"problem", trial.configuration.problem.to_hex()},
                          {"order_seed", trial.comparison_order_seed},
                          {"scenario", frozen.scenario},
                          {"scenario_digest", frozen.scenario_digest},
                          {"resolved_plan_digest", frozen.resolved_plan_digest}});
    }
    auto profile = plan.exploration ? exploration_profile : plan_profile;
    if (plan.genetic_child) {
        profile = managed_profile(plan.genetic_child->strategy).child;
    } else if (plan.confirmation) {
        const auto& origin = plan.confirmation->genetic;
        profile = origin ? managed_profile(origin->strategy).confirmation : confirmation_profile;
    }
    auto result = Json{{"profile", profile},
                       {"schedule", plan.schedule.id().to_hex()},
                       {"objective", scenario::objective_token(plan.schedule.objective())},
                       {"root_seed", plan.schedule.seed()}, {"budget", plan.schedule.attempt_budget()},
                       {"trials", std::move(trials)}};
    if (!plan.frozen.empty() && !plan.frozen.front().controller.is_null())
        result["controller"] = plan.frozen.front().controller;
    if (plan.exploration) result["exploration"] = exploration_document(*plan.exploration);
    if (plan.confirmation) result["confirmation"] = confirmation_document(*plan.confirmation);
    if (plan.genetic_child)
        result[managed_profile(plan.genetic_child->strategy).child_field] =
            genetic_child_document(*plan.genetic_child);
    result["build_sha256"] = plan.build.to_hex();
    return result;
}

auto namespaced_digest(std::string_view value) -> bool {
    return value.starts_with("sha256:") && Digest::parse(value.substr(7)).has_value();
}

auto whole_group_scenario(const Json& authored) -> bool {
    return authored.at("mode") == "mixed" || authored.at("mode") == "pipeline";
}

auto order_seed(const Json& authored) -> const Json& {
    return authored.at("execution").at("order_seed");
}

void set_order_seed(Json& authored, std::uint64_t seed) {
    authored.at("execution")["order_seed"] = seed;
}

auto decode_confirmation(const Json& raw, const Plan& plan, std::optional<StudyStrategy> managed)
    -> std::expected<Confirmation, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("plan.confirmation", "Invalid frozen confirmation protocol")};
    };
    if (plan.frozen.size() != 1U || plan.schedule.attempt_budget() != 1U) return invalid();
    const auto& scenario = plan.frozen.front().scenario;
    if (raw.at("protocol") != package_confirmation_protocol || scenario.at("mode") != "comparison")
        return invalid();
    auto with_policy = scenario;
    exp::apply_package_confirmation_policy(with_policy);
    if (scenario != with_policy) return invalid();
    const auto discovery_plan = Digest::parse(raw.at("discovery_plan_sha256").get<std::string_view>());
    const auto discovery_trial = Digest::parse(raw.at("discovery_trial_sha256").get<std::string_view>());
    const auto discovery_build = Digest::parse(raw.at("discovery_build_sha256").get<std::string_view>());
    const auto& result = raw.at("discovery_result");
    const auto result_digest = Digest::parse(result.at("content_sha256").get<std::string_view>());
    const auto run = results::journal::RunId::parse(result.at("run_id").get<std::string_view>());
    const auto artifact = result.at("artifact").get<std::string>();
    if (!discovery_plan || !discovery_trial || !discovery_build || !result_digest || !run ||
        artifact.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.:") !=
            std::string::npos || !artifact.ends_with(std::string{run->text()} + std::string{result_suffix}) ||
        artifact.size() > 255U)
        return invalid();
    for (const auto* field : {"discovery_trial_ordinal", "candidate_ordinal", "discovery_seed", "discovery_order_seed"}) {
        if (!raw.at(field).is_number_unsigned()) return invalid();
    }
    const auto discovery_ordinal = raw.at("discovery_trial_ordinal").get<std::uint64_t>();
    const auto candidate_ordinal = raw.at("candidate_ordinal").get<std::uint64_t>();
    const auto seed = raw.at("discovery_seed").get<std::uint64_t>();
    const auto comparison_seed = raw.at("discovery_order_seed").get<std::uint64_t>();
    const auto& trial = plan.schedule.trials().front();
    if (discovery_ordinal >= maximum_trials || candidate_ordinal >= maximum_trials ||
        seed == plan.schedule.seed() || comparison_seed == trial.comparison_order_seed ||
        *discovery_trial == trial.id)
        return invalid();
    std::optional<std::uint64_t> discovery_calibrated_repetitions;
    if (scenario.at("execution").contains("calibration")) {
        const auto& repetitions = raw.at("discovery_calibrated_repetitions");
        if (!repetitions.is_number_unsigned() || repetitions == 0U ||
            repetitions.get<std::uint64_t>() > scenario.at("execution").at("repetitions").get<std::uint64_t>())
            return invalid();
        discovery_calibrated_repetitions = repetitions.get<std::uint64_t>();
    }
    const auto inputs = Digest::parse(raw.at("discovery_observed_inputs_sha256").get<std::string_view>());
    if (!inputs) return invalid();
    std::optional<GeneticConfirmation> lineage;
    if (managed) {
        const auto& source = raw.at(managed_profile(*managed).discovery_field);
        const auto study_plan = Digest::parse(source.at("study_plan_sha256").get<std::string_view>());
        const auto ledger_head = Digest::parse(source.at("generation_ledger_head_sha256").get<std::string_view>());
        const auto& generation = source.at("generation");
        if (!study_plan || !ledger_head || !generation.is_number_unsigned() ||
            generation.get<std::uint64_t>() >= maximum_trials)
            return invalid();
        lineage.emplace(GeneticConfirmation{*study_plan, generation.get<std::uint64_t>(), *ledger_head, *managed});
    }
    return Confirmation{*discovery_plan, *discovery_trial, static_cast<std::size_t>(discovery_ordinal),
        static_cast<std::size_t>(candidate_ordinal), seed, comparison_seed,
        {std::string{run->text()}, *result_digest, artifact}, *discovery_build, lineage,
        discovery_calibrated_repetitions, *inputs};
}

auto parse_strategy(std::string_view name) -> std::optional<exp::SearchStrategy> {
    for (const auto strategy : {exp::SearchStrategy::enumeration, exp::SearchStrategy::seeded_random}) {
        if (name == exp::strategy_token(strategy)) return strategy;
    }
    return {};
}

// Planning resolves an automatic placement once for the entire recipe. A
// manual override is part of the authored input and must still match exactly.
auto matches_recipe_trial(Json source, const FrozenTrial& frozen, std::uint64_t seed) -> bool {
    set_order_seed(source, seed);
    if (!frozen.controller.is_null() && !source.at("execution").contains("controller_core"))
        source.at("execution")["controller_core"] = frozen.controller.at("core");
    return source == frozen.scenario;
}

auto decode_exploration(const Json& raw, const Plan& plan) -> std::expected<Exploration, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("plan.exploration", "Invalid frozen exploration history")};
    };
    const auto strategy = parse_strategy(raw.at("strategy").get<std::string_view>());
    const auto format = raw.at("format").get<std::string_view>();
    const auto& authored = raw.at("configuration_space").get_ref<const std::string&>();
    const auto& proposals = raw.at("proposals");
    if (!strategy || (format != "yaml" && format != "json") || authored.size() > mib ||
        !proposals.is_array() || proposals.empty() || proposals.size() > maximum_trials)
        return invalid();
    const auto source_format = format == "yaml" ? scenario::ScenarioFormat::yaml : scenario::ScenarioFormat::json;
    const auto space = scenario::ConfigurationSpace::parse(
        {"frozen-exploration-space", authored, source_format}, maximum_trials);
    if (!space || space->objective() != plan.schedule.objective()) return invalid();
    const auto order = exp::candidate_order(*strategy, space->size(), plan.schedule.seed());
    if (!order || proposals.size() > order->size()) return invalid();

    Exploration history{*strategy, source_format, authored, {}};
    history.proposals.reserve(proposals.size());
    std::size_t selected{};
    for (const auto& proposal : proposals) {
        if (selected == plan.schedule.attempt_budget()) return invalid();
        const auto ordinal = order->at(history.proposals.size());
        if (proposal.at("ordinal") != ordinal) return invalid();
        const auto& rejection = proposal.at("rejection");
        if (!rejection.is_null()) {
            auto code = rejection.at("code").get<std::string>();
            auto pointer = rejection.at("pointer").get<std::string>();
            if (code.empty()) return invalid();
            history.proposals.push_back({ordinal, std::pair{std::move(code), std::move(pointer)}});
            continue;
        }
        if (selected >= plan.frozen.size()) return invalid();
        const auto materialized = space->materialize(ordinal);
        if (!materialized) return invalid();
        if (!matches_recipe_trial(*materialized, plan.frozen[selected],
                                  plan.schedule.trials()[selected].comparison_order_seed)) return invalid();
        ++selected;
        history.proposals.push_back({ordinal, {}});
    }
    if (selected != plan.frozen.size() ||
        (selected < plan.schedule.attempt_budget() && proposals.size() != order->size()))
        return invalid();
    return history;
}

auto decode_plan(std::string_view bytes) -> std::expected<Plan, Error> {
    const auto invalid = [] { return std::unexpected{problem("plan.invalid", "Invalid frozen experiment plan")}; };
    try {
        const auto raw = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 64) throw std::invalid_argument{"experiment plan nesting"};
            return true;
        });
        const auto& trials = raw.at("trials");
        const auto& budget = raw.at("budget");
        const auto& seed = raw.at("root_seed");
        const bool exploration = raw.at("profile") == exploration_profile;
        const auto& profile = raw.at("profile");
        const auto managed_confirmation = managed_strategy(profile, &ManagedStudyProfile::confirmation);
        const bool confirmation = managed_confirmation || profile == confirmation_profile;
        const auto managed_child = managed_strategy(profile, &ManagedStudyProfile::child);
        if ((!exploration && !confirmation && !managed_child && profile != plan_profile) ||
            !trials.is_array() || (!exploration && !managed_child && trials.empty()) ||
            trials.size() > maximum_trials || !budget.is_number_unsigned() ||
            budget.get<std::uint64_t>() > maximum_trials || !seed.is_number_unsigned())
            return invalid();
        const auto& objective = raw.at("objective");
        if (objective != scenario::objective_token(Objective::throughput) &&
            objective != scenario::objective_token(Objective::time_per_task))
            return invalid();
        auto build = Digest::parse(raw.at("build_sha256").get<std::string_view>());
        if (!build) return invalid();
        std::vector<exp::CandidateIdentity> identities;
        std::vector<FrozenTrial> frozen;
        const auto controller = raw.value("controller", Json(nullptr));
        if (!controller.is_null() && (!controller.is_object() || controller.size() != 2 ||
            !controller.at("core").is_string() ||
            !topology::StableCoreId::parse(controller.at("core").get<std::string_view>()) ||
            !controller.at("cpu").is_number_unsigned() || controller.at("cpu") > UINT32_MAX))
            return invalid();
        identities.reserve(trials.size());
        frozen.reserve(trials.size());
        for (const auto& trial : trials) {
            auto candidate = Digest::parse(trial.at("candidate").get<std::string_view>());
            auto task = Digest::parse(trial.at("problem").get<std::string_view>());
            const auto& authored = trial.at("scenario");
            auto source = trial.at("scenario_digest").get<std::string>();
            auto resolved = trial.at("resolved_plan_digest").get<std::string>();
            if (!candidate || !task || authored.at("schema") != scenario::package_scenario_schema ||
                (authored.at("execution").contains("controller_core") != !controller.is_null()) ||
                (!controller.is_null() && authored.at("execution").at("controller_core") != controller.at("core")) ||
                !order_seed(authored).is_number_unsigned() ||
                !namespaced_digest(source) || !namespaced_digest(resolved))
                return invalid();
            identities.push_back({*candidate, *task});
            frozen.push_back({authored, std::move(source), std::move(resolved), *build, controller});
        }
        auto schedule = exp::Schedule::create(
            objective == scenario::objective_token(Objective::throughput) ? Objective::throughput
                                                                          : Objective::time_per_task,
            seed.get<std::uint64_t>(), budget.get<std::size_t>(), identities);
        if (!schedule) return std::unexpected{schedule.error()};
        for (const auto& trial : schedule->trials()) {
            if (order_seed(frozen[trial.ordinal].scenario) != trial.comparison_order_seed) return invalid();
        }
        Plan plan{std::move(*schedule), std::move(frozen), {}, {}, *build};
        if (exploration) {
            auto history = decode_exploration(raw.at("exploration"), plan);
            if (!history) return std::unexpected{std::move(history.error())};
            plan.exploration = std::move(*history);
        }
        if (confirmation) {
            auto origin = decode_confirmation(raw.at("confirmation"), plan, managed_confirmation);
            if (!origin) return std::unexpected{std::move(origin.error())};
            plan.confirmation = std::move(*origin);
            if (plan.build != plan.confirmation->discovery_build) return invalid();
        }
        if (managed_child) {
            auto child = decode_genetic_child(raw.at(managed_profile(*managed_child).child_field), plan, *managed_child);
            if (!child) return std::unexpected{std::move(child.error())};
            plan.genetic_child = std::move(*child);
        }
        if (document(plan).dump() != bytes) return invalid();
        return plan;
    } catch (const Json::exception&) {
        return invalid();
    } catch (const std::invalid_argument&) {
        return invalid();
    }
}

auto resolve_package_trial(Json source, const scenario::ResourceResolutionContext& context,
                           const Digest& build, const FrozenTrial* expected = nullptr)
    -> std::expected<FrozenTrial, Error> {
    auto resolved = scenario::resolve_package(source, *context.packages, context);
    if (!resolved) return std::unexpected{std::move(resolved.error())};
    const auto scenario_digest = "sha256:" + resolved->scenario_digest.to_hex();
    const auto resolved_digest = "sha256:" + resolved->resolved_digest.to_hex();
    if (expected &&
        (scenario_digest != expected->scenario_digest || resolved_digest != expected->resolved_plan_digest ||
         expected->controller != resolved->evidence.at("controller")))
        return std::unexpected{problem("plan.target", "Trial does not reproduce its frozen Scenario and resolved plan")};
    return FrozenTrial{std::move(source), scenario_digest, resolved_digest, build, resolved->evidence.at("controller")};
}

auto freeze_trial(Json source, std::uint64_t order_seed_value,
                  const scenario::ResourceResolutionContext& context, const Digest& build)
    -> std::expected<FrozenTrial, Error> {
    set_order_seed(source, order_seed_value);
    return resolve_package_trial(std::move(source), context, build);
}

struct SuppliedExperimentWorkers {
    std::optional<fs::path> file;
    topology::LoadedTopology topology;
    std::size_t block_count;
};

auto load_supplied_experiment_workers(const fs::path& file, topology::cli::Environment& environment)
    -> std::expected<SuppliedExperimentWorkers, Error> {
    const auto bytes = environment.read_file(file);
    if (!bytes || bytes->size() > maximum_plan_bytes)
        return std::unexpected{problem("supplied_workers.input", "Cannot read the bounded supplied worker fixture")};
    try {
        const auto fixture = Json::parse(*bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 64) throw std::invalid_argument{"supplied worker fixture nesting"};
            return true;
        });
        const auto& blocks = fixture.at("blocks");
        if (fixture.at("schema") != "epycbench.supplied-workers/v1" || !fixture.at("topology").is_object() ||
            !blocks.is_array() || blocks.empty() || blocks.size() > maximum_trials)
            return std::unexpected{problem("supplied_workers.fixture", "Expected supplied workers with a saved topology")};
        auto topology = environment.deserialize(fixture.at("topology").dump(), topology::TopologyJsonInputPolicy::accept_equivalent);
        if (!topology) return std::unexpected{std::move(topology.error())};
        return SuppliedExperimentWorkers{file, std::move(*topology), blocks.size()};
    } catch (const Json::exception&) {
        return std::unexpected{problem("supplied_workers.fixture", "Invalid supplied worker fixture")};
    } catch (const std::invalid_argument&) {
        return std::unexpected{problem("supplied_workers.fixture", "Invalid supplied worker fixture")};
    }
}

auto admit_pending_trials(topology::cli::Environment& environment, const exp::Ledger& ledger,
                          std::span<const FrozenTrial> frozen, const std::optional<fs::path>& module_directory,
                          const SuppliedExperimentWorkers* supplied, bool one_attempt)
    -> std::expected<void, Error> {
    const auto* next = ledger.next();
    if (!next) return {};
    const auto catalog = runtime::discover_packages(module_directory);
    if (!catalog) return std::unexpected{catalog.error()};
    const auto verify = [&](const scenario::ResourceResolutionContext& context) -> std::expected<void, Error> {
        for (auto i = next->ordinal; i < frozen.size(); ++i) {
            const auto& trial = frozen[i];
            if (supplied && (!one_attempt || i == next->ordinal) &&
                trial.scenario.at("execution").at("blocks") != supplied->block_count)
                return std::unexpected{problem("supplied_workers.blocks", "Supply each trial's frozen block count")};
            auto admitted = resolve_package_trial(trial.scenario, context, trial.build, &trial);
            if (!admitted) {
                auto error = std::move(admitted.error());
                error.context["trial_ordinal"] = i;
                return std::unexpected{std::move(error)};
            }
        }
        return {};
    };
    const auto* packages_catalog = &*catalog;
    if (supplied) return verify({supplied->topology.topology, supplied->topology.stable_ids,
                                 supplied->topology.fingerprints, packages_catalog});
    const auto host = load_live_topology(environment);
    if (!host) return std::unexpected{std::move(host.error())};
    return verify({host->topology, host->stable_ids, host->fingerprints, packages_catalog});
}

struct PlanSourceFiles {
    std::string recipe, topology;
    scenario::ScenarioFormat format;
};

auto changed_after_review(std::string_view input) -> Error {
    return problem("plan.review_changed", "The " + std::string(input) +
        " changed after review. Review the plan again before freezing.");
}

auto read_plan_sources(topology::cli::Environment& environment, const fs::path& space_file,
                       const fs::path& topology_file, const ReviewedPlanInputs* reviewed)
    -> std::expected<PlanSourceFiles, Error> {
    const auto extension = space_file.extension();
    if (extension != ".json" && extension != ".yaml" && extension != ".yml")
        return std::unexpected{problem("plan.format", "Configuration space requires a JSON or YAML file")};
    const auto format = extension == ".json" ? scenario::ScenarioFormat::json : scenario::ScenarioFormat::yaml;
    auto recipe = environment.read_file(space_file);
    if (reviewed && (!recipe || format != reviewed->recipe_format || core::sha256(*recipe) != reviewed->recipe))
        return std::unexpected{changed_after_review("recipe")};
    auto topology = environment.read_file(topology_file);
    if (reviewed && (!topology || core::sha256(*topology) != reviewed->topology))
        return std::unexpected{changed_after_review("topology file")};
    if (!recipe || !topology)
        return std::unexpected{problem("plan.input", "Cannot read the configuration space or saved topology")};
    return PlanSourceFiles{std::move(*recipe), std::move(*topology), format};
}

auto read_plan_catalog(const scenario::ConfigurationSpace& space,
                       const std::optional<fs::path>& module_directory, const ReviewedPlanInputs* reviewed)
    -> std::expected<kernel::packages::Catalog, Error> {
    auto catalog = runtime::discover_packages(module_directory);
    if (!catalog) return std::unexpected{std::move(catalog.error())};
    if (reviewed && plan_package_digest(space, *catalog) != reviewed->packages)
        return std::unexpected{changed_after_review("task or implementation package")};
    return catalog;
}

auto make_plan(topology::cli::Environment& environment, const fs::path& space_file,
               const fs::path& topology_file, std::uint64_t seed, std::size_t budget,
               std::span<const std::size_t> explicit_order, std::optional<exp::SearchStrategy> strategy,
               const std::optional<fs::path>& module_directory, const ReviewedPlanInputs* reviewed)
    -> std::expected<Plan, Error> {
    if (budget == 0U || explicit_order.size() > budget || budget > maximum_trials)
        return std::unexpected{problem("plan.budget", "Candidate count must fit the finite attempt budget")};
    const auto sources = read_plan_sources(environment, space_file, topology_file, reviewed);
    if (!sources) return std::unexpected{sources.error()};
    const auto space = scenario::ConfigurationSpace::parse(
        {"configuration-space", sources->recipe, sources->format}, maximum_trials);
    if (!space) return std::unexpected{space.error()};
    const auto loaded = environment.deserialize(sources->topology, topology::TopologyJsonInputPolicy::require_canonical);
    if (!loaded) return std::unexpected{loaded.error()};
    const auto catalog = read_plan_catalog(*space, module_directory, reviewed);
    if (!catalog) return std::unexpected{catalog.error()};
    const scenario::ResourceResolutionContext context{loaded->topology, loaded->stable_ids, loaded->fingerprints,
                                                      &*catalog};

    std::optional<Exploration> exploration;
    std::vector<std::size_t> generated_order;
    auto order = explicit_order;
    if (strategy) {
        auto generated = exp::candidate_order(*strategy, space->size(), seed);
        if (!generated) return std::unexpected{std::move(generated.error())};
        generated_order = std::move(*generated);
        order = generated_order;
        exploration.emplace(*strategy, sources->format, sources->recipe, std::vector<Proposal>{});
    }
    std::vector<exp::CandidateIdentity> identities;
    std::vector<Json> authored;
    identities.reserve(std::min(budget, order.size()));
    authored.reserve(std::min(budget, order.size()));
    std::size_t authored_bytes = exploration ? exploration->authored_space.size() : 0U;
    for (const auto ordinal : order) {
        if (identities.size() == budget) break;
        auto candidate = space->resolve(ordinal, context);
        if (!candidate) return std::unexpected{candidate.error()};
        if (!candidate->accepted() || !candidate->candidate_identity || !candidate->problem_identity) {
            if (exploration) {
                const auto& diagnostics = candidate->diagnostics;
                exploration->proposals.push_back({ordinal, diagnostics.empty()
                    ? std::pair{std::string{"experiment.plan.unresolved"}, std::string{}}
                    : std::pair{diagnostics.front().code.value(), diagnostics.front().json_pointer}});
                continue;
            }
            auto error = problem("plan.unresolved", "A scheduled configuration did not resolve");
            error.context = {{"ordinal", ordinal}};
            if (!candidate->diagnostics.empty()) {
                error.context["code"] = candidate->diagnostics.front().code.value();
                error.context["pointer"] = candidate->diagnostics.front().json_pointer;
            }
            return std::unexpected{std::move(error)};
        }
        const auto bytes = candidate->authored_scenario.dump().size();
        if (bytes > maximum_plan_bytes - authored_bytes)
            return std::unexpected{problem("plan.capacity", "Frozen Scenarios exceed the experiment plan limit")};
        authored_bytes += bytes;
        identities.push_back({*candidate->candidate_identity, *candidate->problem_identity});
        authored.emplace_back(std::move(candidate->authored_scenario));
        if (exploration) exploration->proposals.push_back({ordinal, {}});
    }
    auto schedule = exp::Schedule::create(space->objective(), seed, budget, identities);
    if (!schedule) return std::unexpected{schedule.error()};
    const auto build = current_study_build();
    if (!build) return std::unexpected{build.error()};
    std::vector<FrozenTrial> frozen;
    frozen.reserve(authored.size());
    for (const auto& trial : schedule->trials()) {
        auto resolved = freeze_trial(std::move(authored[trial.ordinal]), trial.comparison_order_seed, context, *build);
        if (!resolved) return std::unexpected{std::move(resolved.error())};
        frozen.push_back(std::move(*resolved));
    }
    return Plan{std::move(*schedule), std::move(frozen), std::move(exploration), {}, *build};
}

struct Evaluation {
    const ops::Directory& directory;
    const std::vector<FrozenTrial>& frozen;
    const std::optional<fs::path>& module_directory;
    exp::LedgerStore& store;
    SignalScope* signals{};
    std::optional<fs::path> supplied_workers{};
};

auto cancelled(void* context) noexcept -> bool {
    const auto* signals = static_cast<const Evaluation*>(context)->signals;
    return signals && signals->observe() != ControllerSignal::none;
}

auto persist(void* context, const exp::Entry& entry) -> std::expected<void, Error> {
    return static_cast<Evaluation*>(context)->store.persist(entry);
}

struct GroupThroughput {
    double median;
    std::size_t workers, physical_cores;
};

struct InspectedResult {
    exp::ResultAssociation association;
    std::string execution;
    std::string validity;
    std::string conclusion;
    std::optional<Digest> build;
    std::size_t eligible_blocks;
    Json summary;
    Json scaling;
    std::optional<std::string> physical_topology;
    std::optional<std::string> effective_topology;
    bool discovery_eligible{true};
    std::optional<std::uint64_t> calibrated_repetitions{};
    std::optional<Digest> observed_inputs{};
    std::optional<GroupThroughput> whole_group{};
};

auto measured_problem(const exp::ScheduledTrial& trial, const InspectedResult& result) -> Digest {
    const auto& problem = trial.configuration.problem;
    if (!result.observed_inputs) return problem;
    return core::sha256(Json{{"profile", "epycbench.observed-problem/v1"}, {"problem", problem.to_hex()},
        {"inputs", result.observed_inputs->to_hex()}, {"selected_repetitions", result.calibrated_repetitions
            ? Json(*result.calibrated_repetitions) : Json(nullptr)}}.dump());
}

auto group_throughput(const results::PackageResult& result) -> std::optional<GroupThroughput> {
    const auto whole_group = result.summary.find("whole_group");
    if (whole_group == result.summary.end() || whole_group->at("unit") != "composite_tasks_per_second" ||
        !whole_group->at("median").is_number()) return std::nullopt;
    std::set<std::uint32_t> workers, cores;
    for (const auto& role : result.document.at("plan").at("placement")) {
        for (const auto& cpu : role.at("cpus")) workers.insert(cpu.get<std::uint32_t>());
        for (const auto& core : role.at("cores")) cores.insert(core.get<std::uint32_t>());
    }
    return GroupThroughput{whole_group->at("median").get<double>(), workers.size(), cores.size()};
}

auto matches_package_journal(const ops::Directory& journals, const results::PackageResult& result) -> bool try {
    namespace journal = results::journal;
    const auto leaf = result.run_id + std::string{journal_suffix};
    const auto& claimed = result.document.at("journal");
    const auto& blocks = result.document.at("blocks");
    const auto probes=result.document.contains("calibration") ? result.document.at("calibration").at("attempts").size() : 0U;
    const auto records=blocks.size()+probes+2U;
    if (claimed.at("file") != leaf || claimed.at("records") != records) return false;
    const auto raw = journals.read(leaf, 64U * mib);
    if (!raw) return false;
    auto run = journal::RunId::parse(result.run_id);
    if (!run) return false;
    auto reader = journal::Reader::create(std::move(*run), {16U * mib, 32U, 4096U, records});
    if (!reader) return false;
    std::string_view remaining = *raw;
    std::size_t index{};
    while (!remaining.empty()) {
        const auto frame = reader->feed(remaining);
        if (!frame || !frame->consumed) return false;
        remaining.remove_prefix(frame->consumed);
        if (!frame->record) continue;
        const auto& envelope = frame->record->envelope;
        const auto& payload = envelope.at("payload");
        if (envelope.at("producer") != "controller") return false;
        if (index == 0U) {
            Json expected{{"plan_sha256", result.resolved_plan_digest}, {"source", result.document.at("evidence_source")}};
            if (result.document.contains("controller_placement")) {
                expected["controller_placement"] = result.document.at("controller_placement");
                expected["controller_placement"].erase("cpu_after");
            }
            if (envelope.at("type") != "creation" || payload != expected) return false;
        } else if (index <= probes) {
            const auto ordinal=index-1U;
            const auto& probe=result.document.at("calibration").at("attempts").at(ordinal);
            if (envelope.at("type")!="calibration" || payload!=Json{{"ordinal",ordinal},{"attempt_json",probe.dump()}})
                return false;
        } else if (index <= probes+blocks.size()) {
            const auto ordinal = index - probes - 1U;
            if (envelope.at("type") != "blocks" ||
                payload != Json{{"ordinal", ordinal}, {"block_json", blocks.at(ordinal).dump()}})
                return false;
        } else {
            Json expected{{"retired_blocks", blocks.size()}};
            if (result.document.contains("controller_placement"))
                expected["controller_cpu_after"] = result.document.at("controller_placement").at("cpu_after");
            if (result.document.contains("precision_stop_reason"))
                expected["precision_stop_reason"] = result.document.at("precision_stop_reason");
            if (envelope.at("type") != "finalization" || payload != expected)
                return false;
        }
        ++index;
    }
    const auto summary = reader->finish();
    return summary && summary->tail == journal::Tail::clean_prefix && summary->count == records &&
           summary->last_digest && claimed.at("last_digest") == summary->last_digest->to_hex();
} catch (const Json::exception&) {
    return false;
}

auto inspect_result(const ops::Directory& directory, const FrozenTrial& frozen, std::string_view artifact,
                    const ops::Directory* held_journals = nullptr)
    -> std::expected<std::optional<InspectedResult>, Error> {
    const auto bytes = directory.read(artifact, results_document_input_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    auto result = results::validate_package_result(*bytes);
    if (!result || result->run_id + std::string{result_suffix} != artifact ||
        result->scenario_digest != frozen.scenario_digest ||
        result->resolved_plan_digest != frozen.resolved_plan_digest ||
        result->document.at("plan").value("controller", Json(nullptr)) != frozen.controller ||
        result->build != frozen.build)
        return std::nullopt;
    std::optional<ops::Directory> journals;
    if (!held_journals) {
        auto acquired = ops::Directory::acquire(directory.path() / "epycbench");
        if (!acquired) return std::nullopt;
        journals.emplace(std::move(*acquired));
        held_journals = &*journals;
    }
    if (!matches_package_journal(*held_journals, *result)) return std::nullopt;
    const bool native = result->document.at("evidence_source") == "native";
    const auto whole_group = native && result->validity == "valid" ? group_throughput(*result) : std::nullopt;
    return InspectedResult{{std::move(result->run_id), result->digest, std::string{artifact}},
        std::move(result->execution), std::move(result->validity),
        native ? std::move(result->conclusion) : "not_evaluated", result->build,
        native ? result->eligible_blocks : 0U, std::move(result->summary),
        native ? std::move(result->scaling) : Json::array(),
        std::move(result->physical_topology), std::move(result->effective_topology), native,
        result->calibrated_repetitions, result->observed_inputs_sha256, whole_group};
}

auto read_outcome(const ops::Directory& directory, const ops::Directory& journals,
                  const exp::ScheduledTrial& trial,
                  const FrozenTrial& frozen, const ops::RunExit& exit)
    -> std::expected<exp::Outcome, Error> {
    using enum exp::Disposition;
    using exp::UnavailableResult;
    const bool stopped = exit.cancelled;
    exp::Outcome outcome{trial.id, stopped ? interrupted : failed, {},
        stopped ? UnavailableResult::interrupted : exit.exit_code == 0 ? UnavailableResult::result_missing
                                                                   : UnavailableResult::run_failed,
        exit.exit_code, exit.signal};
    auto leaves = directory.list(64U);
    if (!leaves) return std::unexpected{leaves.error()};
    auto raw_leaves = journals.list(64U);
    if (!raw_leaves) return std::unexpected{raw_leaves.error()};
    for (const auto& name : *raw_leaves) {
        if (name.ends_with(journal_suffix)) {
            auto synced = journals.sync_file(name);
            if (!synced) return std::unexpected{synced.error()};
        }
    }
    const auto is_result = [](const auto& name) { return name.ends_with(result_suffix); };
    for (const auto& name : *leaves) {
        if (is_result(name)) {
            auto synced = directory.sync_file(name);
            if (!synced) return std::unexpected{synced.error()};
        }
    }
    const auto artifact = std::ranges::find_if(*leaves, is_result);
    if (artifact == leaves->end()) return outcome;
    outcome.unavailable = UnavailableResult::invalid_result;
    if (std::ranges::any_of(artifact + 1, leaves->end(), is_result))
        return outcome;

    auto inspected = inspect_result(directory, frozen, *artifact, &journals);
    if (!inspected) return std::unexpected{inspected.error()};
    if (!*inspected) return outcome;
    const auto& result = **inspected;
    const auto raw_leaf = result.association.run_id + std::string{journal_suffix};
    if (!std::ranges::binary_search(*raw_leaves, raw_leaf)) return outcome;
    outcome.result.emplace(result.association);
    outcome.unavailable.reset();
    if (result.execution == "completed") {
        outcome.disposition = result.validity == "valid" ? completed : invalid;
    } else if (result.execution == "interrupted") {
        outcome.disposition = interrupted;
    } else {
        outcome.disposition = failed;
    }
    return outcome;
}

auto evaluate(void* context, const exp::ScheduledTrial& trial) -> std::expected<exp::Outcome, Error> {
    auto& evaluation = *static_cast<Evaluation*>(context);
    if (cancelled(context)) return exp::Outcome{trial.id, exp::Disposition::interrupted, {},
                                               exp::UnavailableResult::interrupted};
    auto child_path = evaluation.directory.create_subdirectory("trial-" + std::to_string(trial.ordinal));
    if (!child_path) return std::unexpected{child_path.error()};
    auto child = ops::Directory::acquire(*child_path);
    if (!child) return std::unexpected{child.error()};
    const auto runtime = child->create_subdirectory("epycbench");
    if (!runtime) return std::unexpected{runtime.error()};
    auto journals = ops::Directory::acquire(*runtime);
    if (!journals) return std::unexpected{journals.error()};
    const auto& frozen = evaluation.frozen.at(trial.ordinal);
    auto source = frozen.scenario;
    source["output"]["directory"] = child_path->string();
    constexpr std::string_view scenario_file = "scenario.json";
    auto saved = child->write_new(scenario_file, source.dump(), mib);
    if (!saved) return std::unexpected{saved.error()};
    const auto exit = ops::run(evaluation.directory, *child_path / scenario_file, frozen.resolved_plan_digest,
                               evaluation.module_directory, *child_path, {context, cancelled}, evaluation.supplied_workers);
    if (!exit) return std::unexpected{exit.error()};
    return read_outcome(*child, *journals, trial, frozen, *exit);
}

auto verified_completed(const ops::Directory& directory, std::size_t ordinal,
                        const FrozenTrial& frozen, const exp::Outcome& outcome)
    -> std::expected<InspectedResult, Error> {
    const auto invalid = [] {
        return std::unexpected{problem("confirmation.result", "A complete and validated Result association is required")};
    };
    if (outcome.disposition != exp::Disposition::completed || !outcome.result) return invalid();
    const auto child = ops::Directory::acquire(directory.path() / ("trial-" + std::to_string(ordinal)));
    if (!child) return std::unexpected{child.error()};
    auto result = inspect_result(*child, frozen, outcome.result->artifact);
    if (!result) return std::unexpected{result.error()};
    if (!*result || (*result)->execution != "completed" || (*result)->validity != "valid" ||
        (*result)->association.run_id != outcome.result->run_id ||
        (*result)->association.content_digest != outcome.result->content_digest)
        return invalid();
    const auto journals = ops::Directory::acquire(child->path() / "epycbench");
    if (!journals) return std::unexpected{journals.error()};
    const auto names = journals->list(64U);
    if (!names) return std::unexpected{names.error()};
    if (!std::ranges::binary_search(*names, outcome.result->run_id + std::string{journal_suffix}))
        return invalid();
    return std::move(**result);
}

auto trial_outcome(const exp::Ledger& ledger, const Digest& trial) -> const exp::Outcome* {
    for (const auto& entry : ledger.entries()) {
        const auto* outcome = std::get_if<exp::Outcome>(&entry.event);
        if (outcome && outcome->trial == trial) return outcome;
    }
    return nullptr;
}

auto accepted_candidate(const Exploration& exploration, std::size_t ordinal) -> const Proposal* {
    auto accepted = exploration.proposals | std::views::filter([](const auto& proposal) {
        return !proposal.rejection;
    });
    const auto selected = std::ranges::next(accepted.begin(), static_cast<std::ptrdiff_t>(ordinal), accepted.end());
    return selected == accepted.end() ? nullptr : &*selected;
}

struct GeneticConfirmationSource {
    GeneticConfirmation lineage;
    const scenario::ConfigurationSpace& space;
    const topology::TopologyFingerprints& topology;
    std::size_t candidate_ordinal;
    std::uint64_t root_seed;
};

auto matches_topology(const InspectedResult& evidence, const topology::TopologyFingerprints& expected) -> bool {
    return evidence.physical_topology == expected.physical.to_string() &&
           evidence.effective_topology == expected.effective.to_string();
}

auto make_confirmation(topology::cli::Environment& environment, const fs::path& topology_file,
                       const std::optional<fs::path>& module_directory, const ops::Directory& directory,
                       const Digest& plan_digest, const Plan& discovery, const exp::Ledger& ledger,
                       std::size_t ordinal, std::uint64_t seed,
                       const GeneticConfirmationSource* genetic = nullptr) -> std::expected<Plan, Error> {
    if ((!discovery.exploration && !genetic) || ledger.active() || ledger.scientifically_stopped() ||
        ledger.next() || ordinal >= ledger.schedule().trials().size())
        return std::unexpected{problem("confirmation.discovery", "Confirmation requires a finished, unstopped exploration trial")};
    const auto& previous = discovery.frozen.at(ordinal);
    if (previous.scenario.at("mode") != "comparison")
        return std::unexpected{problem("confirmation.comparison", "Confirmation requires a two-role package comparison")};
    const auto& source_trial = ledger.schedule().trials()[ordinal];
    const auto* outcome = trial_outcome(ledger, source_trial.id);
    if (!outcome || outcome->disposition != exp::Disposition::completed)
        return std::unexpected{problem("confirmation.trial", "The selected exploration trial did not complete with valid evidence")};
    auto prospective=exp::prospective_package_confirmation(ledger.schedule(),ordinal,seed,
        genetic ? std::optional{genetic->root_seed}:std::nullopt,previous.scenario);
    if (!prospective) return std::unexpected{prospective.error()};

    std::size_t candidate_ordinal{};
    if (genetic) {
        candidate_ordinal = genetic->candidate_ordinal;
    } else {
        const auto* selected = accepted_candidate(*discovery.exploration, ordinal);
        if (!selected)
            return std::unexpected{problem("confirmation.candidate", "The discovery proposal is unavailable")};
        candidate_ordinal = selected->ordinal;
    }
    const auto topology_bytes = environment.read_file(topology_file);
    if (!topology_bytes)
        return std::unexpected{problem("confirmation.topology", "Cannot read the saved confirmation topology")};
    const auto loaded = environment.deserialize(*topology_bytes, topology::TopologyJsonInputPolicy::require_canonical);
    if (!loaded) return std::unexpected{loaded.error()};
    if (genetic && loaded->fingerprints != genetic->topology)
        return std::unexpected{problem("confirmation.topology", "Confirmation requires the genetic study topology")};
    std::optional<scenario::ConfigurationSpace> exploration_space;
    if (!genetic) {
        const auto& history = *discovery.exploration;
        auto parsed = scenario::ConfigurationSpace::parse(
            {"frozen-exploration-space", history.authored_space, history.format}, maximum_trials);
        if (!parsed) return std::unexpected{parsed.error()};
        exploration_space.emplace(std::move(*parsed));
    }
    const auto& space = genetic ? genetic->space : *exploration_space;
    const auto catalog = runtime::discover_packages(module_directory);
    if (!catalog) return std::unexpected{catalog.error()};
    const scenario::ResourceResolutionContext context{loaded->topology, loaded->stable_ids, loaded->fingerprints,
                                                      &*catalog};
    auto candidate = space.resolve(candidate_ordinal, context);
    if (!candidate) return std::unexpected{candidate.error()};
    if (!candidate->accepted() || candidate->candidate_identity != source_trial.configuration.candidate ||
        candidate->problem_identity != source_trial.configuration.problem)
        return std::unexpected{problem("confirmation.candidate", "The saved topology and modules do not resolve the discovery candidate")};

    const auto expected = freeze_trial(Json(candidate->authored_scenario), source_trial.comparison_order_seed,
                                       context, discovery.build);
    if (!expected) return std::unexpected{expected.error()};
    if (expected->scenario_digest != previous.scenario_digest ||
        expected->resolved_plan_digest != previous.resolved_plan_digest)
        return std::unexpected{problem("confirmation.discovery_plan", "The discovery Scenario does not resolve to its frozen digests")};
    auto evidence = verified_completed(directory, ordinal, previous, *outcome);
    if (!evidence) return std::unexpected{evidence.error()};
    if (!evidence->discovery_eligible)
        return std::unexpected{problem("confirmation.supplied_evidence", "Confirmation requires native discovery evidence")};
    if (!evidence->observed_inputs)
        return std::unexpected{problem("confirmation.inputs", "Confirmation requires a retained identity for the observed discovery inputs")};
    auto binding = admit_study_build(discovery.build);
    if (!binding) return std::unexpected{binding.error()};
    if (genetic && !matches_topology(*evidence, genetic->topology))
        return std::unexpected{problem("confirmation.topology", "The discovery Result differs from the genetic study topology")};

    auto trial = resolve_package_trial(std::move(prospective->authored), context, discovery.build);
    if (!trial) return std::unexpected{trial.error()};
    std::vector<FrozenTrial> frozen;
    frozen.push_back(std::move(*trial));
    return Plan{std::move(prospective->schedule), std::move(frozen), {},
        Confirmation{plan_digest, source_trial.id, ordinal, candidate_ordinal, ledger.schedule().seed(),
                     source_trial.comparison_order_seed, std::move(evidence->association), discovery.build,
                     genetic ? std::optional{genetic->lineage} : std::nullopt, evidence->calibrated_repetitions,
                     *evidence->observed_inputs}, discovery.build};
}

auto verify_confirmation_trial(const ops::Directory& parent, const Plan& discovery, const exp::Ledger& ledger,
                               std::size_t candidate_ordinal, const Plan& confirmation)
    -> std::expected<InspectedResult, Error> {
    const auto& origin = *confirmation.confirmation;
    const auto invalid = [] {
        return std::unexpected{problem("confirmation.origin", "Confirmation must match its completed discovery trial")};
    };
    if (origin.discovery_trial_ordinal >= discovery.frozen.size() ||
        ledger.active() || ledger.scientifically_stopped() || ledger.next())
        return invalid();
    const auto& trial = ledger.schedule().trials()[origin.discovery_trial_ordinal];
    const auto& next_trial = confirmation.schedule.trials().front();
    if (trial.id != origin.discovery_trial || ledger.schedule().seed() != origin.discovery_seed ||
        trial.comparison_order_seed != origin.discovery_order_seed ||
        trial.configuration != next_trial.configuration || ledger.schedule().objective() != confirmation.schedule.objective() ||
        candidate_ordinal != origin.candidate_ordinal ||
        std::ranges::any_of(ledger.schedule().trials(), [&](const auto& previous) {
            return next_trial.comparison_order_seed == previous.comparison_order_seed;
        }))
        return invalid();
    const auto& frozen = discovery.frozen[origin.discovery_trial_ordinal];
    auto expected = frozen.scenario;
    exp::apply_package_confirmation_policy(expected);
    set_order_seed(expected, next_trial.comparison_order_seed);
    if (frozen.controller != confirmation.frozen.front().controller || expected != confirmation.frozen.front().scenario) return invalid();
    const auto* outcome = trial_outcome(ledger, trial.id);
    if (!outcome || !outcome->result || outcome->result->run_id != origin.discovery_result.run_id ||
        outcome->result->artifact != origin.discovery_result.artifact ||
        outcome->result->content_digest != origin.discovery_result.content_digest)
        return invalid();
    auto evidence = verified_completed(parent, origin.discovery_trial_ordinal, frozen, *outcome);
    if (!evidence) return std::unexpected{evidence.error()};
    if (!evidence->discovery_eligible || evidence->build != origin.discovery_build ||
        evidence->calibrated_repetitions != origin.discovery_calibrated_repetitions ||
        evidence->observed_inputs != origin.discovery_observed_inputs) return invalid();
    return evidence;
}

auto verify_confirmation_origin(const ops::Directory& parent, const Plan& confirmation)
    -> std::expected<void, Error> {
    const auto& origin = *confirmation.confirmation;
    const auto invalid = [] {
        return std::unexpected{problem("confirmation.origin", "Confirmation must match its completed parent exploration")};
    };
    const auto bytes = parent.read(plan_file, maximum_plan_bytes);
    if (!bytes) return std::unexpected{bytes.error()};
    if (core::sha256(*bytes) != origin.discovery_plan) return invalid();
    auto discovery = decode_plan(*bytes);
    if (!discovery) return std::unexpected{discovery.error()};
    if (!discovery->exploration || origin.discovery_trial_ordinal >= discovery->frozen.size()) return invalid();
    const auto* proposal = accepted_candidate(*discovery->exploration, origin.discovery_trial_ordinal);
    if (!proposal) return invalid();
    exp::Ledger ledger{std::move(discovery->schedule)};
    const auto store = exp::LedgerStore::open(parent, origin.discovery_plan.to_hex(), ledger);
    if (!store) return std::unexpected{store.error()};
    auto verified = verify_confirmation_trial(parent, *discovery, ledger, proposal->ordinal, confirmation);
    if (!verified) return std::unexpected{verified.error()};
    return {};
}

auto confirmation_status(const ops::Directory& directory, const Plan& plan, const exp::Ledger& ledger)
    -> std::expected<Json, Error> {
    const auto& origin = *plan.confirmation;
    auto summary = confirmation_document(origin);
    const auto* outcome = trial_outcome(ledger, ledger.schedule().trials().front().id);
    summary["eligible_for_reuse"] = false;
    summary["improvement_confirmed"] = false;
    if (ledger.scientifically_stopped()) {
        summary["state"] = "scientifically_stopped";
        return summary;
    }
    if (!outcome) {
        summary["state"] = ledger.active() ? "unresolved_reservation" : "pending";
        return summary;
    }
    if (outcome->disposition != exp::Disposition::completed) {
        summary["state"] = disposition_names.at(static_cast<std::size_t>(outcome->disposition));
        return summary;
    }
    auto evidence = verified_completed(directory, 0U, plan.frozen.front(), *outcome);
    if (!evidence) return std::unexpected{evidence.error()};
    if (evidence->calibrated_repetitions)
        summary["observed_calibrated_repetitions"] = *evidence->calibrated_repetitions;
    if (evidence->observed_inputs) summary["observed_inputs_sha256"] = evidence->observed_inputs->to_hex();
    if (evidence->build != origin.discovery_build ||
        evidence->calibrated_repetitions != origin.discovery_calibrated_repetitions ||
        evidence->observed_inputs != origin.discovery_observed_inputs ||
        evidence->association.run_id == origin.discovery_result.run_id) {
        summary["state"] = "conditions_changed";
        return summary;
    }
    summary["result_content_sha256"] = evidence->association.content_digest.to_hex();
    summary["eligible_blocks"] = evidence->eligible_blocks;
    if (evidence->eligible_blocks != exp::package_confirmation_blocks || evidence->conclusion == "not_evaluated") {
        summary["state"] = "insufficient_evidence";
        return summary;
    }
    summary["state"] = evidence->conclusion;
    summary["eligible_for_reuse"] = true;
    summary["improvement_confirmed"] = evidence->conclusion == "candidate_faster";
    return summary;
}

auto status(std::ostream& output, std::string_view plan_digest, const exp::Ledger& ledger,
            const Plan& plan, const ops::Directory& directory) -> std::expected<void, Error> {
    const auto last = ledger.entries().empty() ? nullptr : std::get_if<exp::Stop>(&ledger.entries().back().event);
    const auto state = ledger.scientifically_stopped() ? "scientifically_stopped" :
                       ledger.active() ? "unresolved_reservation" :
                       !ledger.next() ? "exhausted" :
                       last && last->reason == exp::StopReason::user_interrupted ? "interrupted" : "ready";
    auto counts = Json{{"completed", 0U}, {"invalid", 0U}, {"failed", 0U}, {"interrupted", 0U}};
    for (const auto& entry : ledger.entries()) {
        if (const auto* outcome = std::get_if<exp::Outcome>(&entry.event)) {
            auto& count = counts.at(disposition_names.at(static_cast<std::size_t>(outcome->disposition)));
            count = count.get<std::size_t>() + 1U;
        }
    }
    auto summary = Json{{"plan_sha256", plan_digest}, {"schedule", ledger.schedule().id().to_hex()},
                        {"state", state}, {"attempt_budget", ledger.schedule().attempt_budget()},
                        {"scheduled", ledger.schedule().trials().size()}, {"charged", ledger.charged_attempts()},
                        {"events", ledger.entries().size()}, {"outcomes", std::move(counts)}};
    if (!plan.frozen.empty() && !plan.frozen.front().controller.is_null())
        summary["controller"] = plan.frozen.front().controller;
    if (plan.exploration) {
        summary["exploration"] = {
            {"strategy", exp::strategy_token(plan.exploration->strategy)},
            {"proposed", plan.exploration->proposals.size()},
            {"rejected", plan.exploration->proposals.size() - ledger.schedule().trials().size()}};
    }
    if (plan.confirmation) {
        auto confirmation = confirmation_status(directory, plan, ledger);
        if (!confirmation) return std::unexpected{confirmation.error()};
        summary["confirmation"] = std::move(*confirmation);
    }
    output << summary.dump(2) << '\n';
    return {};
}

constexpr std::size_t maximum_report_bytes = 16U * mib;
struct AnalysisPage {
    Json rows = Json::array();
    std::vector<exp::ComparisonPoint> points;
    std::size_t row_bytes{};
};

auto append_analysis_trial(AnalysisPage& page, const ops::Directory& directory, const FrozenTrial& frozen,
                           const exp::Ledger& ledger, const exp::ScheduledTrial& trial, std::size_t ordinal,
                           bool eligible, const fs::path& prefix, Json row)
    -> std::expected<void, Error> {
    const auto* outcome = trial_outcome(ledger, trial.id);
    row["ordinal"] = ordinal;
    row["trial_sha256"] = trial.id.to_hex();
    row["candidate_sha256"] = trial.configuration.candidate.to_hex();
    row["problem_sha256"] = trial.configuration.problem.to_hex();
    row["state"] = outcome ? disposition_names.at(static_cast<std::size_t>(outcome->disposition)) :
                   ledger.active() == &trial ? "unresolved_reservation" :
                   ledger.scientifically_stopped() ? "scientifically_stopped" : "pending";
    row["result"] = nullptr;
    if (outcome && outcome->result) {
        const auto trial_dir = directory.path() / ("trial-" + std::to_string(trial.ordinal));
        const auto child = ops::Directory::acquire(trial_dir);
        if (!child) return std::unexpected{child.error()};
        const auto& source = *outcome->result;
        auto result = inspect_result(*child, frozen, source.artifact);
        if (!result) return std::unexpected{result.error()};
        if (!*result || (*result)->association.content_digest != source.content_digest ||
            (*result)->association.run_id != source.run_id)
            return std::unexpected{problem("analysis.result", "A retained Result does not match its ledger association")};
        auto& observed = **result;
        const auto problem = measured_problem(trial, observed);
        if (observed.observed_inputs) row["measured_problem_sha256"] = problem.to_hex();
        if (eligible && outcome->disposition == exp::Disposition::completed && observed.validity == "valid" &&
            observed.discovery_eligible && observed.observed_inputs &&
            observed.build && observed.scaling.size() == 1U) {
            const auto& ratio = observed.scaling.front().at("oriented_ratio");
            if (ratio.is_number()) {
                const auto value = ratio.get<double>();
                if (std::isfinite(value) && value > 0.0)
                    page.points.push_back({ordinal, trial.configuration.candidate, problem,
                                           *observed.build, value});
            }
        }
        row["result"] = {{"run_id", source.run_id}, {"content_sha256", source.content_digest.to_hex()},
                         {"artifact", (prefix / trial_dir.filename() / source.artifact).string()},
                         {"summary", std::move(observed.summary)}, {"scaling", std::move(observed.scaling)}};
        row["result"]["package_mode"] = frozen.scenario.at("mode");
    }
    const auto size = row.dump().size();
    if (size > maximum_report_bytes - page.row_bytes)
        return std::unexpected{problem("analysis.capacity", "The analysis page is too large, reduce its limit")};
    page.row_bytes += size;
    page.rows.push_back(std::move(row));
    return {};
}

auto emit_analysis(std::ostream& output, Json analysis, AnalysisPage& page) -> std::expected<void, Error> {
    analysis["trials"] = std::move(page.rows);
    analysis["distribution_scope"] = "visible_page";
    analysis["distributions"] = exp::comparison_distributions(page.points);
    const auto bytes = analysis.dump();
    if (bytes.size() > maximum_report_bytes)
        return std::unexpected{problem("analysis.capacity", "The analysis page is too large, reduce its limit")};
    output << bytes << '\n';
    return {};
}

auto analyze(std::ostream& output, const ops::Directory& directory, const Plan& plan,
             const exp::Ledger& ledger, std::string_view plan_digest, std::size_t offset,
             std::size_t limit)
    -> std::expected<void, Error> {
    const auto trials = ledger.schedule().trials();
    if (offset > trials.size() || limit == 0U || limit > 64U)
        return std::unexpected{problem("analysis.page", "Use a valid offset and a page size from 1 through 64")};
    const auto end = offset + std::min(limit, trials.size() - offset);
    const auto objective = ledger.schedule().objective();
    auto analysis = Json{{"schema", "epycbench.experiment-analysis/v1"},
                         {"plan_sha256", plan_digest}, {"schedule", ledger.schedule().id().to_hex()},
                         {"purpose", plan.confirmation ? "confirmation" : plan.exploration ? "exploration" : "manual"},
                         {"objective", scenario::objective_token(objective)},
                         {"objective_orientation", scenario::objective_orientation(objective)},
                         {"trial_count", trials.size()}, {"offset", offset},
                         {"next_offset", end == trials.size() ? Json(nullptr) : Json(end)}};
    bool eligible = !ledger.scientifically_stopped();
    if (plan.confirmation) {
        auto confirmation = confirmation_status(directory, plan, ledger);
        if (!confirmation) return std::unexpected{confirmation.error()};
        eligible = confirmation->at("eligible_for_reuse").get<bool>();
        analysis["confirmation"] = std::move(*confirmation);
    }
    AnalysisPage page;
    for (const auto& trial : trials.subspan(offset, end - offset)) {
        auto added = append_analysis_trial(page, directory, plan.frozen.at(trial.ordinal), ledger, trial,
                                           trial.ordinal, eligible, {}, Json::object());
        if (!added) return std::unexpected{added.error()};
    }
    return emit_analysis(output, std::move(analysis), page);
}

auto initial_status(std::ostream& output, const ops::Directory& directory, Plan plan)
    -> std::expected<void, Error> {
    const auto bytes = document(plan).dump();
    const auto written = directory.write_new(plan_file, bytes, maximum_plan_bytes);
    if (!written) return std::unexpected{written.error()};
    const exp::Ledger ledger{std::move(plan.schedule)};
    return status(output, core::sha256(bytes).to_hex(), ledger, plan, directory);
}

auto tradeoff_sample(const exp::ScheduledTrial& trial, const InspectedResult& observed)
    -> std::optional<exp::TradeoffSample> {
    if (!observed.discovery_eligible || !observed.observed_inputs ||
        !observed.build || !observed.physical_topology || !observed.effective_topology) return {};
    double throughput{};
    std::optional<std::size_t> workers, cores;
    if (observed.whole_group) {
        throughput = observed.whole_group->median;
        workers = observed.whole_group->workers;
        cores = observed.whole_group->physical_cores;
    } else {
        if (observed.scaling.size() != 1U) return {};
        const auto& point = observed.scaling.front();
        const auto& metric = point.at("metric");
        const auto& median = point.at("candidate_median");
        if (!metric.is_object() || metric.at("family") != "task_throughput" ||
            metric.at("unit") != "tasks_per_second" || !median.is_number())
            return {};
        throughput = median.get<double>();
        const auto count = [&](const char* name) -> std::optional<std::size_t> {
            const auto value = point.find(name);
            if (value == point.end() || value->is_null()) return std::nullopt;
            return value->get<std::size_t>();
        };
        workers = count("candidate_workers");
        cores = count("candidate_physical_cores");
    }
    if (!std::isfinite(throughput) || throughput <= 0.0 || !std::isfinite(1.0 / throughput) || 1.0 / throughput <= 0.0)
        return {};
    return exp::TradeoffSample{trial.ordinal, trial.configuration.candidate, measured_problem(trial, observed),
        *observed.build, *observed.physical_topology, *observed.effective_topology, throughput, workers, cores};
}

using DiscoveryTradeoffs = std::map<std::string, exp::TradeoffSample, std::less<>>;

auto render_tradeoffs(std::ostream& output, Json report, const exp::TradeoffPolicy& policy,
                      std::span<const exp::TradeoffSample> samples, bool whole_group) -> std::expected<void, Error> {
    auto groups = exp::tradeoff_frontiers(samples, policy);
    if (!groups) return std::unexpected{groups.error()};
    const auto nullable = [](const auto& value) -> Json { return value ? Json(*value) : Json(nullptr); };
    report["sample_unit"] = "run";
    report["resource_scope"] = whole_group ? "whole_group_assigned_workers_and_cores" : "candidate_worker_cohort";
    report["aggregation"] = whole_group ? "median_of_run_whole_group_medians" : "median_of_run_candidate_medians";
    report["time_derivation"] = whole_group ? "reciprocal_of_each_run_whole_group_throughput_median_before_run_aggregation"
        : "reciprocal_of_each_run_candidate_throughput_median_before_run_aggregation";
    if (whole_group) report["performance_unit"] = policy.performance == Objective::throughput
        ? "composite_tasks_per_second" : "seconds_per_composite_task";
    report["interpretation"] = "descriptive_nondominated_points_not_statistical_pareto";
    report["objectives"] = {{"performance", scenario::objective_token(policy.performance)},
        {"performance_orientation", scenario::objective_orientation(policy.performance)},
        {"resource", policy.resource == exp::ResourceObjective::workers ? "workers" : "physical_cores"},
        {"resource_orientation", "minimize"}};
    report["constraints"] = {{"maximum_workers", nullable(policy.maximum_workers)},
        {"maximum_physical_cores", nullable(policy.maximum_physical_cores)},
        {"minimum_tasks_per_second", nullable(policy.minimum_tasks_per_second)},
        {"maximum_seconds_per_completed_task", nullable(policy.maximum_seconds_per_task)}, {"boundary", "inclusive"}};
    report["groups"] = std::move(*groups);

        const auto bytes = report.dump();
        if (bytes.size() > 16U * mib)
            return std::unexpected{problem("tradeoffs.capacity", "The tradeoff report exceeds its bounded output size")};
        output << bytes << '\n';

    return {};
}

auto tradeoffs(std::ostream& output, const ops::Directory& directory, const Plan& plan,
               const exp::Ledger& ledger, std::string_view plan_digest, const exp::TradeoffPolicy& policy)
    -> std::expected<void, Error> {
    if (auto checked = exp::validate_tradeoff_policy(policy); !checked) return std::unexpected{checked.error()};
    bool eligible = !ledger.scientifically_stopped();
    auto confirmation = Json(nullptr);
    if (plan.confirmation) {
        auto checked = confirmation_status(directory, plan, ledger);
        if (!checked) return std::unexpected{checked.error()};
        eligible = checked->at("eligible_for_reuse").get<bool>();
        confirmation = std::move(*checked);
    }
    auto rows = Json::array();
    std::vector<exp::TradeoffSample> samples;
    for (const auto& trial : ledger.schedule().trials()) {
        const auto* outcome = trial_outcome(ledger, trial.id);
        auto row = Json{{"ordinal", trial.ordinal}, {"trial_sha256", trial.id.to_hex()},
            {"candidate_sha256", trial.configuration.candidate.to_hex()},
            {"problem_sha256", trial.configuration.problem.to_hex()},
            {"state", outcome ? disposition_names.at(static_cast<std::size_t>(outcome->disposition)) :
                ledger.active() == &trial ? "unresolved_reservation" :
                ledger.scientifically_stopped() ? "scientifically_stopped" : "pending"},
            {"evaluation", "excluded"}, {"reason", "no_completed_valid_result"}, {"result", nullptr}};
        if (!eligible) row["reason"] = plan.confirmation ? "confirmation_ineligible" : "scientific_stop";
        if (outcome && outcome->result) {
            const auto trial_dir = directory.path() / ("trial-" + std::to_string(trial.ordinal));
            auto child = ops::Directory::acquire(trial_dir);
            if (!child) return std::unexpected{child.error()};
            const auto& source = *outcome->result;
            const bool completed = outcome->disposition == exp::Disposition::completed;
            auto result = inspect_result(*child, plan.frozen.at(trial.ordinal), source.artifact);
            if (!result) return std::unexpected{result.error()};
            if (!*result || (*result)->association.content_digest != source.content_digest ||
                (*result)->association.run_id != source.run_id)
                return std::unexpected{problem("tradeoffs.result", "A retained Result does not match its ledger association")};
            const auto& observed = **result;
            if (completed && (observed.execution != "completed" || observed.validity != "valid"))
                return std::unexpected{problem("tradeoffs.disposition", "Completed ledger outcome lacks a valid completed Result")};
            row["result"] = {{"run_id", source.run_id}, {"content_sha256", source.content_digest.to_hex()},
                {"artifact", (trial_dir.filename() / source.artifact).string()}};
            if (eligible && completed) {
                row["evaluation"] = "unavailable";
                row["reason"] = !observed.discovery_eligible ? "supplied_evidence"
                    : !observed.observed_inputs ? "observed_inputs_unavailable"
                    : "task_metric_build_or_topology_unavailable";
                if (auto sample = tradeoff_sample(trial, observed)) {
                    if (observed.observed_inputs) {
                        row["measured_problem_sha256"] = sample->problem.to_hex();
                        row["observed_inputs_sha256"] = observed.observed_inputs->to_hex();
                    }
                    if (observed.calibrated_repetitions) row["calibrated_repetitions"] = *observed.calibrated_repetitions;
                    samples.push_back(std::move(*sample));
                    row["evaluation"] = "included";
                    row["reason"] = nullptr;
                }
            }
        }
        rows.push_back(std::move(row));
    }
    auto rejected = Json::array();
    if (plan.exploration) {
        for (const auto& proposal : plan.exploration->proposals) {
            if (proposal.rejection) {
                rejected.push_back({{"candidate_ordinal", proposal.ordinal},
                    {"code", proposal.rejection->first}, {"pointer", proposal.rejection->second}});
            }
        }
    }
    auto report = Json{{"schema", "epycbench.experiment-tradeoffs/v1"},
        {"plan_sha256", plan_digest}, {"schedule", ledger.schedule().id().to_hex()},
        {"purpose", plan.confirmation ? "confirmation" : plan.exploration ? "exploration" : "manual"},
        {"scope", "entire_experiment"}, {"trial_count", rows.size()}, {"trials", std::move(rows)},
        {"rejected_proposals", std::move(rejected)}, {"confirmation", std::move(confirmation)}};
    return render_tradeoffs(output, std::move(report), policy, samples,
        !plan.frozen.empty() && whole_group_scenario(plan.frozen.front().scenario));
}

auto confirmation_directory(const ops::Directory& discovery, std::size_t discovery_trials)
    -> std::expected<ops::Directory, Error> {
    const auto leaves = discovery.list(4U * discovery_trials + 18U);
    if (!leaves) return std::unexpected{leaves.error()};
    if (!std::ranges::binary_search(*leaves, confirmation_leaf)) {
        const auto created = discovery.create_subdirectory(confirmation_leaf);
        if (!created) return std::unexpected{created.error()};
    }
    auto directory = ops::Directory::acquire(discovery.path() / confirmation_leaf);
    if (!directory) return std::unexpected{directory.error()};
    const auto files = directory->list(1U);
    if (!files || !files->empty())
        return std::unexpected{problem("confirmation.exists", "The exploration already has a frozen confirmation")};
    return directory;
}

#include "cli/genetic_execution.inc"
#include "cli/search_comparability.inc"
#include "cli/managed_tradeoffs.inc"
#include "cli/population_command.inc"
} // namespace

auto run_experiment_command(int argc, char* argv[], topology::cli::Environment& environment,
                            std::ostream& output, std::ostream& error,
                            std::optional<fs::path> module_directory, SignalScope* active_signals,
                            const ReviewedPlanInputs* reviewed) -> int {
    const auto report = [&error](const Error& failure) {
        error << "epycbench experiment: " << failure.code.value() << ' ' << failure.message;
        if (!failure.context.empty()) error << ' ' << failure.context.dump();
        error << '\n';
        return 3;
    };
    try {
        CLI::App app{"Coordinate a private experiment", "epycbench experiment"};
        app.require_subcommand(1);
        auto* plan_command = app.add_subcommand("plan", "Plan a saved sweep recipe on a saved topology with a search strategy or candidate order and an attempt budget");
        auto* confirm_command = app.add_subcommand("confirm", "Freeze a fresh independent comparison for one explored trial");
        auto* run_command = app.add_subcommand("run", "Run remaining charged-on-dispatch trials");
        bool one_attempt{};
        run_command->add_flag("--one-attempt", one_attempt,
            "Return after retiring one attempt, including recovery of an existing reservation");
        fs::path supplied_workers_file;
        const auto* supplied_workers_option = run_command->add_option("--supplied-workers", supplied_workers_file,
            "Use a saved topology and supplied worker evidence for a package study")->expected(1);
        auto* status_command = app.add_subcommand("status", "Read the frozen plan and replay the attempt ledger");
        auto* analysis_command = app.add_subcommand("analyze", "Analyze a page of retained experiment evidence");
        auto* tradeoffs_command = app.add_subcommand("tradeoffs", "Describe constrained nondominated retained candidates");
        auto* stop_command = app.add_subcommand("stop", "Record a terminal scientific stop");
        auto* freeze_population_command = app.add_subcommand("freeze-population", "Resolve and retain every finite candidate before a future study");
        auto* compare_search_command = app.add_subcommand("compare-search", "Compare declared protocols and retained evidence of two managed studies");
        auto* replay_command = app.add_subcommand("replay", "Replay retained managed-search proposals and evidence offline");
        fs::path directory_path, space_file, topology_file, first_study_path, second_study_path, population_output;
        std::vector<std::size_t> order;
        std::string strategy_name;
        std::uint64_t seed{};
        std::size_t budget{};
        exp::GeneticProposalOptions genetic_options;
        std::size_t genetic_generations{};
        std::size_t hill_batch_size{};
        exp::AnnealingCooling cooling{exp::annealing_loss_scale / 8U, exp::annealing_loss_scale / 1024U, 4U};
        std::size_t discovery_ordinal{};
        std::size_t analysis_offset{}, analysis_limit{8U};
        bool replay_population{}, bind_population{};
        std::string performance_name, resource_name{"workers"};
        exp::TradeoffPolicy tradeoff_policy;
        freeze_population_command->add_option("SPACE", space_file)->required();
        freeze_population_command->add_option("--topology", topology_file)->required();
        freeze_population_command->add_option("--output", population_output)->required();
        replay_command->add_option("STUDY", directory_path)->required();
        replay_command->add_flag("--population", replay_population, "Include frozen candidate identities, axis choices and recorded coverage");

        compare_search_command->add_option("LEFT", first_study_path)->required();
        compare_search_command->add_option("RIGHT", second_study_path)->required();
        plan_command->add_option("SPACE", space_file, "Saved sweep recipe (ConfigurationSpace JSON or YAML)")->required();
        plan_command->add_option("--topology", topology_file)->required();
        plan_command->add_option("--directory", directory_path)->required();
        plan_command->add_option("--seed", seed)->required();
        plan_command->add_option("--budget", budget, "Maximum number of Run attempts")->required();
        auto* candidates = plan_command->add_option("--candidate", order, "Candidate ordinal, in dispatch order");
        plan_command->add_option("--strategy", strategy_name, "Exploration order or adaptive proposal strategy")
            ->check(CLI::IsMember({"enumeration", "seeded-random", "genetic", "hill-climb", "annealing", "bayesian", "hybrid"}))->excludes(candidates);
        auto* population = plan_command->add_option("--population", genetic_options.population);
        auto* batch_size = plan_command->add_option("--batch-size", hill_batch_size);
        auto* generations = plan_command->add_option("--generations", genetic_generations);
        auto* mutation = plan_command->add_option("--mutation-per-mille", genetic_options.mutation_per_mille);
        plan_command->add_flag("--freeze-population", bind_population, "Freeze every ordinal and module identity for an adaptive study");
        auto* initial_temperature = plan_command->add_option("--initial-temperature", cooling.initial_temperature,
            "Annealing initial temperature in 2^-20 relative-loss units (default 131072)");
        auto* minimum_temperature = plan_command->add_option("--minimum-temperature", cooling.minimum_temperature,
            "Annealing minimum temperature in 2^-20 relative-loss units (default 1024)");
        auto* steps_per_halving = plan_command->add_option("--steps-per-halving", cooling.steps_per_halving,
            "Annealing steps per temperature halving (default 4)");
        confirm_command->add_option("EXPLORATION", directory_path, "Exploration directory or STUDY/generation-N")->required();
        confirm_command->add_option("--topology", topology_file)->required();
        confirm_command->add_option("--trial", discovery_ordinal, "Completed trial ordinal within the directory")->required();
        confirm_command->add_option("--seed", seed)->required();
        analysis_command->add_option("--offset", analysis_offset);
        analysis_command->add_option("--limit", analysis_limit);


        tradeoffs_command->add_option("--performance", performance_name)
            ->check(CLI::IsMember({"tasks_per_second", "seconds_per_completed_task"}));
        tradeoffs_command->add_option("--resource", resource_name)
            ->check(CLI::IsMember({"workers", "physical-cores"}));
        tradeoffs_command->add_option("--max-workers", tradeoff_policy.maximum_workers);
        tradeoffs_command->add_option("--max-physical-cores", tradeoff_policy.maximum_physical_cores);
        tradeoffs_command->add_option("--min-tasks-per-second", tradeoff_policy.minimum_tasks_per_second);
        tradeoffs_command->add_option("--max-seconds-per-task", tradeoff_policy.maximum_seconds_per_task);
        for (auto* command : {run_command, status_command, analysis_command, tradeoffs_command, stop_command})
            command->add_option("DIRECTORY", directory_path)->required();
        try {
            app.parse(argc - 1, argv + 1);
        } catch (const CLI::ParseError& failure) {
            return app.exit(failure, output, error);
        }

        if (*freeze_population_command) {
            const auto frozen = freeze_population(output, environment, space_file, topology_file, population_output, module_directory);
            return frozen ? 0 : report(frozen.error());
        }
        if (*compare_search_command) {
            const auto compared = compare_search_studies(output, environment, first_study_path, second_study_path);
            return compared ? 0 : report(compared.error());
        }
        if (*replay_command) {
            const auto replayed = replay_search_study(output, environment, directory_path, replay_population);
            return replayed ? 0 : report(replayed.error());
        }
        std::optional<ops::Directory> study_owner;
        std::optional<ops::Directory> discovery_owner;
        std::optional<ops::Directory> directory;
        {
            auto acquired = ops::Directory::acquire(directory_path);
            if (!acquired) return report(acquired.error());
            directory.emplace(std::move(*acquired));
        }
        if (*plan_command) {
            const bool genetic = strategy_name == "genetic";
            const bool hill = strategy_name == "hill-climb";
            const bool annealing = strategy_name == "annealing";
            const bool hybrid = strategy_name == "hybrid";
            const bool bayesian = strategy_name == "bayesian" || hybrid;
            const bool adaptive = genetic || hill || annealing || bayesian;
            const auto strategy = parse_strategy(strategy_name);
            if (order.empty() != (adaptive || strategy.has_value()))
                return report(problem("plan.selection", "Choose candidate ordinals or one exploration strategy"));
            if (genetic && (!population->count() || !generations->count() || batch_size->count()))
                return report(problem("genetic.options", "Genetic plans require --population and --generations"));
            if (hill && (!batch_size->count() || !generations->count() || population->count() || mutation->count()))
                return report(problem("hill_climb.options", "Hill-climb plans require --batch-size and --generations"));
            if ((annealing || bayesian) && (!generations->count() || population->count() || mutation->count() || batch_size->count()))
                return report(annealing ? problem("annealing.options", "Annealing requires --generations and uses one proposal per step")
                    : problem("bayesian.options", "Bayesian and hybrid search require --generations and use one proposal per step"));
            if (!annealing && (initial_temperature->count() || minimum_temperature->count() || steps_per_halving->count()))
                return report(problem("annealing.options", "Cooling options require the annealing strategy"));
            if (!adaptive && (population->count() || batch_size->count() || generations->count() || mutation->count()))
                return report(problem("plan.options", "These options require an adaptive strategy"));
            if (!adaptive && bind_population)
                return report(problem("plan.options", "Population binding requires an adaptive strategy"));
            const auto files = directory->list(1U);
            if (!files || !files->empty())
                return report(problem("plan.directory", "Planning requires an empty private directory"));
            if (adaptive) {
                if (hill) genetic_options = {hill_batch_size, 0U};
                if (annealing || bayesian) genetic_options = {1U, 0U};
                auto planned = write_genetic_study(output, environment, *directory, space_file, topology_file,
                    seed, budget, genetic_options, genetic_generations, module_directory,
                    hybrid ? StudyStrategy::hybrid : bayesian ? StudyStrategy::bayesian : annealing ? StudyStrategy::annealing :
                    hill ? StudyStrategy::hill_climb : StudyStrategy::genetic,
                    annealing ? std::optional{cooling} : std::nullopt, bind_population, reviewed);
                if (!planned) return report(planned.error());
                return output ? 0 : 3;
            }
            auto plan = make_plan(environment, space_file, topology_file, seed, budget, order, strategy,
                                  module_directory, reviewed);
            if (!plan) return report(plan.error());
            const auto saved = initial_status(output, *directory, std::move(*plan));
            if (!saved) return report(saved.error());
            return output ? 0 : 3;
        }

        const auto bytes = directory->read(plan_file, maximum_plan_bytes);
        if (!bytes) return report(bytes.error());
        std::optional<SuppliedExperimentWorkers> supplied_workers;
        if (supplied_workers_option->count()) {
            auto supplied = load_supplied_experiment_workers(supplied_workers_file, environment);
            if (!supplied) return report(supplied.error());
            supplied_workers.emplace(std::move(*supplied));
        }
        const auto* supplied = supplied_workers ? &*supplied_workers : nullptr;
        if (*tradeoffs_command)
            tradeoff_policy.resource = resource_name == "workers" ? exp::ResourceObjective::workers :
                                                                   exp::ResourceObjective::physical_cores;
        if (is_genetic_study_plan(*bytes)) {
            if (*analysis_command) {
                const auto reported = managed_analysis(output, *directory, environment, analysis_offset, analysis_limit);
                return reported ? 0 : report(reported.error());
            }
            if (*tradeoffs_command) {
                const auto result = managed_tradeoffs(output, *directory, environment, tradeoff_policy, performance_name);
                return result ? 0 : report(result.error());
            }
            if (*confirm_command)
                return report(problem("confirmation.genetic_location", "Select a completed STUDY/generation-N directory"));
            if (!*run_command && !*status_command && !*stop_command)
                return report(problem("genetic.command", "Use run, status, or stop for the genetic study directory"));
            const auto action = *run_command ? GeneticAction::run :
                                *stop_command ? GeneticAction::stop : GeneticAction::status;
            const auto result = run_genetic_study(action, output, environment, *directory, module_directory,
                                                  *bytes, one_attempt, supplied, active_signals);
            return result ? *result : report(result.error());
        }
        auto plan = decode_plan(*bytes);
        if (!plan) return report(plan.error());
        if (supplied && plan->confirmation)
            return report(problem("supplied_workers.confirmation", "Confirmation requires native workers"));
        if (plan->genetic_child) {
            if (!*confirm_command)
                return report(problem("genetic.child", "Use the owning genetic study directory for a managed generation"));
            const auto path = directory->path();
            // The leaf-only probe has ended before taking any ancestor locks.
            directory.reset();
            auto study = ops::Directory::acquire(path.parent_path());
            if (!study) return report(study.error());
            auto saved = make_genetic_confirmation(output, environment, topology_file, module_directory,
                *study, path, *plan->genetic_child, core::sha256(*bytes), discovery_ordinal, seed);
            if (!saved) return report(saved.error());
            return output ? 0 : 3;
        }
        if (plan->confirmation) {
            const auto child_path = directory->path();
            if (child_path.filename() != confirmation_leaf)
                return report(problem("confirmation.location", "Keep the confirmation in its parent exploration"));
            const auto parent_path = child_path.parent_path();
            // Reacquire in parent-to-child order and verify the same child plan.
            directory.reset();
            std::optional<GeneticConfirmationDiscovery> genetic;
            if (const auto& lineage = plan->confirmation->genetic) {
                if (parent_path.filename() != genetic_child_leaf(lineage->generation))
                    return report(problem("confirmation.location", "Keep the confirmation in its genetic generation"));
                auto study = ops::Directory::acquire(parent_path.parent_path());
                if (!study) return report(study.error());
                study_owner.emplace(std::move(*study));
                auto source = finished_genetic_generation(environment, *study_owner, lineage->study_plan, lineage->generation);
                if (!source) return report(source.error());
                genetic.emplace(std::move(*source));
            }
            auto parent = ops::Directory::acquire(parent_path);
            if (!parent) return report(parent.error());
            discovery_owner.emplace(std::move(*parent));
            auto child = ops::Directory::acquire(child_path);
            if (!child) return report(child.error());
            directory.emplace(std::move(*child));
            const auto unchanged = directory->read(plan_file, maximum_plan_bytes);
            if (!unchanged || *unchanged != *bytes)
                return report(problem("confirmation.plan_changed", "The confirmation plan changed during admission"));
            auto origin = genetic ? verify_genetic_confirmation_origin(*discovery_owner, *genetic, *plan)
                                  : verify_confirmation_origin(*discovery_owner, *plan);
            if (!origin) return report(origin.error());
        }
        const auto raw_digest = core::sha256(*bytes);
        const auto digest = raw_digest.to_hex();
        exp::Ledger ledger{std::move(plan->schedule)};
        auto store = exp::LedgerStore::open(*directory, digest, ledger);
        if (!store) return report(store.error());
        if (*confirm_command) {
            auto confirmation = make_confirmation(environment, topology_file, module_directory, *directory,
                                                     raw_digest, *plan, ledger, discovery_ordinal, seed);
            if (!confirmation) return report(confirmation.error());
            auto child = confirmation_directory(*directory, ledger.schedule().trials().size());
            if (!child) return report(child.error());
            const auto saved = initial_status(output, *child, std::move(*confirmation));
            if (!saved) return report(saved.error());
            return output ? 0 : 3;
        }
        if (*status_command) {
            const auto reported = status(output, digest, ledger, *plan, *directory);
            if (!reported) return report(reported.error());
            return output ? 0 : 3;
        }
        if (*analysis_command) {
            const auto reported = analyze(output, *directory, *plan, ledger, digest,
                                          analysis_offset, analysis_limit);
            if (!reported) return report(reported.error());
            return output ? 0 : 3;
        }
        if (*tradeoffs_command) {
            tradeoff_policy.performance = performance_name.empty() ? ledger.schedule().objective() :
                performance_name == "tasks_per_second" ? Objective::throughput : Objective::time_per_task;
            const auto reported = tradeoffs(output, *directory, *plan, ledger, digest, tradeoff_policy);
            if (!reported) return report(reported.error());
            return output ? 0 : 3;
        }

        Evaluation evaluation{*directory, plan->frozen, module_directory, *store, nullptr,
                              supplied ? supplied->file : std::nullopt};
        const exp::Operations operations{&evaluation, persist, evaluate, cancelled};
        ExperimentSignals signals(active_signals);
        if (*run_command) {
            if (!signals.acquire()) return report(problem("signals", "Cannot acquire coordinator signal ownership"));
            evaluation.signals = &signals.scope();
        }
        const bool recovered_active = ledger.active() != nullptr;
        if (recovered_active || (*run_command && ledger.next())) {
            auto admitted = admit_study_build(plan->build);
            if (!admitted) return report(admitted.error());
        }
        if (recovered_active) {
            // Directory admission has confirmed that the prior controller and worker lease retired.
            auto recovered = exp::recover_retired_trial(ledger, operations);
            if (!recovered) return report(recovered.error());
        }
        if (*run_command && (!one_attempt || !recovered_active)) {
            auto admitted = admit_pending_trials(environment, ledger, plan->frozen, module_directory,
                                                  supplied, one_attempt);
            if (!admitted) return report(admitted.error());
        }
        if (*stop_command || !one_attempt || !recovered_active) {
            const auto extent = one_attempt ? exp::RunExtent::one_attempt : exp::RunExtent::remaining;
            auto completed = *stop_command ? exp::record_scientific_stop(ledger, operations) : exp::run(ledger, operations, extent);
            if (!completed) return report(completed.error());
        }
        const auto signal = *run_command ? signals.finish() : std::optional{ControllerSignal::none};
        if (!signal) return report(problem("signals", "Cannot restore coordinator signals"));
        const auto reported = status(output, digest, ledger, *plan, *directory);
        if (!reported) return report(reported.error());
        if (*signal != ControllerSignal::none) return *signal == ControllerSignal::interrupt ? 130 : 143;
        return output ? 0 : 3;
    } catch (const std::exception&) {
        return report(problem("unexpected", "Experiment command could not continue"));
    }
}

} // namespace epycbench::cli
