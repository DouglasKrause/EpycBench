#pragma once

#include "results/private_source_serialization.hpp"
#include "results/private_plan_composition.hpp"
#include "results/private_topology_serialization.hpp"

namespace epycbench::results::normalization_serialization {
inline constexpr std::string_view profile = "epycbench.private-successful-normalization-artifact/v1";
enum class Origin { session_selected, decoded_unassociated };
enum class Error { state, reference, identity, invalid_limits, capacity, encoding, schema, source, child, topology, association };
struct Quantities {
    std::size_t input_bytes{}, nodes{}, depth{}, text_bytes{}, canonical_bytes{}, provenance_bytes{},
        source_preparation_bytes{}, plan_preparation_bytes{}, topology_preparation_bytes{},
        association_entries{}, association_bytes{}, association_work{}, publication_bytes{}, retained_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
struct Limits : Quantities {
    source_serialization::Limits source;
    private_plan_composition::Limits plan;
    topology_serialization::Limits topology;
};
struct Usage : Quantities {
    source_serialization::Usage source;
    private_plan_composition::Usage plan;
    topology_serialization::Usage topology;
    auto operator==(const Usage&) const -> bool = default;
};
struct Provenance {
    std::string run, time;
    process_subject::Domain domain;
    std::uint64_t sequence, topology_sequence;
    auto operator==(const Provenance&) const -> bool = default;
};
class Owner;
namespace detail {
struct View {
    std::string_view run, time;
    process_subject::Domain domain;
    std::uint64_t sequence, topology_sequence;
};
class ResolvedInput final {
    const authored_source::ResolutionRecord* record;
    View view;
    std::optional<source_serialization::Owner>* source;
    std::optional<private_plan_composition::Owner>* plan;
    std::optional<topology_serialization::Owner>* topology;
    std::size_t upper_fixed;
    ResolvedInput(const authored_source::ResolutionRecord* r, View v,
                  std::optional<source_serialization::Owner>* s,
                  std::optional<private_plan_composition::Owner>* p,
                  std::optional<topology_serialization::Owner>* t, std::size_t upper) noexcept
        : record(r), view(v), source(s), plan(p), topology(t), upper_fixed(upper) {}
    friend class process_subject::Session;
    friend class normalization_serialization::Owner;
  public:
    ResolvedInput(const ResolvedInput&) = delete;
    ResolvedInput(ResolvedInput&&) = delete;
    auto operator=(const ResolvedInput&) -> ResolvedInput& = delete;
    auto operator=(ResolvedInput&&) -> ResolvedInput& = delete;
};
struct State {
    Provenance provenance;
    std::optional<source_serialization::Owner> source;
    std::optional<private_plan_composition::Owner> plan;
    std::optional<topology_serialization::Owner> topology;
    std::string canonical;
    core::Sha256Digest digest;
    Usage usage;
    Origin origin;
};
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto session_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t, std::size_t) noexcept -> std::optional<std::size_t>;
}
class Owner {
  public:
    ~Owner();
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto provenance() const noexcept -> const Provenance*;
    [[nodiscard]] auto source() const noexcept -> const source_serialization::Owner*;
    [[nodiscard]] auto plan() const noexcept -> const private_plan_composition::Owner*;
    [[nodiscard]] auto topology() const noexcept -> const topology_serialization::Owner*;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] auto origin() const noexcept -> std::optional<Origin>;
    [[nodiscard]] static auto decode(std::string_view, Limits) -> std::expected<Owner, Error>;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto preflight(const detail::ResolvedInput&, const Limits&) -> std::expected<void, Error>;
    static auto prepare(const detail::ResolvedInput&, Limits) -> std::expected<Owner, Error>;
    friend class process_subject::Session;
};
} // namespace epycbench::results::normalization_serialization
