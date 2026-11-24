#pragma once

#include "cli/run_energy.hpp"

namespace epycbench::cli::run_energy {

[[nodiscard]] auto decimal(const nlohmann::json&) -> std::uint64_t;
// The canonical encoder recomputes the outcome from source snapshots, bound
// and raw observations. The decoder accepts the original facts for replay.
[[nodiscard]] auto qualification_json(const Report&) -> nlohmann::json;
[[nodiscard]] auto parse_qualification(const nlohmann::json&) -> Qualification;
[[nodiscard]] auto estimate_json(const Report&, const EstimateOptions&) -> nlohmann::json;

} // namespace epycbench::cli::run_energy
