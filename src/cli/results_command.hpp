#pragma once

#include "topology/topology_cli.hpp"

#include <iosfwd>
#include <optional>
#include <string_view>

namespace epycbench::results {
struct PackageResult;
}

namespace epycbench::cli {
enum class ExportFormat { json, csv };
// Shared outer-document limit for saved Result readers.
inline constexpr std::size_t results_document_input_bytes = 256U * 1024U * 1024U;

[[nodiscard]] auto save_results_export(const results::PackageResult&, const std::filesystem::path& source,
    std::ostream& output, std::ostream& error, bool present) -> int;
auto run_results_command(int argc, char* argv[], topology::cli::Environment&,
                         std::ostream& output, std::ostream& error, bool save_export = false) -> int;
} // namespace epycbench::cli
