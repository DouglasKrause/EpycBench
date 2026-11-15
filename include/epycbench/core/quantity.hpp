#pragma once

#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>

namespace epycbench::core {

enum class QuantityErrorCode {
    negative_value,
    overflow,
    non_finite,
    out_of_range,
};

struct QuantityError {
    QuantityErrorCode code;

    constexpr auto operator<=>(const QuantityError&) const noexcept = default;
};

template <typename Tag> class IntegerQuantity {
  public:
    using representation_type = std::uint64_t;

    [[nodiscard]] static constexpr auto from_authored(std::int64_t value)
        -> std::expected<IntegerQuantity, QuantityError> {
        if (value < 0) {
            return std::unexpected{QuantityError{QuantityErrorCode::negative_value}};
        }
        return IntegerQuantity{static_cast<representation_type>(value)};
    }

    [[nodiscard]] static constexpr auto from_base_units(representation_type value) noexcept
        -> IntegerQuantity {
        return IntegerQuantity{value};
    }

    [[nodiscard]] constexpr auto value() const noexcept -> representation_type { return value_; }

    [[nodiscard]] constexpr auto checked_add(IntegerQuantity other) const noexcept
        -> std::expected<IntegerQuantity, QuantityError> {
        if (other.value_ > std::numeric_limits<representation_type>::max() - value_) {
            return std::unexpected{QuantityError{QuantityErrorCode::overflow}};
        }
        return IntegerQuantity{value_ + other.value_};
    }

    [[nodiscard]] constexpr auto checked_multiply(representation_type multiplier) const noexcept
        -> std::expected<IntegerQuantity, QuantityError> {
        if (multiplier != 0 &&
            value_ > std::numeric_limits<representation_type>::max() / multiplier) {
            return std::unexpected{QuantityError{QuantityErrorCode::overflow}};
        }
        return IntegerQuantity{value_ * multiplier};
    }

    constexpr auto operator<=>(const IntegerQuantity&) const noexcept = default;

  private:
    constexpr explicit IntegerQuantity(representation_type value) noexcept : value_{value} {}

    representation_type value_;
};

namespace detail {

struct ByteCountTag;
struct DurationTag;
struct WorkCountTag;
struct PageCountTag;
struct FrequencyTag;

} // namespace detail

using ByteCount = IntegerQuantity<detail::ByteCountTag>;
using Duration = IntegerQuantity<detail::DurationTag>;
using WorkCount = IntegerQuantity<detail::WorkCountTag>;
using PageCount = IntegerQuantity<detail::PageCountTag>;
using Frequency = IntegerQuantity<detail::FrequencyTag>;

class Fraction {
  public:
    [[nodiscard]] static auto from_authored(double value)
        -> std::expected<Fraction, QuantityError> {
        if (!std::isfinite(value)) {
            return std::unexpected{QuantityError{QuantityErrorCode::non_finite}};
        }
        if (value < 0.0) {
            return std::unexpected{QuantityError{QuantityErrorCode::negative_value}};
        }
        if (value > 1.0) {
            return std::unexpected{QuantityError{QuantityErrorCode::out_of_range}};
        }
        return Fraction{value};
    }

    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }

    constexpr auto operator<=>(const Fraction&) const noexcept = default;

  private:
    constexpr explicit Fraction(double value) noexcept : value_{value} {}

    double value_;
};

static_assert(!std::same_as<ByteCount, Duration>);
static_assert(!std::same_as<Duration, WorkCount>);
static_assert(!std::same_as<WorkCount, PageCount>);
static_assert(!std::same_as<PageCount, Frequency>);

} // namespace epycbench::core
