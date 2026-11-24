#pragma once
#include "epycbench/scenario/parser.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::cli::benchmark_run_command {
enum class ArgumentError { usage };
struct Arguments {
    std::string_view scenario_path;
    scenario::ScenarioFormat format{scenario::ScenarioFormat::yaml};
    bool help{};
    std::string_view expected_plan_digest{};
    std::optional<std::uint32_t> package_energy{};
    std::string_view package_increment_bound{};
};
// Arguments after the literal `run`; all views borrow the original argument
// storage. Parsing performs no allocation, acquisition or environment access.
[[nodiscard]] auto parse_arguments(std::span<const std::string_view>) noexcept
    -> std::expected<Arguments, ArgumentError>;
} // namespace epycbench::cli::benchmark_run_command
