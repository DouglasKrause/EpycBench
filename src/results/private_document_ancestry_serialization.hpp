#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"

#include <array>
#include <memory>

namespace epycbench::results::document_ancestry_serialization {
inline constexpr std::string_view profile = "epycbench.private-document-ancestry-artifact/v1";
using Json = nlohmann::ordered_json;
using LocationMap = std::map<std::string, scenario::NodeLocations>;
using OriginMap = std::map<std::string, scenario::ValueOrigin>;
enum class Origin { plan_captured, decoded_unassociated };
enum class Error { state, invalid_limits, capacity, schema, encoding, reference };
struct Quantities {
    std::array<std::size_t, 3> tree_nodes{};
    std::size_t tree_depth{}, location_rows{}, origin_rows{}, source_name_bytes{}, tree_octets{},
        metadata_octets{}, index_entries{}, pointer_scratch_bytes{}, canonical_bytes{},
        retained_bytes{}, consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits = Quantities;
using Usage = Quantities;
// Measurement is allocation-free resource preflight, not owning/local admission.
[[nodiscard]] auto measure_capture(const scenario::ScenarioDigestedPlan&, Limits) -> std::expected<Usage, Error>;
[[nodiscard]] auto measure_decode(std::string_view, Limits) -> std::expected<Usage, Error>;
namespace detail {
struct State;
[[nodiscard]] auto capture_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto decode_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto checked_add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t, std::size_t) noexcept -> std::optional<std::size_t>;
}
class Owner {
  public:
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    ~Owner();
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto authored() const noexcept -> const Json*;
    [[nodiscard]] auto unit_normalized() const noexcept -> const Json*;
    [[nodiscard]] auto default_expanded() const noexcept -> const Json*;
    [[nodiscard]] auto locations() const noexcept -> const LocationMap*;
    [[nodiscard]] auto origins() const noexcept -> const OriginMap*;
    [[nodiscard]] auto source_name() const noexcept -> std::string_view;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto origin() const noexcept -> std::optional<Origin>;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] static auto capture(const scenario::ScenarioDigestedPlan&, Limits)
        -> std::expected<Owner, Error>;
    [[nodiscard]] static auto decode(std::string_view, Limits) -> std::expected<Owner, Error>;

  private:
    std::unique_ptr<detail::State> state_;
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
};
} // namespace epycbench::results::document_ancestry_serialization
