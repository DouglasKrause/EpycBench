#include "epycbench/core/digest.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

namespace epycbench::core {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

auto digest_error(std::string code, std::string message, std::string classification) -> Error {
    return Error{
        .category = ErrorCategory::core,
        .code = StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = "parse_sha256_digest",
        .os_error = std::nullopt,
        .context = nlohmann::json{{"classification", std::move(classification)}},
        .severity = ErrorSeverity::error,
        .continuation = ContinuationPolicy::abort_command,
    };
}

void compress(std::array<std::uint32_t, 8>& state, const std::byte* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4U;
        words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
                       (std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
                       (std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
                       std::to_integer<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto s0 = std::rotr(words[index - 15U], 7) ^
                        std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
        const auto s1 = std::rotr(words[index - 2U], 17) ^
                        std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto temporary1 = h + sum1 + choose + round_constants[index] + words[index];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

auto Sha256Digest::parse(std::string_view text) -> std::expected<Sha256Digest, Error> {
    if (text.size() != 64U) {
        return std::unexpected{digest_error("core.digest.invalid_length",
                                            "SHA-256 digest text has an invalid length",
                                            "invalid_length")};
    }
    std::array<std::byte, 32> parsed{};
    const auto nibble = [](char character) -> std::optional<std::uint8_t> {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<std::uint8_t>(character - 'a' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = text[index];
        if (character >= 'A' && character <= 'F') {
            return std::unexpected{digest_error("core.digest.noncanonical_case",
                                                "SHA-256 digest text must use lowercase hexadecimal",
                                                "noncanonical_case")};
        }
        const auto value = nibble(character);
        if (!value) {
            return std::unexpected{digest_error("core.digest.invalid_character",
                                                "SHA-256 digest text contains a nonhexadecimal character",
                                                "invalid_character")};
        }
        const auto byte_index = index / 2U;
        const auto shifted = index % 2U == 0U ? static_cast<std::uint8_t>(*value << 4U) : *value;
        parsed[byte_index] |= static_cast<std::byte>(shifted);
    }
    return Sha256Digest{parsed};
}

auto Sha256Digest::to_hex() const -> std::string {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(64);
    for (const auto byte : bytes_) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

auto sha256(std::span<const std::byte> bytes) noexcept -> Sha256Digest {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t),
                  "SHA-256 input length must fit its 64-bit length field");
    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
    std::size_t offset = 0;
    while (bytes.size() - offset >= 64U) {
        compress(state, bytes.data() + offset);
        offset += 64U;
    }

    const auto remainder = bytes.size() - offset;
    std::array<std::byte, 128> final_blocks{};
    std::ranges::copy(bytes.subspan(offset), final_blocks.begin());
    final_blocks[remainder] = std::byte{0x80};
    const auto final_size = remainder < 56U ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (std::size_t index = 0; index < 8U; ++index) {
        final_blocks[final_size - 1U - index] =
            static_cast<std::byte>(bit_length >> (index * 8U));
    }
    compress(state, final_blocks.data());
    if (final_size == 128U) {
        compress(state, final_blocks.data() + 64U);
    }

    std::array<std::byte, 32> result{};
    for (std::size_t word = 0; word < state.size(); ++word) {
        for (std::size_t byte = 0; byte < 4U; ++byte) {
            result[word * 4U + byte] =
                static_cast<std::byte>(state[word] >> ((3U - byte) * 8U));
        }
    }
    return Sha256Digest{result};
}

auto sha256(std::string_view bytes) noexcept -> Sha256Digest {
    return sha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}

} // namespace epycbench::core
