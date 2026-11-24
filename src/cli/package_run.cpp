#include "cli/package_run.hpp"

#include "cli/benchmark_run_command.hpp"
#include "cli/results_command.hpp"
#include "cli/run_energy.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "cli/run_output_policy.hpp"
#include "cli/scenario_command.hpp"
#include "epycbench/scenario/parser.hpp"
#include "measurement/package_control_json.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/controller_affinity.hpp"
#include "platform_linux/current_process_image.hpp"
#include "platform_linux/journal_file.hpp"
#include "platform_linux/package_process.hpp"
#include "platform_linux/presentation_files.hpp"
#include "platform_linux/run_start_identity.hpp"
#include "results/current_build_manifest.hpp"
#include "results/package_monitoring_wire.hpp"
#include "results/package_result.hpp"
#include "runtime/package_catalog.hpp"
#include "scenario/package_mixed.hpp"
#include "scenario/package_scenario.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <ostream>

namespace epycbench::cli {
auto current_package_build() -> const std::expected<PackageBuild, core::Error>& {
    static const auto build = []() -> std::expected<PackageBuild, core::Error> {
        const auto error = [](const platform_linux::detail::ImageFailure& failure) {
            return core::Error{.category = core::ErrorCategory::runtime,
                .code = core::StableErrorCode{"runtime.package.executable_digest"},
                .message = "Cannot bind the current executable",
                .operation = "package_build", .os_error = {},
                .context = {{"errno", failure.error_number}}};
        };
        const auto image = platform_linux::detail::CurrentProcessImage::acquire();
        if (!image) return std::unexpected{error(image.error())};
        const auto executable = image->digest();
        if (!executable) return std::unexpected{error(executable.error())};
        const auto metadata = results::build_manifest::retain_current(
            {32, 8192, 256U * 1024U, 256U * 1024U, 256U * 1024U, 1024U * 1024U});
        if (!metadata) return std::unexpected{metadata.error()};
        auto document = nlohmann::json::parse(metadata->canonical());
        document["executable_sha256"] = executable->to_hex();
        auto manifest = core::canonicalize_json(document);
        if (!manifest) return std::unexpected{manifest.error()};
        const auto digest = core::sha256(*manifest);
        return PackageBuild{std::move(*manifest), digest};
    }();
    return build;
}

namespace {
using Json = nlohmann::json;
using U64 = std::uint64_t;
using SignalScope = platform_linux::detail::ControllerSignalScope;
using ControllerSignal = platform_linux::detail::ControllerSignal;
namespace control = measurement::package_control;
namespace journal_file = platform_linux::journal_file;
namespace journal = results::journal;
constexpr std::size_t mib = 1024U * 1024U;
constexpr journal_file::Limits file_limits{4095, 256, 255, 1, 3, 64 * mib, mib, 65536};

int failure(std::ostream& out, const core::Error& reason) {
    out << reason.code.value() << ' ' << reason.message << ' ' << reason.context.dump() << '\n';
    return 6;
}

int io_failure(std::ostream& out, std::string_view operation, const journal_file::Failure& reason) {
    out << operation << " failed; phase " << static_cast<int>(reason.primary.phase) << ", reason "
        << static_cast<int>(reason.primary.reason) << ", errno " << reason.primary.native_error
        << '\n';
    return 3;
}

template <class Execute> int with_signals(Execute run, std::ostream& error) {
    SignalScope signals;
    if (!signals.acquire_for_package()) {
        error << "Cannot acquire package Controller signal ownership\n";
        return 3;
    }
    int status;
    try {
        status = run(signals);
    } catch (const std::exception& reason) {
        error << "Package Run: " << reason.what() << '\n';
        status = 6;
    }
    const auto closed = signals.close();
    if (!closed) {
        error << "Cannot restore package Controller signal ownership\n";
        return 3;
    }
    if (closed->observed == ControllerSignal::interrupt)
        return 130;
    if (closed->observed == ControllerSignal::terminate)
        return 143;
    return status;
}

bool append_record(journal::Writer& journal, journal_file::File& file, std::string_view type,
                   const Json& payload, std::ostream& error) {
    const auto now = platform_linux::detail::RunStartIdentity::acquire();
    if (!now) {
        error << "Journal clock unavailable\n";
        return false;
    }
    const auto timestamp = journal::Timestamp::parse(now->utc_start());
    if (!timestamp) {
        failure(error, timestamp.error());
        return false;
    }
    auto frame = journal.append({*timestamp, type, "controller", payload});
    if (!frame) {
        failure(error, frame.error());
        return false;
    }
    auto bytes = std::as_bytes(std::span(*frame));
    while (!bytes.empty()) {
        auto wrote = file.write_some(bytes);
        if (!wrote) {
            io_failure(error, "journal write", wrote.error());
            return false;
        }
        bytes = bytes.subspan(*wrote);
    }
    const auto sync = file.sync_file();
    if (!sync) {
        io_failure(error, "journal sync", sync.error());
        return false;
    }
    return true;
}

int execute_pinned(const benchmark_run_command::Arguments& args, const Json& authored,
                     const scenario::PackagePlan& resolved,
                     const kernel::packages::Catalog& catalog, const topology::LoadedTopology& topology,
                     const std::optional<Json>& fixture, topology::cli::Environment& environment,
                     SignalScope& signals, const platform_linux::ControllerAffinity& affinity,
                     core::LinuxCpuId controller_cpu, std::ostream& output, std::ostream& error) {
    const auto digest = "sha256:" + resolved.resolved_digest.to_hex();
    if (!args.expected_plan_digest.empty() && args.expected_plan_digest != digest) {
        error << "Frozen resolved plan digest differs from installed packages, settings or "
                 "topology\n";
        return 6;
    }
    std::optional<run_energy::IncrementBound> increment_bound;
    if (!args.package_increment_bound.empty()) {
        auto read = run_energy::read_increment_bound(args.package_increment_bound);
        if (!read)
            return failure(error, read.error());
        increment_bound = std::move(*read);
    }
    const nlohmann::ordered_json output_source = authored;
    const auto directory = run_output_policy::check(output_source);
    if (!directory) {
        error << directory.error() << '\n';
        return 6;
    }
    const std::filesystem::path root{*directory};
    auto root_check = journal_file::File::preflight_root(root.string(), file_limits);
    if (!root_check)
        return io_failure(error, "Result directory preflight", root_check.error());
    std::error_code ec;
    const bool nested = std::filesystem::is_directory(root / "epycbench", ec);
    const auto journal_root = nested ? root / "epycbench" : root;
    const auto& build = current_package_build();
    if (!build) return failure(error, build.error());
    auto start = platform_linux::detail::RunStartIdentity::acquire();
    if (!start) {
        error << "Run identity unavailable\n";
        return 3;
    }
    const auto run = journal::RunId::parse(start->run_id());
    if (!run)
        return failure(error, run.error());
    const auto& source = resolved.evidence.at("scenario");
    const auto& execution = source.at("execution");
    const bool mixed = resolved.evidence.contains("mixed_protocol");
    const auto& roles = source.at("roles");
    const auto count = execution.at("blocks").get<std::size_t>();
    if (fixture && fixture->contains("calibration") && !execution.contains("calibration")) {
        error << "Supplied calibration requires a Scenario calibration policy\n";
        return 6;
    }
    if (fixture && (!fixture->at("blocks").is_array() || fixture->at("blocks").size() != count)) {
        error << "Supply one worker evidence block per measured block\n";
        return 6;
    }
    if (mixed && fixture) {
        for (const auto& block : fixture->at("blocks")) {
            if (!block.contains("alone") || !block.at("alone").is_object() ||
                block.at("alone").size() != roles.size() ||
                !std::ranges::all_of(roles, [&](const auto& role) {
                    const auto& id = role.at("id").template get_ref<const std::string&>();
                    const auto& alone = block.at("alone");
                    return alone.contains(id) && alone.at(id).is_object() &&
                           alone.at(id).contains("ranks") && alone.at(id).at("ranks").is_array();
                })) {
                error << "Supply one alone worker group for each mixed role in every block\n";
                return 6;
            }
        }
    }
    const std::string id{start->run_id()};
    const auto leaf = id + ".epycbench.journal.jsonseq";
    auto file = journal_file::File::create(journal_root.string(), leaf, file_limits);
    if (!file)
        return io_failure(error, "journal create", file.error());
    if (auto sync = file->sync_directory(); !sync)
        return io_failure(error, "journal directory sync", sync.error());
    const auto probe_limit = execution.contains("calibration")
                                 ? execution.at("calibration").at("max_probes").get<std::size_t>()
                                 : 0U;
    auto writer = journal::Writer::create(*run, {16U * mib, 32, 4096U, count + probe_limit + 2U});
    if (!writer)
        return failure(error, writer.error());
    const auto snapshot =
        environment.serialize(topology.topology, topology.stable_ids, topology.fingerprints);
    if (!snapshot)
        return failure(error, snapshot.error());
    const auto& placement = resolved.evidence.at("controller");
    Json pin{{"core", placement.at("core")}, {"cpu", controller_cpu.value()},
             {"affinity", Json::array({controller_cpu.value()})}, {"cpu_before", controller_cpu.value()}};
    Json doc{{"topology_snapshot", snapshot->canonical_json},
             {"schema", results::package_result_schema},
             {"run_id", id},
             {"authored_scenario", authored},
             {"plan", resolved.evidence},
             {"scenario_digest", "sha256:" + resolved.scenario_digest.to_hex()},
             {"resolved_plan_digest", digest},
             {"build_sha256", build->digest.to_hex()},
             {"build_manifest", build->manifest},
             {"evidence_source", fixture ? "supplied" : "native"},
             {"blocks", Json::array()}};
    if (!append_record(*writer, *file, "creation",
                       {{"plan_sha256", digest}, {"source", doc["evidence_source"]}, {"controller_placement", pin}}, error))
        return 3;
    std::optional<run_energy::Capture> energy;
    if (args.package_energy)
        energy = run_energy::Capture::begin(topology, *args.package_energy,
                                            std::move(increment_bound), fixture.has_value());
    const auto manifests = runtime::package_manifest_paths(resolved.evidence, catalog);
    const auto collect_condition = [&](std::size_t i, U64 repetitions, bool calibrating,
                                       const Json& workers, const Json& plan, const std::string& plan_digest) {
        Json request{{"plan", plan},
                     {"plan_digest", plan_digest},
                     {"ordinal", i},
                     {"manifests", manifests},
                     {"repetitions", repetitions},
                     {"calibrating", calibrating},
                     {"workers", workers}};
        auto reply = platform_linux::package_process::collect(request, &signals);
        Json block{{"ordinal", i}, {"ranks", Json::array()}, {"error", nullptr}};
        bool valid = false;
        if (reply && reply->at("ordinal") == i && reply->at("plan_digest") == plan_digest &&
            reply->at("supplied") == fixture.has_value()) {
            block = std::move(reply->at("block"));
            valid = reply->at("verified").get<bool>();
            block["runner_exit"] = 0;
        } else {
            block["error"] =
                reply
                    ? Json{{"code", "runtime.package.runner_binding"}, {"context", Json::object()}}
                    : Json{{"code", reply.error().code.value()},
                           {"message", reply.error().message},
                           {"context", reply.error().context},
                           {"errno", reply.error().os_error ? Json(reply.error().os_error->value)
                                                            : Json(nullptr)}};
            block["runner_exit"] = nullptr;
        }
        block["runner_verified"] = valid;
        return std::pair{std::move(block), valid};
    };
    const auto collect = [&](std::size_t i, U64 repetitions, bool calibrating, const Json& workers) {
        return collect_condition(i, repetitions, calibrating, workers, resolved.evidence, digest);
    };
    std::vector<std::pair<Json, std::string>> alone_plans;
    if (mixed)
        for (std::size_t i = 0; i < roles.size(); ++i) {
            auto solo = scenario::mixed_role_alone_plan(resolved.evidence, i, catalog);
            auto solo_digest = "sha256:" + core::sha256(solo.dump()).to_hex();
            alone_plans.emplace_back(std::move(solo), std::move(solo_digest));
        }
    const auto collect_mixed = [&](std::size_t ordinal, U64 repetitions) {
        auto order = scenario::mixed_condition_order(roles.size(), ordinal);
        Json together{{"ordinal", ordinal}, {"ranks", Json::array()}, {"error", nullptr},
                      {"runner_exit", nullptr}, {"runner_verified", false}};
        auto alone = Json::array();
        const Json no_workers;
        const auto& workers = fixture ? fixture->at("blocks").at(ordinal) : no_workers;
        std::size_t attempted{};
        bool operational = true;
        for (const auto condition : order) {
            if (condition == roles.size()) {
                together = collect(ordinal, repetitions, false, workers).first;
                operational = together.at("error").is_null();
            } else {
                const auto& role_id = roles.at(condition).at("id").get_ref<const std::string&>();
                const auto& [plan, plan_digest] = alone_plans.at(condition);
                auto result = collect_condition(ordinal, repetitions, false,
                    fixture ? workers.at("alone").at(role_id) : no_workers, plan, plan_digest).first;
                operational = result.at("error").is_null();
                alone.push_back({{"role", role_id}, {"block", std::move(result)}});
            }
            ++attempted;
            if (!operational) break;
        }
        together["mixed"] = {{"condition_order", std::move(order)}, {"attempted", attempted},
                             {"alone", std::move(alone)}};
        return std::pair{std::move(together), operational};
    };
    auto repetitions = execution.at("repetitions").get<U64>();
    std::optional<results::PackagePrecision> precision;
    if (execution.contains("precision"))
        precision.emplace(execution);
    bool ready = true;
    bool qualified = false;
    U64 preceding_end{};
    if (execution.contains("calibration")) {
        const auto settings = control::calibration_settings(execution);
        std::vector<control::CalibrationProbe> observations;
        Json attempts = Json::array();
        auto decision = control::next_probe(settings, roles.size(), observations);
        while (decision && decision->status == control::CalibrationStatus::next_probe) {
            const auto ordinal = attempts.size();
            const auto proposed = *decision->repetitions;
            const bool missing =
                fixture &&
                (!fixture->contains("calibration") || !fixture->at("calibration").is_array() ||
                 ordinal >= fixture->at("calibration").size() ||
                 fixture->at("calibration").at(ordinal).value("repetitions", U64{}) != proposed);
            auto [block, valid] =
                missing ? std::pair{Json{{"ordinal", ordinal},
                                         {"runner_exit", nullptr},
                                         {"runner_verified", false},
                                         {"ranks", Json::array()},
                                         {"error",
                                          {{"code", "runtime.package.calibration_fixture"},
                                           {"context", Json::object()}}}},
                                    false}
                        : collect(ordinal, proposed, true,
                                  fixture ? fixture->at("calibration").at(ordinal) : Json(nullptr));
            control::CalibrationProbe probe{proposed, {}};
            if (valid) {
                const auto intervals = results::qualified_package_intervals(
                    resolved.evidence, catalog, block, proposed, ordinal, preceding_end, false,
                    fixture.has_value(), true);
                if (!intervals)
                    block["error"] = {{"code", intervals.error().code.value()},
                                      {"message", intervals.error().message},
                                      {"context", intervals.error().context}};
                else
                    for (const auto& [begin, end] : *intervals) {
                        preceding_end = std::max(preceding_end, end);
                        probe.role_durations_ns.push_back(end - begin);
                    }
            }
            observations.push_back(std::move(probe));
            Json attempt{{"repetitions", proposed}, {"block", std::move(block)}};
            if (!append_record(*writer, *file, "calibration",
                               {{"ordinal", ordinal}, {"attempt_json", attempt.dump()}}, error))
                return 3;
            attempts.push_back(std::move(attempt));
            decision = control::next_probe(settings, roles.size(), observations);
        }
        if (!decision)
            return failure(error, decision.error());
        ready = decision->status == control::CalibrationStatus::selected;
        doc["calibration"] = {
            {"status", control::disposition(decision->status)},
            {"selected_repetitions", ready ? Json(*decision->repetitions) : Json(nullptr)},
            {"attempts", std::move(attempts)}};
        if (ready)
            repetitions = *decision->repetitions;
    }
    for (std::size_t i = 0; ready && i < count; ++i) {
        auto [block, valid] = mixed ? collect_mixed(i, repetitions) :
            collect(i, repetitions, false, fixture ? fixture->at("blocks").at(i) : Json(nullptr));
        if (valid && mixed && execution.contains("monitoring")) {
            const auto monitoring = results::package_monitoring_wire::inspect(execution, block, false,
                                                                               fixture.has_value());
            if (!monitoring) return failure(error, monitoring.error());
            valid = *monitoring == results::package_monitoring_wire::Assessment::qualified;
        }
        if (valid && !mixed) {
            bool factual_monitoring_rejection = false;
            const auto intervals = results::qualified_package_intervals(
                resolved.evidence, catalog, block, repetitions, i, preceding_end,
                execution.contains("warmup"), fixture.has_value(), false,
                precision ? &factual_monitoring_rejection : nullptr);
            valid = intervals && !intervals->empty();
            if (!intervals)
                block["error"] = {{"code", intervals.error().code.value()},
                                  {"message", intervals.error().message},
                                  {"context", intervals.error().context}};
            if (valid)
                for (const auto& role : *intervals)
                    preceding_end = std::max(preceding_end, role.second);
            if (valid && precision) {
                std::optional<std::pair<U64, U64>> durations;
                if (!factual_monitoring_rejection) {
                    const auto& baseline = intervals->at(0);
                    const auto& candidate = intervals->at(1);
                    durations = std::pair{baseline.second - baseline.first, candidate.second - candidate.first};
                }
                auto evaluation = precision->observe(i, repetitions, durations);
                if (!evaluation)
                    return failure(error, evaluation.error());
                if (*evaluation)
                    block["precision"] = std::move(**evaluation);
            }
        }
        if (!append_record(*writer, *file, "blocks", {{"ordinal", i}, {"block_json", block.dump()}},
                           error))
            return 3;
        doc["blocks"].push_back(std::move(block));
        qualified = valid;
        if (!valid || (precision && precision->attained()))
            break;
    }
    Json terminal{{"retired_blocks", doc.at("blocks").size()}};
    const auto observed = fixture ? std::expected<core::LinuxCpuId, core::Error>{controller_cpu}
                                  : affinity.observe(controller_cpu);
    if (!observed) {
        terminal["controller_placement_error"] = observed.error().code.value();
        append_record(*writer, *file, "finalization", terminal, error);
        return failure(error, observed.error());
    }
    pin["cpu_after"] = observed->value();
    terminal["controller_cpu_after"] = observed->value();
    doc["controller_placement"] = std::move(pin);
    if (precision) {
        doc["precision_stop_reason"] =
            results::package_precision_stop_reason(ready, qualified, precision->attained(), doc);
        terminal["precision_stop_reason"] = doc.at("precision_stop_reason");
    }
    if (!append_record(*writer, *file, "finalization", terminal, error))
        return 3;
    auto finished = writer->finish();
    if (!finished || !finished->last_digest) {
        error << "Unsealed package journal\n";
        return 3;
    }
    doc["journal"] = {{"file", leaf},
                      {"records", finished->count},
                      {"last_digest", finished->last_digest->to_hex()}};
    if (file->close().count) {
        error << "Package journal close failed\n";
        return 3;
    }
    doc["content_sha256"] = core::sha256(doc.dump()).to_hex();
    const auto bytes = doc.dump();
    auto inspected = results::validate_package_result(bytes);
    if (!inspected)
        return failure(error, inspected.error());
    const auto path = root / (id + ".epycbench.json");
    const auto saved = environment.publish_file(path, bytes, false);
    if (energy)
        energy->finish();
    if (!saved) {
        error << saved.error().code << ' ' << saved.error().classification << '\n';
        return 3;
    }
    if (energy) {
        auto saved_energy = run_energy::save(energy->report(), *inspected, path);
        if (!saved_energy)
            return failure(error, saved_energy.error());
    }
    const bool direct = args.expected_plan_digest.empty();
    if (direct)
        output << path.string() << '\n';
    const bool present = direct && &output == &std::cout &&
                         platform_linux::presentation_files::stdout_is_terminal();
    const auto export_status = save_results_export(*inspected, path, output, error, present);
    return inspected->validity == "valid" ? export_status : 7;
}
int execute_resolved(const benchmark_run_command::Arguments& args, const Json& authored,
                     const scenario::PackagePlan& resolved, const kernel::packages::Catalog& catalog,
                     const topology::LoadedTopology& topology, const std::optional<Json>& fixture,
                     topology::cli::Environment& environment, SignalScope& signals,
                     std::ostream& output, std::ostream& error) {
    const auto& controller = resolved.evidence.at("controller");
    const core::LinuxCpuId cpu{controller.at("cpu").get<std::uint32_t>()};
    platform_linux::ControllerAffinity affinity;
    if (fixture) {
        if (fixture->contains("controller_cpu") && (!fixture->at("controller_cpu").is_number_unsigned() ||
            fixture->at("controller_cpu") != controller.at("cpu"))) {
            error << "Supplied controller CPU differs from the resolved dedicated CPU " << cpu.value() << '\n';
            return 6;
        }
    } else {
        const auto observed = affinity.pin(cpu);
        if (!observed) return failure(error, observed.error());
    }
    error << "Controller core " << controller.at("core").get_ref<const std::string&>()
          << " · Linux CPU " << cpu.value() << (fixture ? " (supplied)\n" : " (pinned)\n");
    const int status = execute_pinned(args, authored, resolved, catalog, topology, fixture,
                                      environment, signals, affinity, cpu, output, error);
    const auto restored = affinity.restore();
    return restored ? status : failure(error, restored.error());
}
int execute(const benchmark_run_command::Arguments& args, const Json& authored,
            const kernel::packages::Catalog& catalog, const topology::LoadedTopology& topology,
            const std::optional<Json>& fixture, topology::cli::Environment& environment,
            SignalScope& signals, std::ostream& output, std::ostream& error) {
    auto resolved = scenario::resolve_package(
        authored, catalog, {topology.topology, topology.stable_ids, topology.fingerprints, &catalog});
    if (!resolved) return failure(error, resolved.error());
    return execute_resolved(args, authored, *resolved, catalog, topology, fixture, environment,
                            signals, output, error);
}

int execute_cases(const benchmark_run_command::Arguments& args, const Json& authored,
                  const kernel::packages::Catalog& catalog, const topology::LoadedTopology& topology,
                  const std::optional<Json>& fixture, topology::cli::Environment& environment,
                  SignalScope& signals, std::ostream& output, std::ostream& error) {
    if (!args.expected_plan_digest.empty()) {
        error << "A frozen trial must identify one materialized package comparison case\n";
        return 6;
    }
    const auto cases = scenario::resolve_package_cases(
        authored, catalog, {topology.topology, topology.stable_ids, topology.fingerprints, &catalog});
    if (!cases) return failure(error, cases.error());
    const auto directory = run_output_policy::check(nlohmann::ordered_json(authored));
    if (!directory) {
        error << directory.error() << '\n';
        return 6;
    }
    if (fixture) {
        if (!fixture->contains("cases") || !fixture->at("cases").is_object() ||
            fixture->at("cases").size() != cases->size()) {
            error << "Supply worker evidence indexed by every established case ID\n";
            return 6;
        }
        for (const auto& one : *cases) {
            const auto& id = one.source.at("case_id").get_ref<const std::string&>();
            const auto& provided = fixture->at("cases");
            const auto expected = one.source.at("execution").at("blocks").get<std::size_t>();
            if (!provided.contains(id) || !provided.at(id).is_object() ||
                !provided.at(id).contains("blocks") || !provided.at(id).at("blocks").is_array() ||
                provided.at(id).at("blocks").size() != expected) {
                error << "Supply " << expected << " worker evidence blocks for case " << id << '\n';
                return 6;
            }
        }
    }
    int status = 0;
    for (const auto& one : *cases) {
        if (signals.observe() != ControllerSignal::none) break;
        const auto& id = one.source.at("case_id").get_ref<const std::string&>();
        error << "Case " << id << '\n';
        const auto supplied = fixture ? std::optional<Json>{fixture->at("cases").at(id)} : std::nullopt;
        const auto outcome = execute_resolved(args, one.source, one.plan, catalog, topology,
                                              supplied, environment, signals, output, error);
        if (outcome != 0 && outcome != 7) return outcome;
        if (outcome == 7) status = outcome;
    }
    return status;
}
} // namespace

int run_package_demo(const std::optional<std::filesystem::path>& directory,
                     topology::cli::Environment& environment, std::ostream& output,
                     std::ostream& error) {
    return with_signals(
        [&](SignalScope& signals) {
            const auto path = runtime::package_directory(directory);
            if (!path)
                return failure(error, path.error());
            auto catalog = runtime::discover_packages(*path);
            if (!catalog)
                return failure(error, catalog.error());
            auto topology = load_live_topology(environment);
            if (!topology)
                return failure(error, topology.error());
            std::vector<Json> cases;
            for (const char* file :
                 {"demo-bandwidth.scenario.yaml", "demo-latency.scenario.yaml"}) {
                const auto bytes = environment.read_file(*path / file);
                if (!bytes) {
                    error << "Cannot read demonstration Scenario " << (*path / file) << '\n';
                    return 3;
                }
                auto scenario = scenario::parse_authored_scenario(
                    {file, *bytes, scenario::ScenarioFormat::yaml});
                if (!scenario)
                    return failure(error, scenario.error());
                auto admitted = scenario::resolve_package(
                    scenario->value(), *catalog,
                    {topology->topology, topology->stable_ids, topology->fingerprints, &*catalog});
                if (!admitted)
                    return failure(error, admitted.error());
                cases.push_back(scenario->value());
            }
            const auto root = platform_linux::presentation_files::ensure_private_subdirectory(".", "epycbench-demo-results");
            if (!root || !platform_linux::presentation_files::ensure_private_subdirectory(*root, "epycbench")) {
                error << "Cannot prepare the private demonstration directory\n";
                return 3;
            }
            output << "Running the saved bandwidth and latency comparisons. Results in "
                   << *root << '\n';
            for (auto& source : cases) {
                source["output"] = {{"directory", *root}, {"format", "json"}};
                const auto status = execute({}, source, *catalog, *topology, {}, environment,
                                            signals, output, error);
                if (status)
                    return status;
                if (signals.observe() != ControllerSignal::none)
                    break;
            }
            return 0;
        },
        error);
}

auto run_package(int argc, char* argv[],
                             const std::optional<std::filesystem::path>& directory,
                             topology::cli::Environment& environment, std::ostream& output,
                             std::ostream& error) -> int {
    try {
        std::vector<std::string_view> words;
        std::optional<std::filesystem::path> supplied;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--supplied-workers") {
                if (supplied || i + 1 >= argc) {
                    error << "--supplied-workers requires a fixture file\n";
                    return 2;
                }
                supplied = argv[++i];
            } else
                words.emplace_back(argv[i]);
        }
        const auto args = benchmark_run_command::parse_arguments(words);
        if (!args || args->help) {
            (args ? output : error) << "Usage: epycbench run SCENARIO [--format yaml|json] [--expect-plan sha256:DIGEST] "
                                     "[--package-energy PACKAGE [--package-energy-bound FILE]] [--supplied-workers FILE]\n";
            return args ? 0 : 2;
        }
        auto bytes = environment.read_file(args->scenario_path);
        if (!bytes) {
            error << bytes.error().code << ' ' << bytes.error().classification << '\n';
            return 3;
        }
        auto parsed = scenario::parse_authored_scenario({"run", *bytes, args->format});
        if (!parsed)
            return failure(error, parsed.error());
        if (!parsed->value().is_object() || !parsed->value().contains("schema") ||
            parsed->value().at("schema") != scenario::package_scenario_schema) {
            error << "Kernel Runs require epycbench.scenario/packages-v1. Use scenario build to "
                     "save a package Scenario.\n";
            return 6;
        }
        return with_signals(
            [&](SignalScope& signals) {
                auto catalog = runtime::discover_packages(directory);
                if (!catalog)
                    return failure(error, catalog.error());
                std::optional<Json> fixture;
                if (supplied) {
                    auto input = environment.read_file(*supplied);
                    if (!input) {
                        error << input.error().code << '\n';
                        return 3;
                    }
                    fixture = Json::parse(*input);
                    if (fixture->at("schema") != "epycbench.supplied-workers/v1") {
                        error << "Unknown supplied worker format\n";
                        return 6;
                    }
                }
                auto topology = fixture ? environment.deserialize(
                                              fixture->at("topology").dump(),
                                              topology::TopologyJsonInputPolicy::accept_equivalent)
                                        : load_live_topology(environment);
                if (!topology)
                    return failure(error, topology.error());
                if (scenario::is_package_case_group(parsed->value()))
                    return execute_cases(*args, parsed->value(), *catalog, *topology, fixture,
                                         environment, signals, output, error);
                return execute(*args, parsed->value(), *catalog, *topology, fixture, environment,
                               signals, output, error);
            },
            error);
    } catch (const std::exception& e) {
        error << "Package Run: " << e.what() << '\n';
        return 6;
    }
}
} // namespace epycbench::cli
