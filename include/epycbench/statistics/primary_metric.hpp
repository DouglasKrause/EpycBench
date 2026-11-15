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

} // namespace epycbench::statistics
