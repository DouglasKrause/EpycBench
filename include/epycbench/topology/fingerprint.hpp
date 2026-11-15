#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <compare>
#include <expected>
#include <string>
#include <string_view>

namespace epycbench::topology {

class PhysicalTopologyFingerprint {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<PhysicalTopologyFingerprint, core::Error>;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto to_string() const -> std::string;
    auto operator<=>(const PhysicalTopologyFingerprint&) const = default;

  private:
    explicit PhysicalTopologyFingerprint(core::Sha256Digest digest) : digest_{digest} {}
    core::Sha256Digest digest_;
    friend class FingerprintFactory;
};

class EffectiveTopologyFingerprint {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<EffectiveTopologyFingerprint, core::Error>;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto to_string() const -> std::string;
    auto operator<=>(const EffectiveTopologyFingerprint&) const = default;

  private:
    explicit EffectiveTopologyFingerprint(core::Sha256Digest digest) : digest_{digest} {}
    core::Sha256Digest digest_;
    friend class FingerprintFactory;
};

struct TopologyFingerprints {
    PhysicalTopologyFingerprint physical;
    EffectiveTopologyFingerprint effective;
    auto operator<=>(const TopologyFingerprints&) const = default;
};

[[nodiscard]] auto derive_topology_fingerprints(const TopologySnapshot& topology,
                                                const StableTopologyIds& stable_ids)
    -> std::expected<TopologyFingerprints, core::Error>;

} // namespace epycbench::topology
