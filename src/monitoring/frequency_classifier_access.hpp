#pragma once

#include "epycbench/monitoring/frequency.hpp"

namespace epycbench::monitoring::detail {
class FrequencyClassifierAccess {
  public:
    [[nodiscard]] static auto threshold_detected(std::uint64_t range,
                                                 std::uint64_t maximum) noexcept -> bool;
};
} // namespace epycbench::monitoring::detail
