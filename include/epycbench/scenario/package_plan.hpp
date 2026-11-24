#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <optional>

namespace epycbench::scenario {

inline constexpr std::string_view package_scenario_schema = "epycbench.scenario/packages-v1";
inline constexpr std::string_view package_space_schema = "epycbench.configuration-space/v3";

struct PackageControllerPlacement {
    topology::StableCoreId core;
    core::LinuxCpuId cpu;

    auto operator<=>(const PackageControllerPlacement&) const = default;
};

struct PackagePlan {
    // Includes effective settings, topology and the exact package bindings. Paths and
    // caller-selected publication destinations are outside the scientific identity.
    nlohmann::json evidence;
    core::Sha256Digest scenario_digest, resolved_digest, input_identity, problem_identity,
        candidate_identity;
    std::optional<PackageControllerPlacement> controller;
};

} // namespace epycbench::scenario
