#pragma once

#include "epycbench/scenario/package_plan.hpp"
#include "epycbench/scenario/resource_resolution.hpp"
#include "kernel/package_manifest.hpp"

#include <set>
#include <span>

namespace epycbench::scenario {

inline constexpr std::size_t maximum_package_cases = 4096U;

template <typename Json>
[[nodiscard]] bool is_package_case_group(const Json& source) {
    return source.is_object() && source.contains("mode") && source.at("mode") == "cases";
}
[[nodiscard]] auto expand_package_cases(const nlohmann::json&)
    -> std::expected<std::vector<nlohmann::json>, core::Error>;
// For a structurally checked group, materialize one independently runnable comparison.
template <typename Json>
[[nodiscard]] auto materialize_package_case(const Json& source, std::size_t ordinal) -> nlohmann::json {
    using Leaf = nlohmann::json;
    const auto& one = source.at("cases").at(ordinal);
    Leaf leaf{{"schema", package_scenario_schema},
              {"mode", "comparison"},
              {"case_id", Leaf(one.at("id"))},
              {"workloads", Leaf(source.at("workloads"))},
              {"roles", Leaf(one.at("roles"))},
              {"execution", Leaf(source.at("execution"))}};
    if (one.contains("parameters"))
        leaf["workloads"].begin().value().at("parameters").update(Leaf(one.at("parameters")));
    if (source.contains("output"))
        leaf["output"] = Leaf(source.at("output"));
    return leaf;
}

struct ResolvedPackageCase {
    nlohmann::json source;
    PackagePlan plan;
};
[[nodiscard]] auto resolve_package_cases(const nlohmann::json&, const kernel::packages::Catalog&,
                                         const ResourceResolutionContext&)
    -> std::expected<std::vector<ResolvedPackageCase>, core::Error>;

[[nodiscard]] auto resolve_package(const nlohmann::json&, const kernel::packages::Catalog&,
                                   const ResourceResolutionContext&,
                                   std::vector<std::string>* related_settings = nullptr,
                                   std::span<const std::string> sweep_axes = {})
    -> std::expected<PackagePlan, core::Error>;

[[nodiscard]] auto replay_package(const nlohmann::json&, const kernel::packages::Catalog&,
                                  const ResourceResolutionContext&, const nlohmann::json& saved_plan)
    -> std::expected<PackagePlan, core::Error>;

// A recipe base is checked even when the sweep replaces its worker settings.
[[nodiscard]] auto validate_package_sweep_base(const nlohmann::json&, const kernel::packages::Catalog&,
                                               const ResourceResolutionContext&)
    -> std::expected<nlohmann::json, core::Error>;

void collect_package_occupied_cpu(const nlohmann::json&, std::set<core::LinuxCpuId>&);
void collect_package_occupied_cpus(const nlohmann::json&, std::set<core::LinuxCpuId>&);
[[nodiscard]] auto first_touch_page_size(const topology::Machine&, const nlohmann::json& page_mode,
                                          core::ByteCount base) -> std::optional<core::ByteCount>;
[[nodiscard]] auto choose_package_controller(const nlohmann::json& execution,
                                              const std::set<core::LinuxCpuId>& occupied,
                                              const ResourceResolutionContext&)
    -> std::expected<PackageControllerPlacement, core::Error>;

// One online, effective representative per eligible physical core, ordered by Linux CPU ID.
[[nodiscard]] auto package_controller_choices(const nlohmann::json&, const ResourceResolutionContext&)
    -> std::vector<PackageControllerPlacement>;

// Chunk count is a tuning setting only on an already streaming base Scenario.
template<class Json> bool has_stream_chunks(const Json& base) {
    return base.contains("mode") && base.at("mode") == "pipeline" &&
           base.contains("execution") && base.at("execution").contains("pipeline_chunks");
}

// The same list drives guided authoring and admission of hand-written sweep axes.
[[nodiscard]] auto package_axes(const nlohmann::json&, const kernel::packages::Catalog&)
    -> std::expected<std::vector<std::string>, core::Error>;
[[nodiscard]] auto is_package_input_axis(std::string_view path) noexcept -> bool;

} // namespace epycbench::scenario
