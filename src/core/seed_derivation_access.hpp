#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/seed.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

namespace epycbench::core::detail {

struct SeedDerivationAccess final {
    [[nodiscard]] static constexpr auto seed(std::uint64_t value) noexcept -> DerivedSeed {
        return DerivedSeed{value};
    }
};

// The pairwise latency schedule has one fixed SeedDerivationKey shape. Encoding
// that shape into bounded storage preserves the canonical-json digest exactly
// while making admitted preparation independent of allocating JSON/string state.
[[nodiscard]] inline auto derive_observation_start_seed(
    std::uint64_t root_seed, std::uint64_t experiment_id, std::uint64_t block_id,
    std::string_view component) noexcept -> DerivedSeed {
    std::array<char, 384> storage{};
    std::size_t size{};
    const auto append_component = [&](std::string_view value) noexcept {
        if (value.size() > storage.size() - size)
            std::terminate();
        for (const auto character : value) {
            if (character == '"' || character == '\\' ||
                static_cast<unsigned char>(character) < 0x20U)
                std::terminate();
            storage[size++] = character;
        }
    };
    const auto append_literal = [&](std::string_view value) noexcept {
        if (value.size() > storage.size() - size)
            std::terminate();
        std::ranges::copy(value,
                          storage.begin() + static_cast<std::ptrdiff_t>(size));
        size += value.size();
    };
    const auto append_decimal = [&](std::uint64_t value) noexcept {
        const auto [end, error] =
            std::to_chars(storage.data() + size, storage.data() + storage.size(), value);
        if (error != std::errc{})
            std::terminate();
        size = static_cast<std::size_t>(end - storage.data());
    };

    append_literal("{\"components\":[{\"name\":\"block\",\"value\":\"");
    append_decimal(block_id);
    append_literal("\"},{\"name\":\"component\",\"value\":\"");
    append_component(component);
    append_literal("\"},{\"name\":\"experiment\",\"value\":\"");
    append_decimal(experiment_id);
    append_literal(
        "\"}],\"domain\":\"observation-start\",\"profile\":\"epycbench.seed-derivation.v1\",\"root_seed\":\"");
    append_decimal(root_seed);
    append_literal("\"}");

    const auto digest = sha256(std::string_view{storage.data(), size});
    std::uint64_t value{};
    for (std::size_t index{}; index < 8U; ++index)
        value = (value << 8U) |
                std::to_integer<std::uint64_t>(digest.bytes()[index]);
    return SeedDerivationAccess::seed(value);
}

} // namespace epycbench::core::detail
