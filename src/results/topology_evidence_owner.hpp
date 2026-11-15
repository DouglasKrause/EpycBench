#pragma once
#include "epycbench/topology/fingerprint.hpp"
#include <optional>

namespace epycbench::results::topology_evidence {
struct Limits {
    std::size_t bytes, tokens, depth, string_token, scalar_token, objects, processing_units,
        numa_nodes, groups, array_entries, expanded_ranges, ancestry, derived_ids, retained;
};
struct Usage {
    std::size_t bytes{}, tokens{}, depth{}, string_token{}, scalar_token{}, objects{},
        processing_units{}, numa_nodes{}, groups{}, array_entries{}, expanded_ranges{},
        ancestry{}, derived_ids{}, retained{};
    auto operator==(const Usage&) const -> bool = default;
};
class Owner {
  public:
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view { return bytes_; }
    [[nodiscard]] auto fingerprints() const noexcept -> const topology::TopologyFingerprints* {
        return identities_ ? &*identities_ : nullptr;
    }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest* {
        return digest_ ? &*digest_ : nullptr;
    }
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
  private:
    Owner(std::string, topology::TopologyFingerprints, core::Sha256Digest, Usage);
    std::string bytes_;
    std::optional<topology::TopologyFingerprints> identities_;
    std::optional<core::Sha256Digest> digest_;
    Usage usage_;
    friend auto retain(std::string_view, Limits, std::optional<topology::TopologyFingerprints>)
        -> std::expected<Owner, core::Error>;
};
[[nodiscard]] constexpr auto supported_limits() noexcept -> Limits {
    return {1048576, 65536, 32, 4096, 32, 4096, 1024, 64, 128, 16384,
            16384, 8, 16777216, sizeof(Owner) + 1048576};
}
[[nodiscard]] auto retain(std::string_view, Limits = supported_limits(),
                          std::optional<topology::TopologyFingerprints> = std::nullopt)
    -> std::expected<Owner, core::Error>;
#ifdef EPYCBENCH_TOPOLOGY_OWNER_TESTING
// Compile-time instrumentation in the test's private source copy only.
namespace testing { inline std::size_t dom_calls{}, reload_calls{}; }
#endif
} // namespace epycbench::results::topology_evidence
