#pragma once

#include "topology/topology_cli.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

namespace epycbench::cli {

[[nodiscard]] auto render_machine_architecture(const topology::TopologySnapshot&,
    std::string_view inspected_at, std::string_view saved_path,
    std::size_t columns=80, std::size_t rows=24) -> std::string;
[[nodiscard]] auto inspect_machine(topology::cli::Environment&,std::ostream&,std::ostream&) -> int;
[[nodiscard]] auto view_machine(topology::cli::Environment&,std::ostream&,std::ostream&) -> int;

} // namespace epycbench::cli
