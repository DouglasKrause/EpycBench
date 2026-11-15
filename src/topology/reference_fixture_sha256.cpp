#include "topology/reference_fixture_sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace epycbench::topology::detail {
namespace {

constexpr std::array<std::uint32_t, 64> constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

class Sha256 {
  public:
    void update(const std::uint8_t* data, std::size_t size) {
        total_bytes_ += size;
        while (size != 0U) {
            const auto count = std::min(size, block_.size() - block_size_);
            std::copy_n(data, count, block_.begin() + static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += count;
            data += count;
            size -= count;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    auto finish() -> std::array<std::uint8_t, 32> {
        const auto bit_count = static_cast<std::uint64_t>(total_bytes_) * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0U);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56,
                  0U);
        for (std::size_t index = 0; index < 8; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
        }
        transform(block_.data());
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[word * 4U + byte] =
                    static_cast<std::uint8_t>(state_[word] >> ((3U - byte) * 8U));
            }
        }
        return digest;
    }

  private:
    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^
                            (words[index - 15U] >> 3U);
            const auto s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^
                            (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + sum1 + choice + constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_{};
    std::size_t total_bytes_{};
};

auto hex_digest(const std::array<std::uint8_t, 32>& digest) -> std::string {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

} // namespace

auto sha256(std::string_view data) -> std::string {
    Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return hex_digest(hash.finish());
}

auto sha256_file(const std::filesystem::path& path) -> std::expected<std::string, std::string> {
    std::error_code error;
    const auto before_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected{"unable to stat input before hashing"};
    }
    const auto before_time = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::unexpected{"unable to read input timestamp before hashing"};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected{"unable to open input for hashing"};
    }
    Sha256 hash;
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                        static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        return std::unexpected{"unable to read complete input for hashing"};
    }
    const auto after_size = std::filesystem::file_size(path, error);
    if (error || before_size != after_size) {
        return std::unexpected{"input changed while hashing"};
    }
    const auto after_time = std::filesystem::last_write_time(path, error);
    if (error || before_time != after_time) {
        return std::unexpected{"input changed while hashing"};
    }
    return hex_digest(hash.finish());
}

} // namespace epycbench::topology::detail
