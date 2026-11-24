#pragma once

#include <optional>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace epycbench::results::result_export {
enum class Profile { complete, shareable, summary, observation_table };
[[nodiscard]] auto profile(std::string_view) noexcept -> std::optional<Profile>;
[[nodiscard]] auto name(Profile) noexcept -> std::string_view;
} // namespace epycbench::results::result_export
