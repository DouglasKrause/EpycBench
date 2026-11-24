#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/descriptive_statistics.hpp"

#include <array>
#include <expected>

namespace epycbench::statistics {

template <PrimaryMetric Metric> class BlockRepresentatives {
  public:
    constexpr BlockRepresentatives(Metric first, Metric second) noexcept
        : first_{first}, second_{second} {}

    [[nodiscard]] constexpr auto first() const noexcept -> Metric { return first_; }
    [[nodiscard]] constexpr auto second() const noexcept -> Metric { return second_; }

  private:
    Metric first_;
    Metric second_;
};

[[nodiscard]] auto block_bandwidth_representatives(std::array<Bandwidth, 2> first,
                                                   std::array<Bandwidth, 2> second)
    -> std::expected<BlockRepresentatives<Bandwidth>, core::Error>;

[[nodiscard]] auto block_dependent_load_latency_representatives(
    std::array<DependentLoadLatency, 2> first,
    std::array<DependentLoadLatency, 2> second)
    -> std::expected<BlockRepresentatives<DependentLoadLatency>, core::Error>;

[[nodiscard]] auto block_task_throughput_representatives(
    std::array<TaskThroughput, 2> first, std::array<TaskThroughput, 2> second)
    -> std::expected<BlockRepresentatives<TaskThroughput>, core::Error>;

} // namespace epycbench::statistics
