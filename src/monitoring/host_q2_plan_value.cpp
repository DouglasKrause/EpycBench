#include "monitoring/host_q2_plan_value.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2_plan.invalid"},
            .message = "Monitoring Q2 governed plan is invalid",
            .operation = "validate_monitoring_q2_plan",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
template <std::size_t N>
auto exact(const nlohmann::json& value, const std::array<std::string_view, N>& keys) -> bool {
    if (!value.is_object() || value.size() != keys.size())
        return false;
    for (const auto key : keys)
        if (!value.contains(key))
            return false;
    return true;
}
auto u64(const nlohmann::json& value, bool positive = true) -> std::optional<std::uint64_t> {
    if (!value.is_number_unsigned())
        return std::nullopt;
    const auto result = value.get<std::uint64_t>();
    if (positive && result == 0U)
        return std::nullopt;
    return result;
}
} // namespace

auto decode_monitoring_q2_plan(std::string_view bytes)
    -> std::expected<MonitoringQ2PlanValue, core::Error> {
    if (bytes.empty() || bytes.size() > 65536U || bytes.back() != '\n')
        return std::unexpected{failure("framing")};
    try {
        const auto value = nlohmann::json::parse(bytes.begin(), bytes.end());
        auto canonical = core::canonicalize_json(value);
        if (!canonical || *canonical + "\n" != bytes)
            return std::unexpected{failure("canonical")};
        if (!exact(value,
                   std::array<std::string_view, 5>{"schema", "collection", "controlled_events",
                                                   "evidence_limits", "privacy_validation"}) ||
            value.at("schema") != MonitoringQ2PlanValue::schema ||
            value.at("privacy_validation") != "passed")
            return std::unexpected{failure("shape")};
        const auto& c = value.at("collection");
        const auto& e = value.at("evidence_limits");
        const auto& p = value.at("controlled_events");
        if (!exact(c,
                   std::array<std::string_view, 8>{
                       "voluntary_trials_per_worker", "preemption_trials_per_worker",
                       "periodic_drain_after_trials", "ring_data_pages", "per_trial_deadline_ms",
                       "execution_deadline_seconds", "safety_iteration_ceiling",
                       "fixed_reserve_seconds"}) ||
            !exact(e,
                   std::array<std::string_view, 10>{
                       "maximum_worker_count", "maximum_total_trials", "total_decoded_records",
                       "records_per_drain", "drains", "raw_in_memory_bytes",
                       "canonical_evidence_bytes", "backend_stdout_bytes", "backend_stderr_bytes",
                       "diagnostic_count"}) ||
            !exact(p, std::array<std::string_view, 11>{
                          "voluntary_protocol", "preemption_protocol", "helper_policy",
                          "helper_count", "targets_concurrent", "target_affinity_changed",
                          "target_policy_changed", "priority_changed", "privilege_required",
                          "benchmark_kernel_overlap", "adaptive_retry"}))
            return std::unexpected{failure("shape")};
        for (auto key : {"target_affinity_changed", "target_policy_changed", "priority_changed",
                         "privilege_required", "benchmark_kernel_overlap", "adaptive_retry"})
            if (!p.at(key).is_boolean() || p.at(key).get<bool>())
                return std::unexpected{failure("policy")};
        if (p.at("voluntary_protocol") != "private-worker-synchronization/v1" ||
            p.at("preemption_protocol") != "private-helper-preemption/v1" ||
            p.at("helper_policy") != "SCHED_OTHER" || p.at("helper_count") != 1U ||
            p.at("targets_concurrent") != 1U)
            return std::unexpected{failure("policy")};
        MonitoringQ2PlanValue result;
#define EB_Q2_FIELD(object, member)                                                                \
    do {                                                                                           \
        auto parsed = u64((object).at(#member));                                                   \
        if (!parsed)                                                                               \
            return std::unexpected{failure("numeric")};                                            \
        result.member = *parsed;                                                                   \
    } while (false)
        EB_Q2_FIELD(c, voluntary_trials_per_worker);
        EB_Q2_FIELD(c, preemption_trials_per_worker);
        EB_Q2_FIELD(c, periodic_drain_after_trials);
        EB_Q2_FIELD(c, ring_data_pages);
        EB_Q2_FIELD(c, per_trial_deadline_ms);
        EB_Q2_FIELD(c, execution_deadline_seconds);
        EB_Q2_FIELD(c, safety_iteration_ceiling);
        EB_Q2_FIELD(c, fixed_reserve_seconds);
        EB_Q2_FIELD(e, maximum_worker_count);
        EB_Q2_FIELD(e, maximum_total_trials);
        EB_Q2_FIELD(e, total_decoded_records);
        EB_Q2_FIELD(e, records_per_drain);
        EB_Q2_FIELD(e, drains);
        EB_Q2_FIELD(e, raw_in_memory_bytes);
        EB_Q2_FIELD(e, canonical_evidence_bytes);
        EB_Q2_FIELD(e, backend_stdout_bytes);
        EB_Q2_FIELD(e, backend_stderr_bytes);
        EB_Q2_FIELD(e, diagnostic_count);
#undef EB_Q2_FIELD
        constexpr auto maximum_safety_iterations = std::numeric_limits<std::uint64_t>::max();
        if ((result.ring_data_pages & (result.ring_data_pages - 1U)) != 0U ||
            result.maximum_worker_count != 512U || result.voluntary_trials_per_worker != 4U ||
            result.preemption_trials_per_worker != 4U || result.periodic_drain_after_trials != 2U ||
            result.ring_data_pages != 8U || result.per_trial_deadline_ms != 250U ||
            result.fixed_reserve_seconds != 120U ||
            result.safety_iteration_ceiling != maximum_safety_iterations ||
            result.maximum_total_trials != 4096U || result.execution_deadline_seconds != 1144U ||
            result.total_decoded_records != 1048576U || result.records_per_drain != 65536U ||
            result.drains != 3072U || result.raw_in_memory_bytes != 67108864U ||
            result.canonical_evidence_bytes != 16777216U ||
            result.backend_stdout_bytes != 20971520U || result.backend_stderr_bytes != 1048576U ||
            result.diagnostic_count != 64U)
            return std::unexpected{failure("governed_constant")};
        if (result.maximum_worker_count >
            std::numeric_limits<std::uint64_t>::max() /
                (result.voluntary_trials_per_worker + result.preemption_trials_per_worker))
            return std::unexpected{failure("arithmetic")};
        const auto trials = result.maximum_worker_count * (result.voluntary_trials_per_worker +
                                                           result.preemption_trials_per_worker);
        if (trials != result.maximum_total_trials ||
            trials > std::numeric_limits<std::uint64_t>::max() / result.per_trial_deadline_ms)
            return std::unexpected{failure("arithmetic")};
        const auto budget_ms = trials * result.per_trial_deadline_ms;
        if (budget_ms > std::numeric_limits<std::uint64_t>::max() - 999U)
            return std::unexpected{failure("arithmetic")};
        const auto budget_seconds = (budget_ms + 999U) / 1000U;
        if (budget_seconds >
                std::numeric_limits<std::uint64_t>::max() - result.fixed_reserve_seconds ||
            budget_seconds + result.fixed_reserve_seconds != result.execution_deadline_seconds)
            return std::unexpected{failure("arithmetic")};
        return result;
    } catch (...) {
        return std::unexpected{failure("parse")};
    }
}
} // namespace epycbench::monitoring::qualification::detail
