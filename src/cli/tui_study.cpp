#include "cli/experiment_command.hpp"
#include "cli/tui.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/workbench_terminal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>

namespace epycbench::cli::tui {
namespace {
std::string label(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}
std::string caption(std::string value) {
    std::ranges::replace(value, '_', ' ');
    return value;
}
using Document = std::shared_ptr<const Json>;
void browse(Session& s, const std::string& heading, Document doc, Json::json_pointer pointer,
            Action back, std::size_t offset = 0) {
    const auto& value = doc->at(pointer);
    s.page(heading, back);
    const auto here = [&s, heading, doc, pointer, back, offset] {
        browse(s, heading, doc, pointer, back, offset);
    };
    if (value.is_primitive()) {
        s.note(label(value));
        return;
    }
    std::size_t index = 0;
    constexpr std::size_t limit = 60;
    for (const auto& [key, child] : value.items()) {
        if (index++ < offset)
            continue;
        if (index > offset + limit)
            break;
        std::string name = caption(key);
        if (value.is_array() && child.is_object()) {
            for (const char* field : {"role", "id", "name", "state"})
                if (child.contains(field) && child.at(field).is_string()) {
                    name += "  " + child.at(field).get<std::string>();
                    break;
                }
        }
        if (child.is_structured()) {
            const auto next = pointer / key;
            s.action(
                name, [&s, doc, next, name, here] { browse(s, name, doc, next, here); },
                std::to_string(child.size()) + " entries");
        } else
            s.note(name + "   " + label(child), label(child));
    }
    if (value.empty())
        s.note("No recorded entries");
    if (offset)
        s.action("Previous page", [&s, heading, doc, pointer, back, offset] {
            browse(s, heading, doc, pointer, back, offset - limit);
        });
    if (offset + limit < value.size())
        s.action("Next page", [&s, heading, doc, pointer, back, offset] {
            browse(s, heading, doc, pointer, back, offset + limit);
        });
}
std::optional<Document> read(Session& s, std::vector<std::string> command) {
    const auto result = s.command(std::move(command));
    if (result.code) {
        s.message = result.error.empty() ? "Evidence reader failed" : result.error;
        return {};
    }
    auto value = Json::parse(result.output, nullptr, false);
    if (value.is_discarded()) {
        s.message = "The evidence reader returned an unreadable response";
        return {};
    }
    return std::make_shared<const Json>(std::move(value));
}
void detail(Session& s, std::string title, Document doc, const std::string& pointer, Action back,
            std::string panel = {}) {
    const Json::json_pointer key{pointer};
    if (!doc->contains(key))
        return;
    s.action(
        title, [&s, title, doc, key, back] { browse(s, title, doc, key, back); }, {},
        std::move(panel));
}
void reader_page(Session& s, const std::string& title, std::vector<std::string> args, Action back) {
    const auto data = read(s, std::move(args));
    if (data)
        browse(s, title, *data, Json::json_pointer(""), std::move(back));
}
void compare_search(Session& s, const Path& path, Action back) {
    s.pick_file(
        "Compare search strategies", path.parent_path(),
        [&s, path, back](Path other) {
            reader_page(s, "Search comparison",
                        {"experiment", "compare-search", path.string(),
                         std::filesystem::absolute(other).string()},
                        back);
        },
        back, true);
}
struct ResultIdentity {
    std::string run_id, digest;
};

bool matches_result(const Json& source, const ResultIdentity& expected) {
    return source.value("run_id", "") == expected.run_id &&
           source.value("content_sha256", "") == expected.digest;
}

ResultIdentity result_identity(const Json& source) {
    return {source.at("run_id").get<std::string>(), source.at("content_sha256").get<std::string>()};
}

void compare_results(Session& s, const Path& baseline, Document first, Action back);

void comparison_scope(Session& s, const Path& baseline, Document first, const Path& candidate,
                      Document second, Action back) {
    s.page("Result comparison scope",
           [&s, baseline, first, back] { compare_results(s, baseline, first, back); });
    const auto here = [&s, baseline, first, candidate, second, back] {
        comparison_scope(s, baseline, first, candidate, second, back);
    };
    const auto run = [&s, baseline, candidate, here, left = result_identity(first->at("source")),
                      right = result_identity(second->at("source"))](std::string_view role) {
        std::vector<std::string> args{"result", "compare", baseline.string(), candidate.string()};
        if (!role.empty()) {
            args.push_back("--baseline-variant=" + std::string(role));
            args.push_back("--candidate-variant=" + std::string(role));
        }
        const auto compared = read(s, std::move(args));
        if (!compared)
            return;
        if (!matches_result((*compared)->at("baseline"), left) ||
            !matches_result((*compared)->at("candidate"), right)) {
            s.message =
                "A Result changed after scope selection. Reopen it to refresh the evidence.";
            return;
        }
        browse(s, "Result comparison", *compared, Json::json_pointer(""), here);
    };
    s.note("Baseline", baseline.string());
    s.note("Candidate", candidate.string());
    if (first->at("mode") == "comparison" && second->at("mode") == "comparison") {
        for (std::size_t index = 0; index < 2U; ++index) {
            const auto left = label(first->at("roles").at(index).at("id"));
            const auto right = label(second->at("roles").at(index).at("id"));
            s.action(
                index == 0U ? "First role" : "Second role",
                [run, index] { run(index == 0U ? "first" : "second"); }, left + " across " + right,
                "Comparison role");
        }
    } else {
        const auto& mode = first->at("mode");
        s.action(
            mode == "mixed" || mode == "pipeline" ? "Whole-group comparison" : "Default comparison",
            [run] { run(""); }, "Use the retained execution scope.");
    }
}

void compare_results(Session& s, const Path& baseline, Document first, Action back) {
    s.pick_file(
        "Compare Results", baseline.parent_path(),
        [&s, baseline, first, back](Path other) {
            const auto candidate = std::filesystem::absolute(other);
            const auto second = read(s, {"result", "fingerprint", candidate.string()});
            if (second)
                comparison_scope(s, baseline, first, candidate, *second, back);
        },
        back);
}

void result(Session& s, const Path& path, Action back,
            std::optional<ResultIdentity> expected = {}) {
    s.page("Analysis · Result", back);
    const auto loaded = read(s, {"result", "fingerprint", path.string()});
    if (!loaded) {
        s.note("Invalid or unreadable Result", path.string());
        return;
    }
    const auto doc = *loaded;
    if (expected && !matches_result(doc->at("source"), *expected)) {
        s.note("Trial Result identity changed", path.string());
        s.message =
            "The Result no longer matches the Run ID and content digest recorded by the study.";
        return;
    }
    const auto& summary = doc->at("summary");
    const auto here = [&s, path, back, expected] { result(s, path, back, expected); };
    s.note(path.filename().string(), path.string(), "Saved Result");
    const auto source = summary.at("evidence_source").get<std::string>();
    s.note(source == "supplied" ? "Supplied evidence · timings are unscored" : "Native evidence",
           label(summary.at("execution")) + " · " + label(summary.at("validity")));
    s.note("Outcome   " + caption(label(summary.at("conclusion"))),
           label(summary.at("metric_scope")) + " · " + label(summary.at("execution")) + " · " +
               label(summary.at("validity")));
    s.note("Verified blocks   " + label(summary.at("verified_blocks")) + "    Eligible   " +
           label(summary.at("eligible_blocks")));
    for (const auto& role : doc->at("roles"))
        s.note(role.at("id").get<std::string>() + "   " +
                   role.at("implementation").get<std::string>(),
               role.at("workload").get<std::string>(), "Workloads");
    for (const auto& metric : summary.at("work_metrics"))
        if (metric.at("median").is_number())
            s.note(label(metric.at("role")) + "   " + label(metric.at("median")) + " " +
                       label(metric.at("metric")),
                   label(metric.at("scope")), "Outcomes");
    for (const auto& point : summary.at("scaling")) {
        if (!point.at("oriented_ratio").is_number())
            continue;
        auto estimate = "Candidate / baseline   " + label(point.at("oriented_ratio")) + "×";
        const auto& interval = point.at("ratio_interval");
        if (interval.is_object())
            estimate += "   95% interval " + label(interval.at("lower")) + "–" +
                        label(interval.at("upper"));
        s.note(std::move(estimate), caption(label(point.at("conclusion"))), "Outcomes");
    }
    if (!summary.at("scaling").empty())
        detail(s, "Timing and uncertainty", doc, "/summary/scaling", here, "Outcomes");
    for (const auto* name : {"whole_group", "mixed_slowdown", "precision", "calibration"})
        detail(s, caption(name), doc, "/summary/" + std::string(name), here, "Outcomes");
    detail(s, "Exclusions and block dispositions", doc, "/summary/block_dispositions", here,
           "Evidence");
    detail(s, "CPU and NUMA placement", doc, "/planned_placement", here, "Evidence");
    detail(s, "Controller placement", doc, "/summary/controller_placement", here, "Evidence");
    detail(s, "Work verification", doc, "/observed_work", here, "Evidence");
    detail(s, "Monitoring", doc, "/summary/monitoring", here, "Evidence");
    detail(s, "Worker counters", doc, "/hardware_counters", here, "Evidence");
    s.action(
        "Worker timing and replay",
        [&s, path, here, expected] {
            const auto replay = read(s, {"result", "replay", path.string()});
            if (!replay)
                return;
            if (expected && !matches_result((*replay)->at("source"), *expected)) {
                s.message =
                    "The replayed Result does not match the identity recorded by the study.";
                return;
            }
            browse(s, "Worker timing and replay", *replay, Json::json_pointer(""), here);
        },
        {}, "Evidence");
    s.action(
        "Compare with another Result",
        [&s, path, doc, here] { compare_results(s, path, doc, here); }, {}, "Analysis");
    s.action(
        "Package energy",
        [&s, path, here] {
            reader_page(s, "Package energy", {"result", "energy", path.string()}, here);
        },
        {}, "Analysis");
}

std::expected<Json, std::string> read_verified_plan(Session& s, const Path& path,
                                                    const std::string& expected) {
    const auto bytes = s.environment.read_file(path);
    if (!bytes)
        return std::unexpected{bytes.error().classification};
    if (core::sha256(*bytes).to_hex() != expected)
        return std::unexpected{
            "The plan digest differs from the plan verified during study analysis."};
    return Json::parse(*bytes);
}

std::expected<Document, std::string> frozen_configuration(Session& s, const Path& study_path,
                                                          const Json& row,
                                                          const std::string& study_plan_digest) {
    const bool managed = row.contains("generation");
    const auto plan_path =
        managed ? study_path / ("generation-" + label(row.at("generation"))) / "plan.json"
                : study_path / "plan.json";
    const auto& expected =
        managed ? row.at("child_plan_sha256").get_ref<const std::string&>() : study_plan_digest;
    const auto plan = read_verified_plan(s, plan_path, expected);
    if (!plan)
        return std::unexpected{plan.error()};
    if (plan->contains("trials")) {
        const auto& trials = plan->at("trials");
        const auto matching = std::ranges::find_if(trials, [&row](const Json& trial) {
            return trial.at("trial") == row.at("trial_sha256") &&
                   trial.at("candidate") == row.at("candidate_sha256") &&
                   trial.at("problem") == row.at("problem_sha256");
        });
        if (matching != trials.end() && matching->contains("scenario"))
            return std::make_shared<const Json>(matching->at("scenario"));
    }
    return std::unexpected{"The verified plan contains no matching trial configuration."};
}

void trial(Session& s, const Path& path, Document item, const std::string& plan_digest,
           bool scientifically_stopped, Action back) {
    const auto& row = *item;
    s.page("Trial " + label(row.at("ordinal")), back);
    const auto here = [&s, path, item, plan_digest, scientifically_stopped, back] {
        trial(s, path, item, plan_digest, scientifically_stopped, back);
    };
    s.note("State   " + caption(label(row.at("state"))));
    if (scientifically_stopped)
        s.note("Excluded from study analysis", "Scientific stop");
    else if (row.contains("eligibility"))
        s.note("Study eligibility", caption(label(row.at("eligibility"))));
    if (row.contains("result_unavailable"))
        s.note("Result availability", caption(label(row.at("result_unavailable"))));
    const auto& artifact = row.at("result");
    if (artifact.is_object()) {
        const auto result_path = path / artifact.at("artifact").get<std::string>();
        const auto expected = result_identity(artifact);
        s.action("Open trial Result",
                 [&s, result_path, here, expected] { result(s, result_path, here, expected); });
    }
    const auto configuration = frozen_configuration(s, path, row, plan_digest);
    if (configuration)
        detail(s, "Frozen configuration", *configuration, "", here);
    else
        s.note("Frozen configuration unavailable", configuration.error());
    detail(s, "Trial evidence and exclusions", item, "", here);
}

struct Constraint {
    std::string_view label, option;
    bool count;
};
constexpr std::array tradeoff_constraints{
    Constraint{"Maximum workers", "--max-workers", true},
    Constraint{"Maximum physical cores", "--max-physical-cores", true},
    Constraint{"Minimum tasks per second", "--min-tasks-per-second", false},
    Constraint{"Maximum seconds per task", "--max-seconds-per-task", false},
};
constexpr std::array<const char*, 3> performance_labels{
    "Study objective", "Maximize tasks per second", "Minimize seconds per completed task"};
constexpr std::array<const char*, 3> performance_args{"", "tasks_per_second",
                                                      "seconds_per_completed_task"};
constexpr std::array<const char*, 2> resource_labels{"Workers", "Physical cores"};
constexpr std::array<const char*, 2> resource_args{"workers", "physical-cores"};

struct TradeoffOptions {
    std::size_t performance{}, resource{};
    std::array<std::string, tradeoff_constraints.size()> constraints;
};

bool valid_constraint(std::string_view input, bool count) {
    if (input.empty())
        return true;
    const auto end = input.data() + input.size();
    if (count) {
        std::size_t value{};
        const auto parsed = std::from_chars(input.data(), end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end && value > 0U;
    }
    double value{};
    const auto parsed = std::from_chars(input.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value) && value > 0.0;
}

void tradeoff_options(Session& s, const Path& path, const std::string& plan_digest, Action back,
                      TradeoffOptions options = {}) {
    s.page("Tradeoff options", back);
    const auto here = [&s, path, plan_digest, back, options] {
        tradeoff_options(s, path, plan_digest, back, options);
    };
    s.action(
        "Performance objective",
        [&s, path, plan_digest, back, options, here] {
            s.choose(
                "Performance objective", {performance_labels.begin(), performance_labels.end()},
                [&s, path, plan_digest, back, options](std::size_t selection) mutable {
                    options.performance = selection;
                    tradeoff_options(s, path, plan_digest, back, std::move(options));
                },
                here);
        },
        performance_labels.at(options.performance), "Objectives");
    s.action(
        "Resource objective",
        [&s, path, plan_digest, back, options, here] {
            s.choose(
                "Resource objective", {resource_labels.begin(), resource_labels.end()},
                [&s, path, plan_digest, back, options](std::size_t selection) mutable {
                    options.resource = selection;
                    tradeoff_options(s, path, plan_digest, back, std::move(options));
                },
                here);
        },
        resource_labels.at(options.resource), "Objectives");
    for (std::size_t i = 0; i < tradeoff_constraints.size(); ++i) {
        const auto field = tradeoff_constraints[i];
        const auto& current = options.constraints[i];
        const auto problem =
            field.count ? "Enter a positive whole number" : "Enter a positive finite number";
        auto detail = current.empty() ? "Any" : current;
        if (!valid_constraint(current, field.count))
            detail += " · " + std::string(problem);
        s.action(
            std::string(field.label),
            [&s, path, plan_digest, back, options, here, i, field] {
                s.input(
                    std::string(field.label) + " · blank for any", options.constraints[i],
                    [&s, path, plan_digest, back, options, i](std::string input) mutable {
                        options.constraints[i] = std::move(input);
                        tradeoff_options(s, path, plan_digest, back, std::move(options));
                    },
                    here);
            },
            std::move(detail), "Constraints");
    }
    s.action(
        "Analyze tradeoffs",
        [&s, path, plan_digest, options, here] {
            std::vector<std::string> args{"experiment", "tradeoffs", path.string(),
                                          "--resource=" +
                                              std::string(resource_args.at(options.resource))};
            if (options.performance)
                args.push_back("--performance=" +
                               std::string(performance_args.at(options.performance)));
            for (std::size_t i = 0; i < tradeoff_constraints.size(); ++i) {
                const auto& input = options.constraints[i];
                const auto field = tradeoff_constraints[i];
                if (!valid_constraint(input, field.count)) {
                    s.message =
                        std::string(field.label) + " must be " +
                        (field.count ? "a positive whole number." : "a positive finite number.");
                    return;
                }
                if (!input.empty())
                    args.push_back(std::string(field.option) + "=" + input);
            }
            const auto report = read(s, std::move(args));
            if (!report)
                return;
            if ((*report)->at("plan_sha256") != plan_digest) {
                s.message = "The saved plan changed. Reopen the study to refresh the evidence.";
                return;
            }
            browse(s, "Tradeoff report", *report, Json::json_pointer(""), here);
        },
        "Read retained eligible evidence.");
}

void study(Session& s, const Path& path, std::size_t offset) {
    const auto home = [&s] { s.home(); };
    s.page("Analysis · Study", home);
    const auto status = read(s, {"experiment", "status", path.string()});
    if (!status) {
        s.note("Invalid or unreadable study", path.string());
        return;
    }
    const auto analyzed = read(s, {"experiment", "analyze", path.string(), "--offset",
                                   std::to_string(offset), "--limit", "12"});
    if (!analyzed)
        return;
    const auto doc = *analyzed;
    const auto here = [&s, path, offset] { study(s, path, offset); };
    const auto& state = **status;
    const auto plan_digest = doc->at("plan_sha256").get<std::string>();
    if (state.at("plan_sha256") != plan_digest) {
        s.message = "The saved plan changed while reading the study. Reopen the study to refresh "
                    "the evidence.";
        return;
    }
    const bool scientifically_stopped = state.at("state") == "scientifically_stopped" ||
                                        doc->value("state", "") == "scientifically_stopped";
    s.note(path.filename().string(), path.string(), "Frozen study");
    const auto state_label =
        scientifically_stopped ? "scientifically stopped" : caption(label(state.at("state")));
    s.note("State   " + state_label + "    Attempts   " + label(state.at("charged")) + " / " +
           label(state.at("attempt_budget")));
    if (state.contains("outcomes")) {
        const auto& outcomes = state.at("outcomes");
        s.note("Completed   " + label(outcomes.at("completed")) + "    Invalid   " +
               label(outcomes.at("invalid")) + "    Interrupted   " +
               label(outcomes.at("interrupted")) + "    Failed   " + label(outcomes.at("failed")));
    }
    detail(s, "Controller placement", *status, "/controller", here);
    s.note("Objective   " + caption(label(doc->at("objective"))),
           caption(label(doc->at("purpose"))), "Protocol");
    for (const auto* key : {"best_verified", "incumbent"})
        if (!scientifically_stopped && state.contains(key) && state.at(key).is_object())
            s.note(caption(key) + "   candidate " + label(state.at(key).at("ordinal")),
                   label(state.at(key).at("tasks_per_second")) + " tasks/s", "Search outcomes");
    const auto& distributions = doc->at("distributions");
    for (const auto& group : distributions)
        s.note("Median ratio   " + label(group.at("median")),
               "Run range " + label(group.at("minimum")) + "–" + label(group.at("maximum")) +
                   " · " + std::to_string(group.at("trials").size()) + " on this page",
               "Measured distributions");
    if (scientifically_stopped)
        s.note("Excluded from scoring · scientific stop");
    detail(s, "Measured distributions on this page", doc, "/distributions", here);
    detail(s, "Rejected proposals", doc, "/rejected_proposals", here);
    detail(s, "Confirmation evidence", doc, "/confirmations", here);
    detail(s, "Confirmation evidence", doc, "/confirmation", here);
    for (const auto& row : doc->at("trials")) {
        const auto& data = row.at("result");
        const auto ordinal = label(row.at("ordinal"));
        std::string outcome = caption(label(row.at("state")));
        if (row.contains("generation"))
            outcome = "generation " + label(row.at("generation")) + " · " + outcome;
        if (data.is_object())
            outcome += " · " + label(data.at("summary").at("evidence_source")) + " · " +
                       label(data.at("summary").at("validity"));
        s.action(
            "Trial " + ordinal + "   " + outcome,
            [&s, path, row, here, plan_digest, scientifically_stopped] {
                trial(s, path, std::make_shared<const Json>(row), plan_digest,
                      scientifically_stopped, here);
            },
            row.value("candidate_sha256", ""), "Configurations and trials");
    }
    if (offset > 0)
        s.action("Previous trials",
                 [&s, path, offset] { study(s, path, offset >= 12 ? offset - 12 : 0); });
    if (doc->contains("next_offset") && doc->at("next_offset").is_number_unsigned())
        s.action("Next trials", [&s, path, next = doc->at("next_offset").get<std::size_t>()] {
            study(s, path, next);
        });
    s.action(
        "Tradeoffs",
        [&s, path, plan_digest, here] { tradeoff_options(s, path, plan_digest, here); }, {},
        "Analysis");
    if (state.contains("completed_generations")) {
        s.action(
            "Search history and replay",
            [&s, path, here] {
                reader_page(s, "Search history", {"experiment", "replay", path.string()}, here);
            },
            {}, "Analysis");
        s.action(
            "Compare search strategies", [&s, path, here] { compare_search(s, path, here); }, {},
            "Analysis");
    } else {
        s.action(
            "Plan and search history",
            [&s, path, plan_digest, here] {
                auto plan = read_verified_plan(s, path / "plan.json", plan_digest);
                if (!plan) {
                    s.message = plan.error();
                    return;
                }
                const auto pointer =
                    plan->contains("exploration") ? "/exploration/proposals" : "/trials";
                browse(s, "Plan and search history", std::make_shared<const Json>(std::move(*plan)),
                       Json::json_pointer(pointer), here);
            },
            "Frozen dispatch order and recorded proposals.", "Analysis");
    }
    s.action("Study execution", [&s, path] { s.execution(path); }, {}, "Study");
}

void queue_attempt(Session& s, Path path) {
    s.after_loop = [&s, path] {
        CommandResult run;
        bool pause = false;
        platform_linux::detail::ControllerSignalScope signals;
        if (!signals.acquire_for_package()) {
            s.running = false;
            s.message = "Cannot acquire coordinator signals for the study attempt";
            return;
        }
        {
            platform_linux::WorkbenchAttemptTerminal quiet;
            const auto ready = quiet.open(
                "EpycBench study attempt · P pauses after this attempt · Ctrl+C interrupts\n");
            if (!ready) {
                s.running = false;
                s.message = "Cannot prepare quiet terminal: " + ready.error();
                return;
            }
            if (!*ready) {
                s.running = false;
                s.message = "Paused before dispatch. The remaining attempt budget is available.";
                return;
            }
            std::vector<std::string> args{"epycbench", "experiment", "run", path.string(),
                                          "--one-attempt"};
            if (s.supplied_workers) {
                args.emplace_back("--supplied-workers");
                args.push_back(s.supplied_workers->string());
            }
            std::vector<char*> argv;
            for (auto& arg : args)
                argv.push_back(arg.data());
            std::ostringstream output, error;
            run.code =
                run_experiment_command(static_cast<int>(argv.size()), argv.data(), s.environment,
                                       output, error, s.module_directory, &signals);
            pause = quiet.pause_requested();
            if (run.code)
                run.error = error.str() + quiet.diagnostic_tail();
            if (const auto issue = quiet.restore()) {
                s.running = false;
                s.message =
                    "Cannot restore the terminal after the study attempt: " + issue.message();
                return;
            }
        }
        const auto closed = signals.close();
        if (!closed) {
            s.running = false;
            s.message = "Cannot restore coordinator signals after study attempt";
            return;
        }
        if (closed->observed != platform_linux::detail::ControllerSignal::none && run.code == 0)
            run.code =
                closed->observed == platform_linux::detail::ControllerSignal::interrupt ? 130 : 143;
        s.running = !pause && run.code == 0;
        s.execution(path);
        if (pause)
            s.message = "Paused after this attempt. Remaining budget preserved.";
        if (run.code)
            s.message = "Attempt ended with status " + std::to_string(run.code) + ". " + run.error;
    };
}
} // namespace
void Session::analysis(Path path) {
    authoring = false;
    running = false;
    std::error_code ec;
    path = std::filesystem::absolute(path, ec);
    if (ec) {
        message = "Cannot open the evidence path · " + ec.message();
        return;
    }
    if (std::filesystem::is_directory(path, ec))
        study(*this, path, 0);
    else
        result(*this, path, [this] { home(); });
}
void Session::execution(Path path) {
    authoring = false;
    const auto here = [this, path] { execution(path); };
    page("Study execution", [this] {
        running = false;
        home();
    });
    const auto receipt = read(*this, {"experiment", "status", path.string()});
    if (!receipt) {
        running = false;
        return;
    }
    const auto& status = **receipt;
    const auto state = status.at("state").get<std::string>();
    const bool terminal = state == "exhausted" || state == "scientifically_stopped";
    if (terminal)
        running = false;
    note(path.filename().string(), path.string(), "Frozen study");
    note("State   " + caption(state) + "    Attempts   " + label(status.at("charged")) + " / " +
         label(status.at("attempt_budget")));
    if (status.contains("controller"))
        note("Controller   CPU " + label(status.at("controller").at("cpu")),
             label(status.at("controller").at("core")));
    if (supplied_workers)
        note("Supplied workers · unscored", supplied_workers->string());
    if (!terminal) {
        action(
            "Start or resume study",
            [this, path] {
                running = true;
                queue_attempt(*this, path);
            },
            "Run continuously. Press P during an attempt to pause at its boundary.");
        action(
            "Scientific stop",
            [this, path, here] {
                choose(
                    "Permanently stop this study?",
                    {"Keep remaining attempts", "Record terminal scientific stop"},
                    [this, path, here](std::size_t i) {
                        if (i == 0) {
                            here();
                            return;
                        }
                        auto stop = command({"experiment", "stop", path.string()});
                        running = false;
                        here();
                        if (stop.code)
                            message = stop.error;
                    },
                    here);
            },
            "Closes the study ledger and its remaining budget.");
    }
    action("Open Analysis", [this, path] { analysis(path); });
    if (running)
        queue_attempt(*this, path);
}
} // namespace epycbench::cli::tui
