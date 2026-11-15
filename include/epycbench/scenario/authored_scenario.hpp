#pragma once

#include <nlohmann/json.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::scenario {

struct SourceLocation {
    std::uint64_t byte_offset{};
    std::uint64_t line{};
    std::uint64_t column{};

    auto operator<=>(const SourceLocation&) const = default;
};

struct NodeLocations {
    SourceLocation value;
    std::optional<SourceLocation> mapping_key;

    auto operator<=>(const NodeLocations&) const = default;
};

class AuthoredScenario {
  public:
    AuthoredScenario(const AuthoredScenario&);
    auto operator=(const AuthoredScenario&) -> AuthoredScenario&;
    AuthoredScenario(AuthoredScenario&&) noexcept = default;
    auto operator=(AuthoredScenario&&) noexcept -> AuthoredScenario&;
    ~AuthoredScenario();
    [[nodiscard]] auto value() const noexcept -> const nlohmann::ordered_json& { return value_; }
    [[nodiscard]] auto locations() const noexcept -> const std::map<std::string, NodeLocations>& {
        return locations_;
    }
    [[nodiscard]] auto source_name() const noexcept -> std::string_view { return source_name_; }

  private:
    AuthoredScenario(nlohmann::ordered_json value,
                     std::map<std::string, NodeLocations> locations,
                     std::string source_name);

    std::map<std::string, NodeLocations> locations_;
    std::string source_name_;
    nlohmann::ordered_json value_;

    friend auto make_authored_scenario(nlohmann::ordered_json,
                                       std::map<std::string, NodeLocations>,
                                       std::string) -> AuthoredScenario;
};

} // namespace epycbench::scenario
