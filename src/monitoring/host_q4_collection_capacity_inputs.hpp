#pragma once

#include "epycbench/core/id.hpp"
#include "monitoring/host_q4_structural_capacity.hpp"

#include <array>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ4DigitSelection final {
    std::array<std::uint64_t, 10> selected_by_digits{};
    std::array<std::uint64_t, 10> remaining_by_digits{};
    std::uint64_t selected_count{};
    std::uint64_t remaining_count{};
    std::uint64_t decimal_digit_bytes{};
};

struct MonitoringQ4MultiplicityCategory final {
    std::uint64_t count{};
    std::uint64_t multiplicity{};
};
struct MonitoringQ4CategoryAllocation final {
    std::size_t input_index{};
    MonitoringQ4MultiplicityCategory category{};
    MonitoringQ4DigitSelection selection{};
};
struct MonitoringQ4MultiplicityAllocation final {
    std::vector<MonitoringQ4CategoryAllocation> categories;
    std::uint64_t weighted_decimal_digit_bytes{};
};
enum class MonitoringQ4DigitPopulation : unsigned char { all, admitted, non_admitted };
class MonitoringQ4CollectionCapacityInputsTestAccess;

[[nodiscard]] auto monitoring_q4_checked_add(std::uint64_t, std::uint64_t,
    MonitoringQ4CapacityReason = MonitoringQ4CapacityReason::arithmetic_add) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure>;
[[nodiscard]] auto monitoring_q4_checked_subtract(std::uint64_t, std::uint64_t,
    MonitoringQ4CapacityReason = MonitoringQ4CapacityReason::arithmetic_subtract) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure>;
[[nodiscard]] auto monitoring_q4_checked_multiply(std::uint64_t, std::uint64_t,
    MonitoringQ4CapacityReason = MonitoringQ4CapacityReason::arithmetic_multiply) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure>;
[[nodiscard]] auto
monitoring_q4_select_largest_digits(std::uint64_t, const std::array<std::uint64_t, 10>&) noexcept
    -> std::expected<MonitoringQ4DigitSelection, MonitoringQ4CapacityFailure>;
class MonitoringQ4CollectionCapacityInputs final {
  public:
    MonitoringQ4CollectionCapacityInputs(const MonitoringQ4CollectionCapacityInputs&) = default;
    MonitoringQ4CollectionCapacityInputs(MonitoringQ4CollectionCapacityInputs&&) noexcept = default;
    auto operator=(const MonitoringQ4CollectionCapacityInputs&)
        -> MonitoringQ4CollectionCapacityInputs& = delete;
    auto operator=(MonitoringQ4CollectionCapacityInputs&&)
        -> MonitoringQ4CollectionCapacityInputs& = delete;

    [[nodiscard]] auto structural() const noexcept -> const MonitoringQ4StructuralCapacity& {
        return structural_;
    }
    [[nodiscard]] auto admitted_cpus() const noexcept -> std::span<const core::LinuxCpuId> {
        return admitted_cpus_;
    }
    [[nodiscard]] constexpr auto kernel_digit_buckets() const noexcept
        -> const std::array<std::uint64_t, 10>& {
        return kernel_digit_buckets_;
    }
    [[nodiscard]] constexpr auto admitted_digit_buckets() const noexcept
        -> const std::array<std::uint64_t, 10>& {
        return admitted_digit_buckets_;
    }
    [[nodiscard]] constexpr auto non_admitted_digit_buckets() const noexcept
        -> const std::array<std::uint64_t, 10>& {
        return non_admitted_digit_buckets_;
    }
    [[nodiscard]] constexpr auto admitted_decimal_digit_bytes() const noexcept {
        return admitted_decimal_digit_bytes_;
    }
    [[nodiscard]] auto
        allocate_multiplicity_categories(std::span<const MonitoringQ4MultiplicityCategory>,
                                         MonitoringQ4DigitPopulation) const noexcept
        -> std::expected<MonitoringQ4MultiplicityAllocation, MonitoringQ4CapacityFailure>;

  private:
    enum class AllocationFault : unsigned char { none, order, reserve, push };
    static auto
    allocate_multiplicity_categories_with(std::span<const MonitoringQ4MultiplicityCategory>,
                                          const std::array<std::uint64_t, 10>&, std::uint64_t,
                                          AllocationFault) noexcept
        -> std::expected<MonitoringQ4MultiplicityAllocation, MonitoringQ4CapacityFailure>;
    friend auto monitoring_q4_collection_capacity_inputs(MonitoringQ4StructuralCapacity,
                                                         std::vector<core::LinuxCpuId>)
        -> std::expected<MonitoringQ4CollectionCapacityInputs, MonitoringQ4CapacityFailure>;
    MonitoringQ4CollectionCapacityInputs(MonitoringQ4StructuralCapacity structural,
                                         std::vector<core::LinuxCpuId> admitted,
                                         std::array<std::uint64_t, 10> kernel_buckets,
                                         std::array<std::uint64_t, 10> admitted_buckets,
                                         std::array<std::uint64_t, 10> non_admitted_buckets,
                                         std::uint64_t admitted_digits) noexcept
        : structural_{std::move(structural)}, admitted_cpus_{std::move(admitted)},
          kernel_digit_buckets_{kernel_buckets}, admitted_digit_buckets_{admitted_buckets},
          non_admitted_digit_buckets_{non_admitted_buckets},
          admitted_decimal_digit_bytes_{admitted_digits} {}

    MonitoringQ4StructuralCapacity structural_;
    std::vector<core::LinuxCpuId> admitted_cpus_;
    std::array<std::uint64_t, 10> kernel_digit_buckets_{};
    std::array<std::uint64_t, 10> admitted_digit_buckets_{};
    std::array<std::uint64_t, 10> non_admitted_digit_buckets_{};
    std::uint64_t admitted_decimal_digit_bytes_{};
    friend class MonitoringQ4CollectionCapacityInputsTestAccess;
};

[[nodiscard]] auto monitoring_q4_collection_capacity_inputs(MonitoringQ4StructuralCapacity,
                                                            std::vector<core::LinuxCpuId>)
    -> std::expected<MonitoringQ4CollectionCapacityInputs, MonitoringQ4CapacityFailure>;

} // namespace epycbench::monitoring::qualification::detail
