#pragma once

#include "topology/topology_cli.hpp"

#include <iosfwd>
#include <filesystem>
#include <optional>

namespace epycbench::cli {

[[nodiscard]] auto load_live_topology(topology::cli::Environment&)
    -> std::expected<topology::LoadedTopology, core::Error>;

[[nodiscard]] auto run_scenario_command(int argc, char* argv[],
                                        topology::cli::Environment& environment,
                                        std::ostream& output, std::ostream& error,
                                        std::optional<std::filesystem::path> module_directory = std::nullopt,
                                        std::istream* input = nullptr) -> int;

} // namespace epycbench::cli
