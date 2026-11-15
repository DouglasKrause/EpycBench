#pragma once

#include "epycbench/scenario/default_expansion.hpp"

#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace epycbench::scenario::detail {

enum class DefaultFamily {
    memory,
    worker_selector,
    comparison,
    repetition,
    observation,
    calibration,
    warmup,
    stopping,
    statistics,
    random_seed,
};

struct DefaultBinding {
    std::string_view target_pattern;
    DefaultFamily family;
    std::size_t traversal_rank;
};

[[nodiscard]] auto scenario_v1_default_bindings() noexcept -> std::span<const DefaultBinding>;

[[nodiscard]] auto expand_v1_defaults(nlohmann::ordered_json& effective,
                                      std::map<std::string, ValueOrigin>& origins)
    -> std::expected<void, core::Error>;

[[nodiscard]] auto make_defaults_internal_error(std::string classification,
                                                std::string binding = {}) -> core::Error;

} // namespace epycbench::scenario::detail
