#pragma once

#include "results/private_document_ancestry_serialization.hpp"
#include "results/private_resource_service_serialization.hpp"
#include "results/private_memory_placement_serialization.hpp"
#include "results/private_kernel_dispatch_serialization.hpp"
#include "results/private_comparison_experiments_serialization.hpp"
#include "results/private_matrix_experiments_serialization.hpp"
#include "scenario/retained_plan_projection_resources.hpp"

namespace epycbench::results::private_plan_composition {
inline constexpr std::string_view profile = "epycbench.private-plan-composition-artifact/v1";
enum class Origin { plan_captured, decoded_unassociated };
enum class Error { state, invalid_limits, capacity, schema, encoding, reference, projection };
struct Quantities {
    document_ancestry_serialization::Quantities document_ancestry;
    resource_service_serialization::Quantities resource_service;
    memory_placement_serialization::Quantities memory_placement;
    kernel_dispatch_serialization::Quantities kernel_dispatch;
    comparison_experiments_serialization::Quantities comparison_experiments;
    matrix_experiments_serialization::Quantities matrix_experiments;
    scenario::detail::ProjectionInputUsage projection_input;
    scenario::detail::ProjectionResourceUsage scenario_projection, resolved_projection;
    std::size_t input_bytes{}, canonical_scenario_bytes{}, canonical_resolved_bytes{}, envelope_bytes{},
        association_entries{}, association_work{}, retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits = Quantities;
using Usage = Quantities;
namespace detail {
struct State;
[[nodiscard]] auto checked_add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t, std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto state_stock() noexcept -> std::size_t;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] static auto capture(const scenario::ScenarioDigestedPlan&, Limits) -> std::expected<Owner, Error>;
    [[nodiscard]] static auto decode(std::string_view, Limits) -> std::expected<Owner, Error>;
    [[nodiscard]] auto document_ancestry() const noexcept -> const document_ancestry_serialization::Owner*;
    [[nodiscard]] auto resource_service() const noexcept -> const resource_service_serialization::Owner*;
    [[nodiscard]] auto memory_placement() const noexcept -> const memory_placement_serialization::Owner*;
    [[nodiscard]] auto kernel_dispatch() const noexcept -> const kernel_dispatch_serialization::Owner*;
    [[nodiscard]] auto comparison_experiments() const noexcept -> const comparison_experiments_serialization::Owner*;
    [[nodiscard]] auto matrix_experiments() const noexcept -> const matrix_experiments_serialization::Owner*;
    [[nodiscard]] auto canonical_scenario() const noexcept -> std::string_view;
    [[nodiscard]] auto canonical_resolved_plan() const noexcept -> std::string_view;
    [[nodiscard]] auto scenario_digest() const noexcept -> const scenario::ScenarioDigest*;
    [[nodiscard]] auto resolved_digest() const noexcept -> const scenario::ResolvedScenarioPlanDigest*;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto origin() const noexcept -> std::optional<Origin>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
};
} // namespace epycbench::results::private_plan_composition
