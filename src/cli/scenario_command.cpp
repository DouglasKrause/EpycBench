#include "cli/scenario_command.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/scenario/resolution.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

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
                    std::string_view classification) {
    error << "scenario resolve: failed\ncode: " << code
          << "\nclassification: " << classification << '\n';
}

void print_diagnostics(std::ostream& error,
                       const std::vector<scenario::ScenarioDiagnostic>& diagnostics) {
    error << "scenario resolve: failed\ncode: scenario.cli.authored_invalid\ndiagnostics: "
          << diagnostics.size() << '\n';
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        const auto& diagnostic = diagnostics[index];
        error << "diagnostic " << index << ":\ncode: " << diagnostic.code.value()
              << "\npointer: " << diagnostic.json_pointer << '\n';
        if (diagnostic.location) {
            error << "line: " << diagnostic.location->line << "\ncolumn: "
                  << diagnostic.location->column << '\n';
        }
        for (const auto& [key, value] : diagnostic.safe_context) {
            error << "context." << key << ": " << value << '\n';
        }
    }
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

auto load_live(topology::cli::Environment& environment)
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

auto verified_projection(std::string_view bytes, std::string_view digest)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        auto value = nlohmann::ordered_json::parse(bytes);
        auto canonical = core::canonicalize_json(value);
        if (!canonical || *canonical != bytes) {
            return std::unexpected{make_error(core::ErrorCategory::scenario,
                                              "scenario.cli.projection_verification_failed",
                                              "Retained projection is not Core-canonical",
                                              "scenario.cli.envelope")};
        }
        auto hash = core::hash_canonical_json(value);
        if (!hash || "sha256:" + hash->to_hex() != digest) {
            return std::unexpected{make_error(core::ErrorCategory::scenario,
                                              "scenario.cli.digest_verification_failed",
                                              "Retained projection digest disagrees",
                                              "scenario.cli.envelope")};
        }
        return value;
    } catch (...) {
        return std::unexpected{make_error(core::ErrorCategory::scenario,
                                          "scenario.cli.projection_parse_failed",
                                          "Retained projection could not be parsed",
                                          "scenario.cli.envelope")};
    }
}

auto parser_authored_error(const core::Error& value) -> bool {
    return value.code.value().starts_with("scenario.parse.") &&
           value.code.value() != "scenario.parse.internal_parser_failure";
}

} // namespace

auto run_scenario_command(int argc, char* argv[], topology::cli::Environment& environment,
                          std::ostream& output, std::ostream& error) -> int {
    try {
        CLI::App app{"Topology-linked benchmarking for AMD EPYC systems", "epycbench"};
        auto* scenario_command = app.add_subcommand("scenario", "Scenario inspection");
        scenario_command->require_subcommand(1, 1);
        auto* resolve = scenario_command->add_subcommand(
            "resolve",
            "Parse and resolve one Scenario v1 document using serialized or live topology. "
            "Performs no benchmark execution and makes no host changes. Produces a machine-specific "
            "inspection envelope that may disclose topology-linked resource identities. Does not "
            "produce a final execution-plan digest or create a persisted runtime plan.");
        std::filesystem::path scenario_path;
        std::filesystem::path topology_path;
        std::optional<std::string> format;
        bool live = false;
        resolve->add_option("scenario", scenario_path, "Scenario v1 regular-file path")->required();
        resolve->add_option("--format", format, "Scenario parser: yaml or json")
            ->check(CLI::IsMember({"yaml", "json"}));
        resolve->add_option("--topology", topology_path, "Canonical serialized topology file");
        resolve->add_flag("--live", live, "Use live read-only topology discovery");

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& parse_error) {
            if (parse_error.get_exit_code() == 0) {
                output << ((argc >= 3 && std::string_view{argv[2]} == "resolve")
                               ? resolve->help()
                               : scenario_command->help());
                return static_cast<int>(ExitCode::success);
            }
            print_internal(error, "scenario.cli.usage", "invalid command usage");
            return static_cast<int>(ExitCode::usage);
        }

        if (!*resolve || live == !topology_path.empty()) {
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

        std::expected<topology::LoadedTopology, core::Error> loaded =
            std::unexpected{core::Error{}};
        if (live) {
            loaded = load_live(environment);
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
            loaded->topology, loaded->stable_ids, loaded->fingerprints};
        auto resolution = scenario::resolve_scenario(
            {.logical_name = "scenario", .contents = *scenario_bytes, .format = *selected_format},
            context);
        if (!resolution) {
            const auto& failure = resolution.error();
            if (parser_authored_error(failure)) {
                print_parser_failure(error, failure);
                return static_cast<int>(ExitCode::authored);
            }
            print_internal(error, failure.code.value(), "scenario resolution failed");
            return static_cast<int>(ExitCode::unexpected);
        }
        if (!resolution->scenario) {
            print_diagnostics(error, resolution->diagnostics);
            return static_cast<int>(ExitCode::authored);
        }
        const auto& plan = *resolution->scenario;
        auto scenario_json = verified_projection(plan.canonical_scenario_json(),
                                                 plan.scenario_digest().text());
        auto resolved_json = verified_projection(plan.canonical_resolved_scenario_plan_json(),
                                                 plan.resolved_scenario_plan_digest().text());
        if (!scenario_json || !resolved_json) {
            const auto& failure = scenario_json ? resolved_json.error() : scenario_json.error();
            print_internal(error, failure.code.value(), "inspection envelope verification failed");
            return static_cast<int>(ExitCode::unexpected);
        }
        const auto& topology_json = resolved_json->at("topology");
        nlohmann::ordered_json envelope = {
            {"schema", "epycbench.scenario-resolution/v1"},
            {"topology_source_mode", live ? "live" : "serialized"},
            {"scenario_id", scenario_json->at("scenario_id")},
            {"scenario_digest", plan.scenario_digest().text()},
            {"resolved_scenario_plan_digest", plan.resolved_scenario_plan_digest().text()},
            {"topology", {{"physical_fingerprint", topology_json.at("physical_fingerprint")},
                           {"effective_fingerprint", topology_json.at("effective_fingerprint")}}},
            {"canonical_scenario", std::move(*scenario_json)},
            {"resolved_scenario_plan", std::move(*resolved_json)},
        };
        const auto presentation = envelope.dump(2) + '\n';
        output << presentation;
        if (!output) {
            print_internal(error, "scenario.cli.output_failed", "output stream failure");
            return static_cast<int>(ExitCode::filesystem);
        }
        return static_cast<int>(ExitCode::success);
    } catch (...) {
        print_internal(error, "scenario.cli.unexpected", "unexpected internal failure");
        return static_cast<int>(ExitCode::unexpected);
    }
}

} // namespace epycbench::cli
