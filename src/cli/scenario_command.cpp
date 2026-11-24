#include "cli/scenario_command.hpp"
#include "cli/package_authoring.hpp"
#include "runtime/package_catalog.hpp"
#include "cli/run_output_policy.hpp"


#include "epycbench/core/canonical_json.hpp"
#include "epycbench/scenario/configuration_space.hpp"
#include "epycbench/scenario/configuration_space.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::cli {
namespace {

enum class ExitCode : int {
    success = 0,
    unexpected = 1,
    usage = 2,
    filesystem = 3,
    discovery = 4,
    topology = 5,
    authored = 6,
};

auto make_error(core::ErrorCategory category, std::string code, std::string message,
                std::string operation) -> core::Error {
    return {.category = category,
            .code = core::StableErrorCode{std::move(code)},
            .message = std::move(message),
            .operation = std::move(operation),
            .os_error = std::nullopt,
            .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

void print_internal(std::ostream& error, std::string_view code,
                    std::string_view classification, std::string_view action = "scenario resolve") {
    error << action << ": failed\ncode: " << code
          << "\nclassification: " << classification << '\n';
}

void print_sweep_usage(std::ostream& error, std::string_view reason) {
    print_internal(error, "scenario.cli.usage", reason, "scenario sweep");
    error << "Create a sweep with a saved Scenario\n"
             "  epycbench scenario sweep --scenario FILE --topology FILE\n"
             "Edit an existing sweep recipe\n"
             "  epycbench scenario sweep --edit FILE --topology FILE\n";
}

void print_parser_failure(std::ostream& error, const core::Error& failure) {
    error << "scenario resolve: failed\ncode: " << failure.code.value()
          << "\nclassification: authored scenario invalid\n";
    for (const auto* key : {"line", "column", "first_line", "first_column", "duplicate_line",
                            "duplicate_column"}) {
        if (failure.context.contains(key) && failure.context.at(key).is_number_unsigned()) {
            error << key << ": " << failure.context.at(key).get<std::uint64_t>() << '\n';
        }
    }
}

auto infer_format(const std::filesystem::path& path, const std::optional<std::string>& explicit_value)
    -> std::optional<scenario::ScenarioFormat> {
    const auto extension = path.extension().string();
    std::optional<scenario::ScenarioFormat> inferred;
    if (extension == ".yaml" || extension == ".yml") {
        inferred = scenario::ScenarioFormat::yaml;
    } else if (extension == ".json") {
        inferred = scenario::ScenarioFormat::json;
    }
    if (!explicit_value) {
        return inferred;
    }
    const auto selected = *explicit_value == "yaml" ? scenario::ScenarioFormat::yaml
                                                     : scenario::ScenarioFormat::json;
    if (inferred && *inferred != selected) {
        return std::nullopt;
    }
    return selected;
}

} // namespace

auto load_live_topology(topology::cli::Environment& environment)
    -> std::expected<topology::LoadedTopology, core::Error> {
    auto hwloc = environment.discover_hwloc();
    if (!hwloc) {
        return std::unexpected{std::move(hwloc.error())};
    }
    auto linux = environment.discover_linux();
    if (!linux) {
        return std::unexpected{std::move(linux.error())};
    }
    auto reconciled = environment.reconcile(*hwloc, *linux);
    if (!reconciled) {
        return std::unexpected{std::move(reconciled.error())};
    }
    if (!reconciled->snapshot || std::ranges::any_of(reconciled->diagnostics,
                                                     &topology::ReconciliationDiagnostic::material)) {
        return std::unexpected{make_error(core::ErrorCategory::topology,
                                          "topology.reconciliation.material_conflict",
                                          "Topology reconciliation did not produce a safe snapshot",
                                          "scenario.cli.live_topology")};
    }
    auto snapshot = std::move(*reconciled->snapshot);
    if (!environment.validate(snapshot).valid()) {
        return std::unexpected{make_error(core::ErrorCategory::topology,
                                          "topology.fingerprint.invalid_topology",
                                          "Discovered topology is invalid",
                                          "scenario.cli.live_topology")};
    }
    auto ids = environment.derive_ids(snapshot);
    if (!ids) {
        return std::unexpected{std::move(ids.error())};
    }
    auto fingerprints = environment.derive_fingerprints(snapshot, *ids);
    if (!fingerprints) {
        return std::unexpected{std::move(fingerprints.error())};
    }
    return topology::LoadedTopology{.topology = std::move(snapshot),
                                    .stable_ids = std::move(*ids),
                                    .fingerprints = std::move(*fingerprints),
                                    .canonical_json = {}};
}

namespace {
auto preview_space(const scenario::ConfigurationSpace& space,
                   const scenario::ResourceResolutionContext& topology, std::size_t offset,
                   std::size_t limit, std::ostream& output, std::ostream& error) -> int {
    constexpr std::size_t mib = 1024U * 1024U;
    const auto count = std::min(limit, space.size() - offset);
    nlohmann::ordered_json rows = nlohmann::ordered_json::array();
    std::size_t row_bytes{};
    for (std::size_t index = offset; index < offset + count; ++index) {
        auto candidate = space.resolve(index, topology);
        if (!candidate) {
            print_internal(error, candidate.error().code.value(), "candidate resolution failed");
            error << candidate.error().message << ' ' << candidate.error().context.dump() << '\n';
            return static_cast<int>(ExitCode::authored);
        }
        nlohmann::ordered_json row{{"ordinal", index}, {"scenario", std::move(candidate->authored_scenario)}};
        if (candidate->accepted()) {
            row["status"] = "resolved";
            row["input_identity"] = "sha256:" + candidate->input_identity->to_hex();
            row["problem_identity"] = "sha256:" + candidate->problem_identity->to_hex();
            row["candidate_identity"] = "sha256:" + candidate->candidate_identity->to_hex();
            row["scenario_digest"] = "sha256:" + candidate->package->scenario_digest.to_hex();
            row["resolved_plan_digest"] = "sha256:" + candidate->package->resolved_digest.to_hex();
            row["resolved_plan"] = candidate->package->evidence;
        } else {
            row["status"] = "rejected";
            auto diagnostics = nlohmann::ordered_json::array();
            for (const auto& diagnostic : candidate->diagnostics) {
                diagnostics.push_back({{"code", diagnostic.code.value()},
                                       {"pointer", diagnostic.json_pointer},
                                       {"context", diagnostic.safe_context}});
            }
            row["diagnostics"] = std::move(diagnostics);
        }
        const auto bytes = row.dump().size();
        if (bytes > 16U * mib - row_bytes) {
            print_internal(error, "scenario.cli.preview_size", "reduce the preview page size");
            return static_cast<int>(ExitCode::usage);
        }
        row_bytes += bytes;
        rows.push_back(std::move(row));
    }
    const nlohmann::ordered_json preview{
        {"schema", "epycbench.configuration-preview/v1"},
        {"objective", {{"metric", scenario::objective_token(space.objective())},
                       {"orientation", scenario::objective_orientation(space.objective())}}},
        {"total_candidates", space.size()},
        {"next_offset", offset + count == space.size()
                            ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(offset + count)},
        {"candidates", std::move(rows)}};
    output << preview.dump() << '\n';
    return static_cast<int>(output ? ExitCode::success : ExitCode::filesystem);
}

} // namespace

auto run_scenario_command(int argc, char* argv[], topology::cli::Environment& environment,
                          std::ostream& output, std::ostream& error,
                          std::optional<std::filesystem::path> module_directory, std::istream* input) -> int {
    try {
        CLI::App app{"Topology-linked benchmarking for AMD EPYC systems", "epycbench"};
        auto* scenario_command = app.add_subcommand("scenario",
            "Build and save a Scenario, create a sweep recipe from it, and preview the candidates.");
        scenario_command->require_subcommand(1, 1);
        auto* resolve = scenario_command->add_subcommand(
            "resolve",
            "Parse and resolve one Scenario using serialized or live topology. "
            "Performs no benchmark execution and makes no host changes. Produces a machine-specific "
            "inspection envelope that may disclose topology-linked resource identities. Does not "
            "produce a final execution-plan digest or create a persisted runtime plan.");
        auto* admit = scenario_command->add_subcommand(
            "admit", "Check a package Scenario against a saved topology without executing a Run.");
        auto* preview = scenario_command->add_subcommand(
            "preview", "Preview a bounded page of candidates from a saved sweep recipe on a topology.");
        auto* build = scenario_command->add_subcommand(
            "build", "Author an editable Scenario on a saved topology. Save it for inspection or to use as a sweep base.");
        auto* sweep = scenario_command->add_subcommand(
            "sweep", "Create a sweep recipe from a saved Scenario or edit an existing recipe. Save it for scenario preview.");
        std::filesystem::path scenario_path;
        std::filesystem::path topology_path;
        std::optional<std::filesystem::path> sweep_source_path;
        std::optional<std::string> format;
        std::optional<std::string> result_directory;
        bool live = false;
        std::vector<std::string> projections;
        bool inspect_partitions = false;
        bool check_output = false;
        std::size_t offset{}, limit = 16U;
        for (auto* command : {resolve, preview, admit}) {
            command->add_option("--format", format, "Document parser (yaml or json)")
                ->check(CLI::IsMember({"yaml", "json"}));
            command->add_option("--topology", topology_path, "Canonical serialized topology file");
            command->add_flag("--live", live, "Use live read-only topology discovery");
        }
        for (auto* command : {build, sweep})
            command->add_option("--topology", topology_path, "Canonical serialized topology file")->required();
        auto* sweep_sources = sweep->add_option_group("Saved input", "Choose one saved Scenario or sweep recipe.")
            ->require_option(1);
        const auto* sweep_edit = sweep_sources->add_option("--edit", sweep_source_path, "Edit an existing package sweep recipe")
            ->type_name("FILE");
        const auto* sweep_base = sweep_sources->add_option(
            "--scenario", sweep_source_path, "Use a saved Scenario as the base of a new sweep")->type_name("FILE");
        sweep->add_option("--format", format, "Input format for --scenario or --edit (yaml or json)")
            ->check(CLI::IsMember({"yaml", "json"}));
        build->add_option("--result-directory", result_directory, "Absolute private directory to author as the Run's Result destination");
        resolve->add_option("scenario", scenario_path, "Scenario regular-file path")->required();
        resolve->add_option_function<std::string>("--inspect",
            [&](const std::string& name) { projections.push_back(name); },
            "Show a named task-package projection for each rank on a saved topology")
            ->type_name("NAME")->trigger_on_parse();
        resolve->add_flag("--partitions", inspect_partitions,
                          "Inspect each worker's package-defined partition and region layout on a saved topology");
        admit->add_option("scenario", scenario_path, "Scenario regular-file path")->required();
        admit->add_flag("--check-output", check_output,
                        "Also check the authored Run Result destination and evidence retention without opening any output directory");
        preview->add_option("space", scenario_path, "Configuration space file")->required();
        preview->add_option("--offset", offset, "First candidate ordinal");
        preview->add_option("--limit", limit, "Candidates per page")->check(CLI::Range(1U, 64U));

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& parse_error) {
            if (parse_error.get_exit_code() == 0) {
                const std::string_view name = argc >= 3 ? argv[2] : "";
                const auto commands = {resolve, preview, admit, build, sweep};
                const auto found = std::ranges::find_if(commands, [&](const auto* command) {
                    return command->get_name() == name;
                });
                output << (found == commands.end() ? scenario_command : *found)->help();
                return static_cast<int>(ExitCode::success);
            }
            if (*sweep) {
                const auto reason = (sweep_base->count() != 0) == (sweep_edit->count() != 0)
                    ? "select either --scenario or --edit" : "invalid command usage";
                print_sweep_usage(error, reason);
            } else print_internal(error, "scenario.cli.usage", "invalid command usage");
            return static_cast<int>(ExitCode::usage);
        }

        if (*build || *sweep) {
            const auto action = *sweep ? "scenario sweep" : "scenario build";
            if (*sweep && (!sweep_source_path || sweep_source_path->empty())) {
                print_sweep_usage(error, "provide a file path for --scenario or --edit");
                return static_cast<int>(ExitCode::usage);
            }
            if (!input) {
                error << action << ": input stream unavailable\n";
                return static_cast<int>(ExitCode::usage);
            }
            std::string source_bytes;
            std::optional<scenario::ScenarioSource> source;
            if (sweep_source_path) {
                const auto label = sweep_edit->count() ? "recipe" : "Scenario";
                const auto source_format = infer_format(*sweep_source_path, format);
                if (!source_format) {
                    error << action << ": unknown or conflicting " << label << " format\n";
                    return static_cast<int>(ExitCode::usage);
                }
                auto bytes = environment.read_file(*sweep_source_path);
                if (!bytes) {
                    print_internal(error, bytes.error().code,
                        sweep_edit->count() ? "recipe input unavailable" : "Scenario input unavailable", action);
                    return static_cast<int>(ExitCode::filesystem);
                }
                source_bytes = std::move(*bytes);
                source.emplace(sweep_edit->count() ? "sweep-recipe" : "scenario", source_bytes, *source_format);
            }
            auto topology_bytes = environment.read_file(topology_path);
            if (!topology_bytes) {
                error << action << ": topology input unavailable\n";
                return static_cast<int>(ExitCode::filesystem);
            }
            auto loaded = environment.deserialize(*topology_bytes,
                topology::TopologyJsonInputPolicy::require_canonical);
            if (!loaded) {
                error << action << ": invalid serialized topology\n";
                return static_cast<int>(ExitCode::topology);
            }
            std::optional<nlohmann::json> document;
            if (source) {
                auto parsed = scenario::parse_authored_scenario(*source);
                if (!parsed) { print_parser_failure(error, parsed.error()); return static_cast<int>(ExitCode::authored); }
                document = parsed->value();
            }
            if (*build || (document && document->is_object() && document->contains("schema") &&
                (document->at("schema") == scenario::package_scenario_schema ||
                 document->at("schema") == scenario::package_space_schema))) {
                auto catalog = runtime::discover_packages(module_directory);
                if (!catalog) {
                    error << action << ": " << catalog.error().message << ' ' << catalog.error().context.dump() << '\n';
                    return static_cast<int>(ExitCode::authored);
                }
                const scenario::ResourceResolutionContext context{loaded->topology, loaded->stable_ids,
                                                                  loaded->fingerprints, &*catalog};
                if (*build) return author_package_scenario(*catalog, context, result_directory, *input, output, error);
                return author_package_sweep(*document, sweep_edit->count() != 0, context, *input, output, error);
            }
            error << "scenario sweep: scenario.sweep.task_scenario_required "
                     "Choose a Scenario authored from installed kernel manifests or a saved package sweep recipe.\n";
            return static_cast<int>(ExitCode::authored);
        }

        if (*admit && live) {
            print_internal(error, "scenario.cli.usage", "production admission requires a saved topology");
            return static_cast<int>(ExitCode::usage);
        }
        if (inspect_partitions && live) {
            print_internal(error, "scenario.cli.usage", "Worker partition inspection requires a saved topology");
            return static_cast<int>(ExitCode::usage);
        }
        if (!projections.empty() && live) {
            print_internal(error, "scenario.cli.usage", "Task projection inspection requires a saved topology");
            return static_cast<int>(ExitCode::usage);
        }
        if (live == !topology_path.empty()) {
            print_internal(error, "scenario.cli.usage", "exactly one topology source is required");
            return static_cast<int>(ExitCode::usage);
        }
        const auto selected_format = infer_format(scenario_path, format);
        if (!selected_format) {
            print_internal(error, "scenario.cli.usage", "unknown or conflicting scenario format");
            return static_cast<int>(ExitCode::usage);
        }
        auto scenario_bytes = environment.read_file(scenario_path);
        if (!scenario_bytes) {
            print_internal(error, scenario_bytes.error().code, "scenario input unavailable");
            return static_cast<int>(ExitCode::filesystem);
        }

        std::optional<scenario::ConfigurationSpace> space;
        if (*preview) {
            auto parsed = scenario::ConfigurationSpace::parse(
                {"configuration-space", *scenario_bytes, *selected_format}, 4096U);
            if (!parsed) {
                error << "scenario preview: " << parsed.error().code.value() << '\n'
                      << parsed.error().context.dump() << '\n';
                return static_cast<int>(ExitCode::authored);
            }
            if (offset >= parsed->size()) {
                print_internal(error, "scenario.cli.preview_offset", "offset is outside the space");
                return static_cast<int>(ExitCode::usage);
            }
            space.emplace(std::move(*parsed));
        }

        std::optional<nlohmann::json> package_source;
        if (!space) {
            auto authored=scenario::parse_authored_scenario({"scenario",*scenario_bytes,*selected_format});
            if (!authored) { print_parser_failure(error,authored.error()); return static_cast<int>(ExitCode::authored); }
            const auto& document=authored->value();
            const auto schema=document.is_object() && document.contains("schema") && document.at("schema").is_string()
                ? document.at("schema").get<std::string>() : std::string{};
            if (schema==scenario::package_scenario_schema) package_source=document;
            else {
                error << "Kernel Scenarios require epycbench.scenario/packages-v1. Use scenario build to select an installed package.\n";
                return static_cast<int>(ExitCode::authored);
            }
        }
        std::optional<kernel::packages::Catalog> catalog;
        if (space || package_source) {
            auto found=runtime::discover_packages(module_directory);
            if (!found) {
                error << found.error().code.value() << ' ' << found.error().message << ' ' << found.error().context.dump() << '\n';
                return static_cast<int>(ExitCode::authored);
            }
            catalog=std::move(*found);
        }
        std::expected<topology::LoadedTopology, core::Error> loaded =
            std::unexpected{core::Error{}};
        if (live) {
            loaded = load_live_topology(environment);
            if (!loaded) {
                print_internal(error, loaded.error().code.value(), "live topology unavailable");
                return static_cast<int>(ExitCode::discovery);
            }
        } else {
            auto topology_bytes = environment.read_file(topology_path);
            if (!topology_bytes) {
                print_internal(error, topology_bytes.error().code, "topology input unavailable");
                return static_cast<int>(ExitCode::filesystem);
            }
            loaded = environment.deserialize(*topology_bytes,
                                             topology::TopologyJsonInputPolicy::require_canonical);
            if (!loaded) {
                print_internal(error, loaded.error().code.value(), "invalid serialized topology");
                return static_cast<int>(ExitCode::topology);
            }
        }

        const scenario::ResourceResolutionContext context{
            loaded->topology, loaded->stable_ids, loaded->fingerprints, catalog ? &*catalog : nullptr};
        if (package_source) {
            using Json = nlohmann::json;
            const auto report_package = [&](const scenario::PackagePlan& plan) -> std::optional<Json> {
                Json receipt{{"schema", *admit ? "epycbench.package-admission/v1" : "epycbench.scenario-resolution/v1"},
                    {"scenario_digest", "sha256:" + plan.scenario_digest.to_hex()},
                    {"resolved_plan_digest", "sha256:" + plan.resolved_digest.to_hex()}};
                const auto& source = plan.evidence.at("scenario");
                receipt["controller"] = plan.evidence.at("controller");
                if (source.contains("case_id")) receipt["case_id"] = source.at("case_id");
                if (*admit) return receipt;
                if (inspect_partitions) receipt["partitions"] = plan.evidence.at("geometry");
                for (const auto& key : projections) {
                    auto inspected = Json::array();
                    for (const auto& rank : plan.evidence.at("geometry")) {
                        if (!rank.contains("task_inspection") || !rank.at("task_inspection").contains(key)) continue;
                        inspected.push_back({{"role", rank.at("role")}, {"rank", rank.at("rank")},
                                             {"projection", rank.at("task_inspection").at(key)}});
                    }
                    if (inspected.empty()) {
                        error << "The selected task package and topology have no " << key << " projection.\n";
                        return {};
                    }
                    receipt["task_inspection"][key] = std::move(inspected);
                }
                receipt["canonical_scenario"] = source;
                receipt["resolved_scenario_plan"] = plan.evidence;
                return receipt;
            };
            const auto fail = [&](const core::Error& reason) {
                error << reason.code.value() << ' ' << reason.message << ' ' << reason.context.dump() << '\n';
                return static_cast<int>(ExitCode::authored);
            };
            Json receipt;
            if (scenario::is_package_case_group(*package_source)) {
                auto cases = scenario::resolve_package_cases(*package_source, *catalog, context);
                if (!cases) return fail(cases.error());
                auto reports = Json::array();
                for (const auto& one : *cases) {
                    auto report = report_package(one.plan);
                    if (!report) return static_cast<int>(ExitCode::authored);
                    reports.push_back(std::move(*report));
                }
                receipt = {{"schema", *admit ? "epycbench.package-case-admission/v1"
                                            : "epycbench.package-case-resolution/v1"},
                           {"cases", std::move(reports)}};
            } else {
                auto plan = scenario::resolve_package(*package_source, *catalog, context);
                if (!plan) return fail(plan.error());
                auto report = report_package(*plan);
                if (!report) return static_cast<int>(ExitCode::authored);
                receipt = std::move(*report);
            }
            if (*admit && check_output) {
                const auto checked = run_output_policy::check(nlohmann::ordered_json(*package_source));
                if (!checked) {
                    error << "scenario admit: " << checked.error() << '\n';
                    return static_cast<int>(ExitCode::authored);
                }
                receipt["output_policy"] = "authored-settings-accepted";
            }
            output << receipt.dump() << '\n';
            return output ? 0 : static_cast<int>(ExitCode::filesystem);
        }
        return preview_space(*space, context, offset, limit, output, error);
    } catch (...) {
        print_internal(error, "scenario.cli.unexpected", "unexpected internal failure");
        return static_cast<int>(ExitCode::unexpected);
    }
}

} // namespace epycbench::cli
