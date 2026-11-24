#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace epycbench::kernel::packages {

enum class InputPolicy { bind, interleave, replica_per_worker, replica_per_node };

inline constexpr std::array input_policy_names{
    std::pair{std::string_view{"bind"}, InputPolicy::bind},
    std::pair{std::string_view{"interleave"}, InputPolicy::interleave},
    std::pair{std::string_view{"replica_per_worker"}, InputPolicy::replica_per_worker},
    std::pair{std::string_view{"replica_per_node"}, InputPolicy::replica_per_node}};

[[nodiscard]] constexpr auto parse_input_policy(std::string_view name) noexcept -> std::optional<InputPolicy> {
    for (const auto& [text, policy] : input_policy_names)
        if (name == text) return policy;
    return std::nullopt;
}

[[nodiscard]] constexpr auto input_policy_name(InputPolicy policy) noexcept -> std::string_view {
    for (const auto& [text, value] : input_policy_names)
        if (policy == value) return text;
    return {};
}

[[nodiscard]] constexpr auto stream_input_placement_supported(InputPolicy policy, std::size_t explicit_nodes) noexcept
    -> bool {
    return policy == InputPolicy::bind && explicit_nodes <= 1U;
}

} // namespace epycbench::kernel::packages
