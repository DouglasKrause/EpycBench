#pragma once

#include "epycbench/core/error.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace epycbench::core {

namespace detail {
struct SeedDerivationAccess;
}

enum class SeedDomain {
    worker_selection,
    pointer_cycle,
    block_order,
    bootstrap,
    observation_start,
};

struct SeedComponent {
    std::string name;
    std::string value;

    auto operator<=>(const SeedComponent&) const = default;
};

struct SeedDerivationKey {
    SeedDomain domain;
    std::vector<SeedComponent> components;

    auto operator<=>(const SeedDerivationKey&) const = default;
};

class DerivedSeed {
  public:
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }

    auto operator<=>(const DerivedSeed&) const = default;

  private:
    explicit constexpr DerivedSeed(std::uint64_t value) noexcept : value_{value} {}

    std::uint64_t value_{};

    friend auto derive_seed(std::uint64_t root_seed, const SeedDerivationKey& key)
        -> std::expected<DerivedSeed, Error>;
    friend struct detail::SeedDerivationAccess;
};

[[nodiscard]] auto derive_seed(std::uint64_t root_seed, const SeedDerivationKey& key)
    -> std::expected<DerivedSeed, Error>;

} // namespace epycbench::core
