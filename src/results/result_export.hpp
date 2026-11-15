#pragma once

#include "results/final_document.hpp"
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace epycbench::results::result_export {
enum class Profile { complete, shareable, summary, observation_table };
enum class Error { state, limits, overflow, allocation, projection, canonical };
struct Limits {
    std::size_t input_bytes{64U << 20U}, output_bytes{64U << 20U};
    std::size_t nodes{1U << 20U}, members{1U << 20U}, text_bytes{64U << 20U}, depth{64};
    std::size_t workspace_bytes{256U << 20U}, tree_bytes{256U << 20U};
    std::size_t renderer_bytes{256U << 20U}, work{1U << 30U};
};
struct Usage {
    event_profile::detail::Shape source{}, output{};
    std::size_t workspace{}, source_tree{}, output_tree{}, renderer{}, work{}, simultaneous{};
};
namespace detail {
// Source fragments remain ordinary evidence; this selector issues no admission.
enum class Metric { bandwidth, latency, no_measurement_family, not_obtained, not_observed, numerical_failure };
[[nodiscard]] auto select_metric(const nlohmann::json& projection, const nlohmann::json& observations,
    std::string_view pair_id, std::size_t& work, std::size_t work_limit) noexcept
    -> std::expected<Metric, Error>;
struct RendererShape { std::size_t members{}, key_bytes{}, longest_key{}, longest_string{}, depth{}; };
// Pure checked form of the renderer worksheet, shared with boundary tests.
[[nodiscard]] auto renderer_stock(RendererShape) noexcept -> std::expected<std::size_t, Error>;
}
[[nodiscard]] auto profile(std::string_view) noexcept -> std::optional<Profile>;
[[nodiscard]] auto name(Profile) noexcept -> std::string_view;
// Complete is emitted directly from Validated::canonical by the caller; derived
// projection owns its return bytes and never retains borrows into the source.
[[nodiscard]] auto render(const final_document::Validated&, Profile, Limits = {}, Usage* = nullptr)
    -> std::expected<std::string, Error>;
} // namespace epycbench::results::result_export
