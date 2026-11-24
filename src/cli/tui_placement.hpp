#pragma once

#include "cli/tui.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::cli::tui {

enum class TopologyGroupKind { numa, cache, core, cpu };

struct TopologyGroup {
    TopologyGroupKind kind;
    std::string label;
    core::CpuSet cpus;
    std::optional<std::uint32_t> linux_node;
    std::uint32_t cache_level{};
};

enum class PlacementTarget { workers, role_memory, rank_memory, input_memory, mapping_service,
                             monitoring_service, controller };

struct PlacementRequest {
    PlacementTarget target;
    bool all_siblings{};
    bool replace_workers{};
    std::size_t rank{};
};

// The same placement checks drive highlighting, keyboard actions and mouse drops.
[[nodiscard]] auto topology_groups(const scenario::ResourceResolutionContext&) -> std::vector<TopologyGroup>;
[[nodiscard]] auto preview_placement(const Json&, const scenario::ResourceResolutionContext&,
                                     std::string_view role_pointer, const TopologyGroup&, PlacementRequest)
    -> std::expected<Json, std::string>;
[[nodiscard]] auto preview_worker_team(const Json&, const scenario::ResourceResolutionContext&,
                                       std::string_view role_pointer, std::span<const TopologyGroup> ordered_cores)
    -> std::expected<Json, std::string>;
[[nodiscard]] auto preview_worker_order(const Json&, const scenario::ResourceResolutionContext&,
                                        std::string_view role_pointer, const Json& ordered_cpus)
    -> std::expected<Json, std::string>;

void select_controller(Session&, std::vector<scenario::PackageControllerPlacement>,
                       std::function<void(std::size_t)> selected, Action cancel);
void restore_placement_field(Session&, const Json& address);

} // namespace epycbench::cli::tui
