#include "core/binary_digest.hpp"

#ifdef EPYCBENCH_BINARY_DIGEST_OPENSSL
#include <openssl/evp.h>
#endif

namespace epycbench::core {
BinaryDigest::BinaryDigest() noexcept {
#ifdef EPYCBENCH_BINARY_DIGEST_OPENSSL
    provider_ = EVP_MD_CTX_new();
    if (provider_ && EVP_DigestInit_ex(provider_, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(provider_);
        provider_ = nullptr;
    }
#endif
}
BinaryDigest::~BinaryDigest() {
#ifdef EPYCBENCH_BINARY_DIGEST_OPENSSL
    EVP_MD_CTX_free(provider_);
#endif
}
bool BinaryDigest::append(std::span<const std::byte> block) noexcept {
#ifdef EPYCBENCH_BINARY_DIGEST_OPENSSL
    if (provider_)
        return EVP_DigestUpdate(provider_, block.data(), block.size()) == 1;
#endif
    return portable_.append(block).has_value();
}
std::optional<Sha256Digest> BinaryDigest::finish() {
#ifdef EPYCBENCH_BINARY_DIGEST_OPENSSL
    if (provider_) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
        unsigned int size{};
        if (EVP_DigestFinal_ex(provider_, result.data(), &size) != 1 || size != 32U)
            return {};
        constexpr std::string_view digits = "0123456789abcdef";
        std::array<char, 64> hex;
        for (std::size_t i = 0; i < size; ++i) {
            hex[2U * i] = digits[result[i] >> 4U];
            hex[2U * i + 1U] = digits[result[i] & 15U];
        }
        return *Sha256Digest::parse({hex.data(), hex.size()});
    }
#endif
    return portable_.digest();
}
} // namespace epycbench::core
