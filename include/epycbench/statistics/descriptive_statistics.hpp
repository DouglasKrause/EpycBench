#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/primary_metric.hpp"

#include <concepts>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::statistics {

template <typename Metric>
concept PrimaryMetric = std::same_as<Metric, Bandwidth> ||
                        std::same_as<Metric, DependentLoadLatency> ||
                        std::same_as<Metric, TaskThroughput>;

template <PrimaryMetric Metric> class DescriptiveStatistics {
  public:
    constexpr DescriptiveStatistics(std::uint64_t count, Metric minimum, Metric maximum,
                                    Metric mean, Metric median,
                                    Metric population_standard_deviation,
                                    double coefficient_of_variation, Metric percentile_5,
                                    Metric percentile_95) noexcept
        : count_{count}, minimum_{minimum}, maximum_{maximum}, mean_{mean}, median_{median},
          population_standard_deviation_{population_standard_deviation},
          coefficient_of_variation_{coefficient_of_variation}, percentile_5_{percentile_5},
          percentile_95_{percentile_95} {}

    [[nodiscard]] constexpr auto count() const noexcept -> std::uint64_t { return count_; }
    [[nodiscard]] constexpr auto minimum() const noexcept -> Metric { return minimum_; }
    [[nodiscard]] constexpr auto maximum() const noexcept -> Metric { return maximum_; }
    [[nodiscard]] constexpr auto mean() const noexcept -> Metric { return mean_; }
    [[nodiscard]] constexpr auto median() const noexcept -> Metric { return median_; }
    [[nodiscard]] constexpr auto population_standard_deviation() const noexcept -> Metric {
        return population_standard_deviation_;
    }
    [[nodiscard]] constexpr auto coefficient_of_variation() const noexcept -> double {
        return coefficient_of_variation_;
    }
    [[nodiscard]] constexpr auto percentile_5() const noexcept -> Metric { return percentile_5_; }
    [[nodiscard]] constexpr auto percentile_95() const noexcept -> Metric {
        return percentile_95_;
    }

  private:
    std::uint64_t count_;
    Metric minimum_;
    Metric maximum_;
    Metric mean_;
    Metric median_;
    Metric population_standard_deviation_;
    double coefficient_of_variation_;
    Metric percentile_5_;
    Metric percentile_95_;
};

[[nodiscard]] auto summarize_bandwidth(std::span<const Bandwidth> samples)
    -> std::expected<DescriptiveStatistics<Bandwidth>, core::Error>;
[[nodiscard]] auto summarize_dependent_load_latency(
    std::span<const DependentLoadLatency> samples)
    -> std::expected<DescriptiveStatistics<DependentLoadLatency>, core::Error>;

[[nodiscard]] auto summarize_task_throughput(std::span<const TaskThroughput> samples)
    -> std::expected<DescriptiveStatistics<TaskThroughput>, core::Error>;

} // namespace epycbench::statistics
