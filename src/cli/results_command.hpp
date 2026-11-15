#pragma once

#include "results/final_document.hpp"
#include "results/result_export.hpp"
#include "topology/topology_cli.hpp"

#include <iosfwd>

namespace epycbench::cli {
enum class ExportFormat { json, csv };
// Candidate outer-document policy; activation requires the reviewed campaign amendment.
inline constexpr std::size_t results_document_input_bytes = 256U * 1024U * 1024U;

// Fixed, source-private caller policy. No input document can enlarge it.
[[nodiscard]] auto results_validation_limits(results::event_profile::Version)
    -> results::final_document::Limits;
// Save a validated export beside its source; summary also prints readable results.
[[nodiscard]] auto save_results_export(const results::final_document::Validated&,
    results::result_export::Profile, const std::filesystem::path& source,
    std::ostream& output, std::ostream& error, ExportFormat format = ExportFormat::json) -> int;
auto run_results_command(int argc, char* argv[], topology::cli::Environment&,
                         std::ostream& output, std::ostream& error, bool save_export = false) -> int;
} // namespace epycbench::cli
