#pragma once

#include "epycbench/core/error.hpp"

#include <expected>

namespace epycbench::results::package_monitoring_wire {

enum class Assessment { qualified, factual_rejection, acquisition_failure };

// Rebuild channel qualification from raw evidence. Optional summary collects
// per-channel dispositions and unavailable reasons across measured callbacks.
[[nodiscard]] auto inspect(const nlohmann::json& execution, const nlohmann::json& block,
                           bool calibrating, bool supplied, nlohmann::json* summary = nullptr,
                           bool include_mixed_alone = true)
    -> std::expected<Assessment, core::Error>;

} // namespace epycbench::results::package_monitoring_wire
