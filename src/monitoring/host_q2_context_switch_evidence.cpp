#include "monitoring/host_q2_context_switch_evidence.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/platform_linux/perf_context_switch.hpp"
#include "monitoring/privacy_validation.hpp"
#include "monitoring/host_q2_plan_value.hpp"

#include <linux/perf_event.h>
#include <nlohmann/json.hpp>
#include <ranges>
#include <limits>
#include <cerrno>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::ordered_json;
auto failure(std::string_view classification = "evidence_construction") -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.evidence.invalid"},
            .message = "Q2 evidence construction failed",
            .operation = "construct_monitoring_q2_context_switch_evidence",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
template <class Object>
auto exact(const Object& value, std::initializer_list<std::string_view> keys) -> bool {
    return value.is_object() && value.size() == keys.size() &&
           std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}
auto duration(const MonitoringQ2DurationSummary& value) -> Json {
    return {{"count", value.count}, {"min_ns", value.minimum_ns},
            {"lower_median_ns", value.lower_median_ns}, {"max_ns", value.maximum_ns}};
}
auto lowercase_hex(std::string_view value, std::size_t size) -> bool {
    return value.size() == size && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
auto prefixed_sha(std::string_view value) -> bool {
    return value.starts_with("sha256:") && lowercase_hex(value.substr(7), 64);
}
auto safe_token(std::string_view value) -> bool {
    return !value.empty() && value.size() <= 128U &&
           std::ranges::all_of(value, [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
           });
}
auto qualification_id(std::string_view value) -> bool {
    constexpr std::string_view prefix = "MON-007.host-v2.";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > prefix.size() + 64U)
        return false;
    const auto suffix = value.substr(prefix.size());
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return alnum(static_cast<unsigned char>(suffix.front())) &&
           alnum(static_cast<unsigned char>(suffix.back())) &&
           std::ranges::all_of(suffix, [&](unsigned char c) { return alnum(c) || c == '-'; });
}
auto unsigned_value(const Json& value) -> bool { return value.is_number_unsigned(); }
auto valid_duration(const Json& value) -> bool {
    if (!exact(value, {"count", "min_ns", "lower_median_ns", "max_ns"}) ||
        !unsigned_value(value["count"]) || !unsigned_value(value["min_ns"]) ||
        !unsigned_value(value["lower_median_ns"]) || !unsigned_value(value["max_ns"])) return false;
    const auto minimum = value["min_ns"].get<std::uint64_t>();
    const auto median = value["lower_median_ns"].get<std::uint64_t>();
    const auto maximum = value["max_ns"].get<std::uint64_t>();
    const auto count = value["count"].get<std::uint64_t>();
    return count == 0U ? minimum == 0U && median == 0U && maximum == 0U
                       : minimum <= median && median <= maximum;
}
auto valid_summary(const Json& value, bool worker) -> bool {
    if (!exact(value, worker
            ? std::initializer_list<std::string_view>{"voluntary_trials","preemption_trials","drains",
                  "decoded_records","prepare_duration","drain_duration","worker_id"}
            : std::initializer_list<std::string_view>{"voluntary_trials","preemption_trials","drains",
                  "decoded_records","prepare_duration","drain_duration"})) return false;
    for (auto key : {"voluntary_trials", "preemption_trials", "drains", "decoded_records"})
        if (!unsigned_value(value[key])) return false;
    return (!worker || unsigned_value(value["worker_id"])) &&
           valid_duration(value["prepare_duration"]) && valid_duration(value["drain_duration"]);
}
auto actual_capacity(const Json& value, bool pass) -> bool {
    auto fixed = value;
    fixed["runtime_binding"]["workers"] = Json::array();
    if (pass) {
        fixed["result"]["trials"] = Json::array();
        fixed["result"]["workers"] = Json::array();
        fixed["result"]["global"] = Json::object();
    }
    if (fixed.dump().size() + 1U > 65536U) return false;
    for (const auto& worker : value["runtime_binding"]["workers"])
        if (worker.dump().size() > 2048U) return false;
    if (pass) {
        for (const auto& trial : value["result"]["trials"])
            if (trial.dump().size() > 768U) return false;
        for (const auto& summary_value : value["result"]["workers"])
            if (summary_value.dump().size() > 1024U) return false;
        if (value["result"]["global"].dump().size() > 1024U) return false;
    }
    return true;
}
auto summary(const MonitoringQ2ContextSwitchSummary& value) -> Json {
    Json result{{"voluntary_trials", value.voluntary_trials},
                {"preemption_trials", value.preemption_trials}, {"drains", value.drains},
                {"decoded_records", value.decoded_records},
                {"prepare_duration", duration(value.prepare_duration)},
                {"drain_duration", duration(value.drain_duration)}};
    if (value.worker) result["worker_id"] = value.worker->value();
    return result;
}
auto source(const MonitoringQ2CollectorPlan& plan, std::string_view permission) -> Json {
    if (plan.plan().ring_data_pages > std::numeric_limits<std::uint64_t>::max() - 1U ||
        plan.plan().ring_data_pages + 1U >
            std::numeric_limits<std::uint64_t>::max() / plan.page_size_bytes())
        throw std::overflow_error{"Q2 mapping size"};
    const auto opened = platform_linux::PerfContextSwitchSource::configuration();
    const perf_event_attr zero_fields{};
    if (opened.type != PERF_TYPE_SOFTWARE || opened.config != PERF_COUNT_SW_DUMMY ||
        opened.sample_type != PERF_SAMPLE_TIME || opened.clock_id != CLOCK_MONOTONIC_RAW ||
        !opened.sample_id_all || !opened.use_clock_id || !opened.context_switch ||
        opened.disabled || opened.inherit || opened.write_backward ||
        zero_fields.read_format != 0U || zero_fields.sample_period != 0U ||
        zero_fields.sample_freq != 0U)
        throw std::invalid_argument{"Q2 perf source configuration"};
    return {{"implementation_id", "linux-perf-context-switch-records"},
            {"clock_id", "CLOCK_MONOTONIC_RAW"},
            {"configuration", {{"type", "PERF_TYPE_SOFTWARE"},
              {"attr_size_bytes", opened.size}, {"config", "PERF_COUNT_SW_DUMMY"},
              {"sample_type", "PERF_SAMPLE_TIME"}, {"sample_id_all", opened.sample_id_all},
              {"use_clockid", opened.use_clock_id}, {"clockid", "CLOCK_MONOTONIC_RAW"},
              {"context_switch", opened.context_switch}, {"disabled", opened.disabled},
              {"inherit", opened.inherit}, {"write_backward", opened.write_backward},
              {"read_format", zero_fields.read_format},
              {"sample_period", zero_fields.sample_period}, {"sample_freq", zero_fields.sample_freq}}},
            {"attachment", {{"scope", "thread"}, {"cpu", -1}, {"group_fd", -1},
                              {"flags", "PERF_FLAG_FD_CLOEXEC"}}},
            {"permission", permission}, {"ring_data_pages", plan.plan().ring_data_pages},
            {"mapping_bytes", (plan.plan().ring_data_pages + 1U) * plan.page_size_bytes()}};
}
auto base(const MonitoringQ2CollectorPlan& plan, std::string_view disposition,
          std::string_view permission) -> Json {
    const auto attestation = Json::parse(plan.running_build_attestation());
    if (!attestation.is_object() || !attestation.contains("build_identity") ||
        !attestation["build_identity"].is_string() ||
        attestation["build_identity"].get_ref<const std::string&>() !=
            plan.running_build_identity().to_hex())
        throw std::invalid_argument{"Q2 build attestation identity"};
    const auto& p = plan.evidence_provenance();
    const auto& q0 = plan.q0_provenance();
    auto values = Json::parse(p.q2_plan_bytes);
    Json workers = Json::array();
    for (const auto& worker : plan.workers())
        workers.push_back({{"worker_id", worker.worker.value()}, {"linux_cpu", worker.linux_cpu.value()},
                           {"stable_core_id", worker.stable_core.value()},
                           {"stable_pu_id", worker.stable_processing_unit.value()}});
    return {{"schema", "epycbench.monitoring-q2-context-switch-evidence/v2"},
            {"qualification_id", plan.qualification_id()}, {"disposition", disposition},
            {"provenance", {{"repository_commit", plan.repository_commit()},
              {"build_identity", "sha256:" + plan.running_build_identity().to_hex()},
              {"profile_sha256", plan.profile_sha256()},
              {"host_environment_digest", p.host_environment_digest},
              {"q0b_evidence_sha256", q0.q0b_evidence_sha256},
              {"q0_profile_repository_commit", p.q0_profile_repository_commit},
              {"q1_live_repository_commit", p.historical_q1_commit},
              {"predecessor_manifest_sha256", p.predecessor_manifest_sha256.to_hex()},
              {"predecessor_checksums_sha256", p.predecessor_checksums_sha256.to_hex()},
              {"q1_evidence_sha256", p.q1_evidence_sha256.to_hex()}}},
            {"runtime_binding", {{"qualification_plan_digest", "sha256:" + plan.qualification_plan_digest().to_hex()},
              {"page_size_bytes", plan.page_size_bytes()}, {"worker_count", plan.workers().size()},
              {"workers", std::move(workers)}}},
            {"collection_plan", {{"schema", MonitoringQ2PlanValue::schema},
              {"path", MonitoringQ2PlanValue::path}, {"sha256", p.q2_plan_sha256.to_hex()},
              {"values", std::move(values)}}},
            {"source", source(plan, permission)}, {"result", Json::object()},
            {"finalization", Json::object()},
            {"proof_limitation", "linux-uapi-preempt-flag-no-independent-scheduler-oracle/v1"}};
}
} // namespace

auto validate_monitoring_q2_evidence_capacity_proof(MonitoringQ2EvidenceAllowances limits) noexcept
    -> bool {
    try {
        const auto maximum_number = std::numeric_limits<std::uint64_t>::max();
        const Json worker{{"worker_id", std::numeric_limits<std::uint32_t>::max()},
                          {"linux_cpu", std::numeric_limits<std::uint32_t>::max()},
                          {"stable_core_id", std::string(255U, 'x')},
                          {"stable_pu_id", std::string(255U, 'x')}};
        const Json duration{{"count", maximum_number}, {"min_ns", maximum_number},
                            {"lower_median_ns", maximum_number}, {"max_ns", maximum_number}};
        const Json summary_value{{"worker_id", std::numeric_limits<std::uint32_t>::max()},
            {"voluntary_trials", maximum_number}, {"preemption_trials", maximum_number},
            {"drains", maximum_number}, {"decoded_records", maximum_number},
            {"prepare_duration", duration}, {"drain_duration", duration}};
        const Json trial{{"worker_id", std::numeric_limits<std::uint32_t>::max()},
            {"kind", "preemption"}, {"ordinal", std::numeric_limits<std::uint32_t>::max()},
            {"interval_start_ns", maximum_number}, {"interval_end_ns", maximum_number},
            {"voluntary_count", maximum_number}, {"involuntary_count", maximum_number},
            {"scoped_record_count", maximum_number}, {"capture_complete", true},
            {"drain_ordinal", maximum_number}};
        auto voluntary_trial = trial;
        voluntary_trial["kind"] = "voluntary";
        auto global_summary = summary_value;
        global_summary.erase("worker_id");
        const auto hex64 = std::string(64U, 'f');
        const auto commit = std::string(40U, 'f');
        const Json plan_values{{"schema", MonitoringQ2PlanValue::schema},
            {"privacy_validation", "passed"},
            {"collection", {{"voluntary_trials_per_worker",4U},{"preemption_trials_per_worker",4U},
                {"periodic_drain_after_trials",2U},{"ring_data_pages",8U},
                {"per_trial_deadline_ms",250U},{"execution_deadline_seconds",1144U},
                {"safety_iteration_ceiling",maximum_number},{"fixed_reserve_seconds",120U}}},
            {"controlled_events", {{"voluntary_protocol","private-worker-synchronization/v1"},
                {"preemption_protocol","private-helper-preemption/v1"},{"helper_policy","SCHED_OTHER"},
                {"helper_count",1U},{"targets_concurrent",1U},{"target_affinity_changed",false},
                {"target_policy_changed",false},{"priority_changed",false},{"privilege_required",false},
                {"benchmark_kernel_overlap",false},{"adaptive_retry",false}}},
            {"evidence_limits", {{"maximum_worker_count",512U},{"maximum_total_trials",4096U},
                {"total_decoded_records",1048576U},{"records_per_drain",65536U},{"drains",3072U},
                {"raw_in_memory_bytes",67108864U},{"canonical_evidence_bytes",16777216U},
                {"backend_stdout_bytes",20971520U},{"backend_stderr_bytes",1048576U},
                {"diagnostic_count",64U}}}};
        const Json configuration{{"type","PERF_TYPE_SOFTWARE"},{"attr_size_bytes",4096U},
            {"config","PERF_COUNT_SW_CONTEXT_SWITCHES"},{"sample_type","PERF_SAMPLE_TIME"},
            {"sample_id_all",true},{"use_clockid",true},{"clockid","CLOCK_MONOTONIC_RAW"},
            {"context_switch",true},{"disabled",false},{"inherit",false},{"write_backward",false},
            {"read_format",0},{"sample_period",1},{"sample_freq",0}};
        const Json pass_fixed{{"schema", "epycbench.monitoring-q2-context-switch-evidence/v1"},
            {"qualification_id", "MON-007.host-v2." + std::string(64U, 'z')},
            {"disposition", "pass"},
            {"provenance", {{"repository_commit", commit}, {"build_identity", "sha256:" + hex64},
                {"profile_sha256", hex64}, {"host_environment_digest", "sha256:" + hex64},
                {"q0b_evidence_sha256", hex64}, {"q0_profile_repository_commit", commit},
                {"q1_live_repository_commit", commit}, {"predecessor_manifest_sha256", hex64},
                {"predecessor_checksums_sha256", hex64}, {"q1_evidence_sha256", hex64}}},
            {"runtime_binding", {{"qualification_plan_digest", "sha256:" + hex64},
                {"page_size_bytes", 65536U}, {"worker_count", 512U}, {"workers", Json::array()}}},
            {"collection_plan", {{"schema", MonitoringQ2PlanValue::schema},
                {"path", MonitoringQ2PlanValue::path}, {"sha256", hex64}, {"values", plan_values}}},
            {"source", {{"implementation_id", "linux-perf-context-switch-records"},
                {"clock_id", "CLOCK_MONOTONIC_RAW"}, {"configuration", configuration},
                {"attachment", {{"scope","thread"},{"cpu",-1},{"group_fd",-1},
                                  {"flags","PERF_FLAG_FD_CLOEXEC"}}}, {"permission", "supported"},
                {"ring_data_pages", 8U}, {"mapping_bytes", 589824U}}},
            {"result", {{"trials", Json::array()}, {"workers", Json::array()},
                {"global", Json::object()}, {"decoded_record_count", maximum_number},
                {"drain_count", maximum_number}, {"capture_complete", true}}},
            {"finalization", {{"target", "retired"}, {"helper", "retired"},
                {"source", "retired"}, {"session", "retired"}, {"authority", "retired"}}},
            {"proof_limitation", "linux-uapi-preempt-flag-no-independent-scheduler-oracle/v1"}};
        auto unsupported_fixed = pass_fixed;
        unsupported_fixed["disposition"] = "unsupported";
        unsupported_fixed["source"]["permission"] = "feature_unavailable";
        unsupported_fixed["result"] = {{"configuration_identity", "linux-perf-context-switch-configuration/v1"},
            {"classification", "feature_unavailable"}, {"underlying_code", std::string(128U, 'e')},
            {"os_error", std::numeric_limits<std::int32_t>::max()}};
        unsupported_fixed["finalization"] = {{"target", "not_started"}, {"helper", "not_started"},
            {"source", "not_started"}, {"session", "retired"}, {"authority", "retired"}};
        // The historical v1 strings are a conservative fixed-size bound for current v2:
        // the version suffix has equal width, DUMMY is shorter, and 0 is no longer than 1.
        auto current_pass_fixed = pass_fixed;
        current_pass_fixed["schema"] = "epycbench.monitoring-q2-context-switch-evidence/v2";
        current_pass_fixed["source"]["configuration"]["config"] = "PERF_COUNT_SW_DUMMY";
        current_pass_fixed["source"]["configuration"]["sample_period"] = 0;
        auto current_unsupported_fixed = unsupported_fixed;
        current_unsupported_fixed["schema"] = "epycbench.monitoring-q2-context-switch-evidence/v2";
        current_unsupported_fixed["source"]["configuration"]["config"] = "PERF_COUNT_SW_DUMMY";
        current_unsupported_fixed["source"]["configuration"]["sample_period"] = 0;
        current_unsupported_fixed["result"]["configuration_identity"] =
            "linux-perf-context-switch-configuration/v2";
        if (current_pass_fixed.dump().size() > pass_fixed.dump().size() ||
            current_unsupported_fixed.dump().size() > unsupported_fixed.dump().size() ||
            pass_fixed.dump().size() + 1U > limits.fixed ||
            unsupported_fixed.dump().size() + 1U > limits.fixed ||
            worker.dump().size() > limits.worker || trial.dump().size() > limits.trial ||
            voluntary_trial.dump().size() > limits.trial ||
            summary_value.dump().size() > limits.summary ||
            global_summary.dump().size() > limits.summary)
            return false;
        constexpr std::uint64_t workers = 512U, trials = 4096U, summaries = 513U;
        if (workers > (std::numeric_limits<std::uint64_t>::max() - limits.fixed) / limits.worker)
            return false;
        auto required = limits.fixed + workers * limits.worker;
        if (trials > (std::numeric_limits<std::uint64_t>::max() - required) / limits.trial)
            return false;
        required += trials * limits.trial;
        if (summaries > (std::numeric_limits<std::uint64_t>::max() - required) / limits.summary)
            return false;
        required += summaries * limits.summary;
        return required == 4785152U && required <= limits.total;
    } catch (...) { return false; }
}

auto construct_monitoring_q2_context_switch_evidence(
    MonitoringQ2ContextSwitchCollectionOutcome&& outcome) noexcept
    -> std::expected<MonitoringQ2EvidenceBytes, core::Error> {
    try {
        if (!validate_monitoring_q2_evidence_capacity_proof())
            return std::unexpected{failure()};
        Json evidence;
        if (auto* passed = std::get_if<MonitoringQ2ContextSwitchCollection>(&outcome)) {
            auto consumed = std::move(*passed);
            outcome.emplace<MonitoringQ2FailedCollection>(MonitoringQ2CollectionStage::preparation,
                                                          failure(),
                                                          decltype(MonitoringQ2FailedCollection::cleanup){},
                                                          0U, false);
            evidence = base(consumed.plan(), "pass", "supported");
            Json trials = Json::array();
            for (const auto& row : consumed.trials()) {
                trials.push_back({{"worker_id", row.worker.value()},
                  {"kind", row.kind == MonitoringQ2TrialKind::voluntary ? "voluntary" : "preemption"},
                  {"ordinal", row.ordinal}, {"interval_start_ns", row.start_ns},
                  {"interval_end_ns", row.end_ns}, {"voluntary_count", row.voluntary_count},
                  {"involuntary_count", row.involuntary_count},
                  {"scoped_record_count", row.scoped_records}, {"capture_complete", row.capture_complete},
                  {"drain_ordinal", row.drain_ordinal}});
            }
            Json workers = Json::array();
            for (const auto& item : consumed.summaries()) workers.push_back(summary(item));
            evidence["result"] = {{"trials", std::move(trials)}, {"workers", std::move(workers)},
              {"global", summary(consumed.global_summary())},
              {"decoded_record_count", consumed.global_summary().decoded_records},
              {"drain_count", consumed.global_summary().drains}, {"capture_complete", true}};
            evidence["finalization"] = {{"target", "retired"}, {"helper", "retired"},
              {"source", "retired"}, {"session", "retired"}, {"authority", "retired"}};
        } else if (auto* unsupported = std::get_if<MonitoringQ2UnsupportedCollection>(&outcome)) {
            auto consumed = std::move(*unsupported);
            outcome.emplace<MonitoringQ2FailedCollection>(MonitoringQ2CollectionStage::preparation,
                                                          failure(),
                                                          decltype(MonitoringQ2FailedCollection::cleanup){},
                                                          0U, false);
            const auto classification = consumed.classification ==
                    MonitoringQ2UnsupportedClassification::permission_denied
                ? "permission_denied" : "feature_unavailable";
            evidence = base(consumed.plan, "unsupported", classification);
            const auto& primary = consumed.detail.primary;
            if (!primary.os_error || primary.os_error->value <= 0)
                return std::unexpected{failure()};
            evidence["result"] = {{"configuration_identity", "linux-perf-context-switch-configuration/v2"},
              {"classification", classification}, {"underlying_code", primary.code.value()},
              {"os_error", primary.os_error->value}};
            evidence["finalization"] = {{"target", "not_started"}, {"helper", "not_started"},
              {"source", "not_started"}, {"session", "retired"}, {"authority", "retired"}};
        } else return std::unexpected{failure()};
        if (!privacy_safe(evidence)) return std::unexpected{failure()};
        auto canonical = core::canonicalize_json(evidence);
        if (!canonical || canonical->size() + 1U > 16777216U)
            return std::unexpected{failure("framing")};
        canonical->push_back('\n');
        if (!validate_monitoring_q2_context_switch_evidence(*canonical))
            return std::unexpected{failure()};
        return MonitoringQ2EvidenceBytes{std::move(*canonical)};
    } catch (...) { return std::unexpected{failure("construction_exception")}; }
}

auto validate_monitoring_q2_context_switch_evidence(std::string_view bytes) noexcept
    -> std::expected<void, core::Error> {
    try {
        if (!validate_monitoring_q2_evidence_capacity_proof())
            return std::unexpected{failure("capacity")};
        if (bytes.empty() || bytes.back() != '\n' || bytes.size() > 16777216U)
            return std::unexpected{failure("top")};
        auto value = Json::parse(bytes.begin(), bytes.end() - 1);
        const bool historical = value.is_object() && value.contains("schema") &&
            value["schema"] == "epycbench.monitoring-q2-context-switch-evidence/v1";
        if (historical && core::sha256(bytes).to_hex() !=
                "016c774dec1cd45a058693066f5751332dab3ba84f4346b1509a4de0aafe3e21")
            return std::unexpected{failure("historical_scope")};
        auto canonical = core::canonicalize_json(value);
        if (!canonical || *canonical + "\n" != bytes || !privacy_safe(value) ||
            !exact(value, {"schema","qualification_id","disposition","provenance","runtime_binding",
                           "collection_plan","source","result","finalization","proof_limitation"}) ||
            value["schema"] != (historical ? "epycbench.monitoring-q2-context-switch-evidence/v1"
                                            : "epycbench.monitoring-q2-context-switch-evidence/v2") ||
            value["proof_limitation"] !=
                "linux-uapi-preempt-flag-no-independent-scheduler-oracle/v1" ||
            (value["disposition"] != "pass" && value["disposition"] != "unsupported") ||
            !exact(value["finalization"], {"target","helper","source","session","authority"}))
            return std::unexpected{failure("common")};
        const auto& provenance = value["provenance"];
        const auto& binding = value["runtime_binding"];
        const auto& collection = value["collection_plan"];
        const auto& source_value = value["source"];
        const auto& plan_values = collection["values"];
        const auto& configuration = source_value["configuration"];
        const auto& attachment = source_value["attachment"];
        if (!exact(provenance, {"repository_commit","build_identity","profile_sha256",
              "host_environment_digest","q0b_evidence_sha256","q0_profile_repository_commit",
              "q1_live_repository_commit","predecessor_manifest_sha256",
              "predecessor_checksums_sha256","q1_evidence_sha256"}) ||
            !exact(binding, {"qualification_plan_digest","page_size_bytes","worker_count","workers"}) ||
            !exact(collection, {"schema","path","sha256","values"}) ||
            !exact(plan_values, {"schema","collection","controlled_events","evidence_limits",
                                 "privacy_validation"}) ||
            !exact(plan_values["collection"], {"execution_deadline_seconds","fixed_reserve_seconds",
                 "per_trial_deadline_ms","periodic_drain_after_trials","preemption_trials_per_worker",
                 "ring_data_pages","safety_iteration_ceiling","voluntary_trials_per_worker"}) ||
            !exact(plan_values["controlled_events"], {"adaptive_retry","benchmark_kernel_overlap",
                 "helper_count","helper_policy","preemption_protocol","priority_changed",
                 "privilege_required","target_affinity_changed","target_policy_changed",
                 "targets_concurrent","voluntary_protocol"}) ||
            !exact(plan_values["evidence_limits"], {"backend_stderr_bytes","backend_stdout_bytes",
                 "canonical_evidence_bytes","diagnostic_count","drains","maximum_total_trials",
                 "maximum_worker_count","raw_in_memory_bytes","records_per_drain",
                 "total_decoded_records"}) ||
            !exact(source_value, {"implementation_id","clock_id","configuration","attachment",
                                  "permission","ring_data_pages","mapping_bytes"}) ||
            !exact(configuration, {"type","attr_size_bytes","config","sample_type","sample_id_all",
                  "use_clockid","clockid","context_switch","disabled","inherit","write_backward",
                  "read_format","sample_period","sample_freq"}) ||
            !exact(attachment, {"scope","cpu","group_fd","flags"}) ||
            !value["qualification_id"].is_string() ||
            !qualification_id(value["qualification_id"].get_ref<const std::string&>()) ||
            !provenance["repository_commit"].is_string() ||
            !lowercase_hex(provenance["repository_commit"].get_ref<const std::string&>(), 40) ||
            !provenance["q0_profile_repository_commit"].is_string() ||
            !lowercase_hex(provenance["q0_profile_repository_commit"].get_ref<const std::string&>(), 40) ||
            !provenance["q1_live_repository_commit"].is_string() ||
            !lowercase_hex(provenance["q1_live_repository_commit"].get_ref<const std::string&>(), 40) ||
            !prefixed_sha(provenance["build_identity"].get_ref<const std::string&>()) ||
            !prefixed_sha(provenance["host_environment_digest"].get_ref<const std::string&>()) ||
            !lowercase_hex(provenance["profile_sha256"].get_ref<const std::string&>(), 64) ||
            !lowercase_hex(provenance["q0b_evidence_sha256"].get_ref<const std::string&>(), 64) ||
            !lowercase_hex(provenance["predecessor_manifest_sha256"].get_ref<const std::string&>(), 64) ||
            !lowercase_hex(provenance["predecessor_checksums_sha256"].get_ref<const std::string&>(), 64) ||
            !lowercase_hex(provenance["q1_evidence_sha256"].get_ref<const std::string&>(), 64) ||
            !prefixed_sha(binding["qualification_plan_digest"].get_ref<const std::string&>()) ||
            !collection["sha256"].is_string() ||
            !lowercase_hex(collection["sha256"].get_ref<const std::string&>(), 64) ||
            !binding["workers"].is_array() || binding["workers"].empty() ||
            binding["workers"].size() > 512U || !binding["worker_count"].is_number_unsigned() ||
            binding["workers"].size() != binding["worker_count"].get<std::size_t>() ||
            collection["schema"] != MonitoringQ2PlanValue::schema ||
            collection["path"] != MonitoringQ2PlanValue::path ||
            source_value["implementation_id"] != "linux-perf-context-switch-records" ||
            source_value["clock_id"] != "CLOCK_MONOTONIC_RAW" ||
            attachment["scope"] != "thread" || attachment["cpu"] != -1 ||
            attachment["group_fd"] != -1 ||
            attachment["flags"] != "PERF_FLAG_FD_CLOEXEC")
            return std::unexpected{failure("common")};
        auto plan_bytes = core::canonicalize_json(plan_values);
        if (!plan_bytes) return std::unexpected{failure("plan")};
        plan_bytes->push_back('\n');
        auto governed_plan = decode_monitoring_q2_plan(*plan_bytes);
        if (core::sha256(*plan_bytes).to_hex() !=
                collection["sha256"].get_ref<const std::string&>() ||
            !governed_plan)
            return std::unexpected{failure("plan")};
        if (!binding["page_size_bytes"].is_number_unsigned() ||
            binding["page_size_bytes"].get<std::uint64_t>() == 0U ||
            binding["page_size_bytes"].get<std::uint64_t>() > 65536U ||
            !source_value["ring_data_pages"].is_number_unsigned() ||
            !source_value["mapping_bytes"].is_number_unsigned() ||
            source_value["ring_data_pages"] != plan_values["collection"]["ring_data_pages"] ||
            source_value["ring_data_pages"].get<std::uint64_t>() + 1U >
                std::numeric_limits<std::uint64_t>::max() /
                    binding["page_size_bytes"].get<std::uint64_t>() ||
            source_value["mapping_bytes"].get<std::uint64_t>() !=
                (source_value["ring_data_pages"].get<std::uint64_t>() + 1U) *
                    binding["page_size_bytes"].get<std::uint64_t>() ||
            configuration["type"] != "PERF_TYPE_SOFTWARE" ||
            configuration["config"] != (historical ? "PERF_COUNT_SW_CONTEXT_SWITCHES"
                                                    : "PERF_COUNT_SW_DUMMY") ||
            configuration["sample_type"] != "PERF_SAMPLE_TIME" ||
            configuration["sample_id_all"] != true || configuration["use_clockid"] != true ||
            configuration["clockid"] != "CLOCK_MONOTONIC_RAW" ||
            configuration["context_switch"] != true || configuration["disabled"] != false ||
            configuration["inherit"] != false || configuration["write_backward"] != false ||
            configuration["read_format"] != 0 ||
            configuration["sample_period"] != (historical ? 1 : 0) ||
            configuration["sample_freq"] != 0 ||
            !configuration["attr_size_bytes"].is_number_unsigned() ||
            configuration["attr_size_bytes"].get<std::uint64_t>() == 0U ||
            configuration["attr_size_bytes"].get<std::uint64_t>() > 4096U)
            return std::unexpected{failure("source")};
        for (std::size_t index{}; index < binding["workers"].size(); ++index) {
            const auto& worker = binding["workers"][index];
            if (!exact(worker, {"worker_id","linux_cpu","stable_core_id","stable_pu_id"}) ||
                !worker["worker_id"].is_number_unsigned() ||
                !worker["linux_cpu"].is_number_unsigned() ||
                worker["linux_cpu"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
                !worker["stable_core_id"].is_string() ||
                worker["stable_core_id"].get_ref<const std::string&>().empty() ||
                worker["stable_core_id"].get_ref<const std::string&>().size() > 255U ||
                !worker["stable_pu_id"].is_string() ||
                worker["stable_pu_id"].get_ref<const std::string&>().empty() ||
                worker["stable_pu_id"].get_ref<const std::string&>().size() > 255U ||
                !topology::StableCoreId::parse(
                    worker["stable_core_id"].get_ref<const std::string&>()) ||
                !topology::StableProcessingUnitId::parse(
                    worker["stable_pu_id"].get_ref<const std::string&>()) ||
                worker["worker_id"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
                (index != 0U && binding["workers"][index - 1U]["worker_id"].get<std::uint64_t>() >=
                                    worker["worker_id"].get<std::uint64_t>()))
                return std::unexpected{failure("worker")};
        }
        const bool pass = value["disposition"] == "pass";
        if (!actual_capacity(value, pass)) return std::unexpected{failure("capacity")};
        const auto& result = value["result"];
        const auto& finalized = value["finalization"];
        if (pass) {
            const auto trials_per_worker = governed_plan->voluntary_trials_per_worker +
                                           governed_plan->preemption_trials_per_worker;
            const auto expected_trials = binding["workers"].size() * trials_per_worker;
            const auto voluntary_groups =
                (governed_plan->voluntary_trials_per_worker +
                 governed_plan->periodic_drain_after_trials - 1U) /
                governed_plan->periodic_drain_after_trials;
            const auto preemption_groups =
                (governed_plan->preemption_trials_per_worker +
                 governed_plan->periodic_drain_after_trials - 1U) /
                governed_plan->periodic_drain_after_trials;
            const auto drains_per_worker = 2U + voluntary_groups + preemption_groups;
            if (source_value["permission"] != "supported" ||
                !exact(result, {"trials","workers","global","decoded_record_count","drain_count",
                                "capture_complete"}) || result["capture_complete"] != true ||
                !result["trials"].is_array() || !result["workers"].is_array() ||
                result["workers"].size() != binding["workers"].size() ||
                result["trials"].size() != expected_trials ||
                !valid_summary(result["global"], false) ||
                !result["decoded_record_count"].is_number_unsigned() ||
                !result["drain_count"].is_number_unsigned() ||
                result["decoded_record_count"] != result["global"]["decoded_records"] ||
                result["drain_count"] != result["global"]["drains"] ||
                result["drain_count"] != binding["workers"].size() * drains_per_worker ||
                finalized["target"] != "retired" || finalized["helper"] != "retired" ||
                finalized["source"] != "retired" || finalized["session"] != "retired" ||
                finalized["authority"] != "retired")
                return std::unexpected{failure("pass")};
            std::uint64_t sum_decoded{}, sum_drains{};
            for (std::size_t index{}; index < result["workers"].size(); ++index) {
                const auto& worker_summary = result["workers"][index];
                if (!valid_summary(worker_summary, true) ||
                    worker_summary["worker_id"] != binding["workers"][index]["worker_id"] ||
                    worker_summary["voluntary_trials"] != governed_plan->voluntary_trials_per_worker ||
                    worker_summary["preemption_trials"] != governed_plan->preemption_trials_per_worker ||
                    worker_summary["drains"] != drains_per_worker ||
                    worker_summary["prepare_duration"]["count"] != 1U ||
                    worker_summary["drain_duration"]["count"] != worker_summary["drains"])
                    return std::unexpected{failure("summary")};
                const auto worker_decoded = worker_summary["decoded_records"].get<std::uint64_t>();
                const auto worker_drains = worker_summary["drains"].get<std::uint64_t>();
                if (sum_decoded > std::numeric_limits<std::uint64_t>::max() - worker_decoded ||
                    sum_drains > std::numeric_limits<std::uint64_t>::max() - worker_drains)
                    return std::unexpected{failure("arithmetic")};
                sum_decoded += worker_decoded;
                sum_drains += worker_drains;
            }
            if (result["global"]["voluntary_trials"] !=
                    binding["workers"].size() * governed_plan->voluntary_trials_per_worker ||
                result["global"]["preemption_trials"] !=
                    binding["workers"].size() * governed_plan->preemption_trials_per_worker ||
                result["global"]["decoded_records"] != sum_decoded ||
                result["global"]["drains"] != sum_drains ||
                result["global"]["prepare_duration"]["count"] != binding["workers"].size() ||
                result["global"]["drain_duration"]["count"] != sum_drains ||
                sum_decoded > governed_plan->total_decoded_records)
                return std::unexpected{failure("summary")};
            std::uint64_t scoped_total{};
            std::uint64_t previous_end{};
            std::size_t trial_index{};
            for (const auto& trial : result["trials"]) {
                const auto worker_index = trial_index / static_cast<std::size_t>(trials_per_worker);
                const auto within = trial_index % static_cast<std::size_t>(trials_per_worker);
                if (!exact(trial, {"worker_id","kind","ordinal","interval_start_ns","interval_end_ns",
                      "voluntary_count","involuntary_count","scoped_record_count","capture_complete",
                      "drain_ordinal"}) || trial["capture_complete"] != true ||
                    !trial["worker_id"].is_number_unsigned() || !trial["ordinal"].is_number_unsigned() ||
                    trial["ordinal"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
                    !trial["interval_start_ns"].is_number_unsigned() ||
                    !trial["interval_end_ns"].is_number_unsigned() ||
                    trial["interval_start_ns"].get<std::uint64_t>() >
                        trial["interval_end_ns"].get<std::uint64_t>() ||
                    (trial_index != 0U && trial["interval_start_ns"].get<std::uint64_t>() < previous_end) ||
                    !trial["voluntary_count"].is_number_unsigned() ||
                    !trial["involuntary_count"].is_number_unsigned() ||
                    !trial["scoped_record_count"].is_number_unsigned() ||
                    !trial["drain_ordinal"].is_number_unsigned() ||
                    trial["drain_ordinal"].get<std::uint64_t>() == 0U ||
                    trial["drain_ordinal"].get<std::uint64_t>() >
                        result["drain_count"].get<std::uint64_t>() ||
                    trial["worker_id"] != binding["workers"][worker_index]["worker_id"] ||
                    trial["kind"] != (within < governed_plan->voluntary_trials_per_worker
                                          ? "voluntary" : "preemption") ||
                    trial["ordinal"].get<std::uint64_t>() !=
                        (within < governed_plan->voluntary_trials_per_worker
                             ? within : within - governed_plan->voluntary_trials_per_worker) ||
                    trial["drain_ordinal"].get<std::uint64_t>() !=
                        worker_index * drains_per_worker + 2U +
                            (within < governed_plan->voluntary_trials_per_worker
                                 ? within / governed_plan->periodic_drain_after_trials
                                 : voluntary_groups +
                                       (within - governed_plan->voluntary_trials_per_worker) /
                                           governed_plan->periodic_drain_after_trials) ||
                    trial["voluntary_count"].get<std::uint64_t>() >
                        std::numeric_limits<std::uint64_t>::max() -
                            trial["involuntary_count"].get<std::uint64_t>() ||
                    trial["voluntary_count"].get<std::uint64_t>() +
                            trial["involuntary_count"].get<std::uint64_t>() !=
                        trial["scoped_record_count"].get<std::uint64_t>() ||
                    (within < governed_plan->voluntary_trials_per_worker
                         ? trial["voluntary_count"].get<std::uint64_t>() == 0U
                         : trial["involuntary_count"].get<std::uint64_t>() == 0U))
                    return std::unexpected{failure("trial")};
                previous_end = trial["interval_end_ns"].get<std::uint64_t>();
                if (scoped_total > std::numeric_limits<std::uint64_t>::max() -
                                       trial["scoped_record_count"].get<std::uint64_t>())
                    return std::unexpected{failure("arithmetic")};
                scoped_total += trial["scoped_record_count"].get<std::uint64_t>();
                ++trial_index;
            }
            if (scoped_total > result["decoded_record_count"].get<std::uint64_t>())
                return std::unexpected{failure("trial")};
        } else {
            if ((source_value["permission"] != "permission_denied" &&
                 source_value["permission"] != "feature_unavailable") ||
                !exact(result, {"configuration_identity","classification","underlying_code","os_error"}) ||
                result["configuration_identity"] !=
                    (historical ? "linux-perf-context-switch-configuration/v1"
                                : "linux-perf-context-switch-configuration/v2") ||
                result["classification"] != source_value["permission"] ||
                !result["underlying_code"].is_string() ||
                !safe_token(result["underlying_code"].get_ref<const std::string&>()) ||
                !result["os_error"].is_number_integer() || result["os_error"].get<std::int64_t>() <= 0 ||
                finalized["target"] != "not_started" || finalized["helper"] != "not_started" ||
                finalized["source"] != "not_started" || finalized["session"] != "retired" ||
                finalized["authority"] != "retired")
                return std::unexpected{failure("unsupported")};
            const auto number = result["os_error"].get<std::int64_t>();
            if (number > std::numeric_limits<std::int32_t>::max() ||
                (result["classification"] == "permission_denied" && number != EPERM && number != EACCES) ||
                (result["classification"] == "feature_unavailable" && number != ENOSYS &&
                 number != EOPNOTSUPP && number != ENODEV))
                return std::unexpected{failure("unsupported")};
        }
        return {};
    } catch (...) { return std::unexpected{failure("validation_exception")}; }
}
} // namespace epycbench::monitoring::qualification::detail
