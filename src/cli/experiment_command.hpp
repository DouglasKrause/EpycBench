#pragma once

#include "epycbench/core/digest.hpp"
#include "topology/topology_cli.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>

namespace epycbench::platform_linux::detail { class ControllerSignalScope; }
namespace epycbench::kernel::packages { class Catalog; }
namespace epycbench::scenario {
class ConfigurationSpace;
enum class ScenarioFormat;
}

namespace epycbench::cli {

struct ReviewedPlanInputs {
    core::Sha256Digest recipe, topology, packages;
    scenario::ScenarioFormat recipe_format;
};

[[nodiscard]] auto plan_package_digest(const scenario::ConfigurationSpace&,
                                       const kernel::packages::Catalog&) -> core::Sha256Digest;

[[nodiscard]] auto run_experiment_command(int, char*[], topology::cli::Environment&,
    std::ostream& output, std::ostream& error,
    std::optional<std::filesystem::path> module_directory,
    platform_linux::detail::ControllerSignalScope* active_signals = nullptr,
    const ReviewedPlanInputs* reviewed = nullptr) -> int;

} // namespace epycbench::cli
