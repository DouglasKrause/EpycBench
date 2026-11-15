#include "monitoring/host_q4_structural_capacity.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
using FailureKind = MonitoringQ4CapacityFailureKind;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

auto add(std::uint64_t left, std::uint64_t right, Reason reason)
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    if (right > maximum - left)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, reason}};
    return left + right;
}
auto multiply(std::uint64_t left, std::uint64_t right, Reason reason)
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    if (left != 0U && right > maximum / left)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, reason}};
    return left * right;
}
auto power10(unsigned exponent) -> std::uint64_t {
    std::uint64_t value = 1;
    while (exponent-- != 0U)
        value *= 10U;
    return value;
}
auto decimal_digits(std::uint64_t value) -> std::uint64_t {
    std::uint64_t digits = 1;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}
} // namespace

auto monitoring_q4_structural_capacity(MonitoringQ4KernelMaximum kernel_maximum,
                                       MonitoringQ4BasePageBytes base_page_bytes,
                                       MonitoringQ4AdmittedCpuCount admitted_cpu_count)
    -> std::expected<MonitoringQ4StructuralCapacity, MonitoringQ4CapacityFailure> {
    const auto k = static_cast<std::uint64_t>(kernel_maximum.value);
    const auto b = base_page_bytes.value;
    const auto a = admitted_cpu_count.value;
    if (b == 0U)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::struct_page_zero}};
    if (a == 0U)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::struct_admitted_zero}};
    const auto n_result = add(k, 1U, Reason::struct_maximum_count);
    if (!n_result)
        return std::unexpected{n_result.error()};
    const auto n = *n_result;
    if (a > n)
        return std::unexpected{MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::struct_admitted_range}};
    const auto q_result = add(n, 1U, Reason::struct_root_count);
    if (!q_result)
        return std::unexpected{q_result.error()};
    const auto q = *q_result;

    std::uint64_t digit_bytes{};
    for (unsigned width = 1; width <= 10; ++width) {
        const auto upper = std::min(n, power10(width));
        const auto lower = width == 1 ? 0U : power10(width - 1U);
        const auto count = upper > lower ? upper - lower : 0U;
        const auto part = multiply(width, count, Reason::struct_digit_product);
        if (!part)
            return std::unexpected{part.error()};
        const auto next = add(digit_bytes, *part, Reason::struct_digit_sum);
        if (!next)
            return std::unexpected{next.error()};
        digit_bytes = *next;
    }
    const auto prefixes = multiply(6U, n, Reason::struct_name_prefixes);
    if (!prefixes)
        return std::unexpected{prefixes.error()};
    const auto policy_names = add(*prefixes, digit_bytes, Reason::struct_policy_names);
    if (!policy_names)
        return std::unexpected{policy_names.error()};
    const auto root_names = add(*policy_names, 5U, Reason::struct_root_names);
    if (!root_names)
        return std::unexpected{root_names.error()};
    const auto probe_bytes = add(b, 1U, Reason::struct_probe_bytes);
    if (!probe_bytes)
        return std::unexpected{probe_bytes.error()};
    const auto attribute_bytes = multiply(7U, n, Reason::struct_attribute_slots);
    if (!attribute_bytes)
        return std::unexpected{attribute_bytes.error()};
    const auto owned_attributes = multiply(*attribute_bytes, *probe_bytes, Reason::struct_attribute_bytes);
    if (!owned_attributes)
        return std::unexpected{owned_attributes.error()};
    const auto owned_content = add(*root_names, *owned_attributes, Reason::struct_owned_bytes);
    if (!owned_content)
        return std::unexpected{owned_content.error()};
    const auto live_roles = add(a, 3U, Reason::struct_live_roles);
    if (!live_roles)
        return std::unexpected{live_roles.error()};

    const auto eight_n = multiply(8U, n, Reason::struct_eight_n);
    if (!eight_n)
        return std::unexpected{eight_n.error()};
    const auto enumeration_cleanup = add(1U, *eight_n, Reason::struct_enumeration_cleanup);
    if (!enumeration_cleanup)
        return std::unexpected{enumeration_cleanup.error()};
    const auto six_cleanup = multiply(6U, *enumeration_cleanup, Reason::struct_six_cleanup);
    if (!six_cleanup)
        return std::unexpected{six_cleanup.error()};
    const auto prepares = multiply(3U, a, Reason::struct_preparations);
    if (!prepares)
        return std::unexpected{prepares.error()};
    const auto prepare_cleanup = multiply(*prepares, *enumeration_cleanup, Reason::struct_prepare_cleanup);
    if (!prepare_cleanup)
        return std::unexpected{prepare_cleanup.error()};
    const auto ninety_nine_a = multiply(99U, a, Reason::struct_ninety_nine_a);
    if (!ninety_nine_a)
        return std::unexpected{ninety_nine_a.error()};
    auto cleanup = add(1U, *six_cleanup, Reason::struct_cleanup_base);
    if (!cleanup)
        return std::unexpected{cleanup.error()};
    cleanup = add(*cleanup, *prepare_cleanup, Reason::struct_cleanup_prepare);
    if (!cleanup)
        return std::unexpected{cleanup.error()};
    cleanup = add(*cleanup, *ninety_nine_a, Reason::struct_cleanup_reads);
    if (!cleanup)
        return std::unexpected{cleanup.error()};

    const auto ten_n = multiply(10U, n, Reason::struct_ten_n);
    if (!ten_n)
        return std::unexpected{ten_n.error()};
    const auto r = add(4U, *ten_n, Reason::struct_enumeration_ops);
    if (!r)
        return std::unexpected{r.error()};
    const auto six_r = multiply(6U, *r, Reason::struct_six_ops);
    if (!six_r)
        return std::unexpected{six_r.error()};
    const auto prepare_operations = multiply(*prepares, *r, Reason::struct_prepare_ops);
    if (!prepare_operations)
        return std::unexpected{prepare_operations.error()};
    const auto observations = multiply(384U, a, Reason::struct_observations);
    if (!observations)
        return std::unexpected{observations.error()};
    auto operations = add(12U, *six_r, Reason::struct_ops_base);
    if (!operations)
        return std::unexpected{operations.error()};
    operations = add(*operations, *prepare_operations, Reason::struct_ops_prepare);
    if (!operations)
        return std::unexpected{operations.error()};
    operations = add(*operations, *observations, Reason::struct_ops_reads);
    if (!operations)
        return std::unexpected{operations.error()};
    const auto extra_quantum = *operations % 99U == 0U ? 0U : 1U;
    const auto quanta = add(*operations / 99U, extra_quantum, Reason::struct_quanta);
    if (!quanta)
        return std::unexpected{quanta.error()};
    const auto deadline = multiply(*quanta, 60U, Reason::struct_deadline);
    if (!deadline)
        return std::unexpected{deadline.error()};

    MonitoringQ4StructuralCapacity result;
    result.maximum_cpu_id_ = {k};
    result.maximum_cpu_count_ = {n};
    result.admitted_cpu_count_ = {a};
    result.maximum_policy_directories_ = {n};
    result.maximum_membership_entries_ = {n};
    result.root_entry_slots_ = {q};
    result.maximum_cpu_decimal_digits_ = {decimal_digits(k)};
    result.policy_name_bytes_ = {*policy_names};
    result.root_entry_name_bytes_ = {*root_names};
    result.per_file_content_bytes_ = {b};
    result.per_file_probe_bytes_ = {*probe_bytes};
    result.root_identity_slots_ = {1U};
    result.policy_identity_slots_ = {n};
    result.attribute_presence_slots_ = {*attribute_bytes};
    result.attribute_identity_slots_ = {*attribute_bytes};
    result.attribute_owned_content_slots_ = {*attribute_bytes};
    result.membership_value_slots_ = {n};
    result.attribute_owned_bytes_ = {*owned_attributes};
    result.owned_content_bytes_ = {*owned_content};
    result.simultaneous_live_roles_ = {std::max<std::uint64_t>(1U, *live_roles)};
    result.cleanup_events_ = {*cleanup};
    result.enumeration_operation_units_ = {*r};
    result.total_operation_units_ = {*operations};
    result.deadline_quanta_ = {*quanta};
    result.deadline_seconds_ = {*deadline};
    return result;
}
} // namespace epycbench::monitoring::qualification::detail
