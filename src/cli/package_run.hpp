#pragma once
#include "epycbench/core/digest.hpp"
#include "topology/topology_cli.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>

namespace epycbench::cli {
struct PackageBuild {
    std::string manifest;
    core::Sha256Digest digest;
};

[[nodiscard]] auto current_package_build() -> const std::expected<PackageBuild, core::Error>&;

[[nodiscard]] int run_package_demo(const std::optional<std::filesystem::path>&,
                                   topology::cli::Environment&, std::ostream&, std::ostream&);
[[nodiscard]] auto run_package(int, char*[],
                                           const std::optional<std::filesystem::path>&,
                                           topology::cli::Environment&, std::ostream&,
                                           std::ostream&) -> int;
} // namespace epycbench::cli
