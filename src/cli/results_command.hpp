#pragma once

#include "results/final_document.hpp"
#include "topology/topology_cli.hpp"

#include <iosfwd>

namespace epycbench::cli {
// Candidate outer-document policy; activation requires the reviewed campaign amendment.
inline constexpr std::size_t results_document_input_bytes = 256U * 1024U * 1024U;

// Fixed, source-private caller policy. No input document can enlarge it.
[[nodiscard]] auto results_validation_limits(results::event_profile::Version)
    -> results::final_document::Limits;
auto run_results_command(int argc, char* argv[], topology::cli::Environment&,
                         std::ostream& output, std::ostream& error) -> int;
} // namespace epycbench::cli
