#pragma once

#include "topology/topology_cli.hpp"

#include <iosfwd>

namespace epycbench::cli {

[[nodiscard]] auto run(int argc, char* argv[], topology::cli::Environment& environment,
                       std::ostream& output, std::ostream& error) -> int;

} // namespace epycbench::cli
