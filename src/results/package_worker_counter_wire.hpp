#pragma once

#include "platform_linux/package_worker_counters.hpp"

namespace epycbench::results::package_worker_counter_wire {

[[nodiscard]] auto project(const platform_linux::package_execution::CounterEvidence&) -> nlohmann::json;
[[nodiscard]] auto decode(const nlohmann::json&, core::WorkerId, std::optional<std::uint64_t> callback_start,
    std::optional<std::uint64_t> callback_end, bool supplied)
    -> std::expected<platform_linux::package_execution::CounterEvidence, core::Error>;

} // namespace epycbench::results::package_worker_counter_wire
