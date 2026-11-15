#pragma once

#include "epycbench/core/error.hpp"

#include <compare>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::scenario {

class ScenarioIdentifier {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<ScenarioIdentifier, core::Error>;

    [[nodiscard]] auto value() const noexcept -> std::string_view { return value_; }
    [[nodiscard]] auto to_string() const -> std::string { return value_; }

    auto operator<=>(const ScenarioIdentifier&) const = default;

  private:
    explicit ScenarioIdentifier(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

} // namespace epycbench::scenario
