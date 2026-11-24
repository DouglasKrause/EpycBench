#include "cli/tui.hpp"

#include "cli/experiment_command.hpp"
#include "epycbench/experiment/coordinator.hpp"
#include "epycbench/experiment/genetic.hpp"
#include "epycbench/experiment/local_search.hpp"
#include "epycbench/experiment/search.hpp"
#include "experiment/annealing.hpp"
#include "experiment/bayesian_search.hpp"
#include "platform_linux/presentation_files.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace epycbench::cli::tui {
namespace {
namespace exp = experiment;
using scenario::ConfigurationSpace;

constexpr auto max_attempts = exp::maximum_attempts;
constexpr std::size_t preview_size = 16U;
constexpr std::string_view order_payload_prefix = "study-order/";

enum class Method { enumeration, random, explicit_order, genetic, hill, annealing, bayesian, hybrid };

struct Strategy {
    Method method;
    const char* label;
    const char* token;
};

constexpr std::array strategies{
    Strategy{Method::enumeration, "Enumeration", "enumeration"},
    Strategy{Method::random, "Seeded random", "seeded-random"},
    Strategy{Method::explicit_order, "Explicit candidate order", "explicit"},
    Strategy{Method::genetic, "Genetic", "genetic"},
    Strategy{Method::hill, "Hill climb", "hill-climb"},
    Strategy{Method::annealing, "Simulated annealing", "annealing"},
    Strategy{Method::bayesian, "Bayesian", "bayesian"},
    Strategy{Method::hybrid, "Hybrid", "hybrid"},
};

const Strategy& get_strategy(const Session& s) {
    const auto token = s.draft.value("strategy", std::string{"enumeration"});
    const auto found = std::ranges::find_if(strategies, [&](const auto& item) { return item.token == token; });
    return found == strategies.end() ? strategies.front() : *found;
}

bool adaptive(Method method) {
    return method != Method::enumeration && method != Method::random && method != Method::explicit_order;
}

std::string describe(const core::Error& error) {
    return error.code.value() + " · " + error.message + " · " + error.context.dump();
}

void plan(Session& s, bool revalidate = false);

struct PlanSource {
    ConfigurationSpace space;
    core::Sha256Digest digest;
    scenario::ScenarioFormat format;
};

std::expected<PlanSource, std::string> read_space(Session& s) {
    const auto extension = s.artifact.extension();
    if (extension != ".json" && extension != ".yaml" && extension != ".yml")
        return std::unexpected{"Use a saved .json, .yaml or .yml sweep recipe."};
    auto bytes = s.environment.read_file(s.artifact);
    if (!bytes) return std::unexpected{bytes.error().classification};
    const auto format = extension == ".json" ? scenario::ScenarioFormat::json : scenario::ScenarioFormat::yaml;
    auto space = ConfigurationSpace::parse({"workbench-study", *bytes, format}, max_attempts);
    if (!space) return std::unexpected{describe(space.error())};
    return PlanSource{std::move(*space), core::sha256(*bytes), format};
}

struct Field {
    const char* key;
    const char* label;
    std::uint64_t minimum;
    std::uint64_t maximum;
};

constexpr Field seed_field{"seed", "Root seed", 0U, std::numeric_limits<std::uint64_t>::max()};
constexpr Field budget_field{"budget", "Attempt budget", 1U, max_attempts};
constexpr Field generations_field{"generations", "Maximum generations", 1U, max_attempts};
constexpr Field population_field{"population", "Population size", 1U, max_attempts};
constexpr Field batch_field{"batch_size", "Batch size", 1U, max_attempts};
constexpr Field mutation_field{"mutation", "Mutation per mille", 0U, 1000U};
constexpr Field initial_field{"initial_temperature", "Initial temperature", 1U, exp::annealing_loss_scale};
constexpr Field minimum_field{"minimum_temperature", "Minimum temperature", 1U, exp::annealing_loss_scale};
constexpr Field steps_field{"steps_per_halving", "Steps per temperature halving", 1U, max_attempts};

std::optional<std::uint64_t> unsigned_number(std::string_view value) {
    std::uint64_t number{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
    return number;
}

std::optional<std::uint64_t> number(const Session& s, Field field) {
    const auto value = unsigned_number(s.draft.at(field.key).get<std::string_view>());
    if (!value || *value < field.minimum || *value > field.maximum) return {};
    return value;
}

std::string range(Field field) {
    return std::to_string(field.minimum) + "–" + std::to_string(field.maximum);
}

void field(Session& s, Field spec, Action refresh, const std::string& panel) {
    const auto raw = s.draft.at(spec.key).get<std::string>();
    const auto detail = number(s, spec) ? raw : raw + " · Enter an unsigned integer in " + range(spec);
    s.field_action(spec.label, [&s, spec, refresh] {
        s.input(std::string(spec.label) + " · " + range(spec), s.draft.at(spec.key).get<std::string>(),
            [&s, spec, refresh](std::string input) {
                s.draft[spec.key] = std::move(input);
                refresh();
                s.persist();
            }, refresh);
    }, detail, panel);
}

std::string describe_choice(const scenario::ConfigurationCandidate& candidate,
                            const nlohmann::ordered_json& axes) {
    std::string summary;
    constexpr std::size_t limit = 240U;
    for (const auto& axis : axes) {
        const auto path = axis.at("scenario_paths").at(0).get<std::string>();
        const auto pointer = nlohmann::ordered_json::json_pointer(path);
        if (!candidate.authored_scenario.contains(pointer)) continue;
        if (!summary.empty()) summary += " · ";
        summary += path + " " + candidate.authored_scenario.at(pointer).dump();
        if (summary.size() > limit) {
            summary.resize(limit);
            summary += " …";
            break;
        }
    }
    return summary.empty() && candidate.candidate_identity ? candidate.candidate_identity->to_hex() : summary;
}

void edit_order(Session& s);

void order_picker(Session& s, std::size_t offset) {
    auto source = read_space(s);
    if (!source) {
        edit_order(s);
        s.message = source.error();
        return;
    }
    const auto count = source->space.size();
    const auto axes = source->space.describe_axes();
    s.page("Add candidate to dispatch order", [&s] { edit_order(s); });
    if (offset > 0U) s.action("Previous page", [&s, offset] { order_picker(s, offset - preview_size); });
    if (offset + preview_size < count)
        s.action("Next page", [&s, offset] { order_picker(s, offset + preview_size); });
    for (auto ordinal = offset; ordinal < std::min(count, offset + preview_size); ++ordinal) {
        auto candidate = source->space.resolve(ordinal, s.context());
        const auto label = "Candidate " + std::to_string(ordinal);
        if (!candidate || !candidate->accepted()) {
            auto issue = candidate ? "The candidate does not resolve on this topology." : describe(candidate.error());
            s.note(label + " · Rejected", std::move(issue), "Candidates");
            continue;
        }
        s.action(label, [&s, ordinal] {
            auto& order = s.draft.at("order");
            if (order.size() == max_attempts) {
                s.message = "The dispatch order has reached the attempt limit.";
                return;
            }
            order.push_back(ordinal);
            edit_order(s);
            s.persist();
        }, describe_choice(*candidate, axes), "Candidates");
    }
}

void move_candidate(Session& s, std::size_t from, std::size_t to) {
    auto& order = s.draft.at("order");
    if (from >= order.size() || to >= order.size() || from == to) return;
    const auto value = order.at(from);
    order.erase(order.begin() + static_cast<Json::difference_type>(from));
    order.insert(order.begin() + static_cast<Json::difference_type>(to), value);
    edit_order(s);
    s.persist();
}

void edit_dispatch_position(Session& s, std::size_t i) {
    const auto limit = s.draft.at("order").size();
    s.input("Dispatch position · 1–" + std::to_string(limit), std::to_string(i + 1U),
        [&s, i, limit](std::string position) {
            const auto target = unsigned_number(position);
            if (target && *target > 0U && *target <= limit && *target - 1U != i)
                move_candidate(s, i, static_cast<std::size_t>(*target - 1U));
            else {
                edit_order(s);
                if (!target || *target == 0U || *target > limit)
                    s.message = "Choose a position in the dispatch order.";
            }
        }, [&s] { edit_order(s); }, {{"view", "dispatch-position"}, {"index", i}});
}

void edit_order(Session& s) {
    s.page("Explicit candidate order", [&s] { plan(s); });
    s.action("Add candidate", [&s] { order_picker(s, 0U); });
    const auto& order = s.draft.at("order");
    if (order.empty()) {
        s.note("Add at least one candidate to schedule.");
        return;
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto& row = s.action(std::to_string(i + 1U) + " · Candidate " + order.at(i).dump(), [&s, i] {
            s.page("Change dispatch position", [&s] { edit_order(s); });
            if (i > 0U) s.action("Move earlier", [&s, i] { move_candidate(s, i, i - 1U); });
            if (i + 1U < s.draft.at("order").size())
                s.action("Move later", [&s, i] { move_candidate(s, i, i + 1U); });
            s.action("Move to position", [&s, i] {
                edit_dispatch_position(s, i);
            });
            s.action("Remove from order", [&s, i] {
                auto& pending_order = s.draft.at("order");
                pending_order.erase(pending_order.begin() + static_cast<Json::difference_type>(i));
                edit_order(s);
                s.persist();
            });
        }, "Activate to move or remove. Drag or pick up to reorder.", "Dispatch order");
        row.payload = std::string(order_payload_prefix) + std::to_string(i);
        row.accepts = [i](const std::string& payload) {
            if (!payload.starts_with(order_payload_prefix)) return false;
            const auto index = unsigned_number(std::string_view(payload).substr(order_payload_prefix.size()));
            return index && *index != i;
        };
        row.drop = [&s, i](const std::string& payload) {
            if (!payload.starts_with(order_payload_prefix)) return;
            auto index = unsigned_number(std::string_view(payload).substr(order_payload_prefix.size()));
            if (index && *index < s.draft.at("order").size())
                move_candidate(s, static_cast<std::size_t>(*index), i);
        };
    }
}

struct Options {
    std::uint64_t seed{};
    std::size_t budget{}, generations{}, batch{1U};
    std::uint16_t mutation{};
    exp::AnnealingCooling cooling{};
    bool bind_population{};
    Path directory;
};

std::expected<Path, std::string> study_directory(const Session& s) {
    const auto& text = s.draft.at("directory").get_ref<const std::string&>();
    const Path path{text};
    if (path.empty() || !path.has_filename() || path.filename() == "." || path.filename() == ".." ||
        text.contains('\0'))
        return std::unexpected{"Choose a private study directory or a new path in an existing parent."};
    std::error_code error;
    auto directory = std::filesystem::absolute(path, error).lexically_normal();
    if (error || directory == directory.root_path())
        return std::unexpected{"Choose a valid study directory path."};
    return directory;
}

std::expected<Options, std::string> options(const Session& s, Method method) {
    const auto seed = number(s, seed_field);
    const auto budget = number(s, budget_field);
    if (!seed) return std::unexpected{"Root seed must be an unsigned 64-bit integer."};
    if (!budget) return std::unexpected{"Attempt budget must be in " + range(budget_field) + "."};
    Options selected;
    selected.seed = *seed;
    selected.budget = static_cast<std::size_t>(*budget);
    if (!adaptive(method)) return selected;
    const auto generations = number(s, generations_field);
    if (!generations) return std::unexpected{"Maximum generations must be in " + range(generations_field) + "."};
    selected.generations = static_cast<std::size_t>(*generations);
    selected.bind_population = s.draft.at("bind_population").get<bool>();
    if (method == Method::genetic || method == Method::hill) {
        const auto spec = method == Method::genetic ? population_field : batch_field;
        const auto batch = number(s, spec);
        if (!batch) return std::unexpected{std::string(spec.label) + " must be in " + range(spec) + "."};
        selected.batch = static_cast<std::size_t>(*batch);
    }
    if (method == Method::genetic) {
        const auto mutation = number(s, mutation_field);
        if (!mutation) return std::unexpected{"Mutation per mille must be in " + range(mutation_field) + "."};
        selected.mutation = static_cast<std::uint16_t>(*mutation);
    }
    if (method == Method::annealing) {
        const auto initial = number(s, initial_field);
        const auto minimum = number(s, minimum_field);
        const auto steps = number(s, steps_field);
        if (!initial || !minimum || *minimum > *initial || !steps)
            return std::unexpected{"Choose positive temperatures with minimum ≤ initial, and a valid halving interval."};
        selected.cooling = {static_cast<std::uint32_t>(*initial), static_cast<std::uint32_t>(*minimum),
                            static_cast<std::uint32_t>(*steps)};
    }
    return selected;
}

std::expected<std::vector<std::size_t>, std::string> adaptive_order(
    const ConfigurationSpace& space, Method method, const Options& selected) {
    if (space.varies_task_parameters())
        return std::unexpected{"Adaptive studies require fixed task parameters."};
    auto genes = exp::GeneticSpace::create(space.axis_cardinalities());
    if (!genes) return std::unexpected{describe(genes.error())};
    const auto limit = std::min(selected.batch, selected.budget);
    std::vector<std::size_t> order;
    if (method == Method::genetic) {
        auto batch = exp::propose_genetic_generation(*genes, selected.seed, 0U, {limit, selected.mutation});
        if (!batch) return std::unexpected{describe(batch.error())};
        for (const auto& proposal : batch->proposals) order.push_back(proposal.ordinal);
    } else if (method == Method::hill) {
        auto batch = exp::propose_hill_climb_batch(*genes, selected.seed, 0U, limit);
        if (!batch) return std::unexpected{describe(batch.error())};
        for (const auto& proposal : batch->proposals) order.push_back(proposal.ordinal);
    } else if (method == Method::annealing) {
        auto step = exp::propose_annealing_step(*genes, selected.seed, 0U, selected.cooling);
        if (!step) return std::unexpected{describe(step.error())};
        if (step->proposal) order.push_back(step->proposal->ordinal);
    } else {
        const auto policy = method == Method::hybrid
            ? exp::BayesianProposalPolicy::hybrid : exp::BayesianProposalPolicy::categorical;
        auto step = exp::propose_bayesian_step(*genes, selected.seed, 0U, {}, {}, policy);
        if (!step) return std::unexpected{describe(step.error())};
        if (step->proposal) order.push_back(step->proposal->ordinal);
    }
    if (order.empty()) return std::unexpected{"The initial generation has no proposals."};
    return order;
}

std::expected<std::vector<std::size_t>, std::string> candidate_order(
    Session& s, const ConfigurationSpace& space, Method method, const Options& selected) {
    if (adaptive(method)) return adaptive_order(space, method, selected);
    if (method == Method::explicit_order) {
        auto order = s.draft.at("order").get<std::vector<std::size_t>>();
        if (order.empty() || order.size() > selected.budget)
            return std::unexpected{"Choose at least one candidate and fit the order within the attempt budget."};
        if (std::ranges::any_of(order, [&](auto ordinal) { return ordinal >= space.size(); }))
            return std::unexpected{"The dispatch order includes an ordinal outside this recipe."};
        return order;
    }
    auto order = exp::candidate_order(method == Method::random ? exp::SearchStrategy::seeded_random
        : exp::SearchStrategy::enumeration, space.size(), selected.seed);
    if (!order) return std::unexpected{describe(order.error())};
    return std::move(*order);
}

struct Proposal {
    std::size_t ordinal;
    bool accepted;
    std::string detail;
};

struct Review {
    std::vector<Proposal> proposed;
    std::size_t scheduled{};
};

std::expected<Review, std::string> inspect_order(Session& s, const ConfigurationSpace& space,
    Method method, const Options& selected, const std::vector<std::size_t>& order) {
    Review review;
    const auto axes = space.describe_axes();
    for (const auto ordinal : order) {
        if (review.scheduled == selected.budget) break;
        auto candidate = space.resolve(ordinal, s.context());
        if (candidate && candidate->accepted()) {
            review.proposed.push_back({ordinal, true, describe_choice(*candidate, axes)});
            ++review.scheduled;
            continue;
        }
        std::string problem = "Candidate did not resolve.";
        if (!candidate) {
            problem = describe(candidate.error());
        } else if (!candidate->diagnostics.empty()) {
            const auto& diagnostic = candidate->diagnostics.front();
            problem = diagnostic.code.value() + " · " + diagnostic.json_pointer + " · " +
                Json(diagnostic.safe_context).dump();
        }
        if (method == Method::explicit_order || (!adaptive(method) && !candidate))
            return std::unexpected{"Candidate " + std::to_string(ordinal) + " · " + problem};
        review.proposed.push_back({ordinal, false, std::move(problem)});
    }
    return review;
}

std::string revalidate_target(Session& s, const ConfigurationSpace& space, Method method) {
    const auto controller = space.resolve_controller(s.context());
    if (!controller) return describe(controller.error());
    if (method == Method::explicit_order && s.draft.at("order").empty())
        return "Controller placement resolved. Add candidates to check worker and memory placements.";
    const auto selected = options(s, method);
    if (!selected) return selected.error();
    const auto order = candidate_order(s, space, method, *selected);
    if (!order) return order.error();
    const auto review = inspect_order(s, space, method, *selected, *order);
    if (!review) return review.error();
    const auto rejected = std::ranges::find_if(review->proposed, [](const auto& proposal) {
        return !proposal.accepted;
    });
    if (rejected != review->proposed.end())
        return "Candidate " + std::to_string(rejected->ordinal) + " · " + rejected->detail;
    return "Target placements resolved for " + std::to_string(review->scheduled) +
        (review->scheduled == 1U ? " candidate." : " candidates.");
}

std::vector<std::string> freeze_args(const Session& s, const Strategy& strategy, const Options& selected,
                                     const std::vector<std::size_t>& order, const Path& directory) {
    std::vector<std::string> words{"experiment", "plan", "--topology", s.topology_path.string(),
        "--directory", directory.string(), "--seed", std::to_string(selected.seed),
        "--budget", std::to_string(selected.budget)};
    if (strategy.method == Method::explicit_order) {
        for (const auto ordinal : order) words.push_back("--candidate=" + std::to_string(ordinal));
    } else {
        words.push_back("--strategy");
        words.emplace_back(strategy.token);
    }
    if (adaptive(strategy.method)) {
        words.push_back("--generations=" + std::to_string(selected.generations));
        if (selected.bind_population) words.push_back("--freeze-population");
    }
    if (strategy.method == Method::genetic) {
        words.push_back("--population=" + std::to_string(selected.batch));
        words.push_back("--mutation-per-mille=" + std::to_string(selected.mutation));
    } else if (strategy.method == Method::hill) {
        words.push_back("--batch-size=" + std::to_string(selected.batch));
    } else if (strategy.method == Method::annealing) {
        words.push_back("--initial-temperature=" + std::to_string(selected.cooling.initial_temperature));
        words.push_back("--minimum-temperature=" + std::to_string(selected.cooling.minimum_temperature));
        words.push_back("--steps-per-halving=" + std::to_string(selected.cooling.steps_per_halving));
    }
    words.push_back(std::filesystem::absolute(s.artifact).string());
    return words;
}

void frozen(Session& s, const Path& directory, const std::string& output) {
    s.artifact = directory;
    s.clear_draft();
    s.page("Frozen experiment", [&s] { s.home(); });
    s.note(directory.string(), "Frozen plan and attempt ledger");
    auto status = Json::parse(output, nullptr, false);
    if (status.is_object()) {
        const auto scheduled = status.contains("pending_generation") && status.at("pending_generation").is_object()
            ? status.at("pending_generation").value("scheduled", Json()) : status.value("scheduled", Json());
        s.note("Attempt budget " + status.value("attempt_budget", Json()).dump(),
            "Charged " + status.value("charged", Json()).dump() +
            (scheduled.is_null() ? "" : " · Scheduled " + scheduled.dump()));
        s.note("Plan identity", status.value("plan_sha256", Json()).dump());
    }
    s.action("Open execution controls", [&s, directory] { s.execution(directory); },
        "Review the frozen study before starting attempts.");
}

void freeze(Session& s, const Strategy& strategy, const Options& selected,
            const std::vector<std::size_t>& order, const ReviewedPlanInputs& reviewed) {
    auto private_dir = platform_linux::presentation_files::ensure_private_subdirectory(
        selected.directory.parent_path(), selected.directory.filename().string());
    if (!private_dir) {
        const auto error = std::error_code(private_dir.error().error_number, std::generic_category());
        s.message = "Cannot open or create the study directory · " + error.message();
        return;
    }
    const auto result = s.command(freeze_args(s, strategy, selected, order, *private_dir), &reviewed);
    if (result.code != 0) {
        plan(s);
        s.message = result.error.empty() ? "The experiment coordinator could not freeze the plan." : result.error;
        return;
    }
    frozen(s, *private_dir, result.output);
}

void placement_preview(Session& s, const ConfigurationSpace& space, const Review& review) {
    auto controller = space.resolve_controller(s.context());
    if (controller) {
        s.note("Fixed controller CPU " + std::to_string(controller->cpu.value()),
            std::string(controller->core.value()), "Placement");
    }
    const auto first = std::ranges::find_if(review.proposed, &Proposal::accepted);
    if (first == review.proposed.end()) return;
    auto candidate = space.materialize(first->ordinal);
    if (!candidate) return;
    bool varies = false;
    for (const auto& definition : space.describe_axes()) {
        if (definition.at("choices_json").size() < 2U) continue;
        const auto path = definition.at("scenario_paths").at(0).get<std::string>();
        if (path == "/case_id" || path.ends_with("/cpus") || path.ends_with("/service_cpu") ||
            path.ends_with("/numa_node") || path.ends_with("/numa_nodes") || path.ends_with("/input_placement")) {
            varies = true;
            s.note("Varies by candidate", path, "Placement");
        }
    }
    if (varies) s.note("Placement for candidate " + std::to_string(first->ordinal), {}, "Placement");
    for (const auto& role : candidate->at("roles")) {
        auto detail = "CPUs " + role.at("cpus").dump();
        if (role.contains("numa_node")) detail += " · NUMA " + role.at("numa_node").dump();
        if (role.contains("numa_nodes")) detail += " · NUMA " + role.at("numa_nodes").dump();
        if (role.contains("service_cpu")) detail += " · service CPU " + role.at("service_cpu").dump();
        if (role.contains("input_placement")) detail += " · " + role.at("input_placement").dump();
        s.note(role.at("id").get<std::string>(), std::move(detail), "Placement");
    }
}

void review_plan(Session& s) {
    const auto& strategy = get_strategy(s);
    auto selected = options(s, strategy.method);
    if (!selected) {
        s.message = selected.error();
        return;
    }
    auto directory = study_directory(s);
    if (!directory) {
        s.message = directory.error();
        return;
    }
    selected->directory = std::move(*directory);
    auto source = read_space(s);
    if (!source) {
        s.message = source.error();
        return;
    }
    const auto topology = s.environment.read_file(s.topology_path);
    const auto selected_topology = s.environment.serialize(s.target->topology, s.target->stable_ids,
                                                           s.target->fingerprints);
    if (!topology || !selected_topology || *topology != selected_topology->canonical_json) {
        s.message = "The topology file changed. Select the target topology and review the plan again.";
        return;
    }
    if (!s.refresh_catalog()) return;
    const ReviewedPlanInputs reviewed{source->digest, core::sha256(*topology),
                                      plan_package_digest(source->space, *s.catalog), source->format};
    const auto controller = source->space.resolve_controller(s.context());
    if (!controller) {
        s.message = describe(controller.error());
        return;
    }
    auto order = candidate_order(s, source->space, strategy.method, *selected);
    if (!order) {
        s.message = order.error();
        return;
    }
    auto schedule = inspect_order(s, source->space, strategy.method, *selected, *order);
    if (!schedule) {
        s.message = schedule.error();
        return;
    }
    s.page("Review experiment plan", [&s] { plan(s); });
    s.note(strategy.label, s.artifact.string() + " · " + source->digest.to_hex(), "Protocol");
    s.note("Target topology", s.topology_label + " · " + s.topology_path.string(), "Protocol");
    s.note("Study directory", selected->directory.string(), "Protocol");
    s.note("Objective", std::string(scenario::objective_orientation(source->space.objective())) + " " +
        std::string(scenario::objective_token(source->space.objective())), "Protocol");
    s.note("Root seed " + std::to_string(selected->seed));
    s.note("Attempt budget " + std::to_string(selected->budget), "0 charged before execution");
    s.note(std::to_string(source->space.size()) + " candidates in the recipe");
    s.note(std::to_string(schedule->scheduled) + " scheduled · " +
        std::to_string(schedule->proposed.size() - schedule->scheduled) + " rejected",
        adaptive(strategy.method) ? "Initial generation" : "Candidate order");
    if (adaptive(strategy.method)) {
        s.note("Maximum generations " + std::to_string(selected->generations),
            "Up to " + std::to_string(selected->batch) + " proposals per generation.");
        if (selected->bind_population) s.note("Bind the resolved population on freeze");
        if (strategy.method == Method::genetic)
            s.note("Mutation per mille " + std::to_string(selected->mutation));
        if (strategy.method == Method::annealing)
            s.note("Annealing cooling", "Initial " + std::to_string(selected->cooling.initial_temperature) +
                " · minimum " + std::to_string(selected->cooling.minimum_temperature) +
                " · halve every " + std::to_string(selected->cooling.steps_per_halving) + " steps");
    }
    placement_preview(s, source->space, *schedule);
    const auto freeze_plan = [&s, strategy, selected = *selected, order = std::move(*order), reviewed] {
        freeze(s, strategy, selected, order, reviewed);
    };
    s.action("Freeze experiment plan", freeze_plan, "Write the immutable plan with zero attempts charged.");
    for (std::size_t i = 0U; i < schedule->proposed.size(); ++i) {
        const auto& proposal = schedule->proposed.at(i);
        s.note(std::to_string(i + 1U) + " · Candidate " + std::to_string(proposal.ordinal) +
            (proposal.accepted ? " · Scheduled" : " · Rejected"), proposal.detail, "Proposal order");
    }
}

void advanced(Session& s) {
    const auto& strategy = get_strategy(s);
    s.page("Strategy advanced settings", [&s] { plan(s); });
    const auto refresh = [&s] { advanced(s); };
    field(s, generations_field, refresh, "Search bounds");
    if (strategy.method == Method::genetic) {
        field(s, population_field, refresh, "Search bounds");
        field(s, mutation_field, refresh, "Genetic");
    }
    if (strategy.method == Method::hill) field(s, batch_field, refresh, "Search bounds");
    if (strategy.method == Method::annealing) {
        s.note("Temperatures are in 2⁻²⁰ relative-loss units.", {}, "Cooling");
        field(s, initial_field, refresh, "Cooling");
        field(s, minimum_field, refresh, "Cooling");
        field(s, steps_field, refresh, "Cooling");
    }
    s.action("Freeze resolved population", [&s] {
        s.draft["bind_population"] = !s.draft.at("bind_population").get<bool>();
        advanced(s);
        s.persist();
    }, s.draft.at("bind_population").get<bool>() ? "On" : "Off", "Population binding");
}

void plan(Session& s, bool revalidate) {
    s.page("Plan experiment", [&s] { s.home(); });
    const auto source = read_space(s);
    s.note(s.artifact.string(), source ? std::to_string(source->space.size()) + " candidates" : source.error(),
        "Recipe");
    s.action("Change target topology", [&s] {
        s.change_target([&s] { plan(s, true); }, [&s] { plan(s); });
    }, s.topology_label, "Target");
    const auto& strategy = get_strategy(s);
    s.action("Search strategy", [&s] {
        std::vector<std::string> labels;
        for (const auto& item : strategies) labels.emplace_back(item.label);
        s.choose("Search strategy", std::move(labels), [&s](std::size_t selection) {
            s.draft["strategy"] = strategies.at(selection).token;
            plan(s);
            s.persist();
        }, [&s] { plan(s); });
    }, strategy.label);
    const auto refresh = [&s] { plan(s); };
    field(s, seed_field, refresh, "Protocol");
    field(s, budget_field, refresh, "Protocol");
    if (strategy.method == Method::explicit_order)
        s.action("Edit candidate order", [&s] { edit_order(s); },
            std::to_string(s.draft.at("order").size()) + " scheduled positions");
    s.field_action("Study directory", [&s] {
        s.input("Study directory", s.draft.at("directory").get<std::string>(), [&s](std::string text) {
            s.draft["directory"] = std::move(text);
            plan(s);
            s.persist();
        }, [&s] { plan(s); });
    }, s.draft.at("directory").get<std::string>(), "Protocol");
    if (adaptive(strategy.method))
        s.action("Advanced", [&s] { advanced(s); }, strategy.label, "Strategy settings");
    if (source) s.action("Review experiment plan", [&s] { review_plan(s); });
    if (revalidate) s.message = source ? revalidate_target(s, source->space, strategy.method) : source.error();
}
} // namespace

void open_plan(Session& s, Path path) {
    s.with_target([&s, path = std::move(path)] {
        const auto original = s.artifact;
        s.artifact = path;
        auto source = read_space(s);
        if (!source) {
            s.artifact = original;
            s.message = source.error();
            return;
        }
        const auto budget = std::to_string(source->space.size());
        const auto directory = path.parent_path() / (path.stem().string() + "-study");
        s.draft_kind = "plan";
        s.draft = {{"strategy", "enumeration"}, {"seed", "42"}, {"budget", budget},
            {"directory", directory.string()}, {"generations", budget}, {"population", "4"},
            {"batch_size", "2"}, {"mutation", "250"}, {"initial_temperature", "131072"},
            {"minimum_temperature", "1024"}, {"steps_per_halving", "4"},
            {"bind_population", false}, {"order", Json::array()}};
        plan(s);
        s.persist();
    });
}

void edit_plan(Session& s, bool revalidate) {
    s.with_target([&s, revalidate] {
        if (s.draft_kind != "plan" || !s.draft.is_object() || !s.draft.contains("order")) {
            s.home();
            s.message = "The plan draft is unavailable.";
            return;
        }
        plan(s, revalidate);
    });
}
void restore_plan_field(Session& s, const Json& address) {
    const auto view = address.value("view", "");
    if (view == "dispatch-position") {
        const auto index = address.find("index");
        if (get_strategy(s).method == Method::explicit_order && index != address.end() &&
            index->is_number_unsigned() && index->get<std::size_t>() < s.draft.at("order").size())
            edit_dispatch_position(s, index->get<std::size_t>());
        return;
    }
    if (view == "Strategy advanced settings" && adaptive(get_strategy(s).method))
        advanced(s);
    s.reopen_field(address);
}

} // namespace epycbench::cli::tui
