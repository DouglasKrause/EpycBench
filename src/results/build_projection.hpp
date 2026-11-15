#pragma once
#include "results/build_manifest_owner.hpp"
#include <array>
#include <memory>

namespace epycbench::results::process_subject { class Session; }
namespace epycbench::results::build_projection {
inline constexpr std::string_view profile = "epycbench.results.reduced-base-build/v1";
enum class Disposition { not_attempted, not_obtained, available };
enum class Domain { supplied_observation, synthetic_execution };
enum class Origin { ordinary, genesis };
enum class Error { state, reference, invalid_limits, capacity, source, rendering };
struct FailureAddress {
    std::uint64_t sequence;
    std::optional<std::uint64_t> creation_ordinal;
    auto operator==(const FailureAddress&) const -> bool = default;
};
struct Limits {
    std::size_t features, failure_references, nodes, depth, text_bytes, canonical_bytes,
        private_text_bytes, retained_bytes, consumer_simultaneous_bytes;
};
struct Usage {
    std::size_t features{}, failure_references{}, nodes{}, depth{}, text_bytes{}, canonical_bytes{},
        private_text_bytes{}, retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct PrivateProvenance {
    std::string run, handoff_time;
    Domain domain;
    Origin origin;
    std::uint64_t sequence;
    Disposition disposition;
    std::optional<FailureAddress> failure;
    std::optional<std::string> producer_checksum;
    std::optional<core::Sha256Digest> manifest_digest;
};
class Owner;
namespace detail {
class ResolvedInput final {
    const build_manifest::Owner* manifest;
    Disposition disposition;
    std::string_view run, time;
    Domain domain;
    Origin origin;
    std::uint64_t sequence;
    std::optional<FailureAddress> failure;
    std::size_t session_fixed;
    ResolvedInput(const build_manifest::Owner* m, Disposition d, std::string_view r,
        std::string_view t, Domain domain_value, Origin o, std::uint64_t s,
        std::optional<FailureAddress> f, std::size_t fixed) noexcept
        : manifest(m), disposition(d), run(r), time(t), domain(domain_value), origin(o),
          sequence(s), failure(f), session_fixed(fixed) {}
    friend class process_subject::Session;
    friend class build_projection::Owner;
  public:
    ResolvedInput(const ResolvedInput&) = delete;
    auto operator=(const ResolvedInput&) -> ResolvedInput& = delete;
    ResolvedInput(ResolvedInput&&) = delete;
    auto operator=(ResolvedInput&&) -> ResolvedInput& = delete;
};
struct State {
    PrivateProvenance provenance;
    std::string canonical;
    core::Sha256Digest digest{core::sha256(std::string_view{})};
    Usage usage;
};
[[nodiscard]] auto valid_limits(const Limits&) noexcept -> bool;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto session_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t, std::size_t) noexcept -> std::optional<std::size_t>;
}
class Owner {
  public:
    ~Owner();
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    [[nodiscard]] auto reduced_canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto reduced_digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto private_provenance() const noexcept -> const PrivateProvenance*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto prepare(const detail::ResolvedInput&, const Limits&) -> std::expected<Owner, Error>;
    std::unique_ptr<detail::State> state_;
    friend class process_subject::Session;
};
} // namespace epycbench::results::build_projection
