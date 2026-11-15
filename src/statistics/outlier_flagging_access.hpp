#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/outlier_flagging.hpp"

#include <expected>

namespace epycbench::statistics::detail {

inline constexpr double modified_z_scaling = 0.6744897501960817;
inline constexpr double modified_z_threshold = 3.5;

[[nodiscard]] auto classify_modified_z_score(double score) noexcept
    -> std::expected<OutlierFlagState, core::Error>;

} // namespace epycbench::statistics::detail
