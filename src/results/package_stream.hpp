#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace epycbench::results::package_stream {
struct MeasuredBlock {
    std::uint64_t release_ns,completion_ns;
    std::array<std::uint64_t,2> callback_ns,work_units,output_counts;
    std::string handoff_identity;
};

using RankValidator = std::function<bool(std::size_t stage,const nlohmann::json& rank,const nlohmann::json& geometry)>;

[[nodiscard]] auto valid_geometry(const nlohmann::json& plan) -> bool;
[[nodiscard]] auto retained_shape(const nlohmann::json& stream,const nlohmann::json& plan,bool require_handoff) -> bool;
[[nodiscard]] auto handoff_binding(const nlohmann::json& receipt, const nlohmann::json& transfers) -> std::string;
[[nodiscard]] auto measured_block(const nlohmann::json& stream,const nlohmann::json& plan,const RankValidator&)
    -> std::optional<MeasuredBlock>;
[[nodiscard]] auto input_identity(const nlohmann::json& stream, const MeasuredBlock&) -> std::string;
}
