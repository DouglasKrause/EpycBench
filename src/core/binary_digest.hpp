#pragma once

#include "epycbench/core/digest.hpp"
#include <optional>

struct evp_md_ctx_st;

namespace epycbench::core {

// Binary images can include large debug sections. Use the system SHA-256
// provider when available for byte-for-byte discovery and load checks.
class BinaryDigest {
  public:
    BinaryDigest() noexcept;
    ~BinaryDigest();
    BinaryDigest(const BinaryDigest&) = delete;
    BinaryDigest& operator=(const BinaryDigest&) = delete;
    [[nodiscard]] bool append(std::span<const std::byte>) noexcept;
    [[nodiscard]] std::optional<Sha256Digest> finish();

  private:
    Sha256Stream portable_;
    [[maybe_unused]] evp_md_ctx_st* provider_{};
};
} // namespace epycbench::core
