#pragma once

#include "epycbench/scenario/configuration_space.hpp"
#include "epycbench/topology/serialization.hpp"

#include <nlohmann/json.hpp>

namespace epycbench::experiment {
inline constexpr std::string_view package_population_profile = "epycbench.resolved-population/v3";
inline constexpr std::size_t resolved_population_maximum_bytes = 16U * 1024U * 1024U;

// Immutable resolution artifact, not authority to execute or alter a study.
// Resolution uses the already-installed catalog and performs no kernel calls.
class ResolvedPopulation {
  public:
    using BuildDigest = std::expected<core::Sha256Digest, core::Error> (*)();
    [[nodiscard]] static auto resolve(const scenario::ScenarioSource&, const topology::LoadedTopology&,
                                     BuildDigest host_build = nullptr,
                                     const kernel::packages::Catalog* packages = nullptr)
        -> std::expected<ResolvedPopulation, core::Error>;
    // Decode without modules or a host query. Source/topology hashes need their
    // original bytes at the future prospective study-admission boundary.
    [[nodiscard]] static auto decode(std::string_view)
        -> std::expected<ResolvedPopulation, core::Error>;
    [[nodiscard]] auto verify_resolution(std::size_t ordinal, const scenario::ConfigurationCandidate&) const
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto document() const noexcept -> const nlohmann::json& { return document_; }
    [[nodiscard]] auto required_host_build() const -> std::optional<core::Sha256Digest>;

  private:
    ResolvedPopulation(nlohmann::json document, std::string canonical)
        : document_(std::move(document)), canonical_{std::move(canonical)}, digest_{core::sha256(canonical_)} {}
    nlohmann::json document_;
    std::string canonical_;
    core::Sha256Digest digest_;
};
} // namespace epycbench::experiment
