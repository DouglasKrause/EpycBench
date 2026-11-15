#pragma once

#include "epycbench/scenario/scenario_plan_digest.hpp"

#include <memory>

namespace epycbench::results::comparison_experiments_serialization {
inline constexpr std::string_view profile="epycbench.private-comparison-experiments-artifact/v1";
enum class Origin { plan_captured, decoded_unassociated };
enum class Error { state, capacity, schema, encoding, reference, typed_factory_failure };
struct Quantities {
    std::size_t experiments{}, workers{}, variants{}, nodes{}, set_members{}, mappings{}, candidates{},
        cpu_members{}, cpu_ranges{}, maximum_cpu_members{}, cpu_input_bytes{}, text_bytes{}, maximum_text{},
        range_components{}, canonical_bytes{}, staged_bytes{}, retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
// Measurement is allocation-free resource preflight, not owning/local admission.
[[nodiscard]] auto measure_capture(const scenario::ScenarioDigestedPlan&,Limits) -> std::expected<Usage,Error>;
[[nodiscard]] auto measure_decode(std::string_view,Limits) -> std::expected<Usage,Error>;
namespace detail {
struct State;
[[nodiscard]] auto capture_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto decode_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto state_stock() noexcept -> std::size_t;
[[nodiscard]] auto identity_stock() noexcept -> std::size_t;
[[nodiscard]] auto name_stock() noexcept -> std::size_t;
[[nodiscard]] auto factory_error_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t&,std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&)=delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto experiments() const noexcept -> const std::vector<scenario::ResolvedComparisonExperiment>*;
    [[nodiscard]] auto expansion_profile() const noexcept -> std::string_view;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto origin() const noexcept -> std::optional<Origin>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] static auto capture(const scenario::ScenarioDigestedPlan&,Limits) -> std::expected<Owner,Error>;
    [[nodiscard]] static auto decode(std::string_view,Limits) -> std::expected<Owner,Error>;
  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
};
} // namespace epycbench::results::comparison_experiments_serialization
