#pragma once

#include "epycbench/core/error.hpp"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <expected>
#include <limits>
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
    friend class Sha256Stream;
};

class Sha256Stream {
  public:
    enum class AppendError { input_length_overflow };
    static constexpr auto maximum_input_bytes = std::numeric_limits<std::uint64_t>::max() / 8U;

    Sha256Stream() noexcept;
    // An append that would exceed the SHA-256 length field leaves the stream unchanged.
    [[nodiscard]] auto append(std::span<const std::byte>) noexcept -> std::expected<void, AppendError>;
    // Return the digest of the current prefix without consuming or changing the stream.
    [[nodiscard]] auto digest() const noexcept -> Sha256Digest;

  private:
    std::array<std::uint32_t, 8> state_;
    std::array<std::byte, 64> pending_{};
    std::uint64_t total_{};
};

[[nodiscard]] auto sha256(std::span<const std::byte> bytes) noexcept -> Sha256Digest;
[[nodiscard]] auto sha256(std::string_view bytes) noexcept -> Sha256Digest;
// Hash concatenated bytes without allocating their concatenation. Reject an
// aggregate length beyond SHA-256's representable (strictly below 2^64) bit count.
[[nodiscard]] auto sha256_chunks(std::span<const std::span<const std::byte>> chunks)
    -> std::expected<Sha256Digest, Error>;

} // namespace epycbench::core
