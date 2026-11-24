#include "cli/workbench_actions.hpp"
#include "cli/scenario_command.hpp"
#include "epycbench/scenario/parser.hpp"
#include "platform_linux/presentation_files.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace epycbench::cli {
namespace {

auto prompt(std::istream& input, std::ostream& output, std::string_view label) -> std::optional<std::string> {
    std::string answer;
    for (;;) {
        output << label << "> " << std::flush;
        if (!output || !std::getline(input, answer)) return std::nullopt;
        constexpr std::string_view whitespace{" \r\t"};
        const auto first = answer.find_first_not_of(whitespace);
        if (first == std::string::npos) return std::string{};
        const auto trimmed = answer.substr(first, answer.find_last_not_of(whitespace) - first + 1U);
        if (trimmed == "q" || trimmed == "quit") return std::nullopt;
        if (!trimmed.contains('\0')) return trimmed;
        output << "Enter a path or q to cancel.\n";
    }
}

auto cancelled(const std::istream& input, const std::ostream& output) -> int {
    return !output || input.bad() || (input.fail() && !input.eof()) ? 1 : 0;
}

auto command_path(std::string_view path) -> std::string {
    // A filename beginning with a dash remains a filename at every CLI layer.
    return path.starts_with('-') ? "./" + std::string{path} : std::string{path};
}

auto display_words(const WorkbenchDispatch& dispatch, std::span<const std::string> words,
             std::ostream& output, std::optional<std::size_t>* next_offset = nullptr) -> int {
    std::ostringstream captured;
    const auto status = dispatch(words, captured);
    if (status != 0) return status;
    auto bytes = std::move(captured).str();
    if (bytes.empty()) return 0;
    const auto document = nlohmann::ordered_json::parse(bytes, nullptr, false);
    if (document.is_discarded()) {
        output << bytes;
    } else {
        if (next_offset && document.is_object()) {
            const auto next = document.find("next_offset");
            if (next != document.end() && next->is_number_unsigned()) {
                const auto offset = next->get<std::uint64_t>();
                if (offset > std::numeric_limits<std::size_t>::max()) return 1;
                next_offset->emplace(static_cast<std::size_t>(offset));
            }
        }
        output << document.dump(2) << '\n';
    }
    return output ? 0 : 1;
}

auto display(const WorkbenchDispatch& dispatch, std::initializer_list<std::string> words,
             std::ostream& output, std::optional<std::size_t>* next_offset = nullptr) -> int {
    return display_words(dispatch, words, output, next_offset);
}

auto save(const WorkbenchPublish& publish, const std::filesystem::path& path, std::string_view bytes,
          std::ostream& output) -> int {
    const auto status = publish(path, bytes);
    if (status != 0) return status;
    output << "Saved " << path.string() << '\n';
    return output ? 0 : 1;
}

auto default_scenario_directory() -> std::optional<std::filesystem::path> {
    if (const auto* data_home = std::getenv("XDG_DATA_HOME")) {
        const std::filesystem::path directory{data_home};
        if (directory.is_absolute()) return directory / "epycbench" / "scenarios";
    }
    if (const auto* home = std::getenv("HOME")) {
        const std::filesystem::path directory{home};
        if (directory.is_absolute()) return directory / ".local" / "share" / "epycbench" / "scenarios";
    }
    return std::nullopt;
}

auto scenario_extension(const std::filesystem::path& path) -> bool {
    const auto extension = path.extension();
    return extension == ".json" || extension == ".yaml" || extension == ".yml";
}

auto quoted_filename(const std::string& name) -> std::string {
    using Json = nlohmann::ordered_json;
    return Json(name).dump(-1, ' ', false, Json::error_handler_t::replace);
}

auto new_scenario_file(std::istream& input, std::ostream& output,
                       const std::optional<std::filesystem::path>& directory)
    -> std::optional<std::filesystem::path> {
    if (directory) {
        output << "Scenario save directory " << quoted_filename(directory->string()) << '\n';
    }
    for (;;) {
        const auto destination = prompt(input, output,
            "New Scenario filename or path (Enter to display, q to cancel)");
        if (!destination) return std::nullopt;
        std::filesystem::path path{*destination};
        if (path.empty()) return path;
        if (!path.has_filename() || path.filename() == "." || path.filename() == "..") {
            output << "Enter a filename for the new Scenario.\n";
            continue;
        }
        if (!path.has_extension()) path += ".json";
        if (!scenario_extension(path)) {
            output << "Use a .json, .yaml or .yml Scenario filename.\n";
            continue;
        }
        if (path.has_parent_path()) return path;
        if (!directory) {
            output << "The default save directory is unavailable. Enter an explicit file path.\n";
            continue;
        }
        std::error_code failure;
        std::filesystem::create_directories(*directory, failure);
        if (failure) {
            output << "Could not create the Scenario save directory (" << failure.message()
                   << "). Enter an explicit file path.\n";
            continue;
        }
        return *directory / path;
    }
}

auto choose_saved_scenario(std::istream& input, std::ostream& output,
                           std::optional<std::filesystem::path> directory)
    -> std::optional<std::filesystem::path> {
    constexpr std::size_t maximum_directory_entries = 4096U;
    for (;;) {
        std::vector<std::string> files;
        if (directory) {
            output << "\nSaved Scenarios in " << quoted_filename(directory->string()) << '\n';
            auto entries = platform_linux::presentation_files::regular_leaf_names(
                *directory, "", maximum_directory_entries);
            if (entries) {
                files = std::move(*entries);
                std::erase_if(files, [](const std::string& file) { return !scenario_extension(file); });
            }
            if (files.empty()) {
                output << (!entries && entries.error().error_number != ENOENT
                    ? "Could not list this directory. Choose another directory or enter a Scenario file path.\n"
                    : "No .json, .yaml or .yml files found.\n");
            }
            for (std::size_t i = 0; i < files.size(); ++i) {
                output << i + 1U << ") " << quoted_filename(files[i]) << '\n';
            }
        } else {
            output << "The default Scenario directory is unavailable.\n";
        }
        output << "d) Browse another directory\np) Enter a Scenario file path\n";
        for (;;) {
            const auto answer = prompt(input, output, "Choose a saved Scenario (q to cancel)");
            if (!answer) return std::nullopt;
            if (*answer == "d") {
                const auto next = prompt(input, output, "Scenario directory (q to cancel)");
                if (!next) return std::nullopt;
                if (next->empty()) continue;
                directory = *next;
                break;
            }
            if (*answer == "p") {
                const auto path = prompt(input, output, "Saved Scenario file (q to cancel)");
                if (!path) return std::nullopt;
                if (path->empty()) continue;
                if (scenario_extension(*path)) return *path;
                output << "Use a .json, .yaml or .yml Scenario file.\n";
                continue;
            }
            std::size_t index{};
            const auto* end = answer->data() + answer->size();
            const auto parsed = std::from_chars(answer->data(), end, index);
            if (parsed.ec == std::errc{} && parsed.ptr == end && index > 0 && index <= files.size())
                return *directory / files[index - 1U];
            output << "Choose a listed file number, d for another directory, or p for a file path.\n";
        }
    }
}

} // namespace

auto make_workbench_entries(std::istream& input, std::ostream& output,
                            WorkbenchDispatch dispatch, WorkbenchPublish publish, MenuAction edit_scenario) -> std::array<MenuEntry, 19> {
    const auto scenario_directory = default_scenario_directory();
    const auto build_sweep = [&input, &output, dispatch, publish](
        const std::filesystem::path& source, std::string_view topology) {
        const std::array words{std::string{"epycbench"}, std::string{"scenario"}, std::string{"sweep"},
                               std::string{"--topology"}, command_path(topology),
                               std::string{"--scenario"}, command_path(source.string())};
        std::ostringstream captured;
        const auto status = dispatch(words, captured);
        if (status != 0) return status;
        const auto bytes = std::move(captured).str();
        if (bytes.empty()) return 0;
        if (publish) {
            const auto destination = prompt(input, output, "New recipe file (Enter to display, q to cancel)");
            if (!destination) return cancelled(input, output);
            if (!destination->empty()) return save(publish, *destination, bytes, output);
        }
        output << bytes;
        return output ? 0 : 1;
    };
    const auto plan_queue = [&input, &output, dispatch](std::string_view strategy) {
        const auto space = prompt(input, output, "Saved sweep recipe (ConfigurationSpace file, q to cancel)");
        if (!space || space->empty()) return cancelled(input, output);
        const auto topology = prompt(input, output, "Saved topology file (q to cancel)");
        if (!topology || topology->empty()) return cancelled(input, output);
        const auto directory = prompt(input, output, "Existing empty experiment directory (mode 0700, q to cancel)");
        if (!directory || directory->empty()) return cancelled(input, output);
        const auto seed = prompt(input, output, "Unsigned root seed (q to cancel)");
        if (!seed || seed->empty()) return cancelled(input, output);
        const auto budget = prompt(input, output, "Attempt budget, 1 to 4096 (q to cancel)");
        if (!budget || budget->empty()) return cancelled(input, output);
        std::vector<std::string> words{"epycbench", "experiment", "plan", "--strategy", std::string{strategy},
            "--topology", command_path(*topology), "--directory", command_path(*directory),
            "--seed=" + *seed, "--budget=" + *budget};
        if (strategy != "enumeration" && strategy != "seeded-random") {
            const auto generations = prompt(input, output, "Maximum generations (q to cancel)");
            if (!generations || generations->empty()) return cancelled(input, output);
            words.push_back("--generations=" + *generations);
            if (strategy == "genetic" || strategy == "hill-climb") {
                const bool genetic = strategy == "genetic";
                const auto size = prompt(input, output, genetic ? "Population size (q to cancel)" : "Batch size (q to cancel)");
                if (!size || size->empty()) return cancelled(input, output);
                words.push_back((genetic ? "--population=" : "--batch-size=") + *size);
            }
        }
        words.push_back(command_path(*space));
        return display_words(dispatch, words, output);
    };
    return {{
        {"Build a Scenario", [&input, &output, dispatch, publish, scenario_directory, build_sweep] {
            const auto topology = prompt(input, output, "Saved topology file (q to cancel)");
            if (!topology || topology->empty()) return cancelled(input, output);
            const std::array words{std::string{"epycbench"}, std::string{"scenario"}, std::string{"build"},
                                   std::string{"--topology"}, command_path(*topology)};
            std::ostringstream captured;
            const auto status = dispatch(words, captured);
            if (status != 0) return status;
            const auto bytes = std::move(captured).str();
            if (bytes.empty()) return 0;
            if (publish) {
                const auto destination = new_scenario_file(input, output, scenario_directory);
                if (!destination) return cancelled(input, output);
                if (!destination->empty()) {
                    const auto saved = save(publish, *destination, bytes, output);
                    if (saved != 0) return saved;
                    for (;;) {
                        const auto answer = prompt(input, output, "Create a sweep from this Scenario (y/N, q to cancel)");
                        if (!answer) return cancelled(input, output);
                        if (answer->empty() || *answer == "n" || *answer == "N") return 0;
                        if (*answer == "y" || *answer == "Y") {
                            const auto sweep_status = build_sweep(*destination, *topology);
                            if (sweep_status != 0)
                                output << "Sweep failed (exit code " << sweep_status << "). The Scenario is saved.\n";
                            return output ? 0 : 1;
                        }
                        output << "Enter y to create a sweep, or n to return to the menu.\n";
                    }
                }
            }
            output << bytes;
            return output ? 0 : 1;
        }},
        {"Browse saved Results", [&input, &output, dispatch] {
            const auto directory = prompt(input, output, "Result directory (q to cancel)");
            if (!directory || directory->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "result", "index", command_path(*directory)}, output);
        }},
        {"Compare saved Results", [&input, &output, dispatch] {
            const auto baseline = prompt(input, output, "Baseline Result file (q to cancel)");
            if (!baseline || baseline->empty()) return cancelled(input, output);
            const auto candidate = prompt(input, output, "Candidate Result file (q to cancel)");
            if (!candidate || candidate->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "result", "compare", command_path(*baseline),
                                       command_path(*candidate)}, output);
        }},
        {"Validate a saved Result", [&input, &output, dispatch] {
            const auto path = prompt(input, output, "Saved Result file (q to cancel)");
            if (!path || path->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "result", "validate", command_path(*path)}, output);
        }},
        {"Inspect experiment status", [&input, &output, dispatch] {
            const auto directory = prompt(input, output, "Experiment directory (q to cancel)");
            if (!directory || directory->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "status", command_path(*directory)}, output);
        }},
        {"Build a sweep recipe", [&input, &output, build_sweep, scenario_directory] {
            const auto source = choose_saved_scenario(input, output, scenario_directory);
            if (!source) return cancelled(input, output);
            const auto topology = prompt(input, output, "Saved topology file (q to cancel)");
            if (!topology || topology->empty()) return cancelled(input, output);
            return build_sweep(*source, *topology);
        }},
        {"Inspect a saved Scenario", [&input, &output, dispatch] {
            const auto scenario = prompt(input, output, "Saved Scenario file (q to cancel)");
            if (!scenario || scenario->empty()) return cancelled(input, output);
            const auto topology = prompt(input, output, "Saved topology file (q to cancel)");
            if (!topology || topology->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "scenario", "resolve", "--topology",
                                       command_path(*topology), command_path(*scenario)}, output);
        }},
        {"Analyze a saved experiment", [&input, &output, dispatch] {
            const auto directory = prompt(input, output, "Experiment directory (q to cancel)");
            if (!directory || directory->empty()) return cancelled(input, output);
            const auto path = command_path(*directory);
            std::optional<std::size_t> offset;
            for (;;) {
                std::optional<std::size_t> next;
                const auto status = offset
                    ? display(dispatch, {"epycbench", "experiment", "analyze", "--offset",
                                        std::to_string(*offset), path}, output, &next)
                    : display(dispatch, {"epycbench", "experiment", "analyze", path}, output, &next);
                if (status != 0 || !next) return status;
                if (*next <= offset.value_or(0U)) return 1;
                for (;;) {
                    const auto answer = prompt(input, output, "Enter for the next page or q to return");
                    if (!answer) return cancelled(input, output);
                    if (answer->empty()) break;
                }
                offset = next;
            }
        }},
        {"Compare saved search studies", [&input, &output, dispatch] {
            const auto first = prompt(input, output, "First study directory (q to cancel)");
            if (!first || first->empty()) return cancelled(input, output);
            const auto second = prompt(input, output, "Second study directory (q to cancel)");
            if (!second || second->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "compare-search",
                                       command_path(*first), command_path(*second)}, output);
        }},
        {"Replay a saved search study", [&input, &output, dispatch] {
            const auto study = prompt(input, output, "Study directory (q to cancel)");
            if (!study || study->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "replay", command_path(*study)}, output);
        }},
        {"Replay worker observations", [&input, &output, dispatch] {
            const auto path = prompt(input, output, "Saved Result file (q to cancel)");
            if (!path || path->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "result", "replay", command_path(*path)}, output);
        }},
        {"Edit a saved Scenario", std::move(edit_scenario)},
        {"Prepare an experiment queue", [plan_queue] { return plan_queue("enumeration"); }},
        {"Inspect saved package energy", [&input, &output, dispatch] {
            const auto source = prompt(input, output, "Result file (q to cancel)");
            if (!source || source->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "result", "energy", command_path(*source)}, output);
        }},
        {"Inspect a workload fingerprint", [&input, &output, dispatch] {
            const auto source = prompt(input, output, "Result file (q to cancel)");
            if (!source || source->empty()) return cancelled(input, output);
            const auto pair = prompt(input, output, "Pair ordinal (Enter for the Result default, q to cancel)");
            if (!pair) return cancelled(input, output);
            if (pair->empty())
                return display(dispatch, {"epycbench", "result", "fingerprint", command_path(*source)}, output);
            return display(dispatch, {"epycbench", "result", "fingerprint", "--pair=" + *pair, command_path(*source)},
                           output);
        }},
        {"Inspect a saved search population", [&input, &output, dispatch] {
            const auto path = prompt(input, output, "Saved study directory (q to cancel)");
            if (!path || path->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "replay", "--population", command_path(*path)}, output);
        }},
        {"Run one experiment queue attempt", [&input, &output, dispatch] {
            const auto directory = prompt(input, output, "Study directory to advance by one attempt (q to cancel)");
            if (!directory || directory->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "run", "--one-attempt", command_path(*directory)}, output);
        }},
        {"Record a terminal scientific stop", [&input, &output, dispatch] {
            const auto directory = prompt(input, output, "Study directory to stop permanently (q to cancel)");
            if (!directory || directory->empty()) return cancelled(input, output);
            return display(dispatch, {"epycbench", "experiment", "stop", command_path(*directory)}, output);
        }},
        {"Prepare a search queue", [&input, &output, plan_queue] {
            constexpr std::array<std::string_view, 6> methods{
                "seeded-random", "genetic", "hill-climb", "annealing", "bayesian", "hybrid"};
            for (;;) {
                const auto method = prompt(input, output,
                    "Search method (seeded-random, genetic, hill-climb, annealing, bayesian, hybrid, q to cancel)");
                if (!method || method->empty()) return cancelled(input, output);
                if (std::ranges::contains(methods, *method)) return plan_queue(*method);
                output << "Choose a listed search method.\n";
            }
        }},
    }};
}

auto edit_saved_scenario(std::istream& input, std::ostream& output, std::ostream& error,
                         topology::cli::Environment& environment,
                         const std::optional<std::filesystem::path>& module_directory) -> int {
    using Json = nlohmann::ordered_json;
    const auto source = prompt(input, output, "Saved Scenario (.json, .yaml or .yml, q to cancel)");
    if (!source || source->empty()) return cancelled(input, output);
    const std::filesystem::path source_path{*source};
    if (!scenario_extension(source_path)) {
        error << "Use a .json, .yaml or .yml Scenario file.\n";
        return 2;
    }
    const auto bytes = environment.read_file(*source);
    if (!bytes) {
        error << "Could not read the Scenario (" << bytes.error().code << ").\n";
        return 3;
    }
    const auto format = source_path.extension() == ".json"
        ? scenario::ScenarioFormat::json : scenario::ScenarioFormat::yaml;
    const auto authored = scenario::parse_authored_scenario({"scenario", *bytes, format});
    if (!authored) {
        error << authored.error().code.value() << ' ' << authored.error().context.dump() << '\n';
        return 6;
    }

    const auto pointer = prompt(input, output, "Existing scalar JSON Pointer (for example /metadata/name, q to cancel)");
    if (!pointer || pointer->empty()) return cancelled(input, output);
    auto edited = authored->value();
    try {
        auto& field = edited.at(Json::json_pointer{*pointer});
        if (!field.is_primitive()) {
            error << "The selected field is an object or array. Select one of its scalar values.\n";
            return 2;
        }
        output << "Current value: " << field.dump() << '\n';
        const auto value = prompt(input, output, "New JSON scalar (quote strings, q to cancel)");
        if (!value || value->empty()) return cancelled(input, output);
        const auto replacement = scenario::parse_authored_scenario({"replacement", *value, scenario::ScenarioFormat::json});
        if (!replacement) {
            error << replacement.error().code.value() << ' ' << replacement.error().context.dump() << '\n';
            return 6;
        }
        if (!replacement->value().is_primitive()) {
            error << "Enter a JSON string, number, boolean or null.\n";
            return 2;
        }
        field = replacement->value();
    } catch (const Json::exception&) {
        error << "The JSON Pointer must identify an existing scalar field.\n";
        return 2;
    }

    const auto topology = prompt(input, output, "Saved topology file (q to cancel)");
    if (!topology || topology->empty()) return cancelled(input, output);
    const auto draft = edited.dump(2) + '\n';
    // Give ordinary resolve an in-memory Scenario alongside the actual saved topology.
    const auto topology_path = command_path(*topology);
    const auto draft_path = topology_path + ".workbench-draft.json";
    auto validation_environment = environment;
    validation_environment.read_bounded_file = [&](const std::filesystem::path& path, std::size_t limit)
        -> std::expected<std::string, topology::cli::CliFailure> {
        if (path == draft_path) return draft;
        return environment.read_bounded_file(path, limit);
    };
    std::array<std::string, 6> words{"epycbench", "scenario", "resolve", "--topology", topology_path, draft_path};
    std::array<char*, 6> argv{};
    for (std::size_t i = 0; i < words.size(); ++i) argv[i] = words[i].data();
    std::ostringstream resolved;
    const auto status = run_scenario_command(static_cast<int>(argv.size()), argv.data(), validation_environment,
                                             resolved, error, module_directory);
    if (status != 0) return status;
    const auto destination = prompt(input, output, "New .json Scenario file (q to cancel)");
    if (!destination || destination->empty()) return cancelled(input, output);
    if (std::filesystem::path{*destination}.extension() != ".json") {
        error << "The edited copy is JSON. Choose a new .json file.\n";
        return 2;
    }
    const auto written = environment.publish_file(*destination, draft, false);
    if (!written) {
        error << "Could not save the edited Scenario (" << written.error().code << ").\n";
        return 3;
    }
    output << "Saved " << *destination << '\n';
    return output ? 0 : 1;
}

} // namespace epycbench::cli
