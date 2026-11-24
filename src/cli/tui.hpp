#pragma once

#include "scenario/package_scenario.hpp"
#include "topology/topology_cli.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace epycbench::cli { struct ReviewedPlanInputs; }

namespace epycbench::cli::tui {
using Json = nlohmann::json;
using Path = std::filesystem::path;
using Action = std::function<void()>;

struct Row {
    std::string label, detail, panel;
    Action activate;
    // Mouse and keyboard transfers both call drop with the same payload.
    std::string payload;
    std::function<void(std::string)> drop;
    std::function<bool(const std::string&)> accepts;
    std::function<std::string(const std::string&)> rejection;
    struct TopologyTile {
        std::size_t line;
        unsigned indent;
        std::string label;
    };
    std::optional<TopologyTile> topology_tile{};
    std::optional<std::string> field_key{};
};

struct CommandResult {
    int code{};
    std::string output, error;
};

struct Session {
    topology::cli::Environment& environment;
    std::optional<Path> module_directory;
    std::function<std::expected<kernel::packages::Catalog, core::Error>(std::optional<Path>)> discover_catalog;
    std::optional<kernel::packages::Catalog> catalog;
    std::optional<topology::LoadedTopology> target;
    Path topology_path, artifact, state_dir, scenario_dir, draft_file;
    std::string topology_label, title, message, draft_kind;
    Json draft; // Authoritative private work-in-progress. Each editor owns its kind's JSON.
    std::vector<Row> rows;
    Action back, after_loop;
    int selected{};
    bool quit{}, running{}, authoring{};
    std::optional<Path> supplied_workers;
    std::uint64_t revision{};

    Session(topology::cli::Environment&, std::optional<Path>);
    scenario::ResourceResolutionContext context() const;
    void page(std::string heading, Action on_back);
    Row& action(std::string label, Action activate, std::string detail = {},
                std::string panel = {});
    void field_action(std::string label, Action open, std::string detail = {}, std::string panel = {},
                      std::string key = {});
    void reopen_field(const Json& address);
    void field_text(const std::string& value);
    void finish_field(Action close);
    void note(std::string label, std::string detail = {}, std::string panel = {});
    void choose(std::string heading, std::vector<std::string> labels,
                std::function<void(std::size_t)> pick, Action cancel);
    void input(std::string heading, std::string initial, std::function<void(std::string)> accept,
               Action cancel, Json address = nullptr);
    void pick_file(std::string heading, Path directory, std::function<void(Path)> pick,
                   Action cancel, bool studies = false, bool save_topology_path = false);
    void home();
    bool refresh_catalog();
    void with_target(Action ready); // Lazy catalog and topology selection
    void change_target(Action ready, Action cancel = {}, std::optional<Path> restore_from = {});
    void analysis(Path);
    void execution(Path);
    void persist(); // Saves draft_kind, draft, artifact and target. Call after draft mutations.
    void clear_draft();
    std::expected<Json, std::string> load(const Path&) const; // ordinary Scenario JSON/YAML loader
    bool save(const Path&, const Json&); // exclusive publication; errors in message
    CommandResult command(std::vector<std::string> words, const ReviewedPlanInputs* reviewed = nullptr);
    struct TextEntry {
        std::string value;
        std::function<void(std::string)> accept;
        Json address;
    };
    std::optional<TextEntry> entry;
};
void restore_scenario_field(Session&, const Json&);
void restore_plan_field(Session&, const Json&);
void new_scenario(Session&);
void edit_scenario(Session&,
                   bool revalidate = false); // draft_kind="scenario", draft=ordinary Scenario
void open_sweep(Session&, Path saved_scenario);
void edit_sweep(Session&); // draft_kind="sweep"; saved recipe initially loaded as draft
void restore_sweep_field(Session&, const Json& address);
void open_plan(Session&, Path saved_recipe);
void edit_plan(Session&, bool revalidate = false); // draft_kind="plan"
void placement(Session&, std::string role_pointer, Action done);
int launch(std::optional<Path> path, topology::cli::Environment&,
           const std::optional<Path>& modules, std::ostream& error,
           std::optional<Path> supplied_workers = {});
} // namespace epycbench::cli::tui
