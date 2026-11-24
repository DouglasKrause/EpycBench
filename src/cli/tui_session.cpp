#include "cli/tui.hpp"

#include "cli/application.hpp"
#include "cli/experiment_command.hpp"
#include "cli/scenario_command.hpp"
#include "cli/tui_placement.hpp"
#include "epycbench/core/digest.hpp"
#include "epycbench/scenario/parser.hpp"
#include "platform_linux/presentation_files.hpp"
#include "runtime/package_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <utility>

namespace epycbench::cli::tui {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view draft_schema = "epycbench.workbench-draft/v1";

auto xdg(const char* name, const char* fallback) -> Path {
    if (const char* value = std::getenv(name); value && Path(value).is_absolute())
        return value;
    if (const char* home = std::getenv("HOME"); home && Path(home).is_absolute())
        return Path(home) / fallback;
    return {};
}
auto private_dir(const Path& base, std::string_view leaf) -> Path {
    if (base.empty())
        return {};
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec)
        return {};
    auto directory = platform_linux::presentation_files::ensure_private_subdirectory(base, leaf);
    return directory ? Path(*directory) : Path{};
}
bool is_study(const Path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::is_regular_file(dir / "plan.json", ec);
}

auto load_saved_target(Session& s, const Path& path)
    -> std::expected<topology::LoadedTopology, std::string> {
    auto bytes = s.environment.read_file(path);
    if (!bytes)
        return std::unexpected{bytes.error().classification};
    auto loaded =
        s.environment.deserialize(*bytes, topology::TopologyJsonInputPolicy::require_canonical);
    if (!loaded)
        return std::unexpected{loaded.error().message};
    return std::move(*loaded);
}

auto target_fingerprints(const topology::LoadedTopology& target) -> Json {
    return {{"physical", target.fingerprints.physical.to_string()},
            {"effective", target.fingerprints.effective.to_string()}};
}

bool is_draft_record(const Json& state) {
    return state.is_object() && state.contains("schema") && state.at("schema") == draft_schema &&
           state.contains("kind") && state.at("kind").is_string() && state.contains("data") &&
           (!state.contains("artifact") || state.at("artifact").is_string()) &&
           (!state.contains("topology") || state.at("topology").is_string()) &&
           (!state.contains("target") || state.at("target").is_string());
}

void restore(Session& s, const Path& path);
void open_work(Session& s, const Path& path) {
    if (is_study(path)) {
        s.execution(path);
        return;
    }
    auto document = s.load(path);
    if (!document) {
        s.message = document.error();
        return;
    }
    const auto schema = document->is_object() ? document->value("schema", "") : std::string{};
    if (schema == scenario::package_scenario_schema) {
        s.page("Saved Scenario", [&s] { s.home(); });
        s.note(path.filename().string(), path.string());
        s.action("Edit Scenario", [&s, path, value = *document] {
            s.with_target([&s, path, value] {
                s.draft_kind = "scenario";
                s.artifact = path;
                s.draft = value;
                edit_scenario(s);
            });
        });
        s.action("Create a sweep from this Scenario",
                 [&s, path] { s.with_target([&s, path] { open_sweep(s, path); }); });
    } else if (schema == scenario::package_space_schema) {
        s.page("Saved sweep recipe", [&s] { s.home(); });
        s.note(path.filename().string(), path.string());
        s.action("Plan experiment",
                 [&s, path] { s.with_target([&s, path] { open_plan(s, path); }); });
        s.action("Edit sweep", [&s, path, value = *document] {
            s.with_target([&s, path, value] {
                s.draft_kind = "sweep";
                s.artifact = path;
                s.draft = value;
                edit_sweep(s);
            });
        });
    } else {
        s.message = "Select a package Scenario, sweep recipe or frozen study.";
    }
}
void resume(Session& s) {
    s.page("Resume Work", [&s] { s.home(); });
    s.action("Open saved Scenario, recipe or study", [&s] {
        s.pick_file(
            "Open saved work", s.scenario_dir, [&s](Path p) { open_work(s, p); },
            [&s] { resume(s); }, true);
    });
    std::error_code ec;
    if (s.state_dir.empty())
        return;
    std::vector<Path> drafts;
    for (fs::directory_iterator it{s.state_dir, ec}, end; it != end && !ec; it.increment(ec)) {
        const auto path = it->path();
        if (path.filename().string().starts_with("draft-") && path.extension() == ".json")
            drafts.push_back(path);
    }
    std::ranges::sort(drafts);
    for (const auto& path : drafts) {
        const auto bytes = s.environment.read_file(path);
        if (!bytes)
            continue;
        const auto data = Json::parse(*bytes, nullptr, false);
        if (!is_draft_record(data))
            continue;
        s.action(
            "Recover " + data.value("kind", "") + " draft", [&s, path] { restore(s, path); },
            data.value("artifact", "").empty() ? path.filename().string()
                                               : data.value("artifact", ""),
            "Unfinished drafts");
    }
}
void draft_editor(Session& s, bool revalidate) {
    if (s.draft_kind == "scenario") edit_scenario(s, revalidate);
    else if (s.draft_kind == "sweep") edit_sweep(s);
    else if (s.draft_kind == "plan") edit_plan(s, revalidate);
}

void restore_text_entry(Session& s, const Json& state) {
    const auto field = state.find("editing");
    if (field == state.end() || !field->is_object()) return;
    const auto address_it = field->find("address");
    const auto text = field->find("text");
    if (address_it == field->end() || !address_it->is_object() ||
        text == field->end() || !text->is_string())
        return;
    const auto& address = *address_it;
    const auto view = address.value("view", Json(""));
    if (!view.is_string()) return;
    const auto prior_message = s.message;
    if (view == "saved-topology-path") {
        const auto directory = address.value("directory", Json());
        if (const auto* value = directory.get_ptr<const std::string*>(); value &&
            !value->contains('\0') && Path(*value).is_absolute()) {
            s.change_target([&s] { draft_editor(s, true); }, [&s] { draft_editor(s, false); }, Path(*value));
        }
    } else if (s.draft_kind == "scenario") {
        if (view == "placement-replica") restore_placement_field(s, address);
        else restore_scenario_field(s, address);
    } else if (s.draft_kind == "sweep") {
        restore_sweep_field(s, address);
    } else if (s.draft_kind == "plan") {
        restore_plan_field(s, address);
    }
    if (s.entry) s.entry->value = text->get<std::string>();
    else if (s.message.empty() || s.message == prior_message)
        s.message = "The unfinished field is unavailable for this draft and target.";
}

void restore(Session& s, const Path& path) {
    const auto raw = s.environment.read_file(path);
    if (!raw) {
        s.message = "Could not read draft: " + raw.error().code;
        return;
    }
    auto state = std::make_shared<const Json>(Json::parse(*raw, nullptr, false));
    if (!is_draft_record(*state)) {
        s.message = "Invalid workbench draft.";
        return;
    }
    const auto kind = state->at("kind").get<std::string>();
    if (kind != "scenario" && kind != "sweep" && kind != "plan") {
        s.message = "Unknown workbench draft type.";
        return;
    }
    const auto open = [&s, path, state, kind](bool bind_target) {
        s.with_target([&s, path, state, kind, bind_target] {
            s.draft_file = path;
            s.draft = state->at("data");
            s.draft_kind = kind;
            s.artifact = state->value("artifact", "");
            draft_editor(s, true);
            restore_text_entry(s, *state);
            if (bind_target)
                s.persist();
        });
    };

    const Path target = state->value("topology", "");
    auto reason = std::string{"This draft has no recorded target topology."};
    if (!target.empty()) {
        auto loaded = load_saved_target(s, target);
        if (!loaded) {
            reason =
                "Cannot reopen the draft target " + target.string() + " (" + loaded.error() + ").";
        } else if (state->contains("target_fingerprints") &&
                   state->at("target_fingerprints") != target_fingerprints(*loaded)) {
            reason = "The recorded draft topology has changed at " + target.string() + ".";
        } else {
            const bool different = s.target && (s.topology_path != target ||
                                                s.target->fingerprints != loaded->fingerprints);
            const auto label = state->value("target", target.filename().string());
            const auto use_recorded =
                [&s, open, target, label,
                 loaded = std::make_shared<topology::LoadedTopology>(std::move(*loaded)),
                 bind = !state->contains("target_fingerprints")] {
                    s.target.emplace(std::move(*loaded));
                    s.topology_path = target;
                    s.topology_label = label;
                    open(bind);
                };
            if (different) {
                s.page("Recover draft target", [&s] { resume(s); });
                s.action("Restore recorded draft target", use_recorded,
                         label + " · " + target.string());
                s.action(
                    "Keep current target and revalidate", [open] { open(true); },
                    s.topology_label + " · " + s.topology_path.string());
            } else {
                use_recorded();
            }
            return;
        }
    }
    s.change_target([open] { open(true); }, [&s] { resume(s); });
    s.message = std::move(reason) + " Choose a target topology to recover the draft.";
}
} // namespace

Session::Session(topology::cli::Environment& env, std::optional<Path> modules)
    : environment(env), module_directory(std::move(modules)), discover_catalog(runtime::discover_packages) {
    const auto state = xdg("XDG_STATE_HOME", ".local/state");
    if (!state.empty())
        state_dir = private_dir(state / "epycbench", "workbench");
    const auto data = xdg("XDG_DATA_HOME", ".local/share");
    if (!data.empty()) {
        scenario_dir = data / "epycbench" / "scenarios";
        std::error_code ec;
        fs::create_directories(scenario_dir, ec);
    }
}
scenario::ResourceResolutionContext Session::context() const {
    return {target->topology, target->stable_ids, target->fingerprints, &*catalog};
}
void Session::page(std::string heading, Action on_back) {
    rows.clear();
    selected = 0;
    entry.reset();
    title = std::move(heading);
    back = std::move(on_back);
    message.clear();
    ++revision;
}
Row& Session::action(std::string label, Action activate, std::string detail, std::string panel) {
    return rows.emplace_back(Row{std::move(label),
                                 std::move(detail),
                                 std::move(panel),
                                 std::move(activate),
                                 {},
                                 {},
                                 {},
                                 {}});
}
void Session::note(std::string label, std::string detail, std::string panel) {
    action(std::move(label), {}, std::move(detail), std::move(panel));
}
void Session::field_action(std::string label, Action open, std::string detail, std::string panel,
                           std::string key) {
    if (key.empty()) key = label;
    Json address{{"view", title}, {"panel", panel}, {"field", key}};
    auto& row = action(std::move(label), [this, open = std::move(open), address] {
        open();
        if (entry)
            entry->address = address;
    }, std::move(detail), std::move(panel));
    row.field_key = std::move(key);
}

void Session::reopen_field(const Json& address) {
    if (address.value("view", "") != title) return;
    for (const auto& row : rows) {
        if (row.field_key && address.contains("field") && address.at("field") == *row.field_key &&
            address.contains("panel") && address.at("panel") == row.panel) {
            auto open = row.activate;
            open();
            return;
        }
    }
}

void Session::field_text(const std::string& value) {
    if (!entry || entry->value == value) return;
    entry->value = value;
    if (!entry->address.is_null()) persist();
}

void Session::finish_field(Action close) {
    if (!entry || entry->address.is_null()) {
        close();
        return;
    }
    const auto editor = revision;
    auto address = std::exchange(entry->address, nullptr);
    close();
    // A rejected path may leave the same editor open for correction.
    if (entry && revision == editor)
        entry->address = std::move(address);
    persist();
}
void Session::choose(std::string heading, std::vector<std::string> labels,
                     std::function<void(std::size_t)> pick, Action cancel) {
    page(std::move(heading), std::move(cancel));
    auto callback = std::make_shared<std::function<void(std::size_t)>>(std::move(pick));
    for (std::size_t i = 0; i < labels.size(); ++i)
        action(std::move(labels[i]), [callback, i] { (*callback)(i); });
}
void Session::input(std::string heading, std::string initial,
                    std::function<void(std::string)> accept, Action cancel, Json address) {
    page(std::move(heading), std::move(cancel));
    entry.emplace(TextEntry{std::move(initial), std::move(accept), std::move(address)});
}
void Session::pick_file(std::string heading, Path directory, std::function<void(Path)> pick,
                        Action cancel, bool studies, bool save_topology_path) {
    std::error_code ec;
    directory = fs::absolute(directory.empty() ? Path(".") : directory, ec).lexically_normal();
    if (directory.has_relative_path() && !directory.has_filename())
        directory = directory.parent_path();
    const auto again = [this, heading, directory, pick, cancel, studies, save_topology_path] {
        pick_file(heading, directory, pick, cancel, studies, save_topology_path);
    };
    page(heading, cancel);
    action(
        "Enter path",
        [this, heading, directory, pick, again, save_topology_path] {
            input(
                heading + " · path", "",
                [this, directory, pick](std::string value) {
                    if (value.empty() || value.contains('\0')) {
                        message = "Enter a file or directory path.";
                        return;
                    }
                    pick((directory / Path{std::move(value)}).lexically_normal());
                },
                again, save_topology_path
                    ? Json{{"view", "saved-topology-path"}, {"directory", directory.string()}} : Json());
        },
        directory.string());
    if (directory != directory.parent_path())
        action("Parent directory", [this, heading, directory, pick, cancel, studies, save_topology_path] {
            pick_file(heading, directory.parent_path(), pick, cancel, studies, save_topology_path);
        });
    std::vector<Path> dirs, files;
    for (fs::directory_iterator it{directory, ec}, end; it != end && !ec; it.increment(ec)) {
        const auto path = it->path();
        if (path.filename().string().starts_with('.'))
            continue;
        const auto status = it->status(ec);
        if (fs::is_directory(status))
            dirs.push_back(path);
        else if (fs::is_regular_file(status) &&
                 (path.extension() == ".json" || path.extension() == ".yaml" ||
                  path.extension() == ".yml"))
            files.push_back(path);
    }
    if (ec)
        message = "Cannot browse " + directory.string() + ": " + ec.message();
    std::ranges::sort(dirs);
    std::ranges::sort(files);
    for (const auto& p : files)
        action(p.filename().string(), [pick, p] { pick(p); }, p.string(), "Files");
    for (const auto& p : dirs) {
        if (studies && is_study(p))
            action(p.filename().string(), [pick, p] { pick(p); }, p.string(), "Frozen studies");
        else
            action(
                p.filename().string() + "/",
                [this, heading, p, pick, cancel, studies, save_topology_path] {
                    pick_file(heading, p, pick, cancel, studies, save_topology_path);
                },
                p.string(), "Directories");
    }
}
void Session::home() {
    authoring = false;
    running = false;
    draft_file.clear();
    draft_kind.clear();
    draft = nullptr;
    artifact.clear();
    page("Workbench Home", [this] { quit = true; });
    action(
        "New Scenario", [this] { with_target([this] { new_scenario(*this); }); },
        "Choose a task, implementations and placement.");
    action(
        "Resume Work", [this] { resume(*this); },
        "Recover a draft or open a saved Scenario, recipe or study.");
    action(
        "Open Results",
        [this] {
            pick_file(
                "Open Results", Path("."), [this](Path p) { analysis(std::move(p)); },
                [this] { home(); }, true);
        },
        "Inspect a Result or study using its saved evidence.");
    if (!target && state_dir.empty())
        message = "Private state directory unavailable; draft recovery needs an accessible "
                  "XDG_STATE_HOME.";
}
bool Session::refresh_catalog() {
    auto discovered = discover_catalog(module_directory);
    if (!discovered) {
        message = discovered.error().message;
        return false;
    }
    if (discovered->implementations().empty()) {
        message = "No installed kernel manifests found";
        return false;
    }
    catalog.emplace(std::move(*discovered));
    return true;
}
void Session::with_target(Action ready) {
    authoring = true;
    if (!catalog && !refresh_catalog()) return;
    if (target)
        ready();
    else
        change_target(std::move(ready));
}
void Session::change_target(Action ready, Action cancel, std::optional<Path> restore_from) {
    if (!cancel)
        cancel = [this, ready] {
            if (target)
                ready();
            else
                home();
        };
    const auto apply = [this, ready](topology::LoadedTopology loaded, Path path, std::string name) {
        target.emplace(std::move(loaded));
        topology_path = std::move(path);
        topology_label = std::move(name);
        message.clear();
        if (!draft_kind.empty())
            persist();
        auto failure = std::move(message);
        ready();
        if (!failure.empty()) {
            if (!message.empty())
                message += "\n";
            message += failure;
        }
    };
    const auto open_saved = [this, apply, cancel](Path directory) {
        pick_file(
            "Select topology", std::move(directory),
            [this, apply](Path path) {
                auto loaded = load_saved_target(*this, path);
                if (!loaded) {
                    message = loaded.error();
                    return;
                }
                auto name = path.filename().string();
                apply(std::move(*loaded), std::move(path), std::move(name));
            },
            cancel, false, !draft_kind.empty());
    };
    if (restore_from) {
        open_saved(std::move(*restore_from));
        auto enter = rows.front().activate;
        enter();
        return;
    }
    page("Target topology", cancel);
    action(
        "Local machine",
        [this, apply] {
            auto loaded = load_live_topology(environment);
            if (!loaded) {
                message = loaded.error().message;
                return;
            }
            auto saved =
                environment.serialize(loaded->topology, loaded->stable_ids, loaded->fingerprints);
            if (!saved || state_dir.empty()) {
                message = "Cannot retain local topology in private workbench state";
                return;
            }
            const auto path =
                state_dir / ("topology-" + core::sha256(saved->canonical_json).to_hex() + ".json");
            auto wrote = environment.publish_file(path, saved->canonical_json, false);
            if (!wrote && wrote.error().code != "topology.cli.output_exists") {
                message = wrote.error().classification;
                return;
            }
            apply(std::move(*loaded), path, "Local machine");
        },
        "Capture the current effective topology.");
    action(
        "Saved topology",
        [open_saved] { open_saved(Path(".")); },
        "Open a topology exported by EpycBench.");
}
std::expected<Json, std::string> Session::load(const Path& path) const {
    auto bytes = environment.read_file(path);
    if (!bytes)
        return std::unexpected{path.string() + ": " + bytes.error().classification};
    const auto name = path.string();
    auto parsed = scenario::parse_authored_scenario({name, *bytes,
                                                     path.extension() == ".json"
                                                         ? scenario::ScenarioFormat::json
                                                         : scenario::ScenarioFormat::yaml});
    if (!parsed)
        return std::unexpected{parsed.error().message + " " + parsed.error().context.dump()};
    return Json(parsed->value());
}
bool Session::save(const Path& path, const Json& data) {
    auto saved = environment.publish_file(path, data.dump(2) + "\n", false);
    if (!saved) {
        message = path.string() + ": " + saved.error().classification;
        return false;
    }
    return true;
}
void Session::persist() {
    if (draft_kind.empty())
        return;
    if (state_dir.empty()) {
        message = "Cannot save draft: private state directory unavailable";
        return;
    }
    Json state = {{"schema", draft_schema},
                  {"kind", draft_kind},
                  {"data", draft},
                  {"artifact", artifact.string()},
                  {"topology", topology_path.string()},
                  {"target", topology_label}};
    if (target)
        state["target_fingerprints"] = target_fingerprints(*target);
    if (entry && !entry->address.is_null())
        state["editing"] = {{"address", entry->address}, {"text", entry->value}};
    const auto bytes = state.dump();
    if (!draft_file.empty()) {
        const auto saved = environment.publish_file(draft_file, bytes, true);
        if (!saved)
            message = "Cannot update draft: " + saved.error().classification;
        return;
    }
    for (unsigned i = 1; i <= 10000; ++i) {
        const auto path = state_dir / ("draft-" + std::to_string(i) + ".json");
        const auto saved = environment.publish_file(path, bytes, false);
        if (saved) {
            draft_file = path;
            return;
        }
        if (saved.error().code != "topology.cli.output_exists") {
            message = saved.error().classification;
            return;
        }
    }
    message = "Private draft directory is full";
}
void Session::clear_draft() {
    if (!draft_file.empty()) {
        std::error_code error;
        fs::remove(draft_file, error);
        if (error)
            message = "Cannot remove draft: " + error.message();
    }
    draft_file.clear();
    draft_kind.clear();
}
CommandResult Session::command(std::vector<std::string> words, const ReviewedPlanInputs* reviewed) {
    words.insert(words.begin(), "epycbench");
    if (!reviewed && module_directory) {
        words.emplace_back("--module-dir");
        words.push_back(module_directory->string());
    }
    std::vector<char*> argv;
    for (auto& word : words)
        argv.push_back(word.data());
    std::ostringstream output, error;
    const auto argc = static_cast<int>(argv.size());
    const auto code = reviewed ? run_experiment_command(argc, argv.data(), environment, output, error,
                                                       module_directory, nullptr, reviewed)
                               : cli::run(argc, argv.data(), environment, output, error);
    return {code, output.str(), error.str()};
}
} // namespace epycbench::cli::tui
