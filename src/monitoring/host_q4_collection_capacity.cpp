#include "monitoring/host_q4_collection_capacity.hpp"

#include "monitoring/host_q4_interval_summary_capacity.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
using FailureKind = MonitoringQ4CapacityFailureKind;
constexpr std::uint64_t pass_common_bytes = 3038;
constexpr std::uint64_t unsupported_not_started_common_bytes = 3049;
constexpr std::uint64_t unsupported_retired_common_bytes = 3045;
constexpr std::uint64_t pass_result_bytes = 80;
constexpr std::uint64_t unsupported_result_bytes = 151;
constexpr std::uint64_t duration_labels_bytes = 53;
constexpr std::uint64_t pass_initial_discovery_bytes = 161;
constexpr std::uint64_t pass_final_discovery_bytes = 159;
constexpr std::uint64_t unsupported_discovery_bytes = 168;
constexpr std::uint64_t domain_fixed_bytes = 249;
constexpr std::uint64_t prepared_fixed_bytes = 112;
constexpr std::uint64_t unsupported_preparation_fixed_bytes = 128;
constexpr std::uint64_t agreement_fixed_bytes = 167;
constexpr std::uint64_t typed_fixed_bytes = 130;
constexpr std::uint64_t unavailable_fixed_bytes = 47;
constexpr std::uint64_t maximum_u64_extra_digits = 19;
constexpr std::uint64_t success_prefix_bytes = 18;
constexpr std::uint64_t success_build_object_bytes = 1048575;
constexpr std::uint64_t success_middle_bytes = 12;
constexpr std::uint64_t success_suffix_lf_bytes = 60;
constexpr std::uint64_t success_capacity_fixed_bytes = 107;
auto digits(std::uint64_t value) noexcept -> std::uint64_t {
    std::uint64_t result = 1;
    while (value >= 10) {
        value /= 10;
        ++result;
    }
    return result;
}
auto extra_digits(std::uint64_t value) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    return monitoring_q4_checked_subtract(digits(value), 1, Reason::collection_extra_digits);
}
auto delta(std::uint64_t after, std::uint64_t before, Reason reason) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    return monitoring_q4_checked_subtract(after, before, reason);
}
auto ordinal_digits(std::uint64_t count) noexcept
    -> std::expected<std::uint64_t, MonitoringQ4CapacityFailure> {
    std::uint64_t result = 0, first = 1;
    for (std::uint64_t width = 1; width <= 20 && first <= count; ++width) {
        const auto next = first > UINT64_MAX / 10 ? UINT64_MAX : first * 10;
        const auto last = std::min(count, next == UINT64_MAX ? UINT64_MAX : next - 1);
        const auto amount = last - first + 1;
        const auto term = monitoring_q4_checked_multiply(amount, width, Reason::collection_ordinal_product);
        if (!term)
            return std::unexpected(term.error());
        const auto sum = monitoring_q4_checked_add(result, *term, Reason::collection_ordinal_sum);
        if (!sum)
            return std::unexpected(sum.error());
        result = *sum;
        if (last == count || next == UINT64_MAX)
            break;
        first = next;
    }
    return result;
}
struct Sum {
    std::uint64_t value{};
    Reason& failure;
    explicit Sum(Reason& reason) noexcept : failure{reason} {}
    bool add(std::uint64_t x, Reason reason) noexcept {
        auto r = monitoring_q4_checked_add(value, x, reason);
        if (!r) { failure = r.error().reason; return false; }
        value = *r;
        return true;
    }
    bool mul(std::uint64_t a, std::uint64_t b, Reason product, Reason sum) noexcept {
        auto r = monitoring_q4_checked_multiply(a, b, product);
        if (!r) { failure = r.error().reason; return false; }
        return add(*r, sum);
    }
};
auto array_commas(std::uint64_t n) noexcept -> std::uint64_t { return n == 0 ? 0 : n - 1; }
struct UnsupportedBound {
    std::uint64_t bytes{}, supported{};
    MonitoringQ4UnsupportedReason reason{};
    MonitoringQ4CapacityAudit audit{};
};

auto pass_bound(const MonitoringQ4CollectionCapacityInputs& in) noexcept
    -> std::expected<MonitoringQ4CapacityAudit, MonitoringQ4CapacityFailure> {
    auto failure_reason = Reason::none;
    const auto a = in.structural().admitted_cpu_count().value,
               n = in.structural().maximum_cpu_count().value;
    const auto b = in.structural().per_file_content_bytes().value;
    const auto ord = ordinal_digits(a);
    if (!ord)
        return std::unexpected(ord.error());
    const auto interval = monitoring_q4_pass_interval_summary_capacity({a});
    if (!interval)
        return std::unexpected(interval.error());
    const auto prep_count = monitoring_q4_checked_multiply(3, a, Reason::pass_preparations),
               reads = monitoring_q4_checked_multiply(48, a, Reason::pass_reads);
    if (!prep_count) return std::unexpected(prep_count.error());
    if (!reads) return std::unexpected(reads.error());
    std::uint64_t all_digits = 0;
    for (std::size_t i = 0; i < 10; ++i) {
        const auto term = monitoring_q4_checked_multiply(in.kernel_digit_buckets()[i], i + 1, Reason::pass_digit_product);
        if (!term)
            return std::unexpected(term.error());
        const auto sum = monitoring_q4_checked_add(all_digits, *term, Reason::pass_digit_sum);
        if (!sum)
            return std::unexpected(sum.error());
        all_digits = *sum;
    }
    const auto ordinal_extra = monitoring_q4_checked_subtract(*ord, a, Reason::pass_ordinal_extra);
    const auto all_digit_extra = monitoring_q4_checked_subtract(all_digits, a, Reason::pass_all_extra);
    const auto non_admitted = monitoring_q4_checked_subtract(n, a, Reason::pass_non_admitted);
    const auto admitted_extra =
        monitoring_q4_checked_subtract(in.admitted_decimal_digit_bytes(), a, Reason::pass_admitted_extra);
    const auto a_extra = extra_digits(a), n_extra = extra_digits(n);
    if (!ordinal_extra) return std::unexpected(ordinal_extra.error());
    if (!all_digit_extra) return std::unexpected(all_digit_extra.error());
    if (!non_admitted) return std::unexpected(non_admitted.error());
    if (!admitted_extra) return std::unexpected(admitted_extra.error());
    if (!a_extra) return std::unexpected(a_extra.error());
    if (!n_extra) return std::unexpected(n_extra.error());
    Sum z{failure_reason};
    MonitoringQ4CapacityAudit audit{};
    // Maximal common object with empty result/durations, including terminal LF; scope zeroes are
    // replaced below.
    if (!z.add(pass_common_bytes, Reason::pass_common_sum))
        goto overflow;
    audit.common_root_bytes = pass_common_bytes;
    {
        const auto before = z.value;
        if (!z.add(*a_extra, Reason::pass_scope_a_sum) || !z.add(*n_extra, Reason::pass_scope_n_sum) || !z.add(*a_extra, Reason::pass_scope_end_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_scope_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.scope_bytes = *change;
    }
    if (!z.add(pass_result_bytes, Reason::pass_result_sum))
        goto overflow;
    audit.result_wrapper_bytes = pass_result_bytes;
    if (!z.add(duration_labels_bytes, Reason::pass_duration_sum))
        goto overflow;
    audit.duration_wrapper_bytes = duration_labels_bytes;
    if (!z.add(interval->total_interval_summary_bytes().value, Reason::pass_interval_sum))
        goto overflow;
    audit.interval_summary_bytes = interval->total_interval_summary_bytes().value;
    // Six discoveries (three initial, three final), excluding their endpoint placeholder digits.
    {
        const auto before = z.value;
        if (!z.mul(3, pass_initial_discovery_bytes + pass_final_discovery_bytes, Reason::pass_discovery_fixed_product, Reason::pass_discovery_fixed_sum) ||
            !z.mul(6, *n_extra, Reason::pass_discovery_digits_product, Reason::pass_discovery_digits_sum) || !z.add(5, Reason::pass_discovery_commas_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_discovery_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.discovery_bytes = *change;
    }
    // Domains: maximal escaped driver/governor, uint64 frequency extrema, complete memberships.
    {
        const auto before = z.value;
        if (!z.mul(a, domain_fixed_bytes, Reason::pass_domain_fixed_product, Reason::pass_domain_fixed_sum) || !z.mul(a, b, Reason::pass_driver1_product, Reason::pass_driver1_sum) || !z.mul(a, b, Reason::pass_driver2_product, Reason::pass_driver2_sum) || !z.mul(a, b, Reason::pass_governor1_product, Reason::pass_governor1_sum) ||
            !z.mul(a, b, Reason::pass_governor2_product, Reason::pass_governor2_sum) || !z.add(*ordinal_extra, Reason::pass_domain_ord_sum) || !z.add(*all_digit_extra, Reason::pass_domain_cpus_sum) ||
            !z.add(*non_admitted, Reason::pass_domain_other_sum) || !z.add(array_commas(a), Reason::pass_domain_commas_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_domain_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.domain_bytes = *change;
    }
    // 3A preparations, 48A agreements and 48A typed reads. Endpoints belong to interval.
    {
        const auto before = z.value;
        if (!z.mul(*prep_count, prepared_fixed_bytes, Reason::pass_prep_fixed_product, Reason::pass_prep_fixed_sum) || !z.mul(3, *ordinal_extra, Reason::pass_prep_ord_product, Reason::pass_prep_ord_sum) ||
            !z.mul(3, *admitted_extra, Reason::pass_prep_cpus_product, Reason::pass_prep_cpus_sum) || !z.add(array_commas(*prep_count), Reason::pass_prep_commas_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_prep_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.preparation_bytes = *change;
    }
    {
        const auto before = z.value;
        if (!z.mul(*reads, agreement_fixed_bytes, Reason::pass_agreement_fixed_product, Reason::pass_agreement_fixed_sum) || !z.mul(*reads, maximum_u64_extra_digits, Reason::pass_agreement_frequency_product, Reason::pass_agreement_frequency_sum) ||
            !z.mul(48, *ordinal_extra, Reason::pass_agreement_ordinal_product, Reason::pass_agreement_ordinal_sum) || !z.mul(a, 21, Reason::pass_agreement_time_product, Reason::pass_agreement_time_sum) || !z.add(array_commas(*reads), Reason::pass_agreement_commas_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_agreement_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.agreement_bytes = *change;
    }
    {
        const auto before = z.value;
        if (!z.mul(*reads, typed_fixed_bytes, Reason::pass_typed_fixed_product, Reason::pass_typed_fixed_sum) || !z.mul(*reads, maximum_u64_extra_digits, Reason::pass_typed_frequency_product, Reason::pass_typed_frequency_sum) ||
            !z.mul(48, *ordinal_extra, Reason::pass_typed_ordinal_product, Reason::pass_typed_ordinal_sum) || !z.mul(a, 21, Reason::pass_typed_time_product, Reason::pass_typed_time_sum) || !z.add(array_commas(*reads), Reason::pass_typed_commas_sum))
            goto overflow;
        const auto change = delta(z.value, before, Reason::pass_typed_delta);
        if (!change)
            return std::unexpected(change.error());
        audit.typed_read_bytes = *change;
    }
    {
        Sum detail{failure_reason};
        if (!detail.add(all_digits, Reason::pass_audit_cpu_sum) || !detail.mul(3, in.admitted_decimal_digit_bytes(), Reason::pass_audit_admitted_product, Reason::pass_audit_admitted_sum))
            goto overflow;
        audit.cpu_decimal_total_bytes = detail.value;
    }
    {
        Sum detail{failure_reason};
        if (!detail.mul(100, *ord, Reason::pass_audit_ordinal_product, Reason::pass_audit_ordinal_sum))
            goto overflow;
        audit.ordinal_decimal_total_bytes = detail.value;
    }
    {
        Sum detail{failure_reason};
        if (!detail.mul(a, b, Reason::pass_audit_driver1_product, Reason::pass_audit_driver1_sum) || !detail.mul(a, b, Reason::pass_audit_driver2_product, Reason::pass_audit_driver2_sum) || !detail.mul(a, b, Reason::pass_audit_governor1_product, Reason::pass_audit_governor1_sum) || !detail.mul(a, b, Reason::pass_audit_governor2_product, Reason::pass_audit_governor2_sum))
            goto overflow;
        audit.token_escaping_total_bytes = detail.value;
    }
    {
        Sum detail{failure_reason};
        if (!detail.mul(38, a, Reason::pass_audit_frequency_product, Reason::pass_audit_frequency_sum) || !detail.mul(*reads, maximum_u64_extra_digits, Reason::pass_audit_raw_product, Reason::pass_audit_raw_sum) ||
            !detail.mul(*reads, maximum_u64_extra_digits, Reason::pass_audit_typed_product, Reason::pass_audit_typed_sum))
            goto overflow;
        audit.frequency_decimal_extra_bytes = detail.value;
    }
    audit.total_bytes = z.value;
    return audit;
overflow:
    return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, failure_reason});
}

auto unsupported_bound(const MonitoringQ4CollectionCapacityInputs& in,
                       std::optional<std::uint64_t> forced_supported = {},
                       std::optional<bool> forced_current = {}) noexcept
    -> std::expected<UnsupportedBound, MonitoringQ4CapacityFailure> {
    auto failure_reason = Reason::none;
    const auto a = in.structural().admitted_cpu_count().value,
               n = in.structural().maximum_cpu_count().value;
    const auto b = in.structural().per_file_content_bytes().value;
    const auto interval = monitoring_q4_unsupported_interval_summary_capacity({a});
    if (!interval)
        return std::unexpected(interval.error());
    UnsupportedBound best{};
    const auto first = forced_supported.value_or(0);
    const auto last = forced_supported ? *forced_supported + 1 : a;
    if (first >= a)
        return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::unsupported_supported_range});
    for (std::uint64_t s = first; s < last; ++s) {
        for (bool current : {false, true}) {
            if (forced_current && current != *forced_current)
                continue;
            const auto non_admitted = monitoring_q4_checked_subtract(n, a, Reason::unsupported_non_admitted);
            if (!non_admitted)
                return std::unexpected(non_admitted.error());
            const auto resolvable_p = monitoring_q4_checked_add(*non_admitted, s, Reason::unsupported_resolvable);
            if (!resolvable_p)
                return std::unexpected(resolvable_p.error());
            const auto p = current ? n : *resolvable_p;
            const auto ord = ordinal_digits(s);
            if (!ord)
                return std::unexpected(ord.error());
            const std::array<MonitoringQ4MultiplicityCategory, 1> cats{{{a, 2}}};
            const auto alloc =
                in.allocate_multiplicity_categories(cats, MonitoringQ4DigitPopulation::admitted);
            if (!alloc)
                return std::unexpected(alloc.error());
            std::uint64_t non_admitted_digits = 0;
            for (std::size_t i = 0; i < 10; ++i) {
                const auto term =
                    monitoring_q4_checked_multiply(in.non_admitted_digit_buckets()[i], i + 1, Reason::unsupported_digit_product);
                if (!term)
                    return std::unexpected(term.error());
                const auto sum = monitoring_q4_checked_add(non_admitted_digits, *term, Reason::unsupported_digit_sum);
                if (!sum)
                    return std::unexpected(sum.error());
                non_admitted_digits = *sum;
            }
            Sum z{failure_reason};
            MonitoringQ4CapacityAudit audit{};
            const auto ordinal_extra = monitoring_q4_checked_subtract(*ord, s, Reason::unsupported_ordinal_extra);
            const auto unavailable = monitoring_q4_checked_subtract(a, s, Reason::unsupported_unavailable);
            const auto a_extra = extra_digits(a), p_extra = extra_digits(p),
                       s_extra = extra_digits(s);
            if (!ordinal_extra) return std::unexpected(ordinal_extra.error());
            if (!unavailable) return std::unexpected(unavailable.error());
            if (!a_extra) return std::unexpected(a_extra.error());
            if (!p_extra) return std::unexpected(p_extra.error());
            if (!s_extra) return std::unexpected(s_extra.error());
            audit.common_root_bytes =
                s > 0 ? unsupported_retired_common_bytes : unsupported_not_started_common_bytes;
            if (!z.add(audit.common_root_bytes, Reason::unsupported_common_sum))
                goto overflow;
            {
                const auto before = z.value;
                if (!z.add(*a_extra, Reason::unsupported_scope_a_sum) || !z.add(*p_extra, Reason::unsupported_scope_p_sum) || !z.add(*s_extra, Reason::unsupported_scope_s_sum))
                    goto overflow;
                const auto change = delta(z.value, before, Reason::unsupported_scope_delta);
                if (!change)
                    return std::unexpected(change.error());
                audit.scope_bytes = *change;
            }
            if (!z.add(unsupported_result_bytes, Reason::unsupported_result_sum))
                goto overflow;
            audit.result_wrapper_bytes = unsupported_result_bytes;
            if (!z.add(duration_labels_bytes, Reason::unsupported_duration_sum))
                goto overflow;
            audit.duration_wrapper_bytes = duration_labels_bytes;
            if (!z.add(interval->total_interval_summary_bytes().value, Reason::unsupported_interval_sum))
                goto overflow;
            audit.interval_summary_bytes = interval->total_interval_summary_bytes().value;
            {
                const auto before = z.value;
                if (!z.add(unsupported_discovery_bytes + *p_extra, Reason::unsupported_discovery_sum))
                    goto overflow;
                const auto change = delta(z.value, before, Reason::unsupported_discovery_delta);
                if (!change)
                    return std::unexpected(change.error());
                audit.discovery_bytes = *change;
            }
            {
                const auto before = z.value;
                if (!z.mul(s, domain_fixed_bytes, Reason::unsupported_domain_fixed_product, Reason::unsupported_domain_fixed_sum) || !z.mul(s, b, Reason::unsupported_driver1_product, Reason::unsupported_driver1_sum) || !z.mul(s, b, Reason::unsupported_driver2_product, Reason::unsupported_driver2_sum) || !z.mul(s, b, Reason::unsupported_governor1_product, Reason::unsupported_governor1_sum) ||
                    !z.mul(s, b, Reason::unsupported_governor2_product, Reason::unsupported_governor2_sum) || !z.add(*ordinal_extra, Reason::unsupported_domain_ord_sum) || !z.add(array_commas(s), Reason::unsupported_domain_commas_sum))
                    goto overflow;
                const auto change = delta(z.value, before, Reason::unsupported_domain_delta);
                if (!change)
                    return std::unexpected(change.error());
                audit.domain_bytes = *change;
            }
            // CPU spellings across memberships, preparations and unavailable projection are
            // allocated jointly.
            const auto preparation_before = z.value;
            const auto cpu_baseline = monitoring_q4_checked_multiply(a, 2, Reason::unsupported_cpu_baseline);
            if (!cpu_baseline)
                return std::unexpected(cpu_baseline.error());
            const auto admitted_cpu_extra =
                monitoring_q4_checked_subtract(alloc->weighted_decimal_digit_bytes, *cpu_baseline, Reason::unsupported_admitted_extra);
            if (!admitted_cpu_extra)
                return std::unexpected(admitted_cpu_extra.error());
            if (!z.add(*admitted_cpu_extra, Reason::unsupported_cpu_extra_sum) || (s > 0 && !z.add(non_admitted_digits, Reason::unsupported_other_digits_sum)) ||
                (s > 0 && !z.add(*non_admitted, Reason::unsupported_other_separators_sum)))
                goto overflow;
            if (!z.mul(s, prepared_fixed_bytes, Reason::unsupported_prep_fixed_product, Reason::unsupported_prep_fixed_sum) || !z.add(*ordinal_extra, Reason::unsupported_prep_ord_sum) ||
                !z.mul(*unavailable, unsupported_preparation_fixed_bytes, Reason::unsupported_prep_missing_product, Reason::unsupported_prep_missing_sum) ||
                !z.add(array_commas(a), Reason::unsupported_prep_commas_sum))
                goto overflow;
            {
                const auto change = delta(z.value, preparation_before, Reason::unsupported_prep_delta);
                if (!change)
                    return std::unexpected(change.error());
                audit.preparation_bytes = *change;
            }
            {
                const auto before = z.value;
                if (!z.mul(*unavailable, unavailable_fixed_bytes, Reason::unsupported_unavailable_fixed_product, Reason::unsupported_unavailable_fixed_sum) ||
                    !z.add(array_commas(*unavailable), Reason::unsupported_unavailable_commas_sum))
                    goto overflow;
                const auto change = delta(z.value, before, Reason::unsupported_unavailable_delta);
                if (!change)
                    return std::unexpected(change.error());
                audit.unavailable_bytes = *change;
            }
            audit.total_bytes = z.value;
            audit.cpu_decimal_total_bytes = alloc->weighted_decimal_digit_bytes;
            if (s > 0) {
                const auto cpu =
                    monitoring_q4_checked_add(audit.cpu_decimal_total_bytes, non_admitted_digits, Reason::unsupported_audit_cpu_sum);
                if (!cpu)
                    return std::unexpected(cpu.error());
                audit.cpu_decimal_total_bytes = *cpu;
            }
            {
                Sum detail{failure_reason};
                if (!detail.mul(2, *ord, Reason::unsupported_audit_ordinal_product, Reason::unsupported_audit_ordinal_sum))
                    goto overflow;
                audit.ordinal_decimal_total_bytes = detail.value;
            }
            {
                Sum detail{failure_reason};
                if (!detail.mul(s, b, Reason::unsupported_audit_driver1_product, Reason::unsupported_audit_driver1_sum) || !detail.mul(s, b, Reason::unsupported_audit_driver2_product, Reason::unsupported_audit_driver2_sum) || !detail.mul(s, b, Reason::unsupported_audit_governor1_product, Reason::unsupported_audit_governor1_sum) ||
                    !detail.mul(s, b, Reason::unsupported_audit_governor2_product, Reason::unsupported_audit_governor2_sum))
                    goto overflow;
                audit.token_escaping_total_bytes = detail.value;
            }
            {
                Sum detail{failure_reason};
                if (!detail.mul(38, s, Reason::unsupported_audit_frequency_product, Reason::unsupported_audit_frequency_sum))
                    goto overflow;
                audit.frequency_decimal_extra_bytes = detail.value;
            }
            audit.supported_count = s;
            audit.reason = current ? MonitoringQ4UnsupportedReason::no_current_attribute
                                   : MonitoringQ4UnsupportedReason::no_resolvable_policy;
            if (z.value > best.bytes)
                best = {z.value, s,
                        current ? MonitoringQ4UnsupportedReason::no_current_attribute
                                : MonitoringQ4UnsupportedReason::no_resolvable_policy,
                        audit};
        }
    }
    return best;
overflow:
    return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, failure_reason});
}
} // namespace

auto monitoring_q4_unsupported_candidate_test(const MonitoringQ4CollectionCapacityInputs& inputs,
                                              std::uint64_t supported,
                                              MonitoringQ4UnsupportedReason reason) noexcept
    -> std::expected<MonitoringQ4CapacityAudit, MonitoringQ4CapacityFailure> {
    const auto value = unsupported_bound(
        inputs, supported, reason == MonitoringQ4UnsupportedReason::no_current_attribute);
    if (!value)
        return std::unexpected(value.error());
    return value->audit;
}

auto monitoring_q4_collection_capacity(const MonitoringQ4CollectionCapacityInputs& inputs) noexcept
    -> std::expected<MonitoringQ4CollectionCapacity, MonitoringQ4CapacityFailure> {
    // Accepted inputs already proved 7*N*(B+1), the larger topology-scaled byte product, fits u64.
    // Here A<=N and token evidence uses only 4*A*B. Record counts are bounded by 99*A with
    // A<=2^32, and every fixed record is below 512 bytes. Consequently arithmetic overflow is
    // unreachable for an accepted structural/input value; checked operations remain defense in
    // depth and make that invariant executable rather than relying on native wraparound.
    auto failure_reason = Reason::none;
    const auto pass = pass_bound(inputs);
    if (!pass)
        return std::unexpected(pass.error());
    const auto unsupported = unsupported_bound(inputs);
    if (!unsupported)
        return std::unexpected(unsupported.error());
    const auto evidence = std::max(pass->total_bytes, unsupported->bytes);
    // {"build_manifest": + object + ,"evidence": + embedded evidence + closed schema suffix.
    const auto embedded_evidence = monitoring_q4_checked_subtract(evidence, 1, Reason::success_embedded);
    if (!embedded_evidence)
        return std::unexpected(embedded_evidence.error());
    Sum z{failure_reason};
    if (!z.add(success_prefix_bytes, Reason::success_prefix_sum) || !z.add(success_build_object_bytes, Reason::success_build_sum) ||
        !z.add(success_middle_bytes, Reason::success_middle_sum) || !z.add(*embedded_evidence, Reason::success_evidence_sum) ||
        !z.add(success_suffix_lf_bytes, Reason::success_suffix_sum) || !z.add(success_capacity_fixed_bytes, Reason::success_capacity_sum) ||
        !z.add(digits(evidence), Reason::success_digits_sum))
        return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, failure_reason});
    auto success=monitoring_q4_checked_add(z.value,1, Reason::success_seed);
    if(!success)return std::unexpected(success.error());
    for(unsigned iteration{};iteration<21;++iteration){
        const auto next=monitoring_q4_checked_add(z.value,digits(*success), Reason::success_iteration_sum);
        if(!next)return std::unexpected(next.error());
        if(*next==*success)break;
        success=next;
        if(iteration==20)return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::arithmetic_overflow, Reason::success_convergence});
    }
    MonitoringQ4CollectionCapacity out;
    out.pass_.value = pass->total_bytes;
    out.unsupported_.value = unsupported->bytes;
    out.evidence_.value = evidence;
    out.success_.value = *success;
    out.pass_audit_ = *pass;
    out.unsupported_audit_ = unsupported->audit;
    out.unsupported_audit_.total_bytes = unsupported->bytes;
    out.unsupported_audit_.supported_count = unsupported->supported;
    out.unsupported_audit_.reason = unsupported->reason;
    return out;
}
} // namespace epycbench::monitoring::qualification::detail
