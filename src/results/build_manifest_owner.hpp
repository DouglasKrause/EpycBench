#pragma once
#include "epycbench/core/digest.hpp"
#include <memory>
#include <optional>
#include <span>

namespace epycbench::results::build_manifest {
inline constexpr std::string_view profile = "epycbench.results.build-manifest.v1";
enum class Feature { linux_platform, cxx23, testing, asan_ubsan, tsan, native_reference };
struct Git {
    std::string_view revision;
    bool dirty;
};
struct Compiler {
    std::string_view id, version, target, cxx_standard, flags;
};
struct Linker {
    std::string_view id, version;
};
struct Dependencies {
    std::string_view hwloc, libnuma, libfyaml, cli11, nlohmann_json, fmt, catch2;
};
struct Input {
    std::string_view project_version;
    Git git;
    Compiler compiler;
    Linker linker;
    std::string_view preset;
    std::span<const Feature> enabled_features;
    Dependencies dependencies;
    std::string_view build_identity;
};
struct Limits {
    std::size_t features, field_bytes, string_bytes, preimage_bytes, canonical_bytes, total_bytes;
};
struct Usage {
    std::size_t features{}, string_bytes{}, preimage_bytes{}, canonical_bytes{}, total_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
class Owner;
class ManifestDigest {
  public:
    [[nodiscard]] auto value() const noexcept -> const core::Sha256Digest& { return value_; }
    auto operator==(const ManifestDigest&) const -> bool = default;

  private:
    explicit ManifestDigest(core::Sha256Digest value) : value_(value) {}
    core::Sha256Digest value_;
    friend auto retain(Input, Limits) -> std::expected<Owner, core::Error>;
};
class Owner {
  public:
    Owner(const Owner&);
    auto operator=(const Owner&) -> Owner&;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    [[nodiscard]] auto input() const noexcept -> std::optional<Input>;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const ManifestDigest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;

  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit Owner(std::unique_ptr<Storage>);
    friend auto retain(Input, Limits) -> std::expected<Owner, core::Error>;
};
[[nodiscard]] auto retain(Input, Limits) -> std::expected<Owner, core::Error>;
} // namespace epycbench::results::build_manifest
