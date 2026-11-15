#pragma once

#include "epycbench/core/error.hpp"

#include <array>
#include <cstddef>
#include <compare>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace epycbench::core {

class Sha256Digest {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<Sha256Digest, Error>;

    [[nodiscard]] auto to_hex() const -> std::string;
    [[nodiscard]] auto bytes() const noexcept -> const std::array<std::byte, 32>& {
        return bytes_;
    }

    auto operator<=>(const Sha256Digest&) const = default;

  private:
    explicit constexpr Sha256Digest(std::array<std::byte, 32> bytes) noexcept : bytes_{bytes} {}

    std::array<std::byte, 32> bytes_{};

    friend auto sha256(std::span<const std::byte> bytes) noexcept -> Sha256Digest;
};

[[nodiscard]] auto sha256(std::span<const std::byte> bytes) noexcept -> Sha256Digest;
[[nodiscard]] auto sha256(std::string_view bytes) noexcept -> Sha256Digest;

} // namespace epycbench::core
