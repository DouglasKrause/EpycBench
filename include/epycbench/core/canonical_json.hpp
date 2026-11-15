#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"

#include <cstddef>
#include <expected>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace epycbench::core {

// This API canonicalizes an existing JSON value. Raw-input codecs remain responsible for
// rejecting duplicate property names before constructing that value when duplicates matter.
[[nodiscard]] auto canonicalize_json(const nlohmann::json& value)
    -> std::expected<std::string, Error>;

[[nodiscard]] auto hash_canonical_json(const nlohmann::json& value)
    -> std::expected<Sha256Digest, Error>;

// Additive binary64 JCS profile. Legacy APIs above retain their integer-only behavior.
struct JcsLimits {
    std::size_t output_bytes;
    std::size_t depth;
    std::size_t nodes;
};
[[nodiscard]] auto canonicalize_json_jcs(const nlohmann::json& value, JcsLimits limits)
    -> std::expected<std::string, Error>;
[[nodiscard]] auto hash_canonical_json_jcs(const nlohmann::json& value, JcsLimits limits)
    -> std::expected<Sha256Digest, Error>;

} // namespace epycbench::core
