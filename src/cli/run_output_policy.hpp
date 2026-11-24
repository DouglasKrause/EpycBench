#pragma once

#include "cli/run_output_directory.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <expected>
#include <string_view>

namespace epycbench::cli::run_output_policy {

// Both views borrow the Scenario's directory. They only check authored settings.
using Directory = std::expected<std::string_view, std::string_view>;
inline constexpr std::string_view invalid_directory =
    "/output/directory must specify an absolute Run-compatible Result directory";
inline constexpr std::string_view invalid_retention =
    "/output/retain must preserve all seven canonical evidence fields";

[[nodiscard]] inline auto check(std::string_view directory, bool complete_evidence) -> Directory {
    if (!run_output_directory::valid(directory)) return std::unexpected{invalid_directory};
    if (!complete_evidence) return std::unexpected{invalid_retention};
    return directory;
}

[[nodiscard]] inline auto check(const nlohmann::ordered_json& document) -> Directory {
    const auto output = document.find("output");
    if (output == document.end() || !output->is_object()) return std::unexpected{invalid_directory};
    const auto directory = output->find("directory");
    if (directory == output->end() || !directory->is_string()) return std::unexpected{invalid_directory};
    if (output->contains("format") && output->at("format") != "json")
        return std::unexpected{std::string_view{"/output/format must be json"}};
    bool complete = true;
    if (const auto retain = output->find("retain"); retain != output->end()) {
        if (!retain->is_object()) return std::unexpected{invalid_retention};
        constexpr std::array fields{"authored_scenario", "normalized_scenario", "resolved_plan",
            "topology_snapshot", "warmups", "raw_observations", "monitoring_samples"};
        for (const auto* field : fields)
            if (retain->contains(field) && retain->at(field) != true) complete = false;
    }
    return check(directory->get_ref<const std::string&>(), complete);
}

} // namespace epycbench::cli::run_output_policy
