#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/quantity.hpp"

#include <compare>
#include <expected>

namespace epycbench::statistics {

namespace detail {
struct PrimaryMetricAccess;
}

class Bandwidth {
  public:
    [[nodiscard]] static auto create(core::ByteCount bytes, core::Duration elapsed) noexcept
        -> std::expected<Bandwidth, core::Error>;

    [[nodiscard]] constexpr auto bytes_per_second() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const Bandwidth&) const noexcept = default;

  private:
    friend struct detail::PrimaryMetricAccess;
    constexpr explicit Bandwidth(double value) noexcept : value_{value} {}
    double value_;
};

class DependentLoadLatency {
  public:
    [[nodiscard]] static auto create(core::Duration elapsed,
                                     core::WorkCount completed_work) noexcept
        -> std::expected<DependentLoadLatency, core::Error>;

    [[nodiscard]] constexpr auto seconds_per_completed_load() const noexcept -> double {
        return value_;
    }
    constexpr auto operator<=>(const DependentLoadLatency&) const noexcept = default;

  private:
    friend struct detail::PrimaryMetricAccess;
    constexpr explicit DependentLoadLatency(double value) noexcept : value_{value} {}
    double value_;
};

class TaskThroughput {
  public:
    [[nodiscard]] static auto create(core::WorkCount completed_tasks,
                                     core::Duration elapsed) noexcept
        -> std::expected<TaskThroughput, core::Error>;

    [[nodiscard]] constexpr auto tasks_per_second() const noexcept -> double { return value_; }
    [[nodiscard]] constexpr auto seconds_per_task() const noexcept -> double { return 1.0 / value_; }
    constexpr auto operator<=>(const TaskThroughput&) const noexcept = default;

  private:
    friend struct detail::PrimaryMetricAccess;
    constexpr explicit TaskThroughput(double value) noexcept : value_{value} {}
    double value_;
};

} // namespace epycbench::statistics
