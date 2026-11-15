#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::topology {

struct ReferenceFixtureLoadOptions {
    std::filesystem::path fixture_root;
};

struct ReferenceFixtureLoadError {
    core::Error primary;
    std::vector<core::Error> invariant_diagnostics;
};

class ReferenceFixtureAuthority {
  public:
    ReferenceFixtureAuthority(const ReferenceFixtureAuthority&) = default;
    ReferenceFixtureAuthority(ReferenceFixtureAuthority&&) = default;
    auto operator=(const ReferenceFixtureAuthority&) -> ReferenceFixtureAuthority& = delete;
    auto operator=(ReferenceFixtureAuthority&&) -> ReferenceFixtureAuthority& = delete;

    [[nodiscard]] auto topology() const noexcept -> const TopologySnapshot& { return topology_; }
    [[nodiscard]] auto fixture_id() const noexcept -> std::string_view { return fixture_id_; }
    [[nodiscard]] auto fixture_version() const noexcept -> std::string_view {
        return fixture_version_;
    }
    [[nodiscard]] auto manifest_sha256() const noexcept -> const core::Sha256Digest& {
        return manifest_sha256_;
    }
    [[nodiscard]] auto checksums_sha256() const noexcept -> const core::Sha256Digest& {
        return checksums_sha256_;
    }

  private:
    ReferenceFixtureAuthority(TopologySnapshot topology, std::string fixture_id,
                              std::string fixture_version, core::Sha256Digest manifest_sha256,
                              core::Sha256Digest checksums_sha256)
        : topology_{std::move(topology)}, fixture_id_{std::move(fixture_id)},
          fixture_version_{std::move(fixture_version)}, manifest_sha256_{manifest_sha256},
          checksums_sha256_{checksums_sha256} {}

    TopologySnapshot topology_;
    std::string fixture_id_;
    std::string fixture_version_;
    core::Sha256Digest manifest_sha256_;
    core::Sha256Digest checksums_sha256_;

    friend auto load_reference_fixture_authority(const ReferenceFixtureLoadOptions&)
        -> std::expected<ReferenceFixtureAuthority, ReferenceFixtureLoadError>;
    friend auto load_reference_fixture(const ReferenceFixtureLoadOptions&)
        -> std::expected<TopologySnapshot, ReferenceFixtureLoadError>;
};

[[nodiscard]] auto load_reference_fixture_authority(const ReferenceFixtureLoadOptions& options)
    -> std::expected<ReferenceFixtureAuthority, ReferenceFixtureLoadError>;

[[nodiscard]] auto load_reference_fixture(const ReferenceFixtureLoadOptions& options)
    -> std::expected<TopologySnapshot, ReferenceFixtureLoadError>;

} // namespace epycbench::topology
