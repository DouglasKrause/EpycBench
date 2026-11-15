#pragma once
#include "results/private_execution_plan.hpp"
#include "results/preflight_admission.hpp"
#include "results/execution_request_association.hpp"

namespace epycbench::results::process_subject { struct Entry; class Session; }
namespace epycbench::results::planning {
enum class Error { state, reference, identity, capacity, membership };
enum class SemanticMembership { not_established, admitted_request };
struct References {
    std::uint64_t controller{}, normalization{}, topology{}, base_build{};
    auto operator==(const References&) const -> bool = default;
};
struct Record { References references; execution_plan::Owner plan; };
struct Membership {
    std::uint64_t planning_sequence{};
    std::optional<std::uint64_t> pair_ordinal;
    std::vector<std::uint64_t> scope_pairs;
    SemanticMembership semantic{SemanticMembership::not_established};
    std::optional<request_association::Evidence> request{};
};
struct Quantities {
    std::size_t records{}, pairs{}, scope_links{}, canonical_bytes{}, child_retained_bytes{},
        comparison_bytes{}, association_work{}, association_bytes{},
        planning_simultaneous_bytes{}, association_preparation_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits = Quantities;
using Usage = Quantities;
namespace detail {
struct MapPreparation {
    const Record* record{};
    std::uint64_t sequence{};
    References references;
    Usage usage;
    std::size_t backing{}, peak{};
};
[[nodiscard]] auto checked_add(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto append_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto map_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto declared_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto prepare_map(std::span<const process_subject::Entry>, std::uint64_t,
    preflight::References, const Limits&, const Usage&) -> std::expected<MapPreparation,Error>;
[[nodiscard]] auto finish_map(const MapPreparation&, std::span<const preflight::Scope>,
    const Limits&, Usage&) -> std::expected<Membership,Error>;
[[nodiscard]] auto declared(std::span<const process_subject::Entry>, std::uint64_t,
    std::optional<std::uint64_t>, std::optional<std::uint64_t>, const Limits&, Usage&)
    -> std::expected<Membership,Error>;
}
} // namespace epycbench::results::planning
