#include "monitoring/host_q4_collection_capacity_inputs.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
using FailureKind = MonitoringQ4CapacityFailureKind;
constexpr auto digits(std::uint32_t value) noexcept -> std::size_t {
    std::size_t result{1};
    while (value >= 10U) {
        value /= 10U;
        ++result;
    }
    return result;
}
auto kernel_buckets(std::uint32_t maximum) noexcept -> std::array<std::uint64_t, 10> {
    std::array<std::uint64_t, 10> result{};
    const auto total = static_cast<std::uint64_t>(maximum) + 1U;
    std::uint64_t lower{};
    std::uint64_t width{10U};
    for (std::size_t index{}; index < result.size(); ++index) {
        if (lower >= total)
            break;
        const auto remaining = total - lower;
        result[index] = remaining < width ? remaining : width;
        lower += result[index];
        if (index != 0U)
            width *= 10U;
        else
            width = 90U;
    }
    return result;
}
} // namespace

auto monitoring_q4_checked_add(std::uint64_t left, std::uint64_t right, Reason reason) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, reason}};
    return left + right;
}
auto monitoring_q4_checked_subtract(std::uint64_t left, std::uint64_t right, Reason reason) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    if (right > left)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, reason}};
    return left - right;
}
auto monitoring_q4_checked_multiply(std::uint64_t left, std::uint64_t right, Reason reason) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, reason}};
    return left * right;
}
auto monitoring_q4_select_largest_digits(std::uint64_t requested,
                                         const std::array<std::uint64_t, 10>& buckets) noexcept
    -> std::expected<MonitoringQ4DigitSelection, MonitoringQ4CapacityFailure> {
    MonitoringQ4DigitSelection result;
    result.remaining_by_digits = buckets;
    for (std::size_t reverse{}; reverse < buckets.size() && result.selected_count < requested;
         ++reverse) {
        const auto index = buckets.size() - 1U - reverse;
        const auto remaining = requested - result.selected_count;
        const auto take = buckets[index] < remaining ? buckets[index] : remaining;
        result.selected_by_digits[index] = take;
        result.remaining_by_digits[index] -= take;
        auto count = monitoring_q4_checked_add(result.selected_count, take, Reason::digits_selected_count);
        auto bytes = monitoring_q4_checked_multiply(take, index + 1U, Reason::digits_selected_product);
        if (!count) return std::unexpected{count.error()};
        if (!bytes) return std::unexpected{bytes.error()};
        auto total = monitoring_q4_checked_add(result.decimal_digit_bytes, *bytes, Reason::digits_selected_sum);
        if (!total)
            return std::unexpected{total.error()};
        result.selected_count = *count;
        result.decimal_digit_bytes = *total;
    }
    if (result.selected_count != requested)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_population_short}};
    for (const auto value : result.remaining_by_digits) {
        auto next = monitoring_q4_checked_add(result.remaining_count, value, Reason::digits_remaining_sum);
        if (!next)
            return std::unexpected{next.error()};
        result.remaining_count = *next;
    }
    return result;
}

auto MonitoringQ4CollectionCapacityInputs::allocate_multiplicity_categories_with(
    std::span<const MonitoringQ4MultiplicityCategory> categories,
    const std::array<std::uint64_t, 10>& available, std::uint64_t admitted_count,
    AllocationFault fault) noexcept
    -> std::expected<MonitoringQ4MultiplicityAllocation, MonitoringQ4CapacityFailure> {
    auto allocation_reason = Reason::digits_order_allocation_scope;
    try {
        auto authoritative_category_max = monitoring_q4_checked_add(admitted_count, 1U, Reason::digits_category_max);
        if (!authoritative_category_max)
            return std::unexpected{authoritative_category_max.error()};
        if (categories.size() > *authoritative_category_max)
            return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_category_count}};
        if (fault == AllocationFault::order)
            throw std::bad_alloc{};
        std::vector<std::size_t> order(categories.size());
        for (std::size_t index{}; index < categories.size(); ++index) {
            if (categories[index].count == 0U)
                return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_count_zero}};
            if (categories[index].multiplicity == 0U)
                return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_multiplicity_zero}};
            order[index] = index;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            return categories[left].multiplicity > categories[right].multiplicity;
        });
        for (std::size_t index{1}; index < order.size(); ++index)
            if (categories[order[index - 1U]].multiplicity == categories[order[index]].multiplicity)
                return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_multiplicity_duplicate}};
        MonitoringQ4MultiplicityAllocation result;
        allocation_reason = Reason::digits_reserve_allocation_scope;
        if (fault == AllocationFault::reserve)
            throw std::bad_alloc{};
        result.categories.reserve(categories.size());
        auto remaining = available;
        for (const auto input_index : order) {
            const auto& category = categories[input_index];
            auto selection = monitoring_q4_select_largest_digits(category.count, remaining);
            if (!selection)
                return std::unexpected{selection.error()};
            remaining = selection->remaining_by_digits;
            auto weighted = monitoring_q4_checked_multiply(selection->decimal_digit_bytes,
                                                           category.multiplicity, Reason::digits_weighted_product);
            if (!weighted)
                return std::unexpected{weighted.error()};
            auto total = monitoring_q4_checked_add(result.weighted_decimal_digit_bytes, *weighted, Reason::digits_weighted_sum);
            if (!total)
                return std::unexpected{total.error()};
            result.weighted_decimal_digit_bytes = *total;
            allocation_reason = Reason::digits_push_allocation_scope;
            if (fault == AllocationFault::push)
                throw std::bad_alloc{};
            result.categories.push_back({input_index, category, *selection});
        }
        return result;
    } catch (...) {
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::allocation_failure, allocation_reason}};
    }
}

auto MonitoringQ4CollectionCapacityInputs::allocate_multiplicity_categories(
    std::span<const MonitoringQ4MultiplicityCategory> categories,
    MonitoringQ4DigitPopulation population) const noexcept
    -> std::expected<MonitoringQ4MultiplicityAllocation, MonitoringQ4CapacityFailure> {
    const std::array<std::uint64_t, 10>* buckets{};
    switch (population) {
    case MonitoringQ4DigitPopulation::all:
        buckets = &kernel_digit_buckets_;
        break;
    case MonitoringQ4DigitPopulation::admitted:
        buckets = &admitted_digit_buckets_;
        break;
    case MonitoringQ4DigitPopulation::non_admitted:
        buckets = &non_admitted_digit_buckets_;
        break;
    default:
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::digits_population}};
    }
    return allocate_multiplicity_categories_with(
        categories, *buckets, structural_.admitted_cpu_count().value, AllocationFault::none);
}

auto monitoring_q4_collection_capacity_inputs(MonitoringQ4StructuralCapacity structural,
                                              std::vector<core::LinuxCpuId> admitted)
    -> std::expected<MonitoringQ4CollectionCapacityInputs, MonitoringQ4CapacityFailure> {
    if (admitted.empty())
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::inputs_empty}};
    if (admitted.size() != structural.admitted_cpu_count().value)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::inputs_count}};
    std::array<std::uint64_t, 10> admitted_buckets{};
    std::uint64_t digit_bytes{};
    for (std::size_t index{}; index < admitted.size(); ++index) {
        const auto value = admitted[index].value();
        if (value > structural.maximum_cpu_id().value)
            return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::inputs_cpu_range}};
        if (index != 0U && admitted[index - 1U].value() >= value)
            return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::inputs_cpu_order}};
        const auto digit_count = digits(value);
        ++admitted_buckets[digit_count - 1U];
        auto next = monitoring_q4_checked_add(digit_bytes, digit_count, Reason::inputs_digit_sum);
        if (!next)
            return std::unexpected{next.error()};
        digit_bytes = *next;
    }
    auto all = kernel_buckets(static_cast<std::uint32_t>(structural.maximum_cpu_id().value));
    auto remaining = all;
    for (std::size_t index{}; index < remaining.size(); ++index)
        remaining[index] -= admitted_buckets[index];
    return MonitoringQ4CollectionCapacityInputs{
        std::move(structural), std::move(admitted), all, admitted_buckets, remaining, digit_bytes};
}
} // namespace epycbench::monitoring::qualification::detail
