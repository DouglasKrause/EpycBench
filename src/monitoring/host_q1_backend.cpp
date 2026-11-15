#include "monitoring/host_q1_backend.hpp"
#include "monitoring/host_q1_backend_test_access.hpp"

#include "core/json_ownership.hpp"

#include "epycbench/build_manifest.hpp"
#include "monitoring/host_running_build_attestation_value.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/monitoring/host_environment.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "epycbench/runtime/runner_control_monitor.hpp"
#include "monitoring/host_q1_page_fault_collector.hpp"
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "monitoring/host_qualification_composition.hpp"
#include "monitoring/privacy_validation.hpp"
#include "runtime/controller_process_production_authority.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <ranges>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::size_t evidence_limit = 8U * 1024U * 1024U;
constexpr std::size_t envelope_limit = 10U * 1024U * 1024U;
constexpr std::array<std::string_view, 13> backend_stages{"repository", "build_attestation",
    "session", "worker_admission", "preparation", "oracle", "production_source", "clock",
    "association", "timing", "finalization", "evidence_construction", "contained_failure"};
thread_local void* test_context{};
thread_local MonitoringHostQ1BackendTestAccess::BuildProducer test_build{};
thread_local MonitoringHostQ1BackendTestAccess::StageHook test_stage{};
thread_local MonitoringHostQ1BackendOperations* test_operations{};

auto backend_error(std::string code, std::string classification,
                   core::ErrorCategory category = core::ErrorCategory::monitoring) -> core::Error {
    return {.category = category,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Monitoring Q1 backend failed",
            .operation = "run_monitoring_host_q1_backend",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto stage(std::string_view classification, const core::Error* underlying = nullptr) -> core::Error {
    auto result = backend_error("monitoring.q1.backend.stage_failure", std::string{classification});
    if (underlying) {
        const auto& code = underlying->code.value();
        const auto safe = !code.empty() && code.size() <= 128U && code.front() >= 'a' &&
            code.front() <= 'z' && std::ranges::all_of(code, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                       c == '_' || c == '-';
            });
        if (safe) result.context["underlying_code"] = code;
        result.os_error = underlying->os_error;
    }
    return result;
}
auto fail(std::string_view classification, const core::Error* underlying = nullptr)
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
    return std::unexpected{MonitoringHostQ1BackendFailure{stage(classification, underlying), {}}};
}
struct ExceptionStock final {
    std::array<MonitoringHostQ1BackendFailure, backend_stages.size()> failures;
    std::array<core::Error, 8> cleanup;
    ExceptionStock()
        : failures{[] {
              std::array<MonitoringHostQ1BackendFailure, backend_stages.size()> result;
              for (std::size_t index{}; index < result.size(); ++index) {
                  result[index].primary = stage(backend_stages[index]);
                  result[index].secondary.reserve(8U);
              }
              return result;
          }()},
          cleanup{stage("finalization"), stage("finalization"), stage("finalization"),
                  stage("finalization"), stage("finalization"), stage("finalization"),
                  stage("finalization"), stage("finalization")} {
        for (auto& item : cleanup) item.context["underlying_code"] = std::string(128U, 'x');
    }
    auto take_cleanup(std::string_view active, MonitoringQ1BoundedRetirement retired) noexcept
        -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
        auto found = std::ranges::find(backend_stages, active);
        const auto index = found == backend_stages.end()
                               ? backend_stages.size() - 1U
                               : static_cast<std::size_t>(found - backend_stages.begin());
        auto result = std::move(failures[index]);
        const auto count = std::min(retired.count, cleanup.size());
        for (std::size_t item{}; item < count; ++item) {
                const auto& retired_error = *retired.failures[item];
                const auto& code = retired_error.code.value();
                auto& projected = cleanup[item].context["underlying_code"].get_ref<std::string&>();
                const auto selected = code.empty() || code.size() > 128U
                                          ? std::string_view{"monitoring.cleanup_failed"}
                                          : std::string_view{code};
                projected.assign(selected);
                cleanup[item].os_error = retired_error.os_error;
                result.secondary.push_back(std::move(cleanup[item]));
        }
        return std::unexpected{std::move(result)};
    }
    auto take(std::string_view active,
              std::optional<MonitoringQ1PlanV2AuthoritySession>& authority) noexcept
        -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
        return take_cleanup(active, authority ? authority->retire_before_binding()
                                              : MonitoringQ1BoundedRetirement{});
    }
};
auto authority_stage(const core::Error& error) -> std::string_view {
    const auto& code = error.code.value();
    if (code == "monitoring.q1_plan_authority.plan_rejected" ||
        (code == "monitoring.q1_plan_authority.public_input_rejected" &&
         error.context.value("classification", "") == "plan"))
        return "collection_plan";
    if (code == "monitoring.q1_plan_authority.q0_rejected" ||
        code == "monitoring.q1_plan_authority.public_input_rejected")
        return "profile_q0";
    if (code == "monitoring.q1_plan_authority.fresh_plan_mismatch")
        return "worker_admission";
    return "repository";
}
auto authority_failure(MonitoringQ1GitCommandFailure failure)
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
    const auto selected = authority_stage(failure.primary);
    MonitoringHostQ1BackendFailure result{stage(selected, &failure.primary), {}};
    for (const auto& cleanup : failure.cleanup)
        result.secondary.push_back(stage("finalization", &cleanup));
    return std::unexpected{std::move(result)};
}
template <class Object>
auto exact_keys(const Object& value, std::initializer_list<std::string_view> expected) -> bool {
    if (!value.is_object() || value.size() != expected.size()) return false;
    return std::ranges::all_of(expected, [&](std::string_view key) { return value.contains(key); });
}
} // namespace
namespace {
auto counters(const MonitoringQ1FaultCounters& value) -> Json {
    return {{"minor_faults", value.minor_faults}, {"major_faults", value.major_faults}};
}
auto summary(const MonitoringQ1PageFaultSummary& value) -> Json {
    Json result = {{"count", value.count}, {"minimum_elapsed_ns", value.minimum_elapsed_ns},
                   {"maximum_elapsed_ns", value.maximum_elapsed_ns},
                   {"lower_median_elapsed_ns", value.lower_median_elapsed_ns},
                   {"minor_equality_count", value.minor_equality_count},
                   {"major_equality_count", value.major_equality_count}};
    if (value.worker_id) result["worker_id"] = value.worker_id->value();
    return result;
}
auto build_success(MonitoringQ1PageFaultCollection&& collection, Json build)
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
    try {
        const auto& plan = collection.plan();
        const auto& provenance = plan.evidence_provenance();
        Json workers = Json::array();
        for (auto worker : plan.worker_ids())
            workers.push_back({{"worker_id", worker.value()},
                               {"observation_count", plan.observations_per_worker()}});
        Json observations = Json::array();
        for (const auto& row : collection.observations())
            observations.push_back({{"cycle_index", row.cycle_index}, {"worker_id", row.worker_id.value()},
                {"collection_start_ns", row.collection_start_ns}, {"collection_end_ns", row.collection_end_ns},
                {"elapsed_ns", row.elapsed_ns}, {"oracle_before", counters(row.oracle_before)},
                {"production", counters(row.production)}, {"oracle_after", counters(row.oracle_after)},
                {"minor_equal", row.minor_equal}, {"major_equal", row.major_equal}});
        Json worker_summaries = Json::array();
        for (const auto& item : collection.worker_summaries()) worker_summaries.push_back(summary(item));
        const auto mode = plan.derivation_mode() == MonitoringQ1PlanDerivationMode::default_mode
                              ? "default" : "governed_override";
        Json derivation = {{"algorithm", MonitoringQ1PlanV2Value::algorithm}, {"mode", mode},
            {"target_total_observations", MonitoringQ1PlanV2Value::target_total},
            {"worker_count", plan.worker_ids().size()},
            {"conservative_observation_capacity_per_worker", plan.capacity()},
            {"observations_per_worker", plan.observations_per_worker()},
            {"total_observations", plan.total_observations()},
            {"per_observation_allowance_ms", plan.per_observation_allowance_ms()},
            {"fixed_reserve_seconds", plan.fixed_reserve_seconds()},
            {"execution_deadline_seconds", plan.execution_deadline_seconds()}};
        Json evidence = {{"schema", MonitoringQ1PlanV2Value::evidence_schema},
            {"qualification_id", plan.qualification_id()},
            {"tool", {{"identity", "epycbench.monitoring-qualification-tool/v1"}, {"version", "1.0.0"}}},
            {"repository", {{"commit", plan.repository_commit()}, {"dirty", false}}},
            {"build", {{"identity", "sha256:" + build.at("build_identity").get<std::string>()}}},
            {"profile", {{"schema", MonitoringQ1PlanV2Value::profile_schema},
                {"identity", "epycbench.monitoring-qualification.host-generic/v2"},
                {"stage", "final_frozen_profile"}, {"sha256", plan.profile_sha256()}}},
            {"q0_provenance", {{"qualification_id", plan.qualification_id()},
                {"manifest_sha256", provenance.manifest_sha256},
                {"checksums_sha256", provenance.checksums_sha256},
                {"host_environment_sha256", provenance.host_environment_sha256},
                {"q0b_evidence_sha256", provenance.q0b_evidence_sha256}}},
            {"collection_plan", {{"schema", MonitoringQ1PlanV2Value::schema},
                {"path", "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json"},
                {"sha256", plan.plan_sha256()}, {"repository_commit", plan.repository_commit()},
                {"observations_per_worker", plan.observations_per_worker()},
                {"conservative_observation_capacity_per_worker", plan.capacity()},
                {"production_warmup_observations", 0}, {"sequencing", "round_robin_cycle_then_worker_id"},
                {"stopping", "fixed_inventory"}, {"retry", "forbidden"}, {"discard", "forbidden"},
                {"execution_deadline_seconds", plan.execution_deadline_seconds()},
                {"success_envelope_bytes", plan.envelope_limit_bytes()},
                {"evidence_artifact_bytes", plan.evidence_limit_bytes()},
                {"build_manifest_bytes", plan.build_manifest_limit_bytes()},
                {"maximum_successor_tree_bytes", plan.maximum_successor_tree_bytes()},
                {"derivation", std::move(derivation)}}},
            {"host_environment", {{"schema", "epycbench.monitoring-host-environment/v2"},
                {"sha256", provenance.host_environment_digest}}},
            {"qualification_plan", {{"schema", "epycbench.monitoring-host-qualification-plan/v2"},
                {"sha256", "sha256:" + plan.qualification_plan_digest().to_hex()}}},
            {"phase", "Q1"}, {"prerequisites", Json::array()},
            {"implementation", {{"source", "linux-proc-task-stat"}, {"fields", Json::array({10, 12})},
                {"counters", "cumulative-uint64"}, {"clock", "clock-monotonic-raw"}}},
            {"oracle", {{"source", "linux-getrusage-thread"}, {"scope", "target-worker"},
                {"comparison", "before-production-after-equality"}}},
            {"workers", std::move(workers)}, {"observations", std::move(observations)},
            {"summaries", {{"workers", std::move(worker_summaries)},
                            {"global", summary(collection.global_summary())}}},
            {"source_semantics", {{"availability", "passed"}, {"readability", "passed"},
                {"field_projection", "passed"}, {"counter_width", "passed"},
                {"cumulative_units", "passed"}, {"worker_scope", "passed"},
                {"worker_tid_association", "passed"}, {"oracle_agreement", "passed"},
                {"operational_errors", "passed"}, {"privacy", "passed"}}},
            {"known_event", {{"required", false}, {"disposition", "not_performed"}}},
            {"finalization", {{"disposition", "succeeded"}}}, {"disposition", "pass"},
            {"privacy_validation", "passed"}};
        if (!privacy_safe(evidence)) return fail("evidence_construction");
        auto artifact = core::canonicalize_json(evidence);
        if (!artifact || artifact->size() + 1U > evidence_limit) return fail("evidence_construction");
        Json wrapper = {{"schema", MonitoringQ1PlanV2Value::backend_schema},
                        {"build_manifest", std::move(build)}, {"evidence", std::move(evidence)}};
        if (!privacy_safe(wrapper)) return fail("evidence_construction");
        auto canonical = core::canonicalize_json(wrapper);
        if (!canonical || canonical->size() + 1U > envelope_limit) return fail("evidence_construction");
        canonical->push_back('\n');
        return MonitoringHostQ1BackendResult{MonitoringHostQ1BackendBytes{std::move(*canonical)}};
    } catch (...) { return fail("evidence_construction"); }
}
auto safe_projection(const core::Error& error) -> std::expected<Json, core::Error> {
    auto projected = runtime::ipc::project_failure(error);
    if (projected) return *projected;
    return std::unexpected{projected.error()};
}
auto safe_token(std::string_view value, std::size_t maximum) noexcept -> bool {
    return !value.empty() && value.size() <= maximum && value.front() >= 'a' &&
        value.front() <= 'z' && std::ranges::all_of(value.substr(1), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                   c == '_' || c == '-';
        });
}
auto safe_pointer(std::string_view value) noexcept -> bool {
    if ((!value.empty() && value.front() != '/') || value.contains('\\')) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (static_cast<unsigned char>(value[index]) < 0x20U) return false;
        if (value[index] == '~' && (index + 1U == value.size() ||
            (value[index + 1U] != '0' && value[index + 1U] != '1'))) return false;
        if (value[index] == '~') ++index;
    }
    return true;
}
} // namespace

auto monitoring_host_q1_usage_failure() noexcept -> MonitoringHostQ1BackendFailure {
    return {{.category = core::ErrorCategory::usage,
             .code = core::StableErrorCode{"monitoring.q1.backend.usage"},
             .message = "Invalid hidden Monitoring Q1 arguments",
             .operation = "run_monitoring_host_q1_backend", .os_error = std::nullopt,
             .context = nlohmann::json::object(), .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}, {}};
}

auto run_monitoring_host_q1_backend(const std::filesystem::path& root, std::string_view qid,
    runtime::ControllerProcessProductionAuthority& controller) noexcept
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
    std::string_view active_stage = "repository";
    std::optional<MonitoringQ1PlanV2AuthoritySession> authority;
    std::optional<ExceptionStock> exception_stock;
    try {
        exception_stock.emplace();
        auto authenticated = MonitoringQ1PlanV2AuthoritySession::authenticate(root, qid);
        if (!authenticated) return authority_failure(std::move(authenticated.error()));
        authority.emplace(std::move(*authenticated));
        const auto reject_before_binding = [&](std::string_view classification,
                                               const core::Error& underlying) {
            MonitoringHostQ1BackendFailure result{stage(classification, &underlying), {}};
            auto retirement = authority->retire_before_binding();
            for (std::size_t index{}; index < retirement.count; ++index)
                result.secondary.push_back(
                    stage("finalization", &*retirement.failures[index]));
            return std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure>{
                std::unexpected{std::move(result)}};
        };
        active_stage = "build_attestation";
        const auto commit = authority->authenticated_repository_commit();
        auto build = attest_monitoring_running_build_value(
            test_build ? test_build(test_context, commit) : build_manifest_json(), commit);
        if (!build) return reject_before_binding("build_attestation", build.error());
        if (test_stage) {
            auto injected = test_stage(test_context, "session");
            if (!injected) return reject_before_binding("session", injected.error());
        }
        active_stage = "session";
        auto input = test_operations
                         ? test_operations->load(test_operations->context, root)
                         : load_monitoring_host_input_template(root);
        if (!input) return reject_before_binding("session", input.error());
        auto session = test_operations
                           ? test_operations->acquire(test_operations->context, std::move(*input))
                           : acquire_runner_context_session(std::move(*input));
        if (!session) return reject_before_binding("session", session.error());
        if (test_stage) { auto observed=test_stage(test_context,"environment");
            if(!observed) return reject_before_binding("session",observed.error()); }
        auto environment = derive_host_environment(*session);
        if (!environment) return reject_before_binding("session", environment.error());
        if (test_stage) { auto observed=test_stage(test_context,"collection_plan");
            if(!observed) return reject_before_binding("session",observed.error()); }
        auto plan = resolve_monitoring_host_qualification_plan(*session, *environment);
        if (!plan) return reject_before_binding("session", plan.error());
        if (test_stage) { auto observed=test_stage(test_context,"runner_context");
            if(!observed) return reject_before_binding("session",observed.error()); }
        auto context = session->runner_context();
        if (!context) return reject_before_binding("session", context.error());
        runtime::PairwiseProductionCompositionContext composition{context->get(), controller.request_ids()};
        active_stage = "worker_admission";
        if (test_stage) { auto observed=test_stage(test_context,"worker_admission");
            if(!observed) return reject_before_binding("worker_admission",observed.error()); }
        auto inputs = compose_host_qualification_calibration_inputs(*plan, *session, composition);
        if (!inputs) return reject_before_binding("worker_admission", inputs.error());
        auto collector_plan = authority->bind_fresh_plan(*session, *plan, inputs->bandwidth().first());
        if (!collector_plan) {
            authority.reset();
            return authority_failure(std::move(collector_plan.error()));
        }
        authority.reset();
        active_stage = "preparation";
        if (test_stage) {
            auto injected = test_stage(test_context, "post_bind");
            if (!injected) return fail("preparation", &injected.error());
        }
        runtime::RunnerCancellation cancellation;
        if (test_stage) { auto observed=test_stage(test_context,"collection");
            if(!observed) return fail("preparation",&observed.error()); }
        auto collection = test_operations
                              ? test_operations->collect(test_operations->context,
                                                         std::move(*collector_plan), cancellation)
                              : collect_monitoring_q1_page_faults(std::move(*collector_plan), cancellation);
        if (!collection) {
            static constexpr std::array names{"preparation", "oracle", "production_source", "clock",
                                               "association", "timing", "finalization"};
            auto index = static_cast<std::size_t>(collection.error().stage());
            MonitoringHostQ1BackendFailure result{stage(names.at(index), &collection.error().primary()), {}};
            result.oracle_mismatch = collection.error().oracle_mismatch();
            for (const auto& error : collection.error().secondary())
                result.secondary.push_back(stage("finalization", &error));
            return std::unexpected{std::move(result)};
        }
        active_stage = "evidence_construction";
        if (test_stage) { auto observed=test_stage(test_context,"evidence");
            if(!observed) return fail("evidence_construction",&observed.error()); }
        return build_success(std::move(*collection), std::move(*build));
    } catch (...) {
        if (exception_stock) return exception_stock->take(active_stage, authority);
        return fail(active_stage);
    }
}

auto project_monitoring_host_q1_error(const MonitoringHostQ1BackendFailure& failure) noexcept
    -> std::expected<Json, core::Error> {
    try {
        auto primary = safe_projection(failure.primary);
        if (!primary) return std::unexpected{primary.error()};
        Json secondary = Json::array();
        for (const auto& error : failure.secondary) {
            auto value = safe_projection(error); if (!value) return std::unexpected{value.error()};
            secondary.push_back(std::move(*value));
        }
        Json result{{"schema", "epycbench.monitoring-host-q1-backend-error/v1"},
                    {"failure", std::move(*primary)}, {"secondary_failures", std::move(secondary)}};
        if (!privacy_safe(result)) return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "privacy")};
        if (failure.oracle_mismatch) {
            const auto& witness = *failure.oracle_mismatch;
            const auto& context = failure.primary.context;
            if (failure.primary.code.value() != "monitoring.q1.backend.stage_failure" ||
                !context.is_object() || !context.contains("classification") ||
                context["classification"] != "oracle" || !context.contains("underlying_code") ||
                context["underlying_code"] != "monitoring.q1.collector.oracle_mismatch" ||
                (witness.counter_kind != MonitoringQ1FaultCounterKind::minor_faults &&
                 witness.counter_kind != MonitoringQ1FaultCounterKind::major_faults) ||
                (witness.before == witness.production && witness.production == witness.after))
                return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "oracle_witness_invalid")};
            // This complete base remains available until the optional projection
            // commits; retire its structured children without allocating on success.
            core::detail::JsonCleanup result_cleanup{result};
            try {
                if (test_stage) {
                    auto allowed = test_stage(test_context, "oracle_mismatch_clone");
                    if (!allowed) return Json(std::move(result));
                }
                auto detailed = core::detail::clone_json(result);
                core::detail::JsonCleanup detailed_cleanup{detailed};
                if (test_stage) {
                    auto allowed = test_stage(test_context, "oracle_mismatch_projection");
                    if (!allowed) return Json(std::move(result));
                }
                auto projected = core::detail::json_object<Json>([&](Json& value) {
                    core::detail::set_json(value, "cycle_index", Json(witness.cycle_index));
                    core::detail::set_json(value, "worker_id", Json(witness.worker_id.value()));
                    core::detail::set_json(value, "counter_kind", Json(
                        witness.counter_kind == MonitoringQ1FaultCounterKind::minor_faults
                            ? "minor_faults" : "major_faults"));
                    core::detail::set_json(value, "before", Json(std::to_string(witness.before)));
                    core::detail::set_json(value, "production", Json(std::to_string(witness.production)));
                    core::detail::set_json(value, "after", Json(std::to_string(witness.after)));
                });
                core::detail::JsonCleanup projected_cleanup{projected};
                core::detail::set_json(detailed, "oracle_mismatch", std::move(projected));
                return Json(std::move(detailed));
            } catch (...) {
                // Only the optional witness is lost; primary and cleanup stay exact.
                return Json(std::move(result));
            }
        }
        return result;
    } catch (...) { return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "contained_failure")}; }
}

auto project_monitoring_host_q1_authored(std::span<const scenario::ScenarioDiagnostic> diagnostics) noexcept
    -> std::expected<Json, core::Error> {
    try {
        if (diagnostics.empty()) return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "empty_diagnostics")};
        Json values = Json::array();
        for (const auto& item : diagnostics) {
            if (!safe_token(item.code.value(), 128U) || !safe_pointer(item.json_pointer) ||
                privacy_prohibited(item.code.value()) || privacy_prohibited(item.json_pointer))
                return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "diagnostic_invalid")};
            Json location = nullptr;
            if (item.location && (item.location->line == 0U || item.location->column == 0U))
                return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "diagnostic_invalid")};
            if (item.location) location = {{"byte_offset", item.location->byte_offset},
                                           {"line", item.location->line}, {"column", item.location->column}};
            Json context = Json::object();
            for (const auto& [key, value] : item.safe_context) {
                if (!safe_token(key, 64U) ||
                    privacy_prohibited(key) || privacy_prohibited(value))
                    return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "diagnostic_invalid")};
                context[key] = value;
            }
            Json value{{"code", item.code.value()}, {"json_pointer", item.json_pointer},
                       {"location", std::move(location)}, {"safe_context", std::move(context)}};
            if (!privacy_safe(value)) return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "privacy")};
            values.push_back(std::move(value));
        }
        return Json{{"schema", "epycbench.monitoring-host-q1-backend-authored/v1"},
                    {"diagnostics", std::move(values)}};
    } catch (...) { return std::unexpected{backend_error("monitoring.q1.backend.transport_failure", "contained_failure")}; }
}

auto MonitoringHostQ1BackendTestAccess::attest(nlohmann::json build, std::string_view commit)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    return attest_monitoring_running_build_value(build, commit);
}
void MonitoringHostQ1BackendTestAccess::install_hooks(void* context, BuildProducer build,
                                                       StageHook stage_hook) noexcept {
    test_context = context; test_build = build; test_stage = stage_hook;
}
void MonitoringHostQ1BackendTestAccess::install_operations(
    MonitoringHostQ1BackendOperations* operations) noexcept { test_operations = operations; }
void MonitoringHostQ1BackendTestAccess::clear_hooks() noexcept {
    test_context = nullptr; test_build = nullptr; test_stage = nullptr; test_operations = nullptr;
}
auto MonitoringHostQ1BackendTestAccess::map_authority(MonitoringQ1GitCommandFailure failure)
    -> MonitoringHostQ1BackendFailure {
    auto result=authority_failure(std::move(failure));
    return std::move(result.error());
}
auto MonitoringHostQ1BackendTestAccess::evidence(MonitoringQ1PageFaultCollection&& collection,
                                                  nlohmann::ordered_json build)
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure> {
    return build_success(std::move(collection), std::move(build));
}
auto MonitoringHostQ1BackendTestAccess::contain(std::string_view active,
                                                 std::vector<core::Error> cleanup)
    -> MonitoringHostQ1BackendFailure {
    ExceptionStock stock;
    MonitoringQ1BoundedRetirement bounded;
    for (auto& item : cleanup) {
        if (bounded.count < bounded.failures.size())
            bounded.failures[bounded.count++].emplace(std::move(item));
        else
            bounded.overflowed = true;
    }
    auto result=stock.take_cleanup(active,std::move(bounded));
    return std::move(result.error());
}
} // namespace epycbench::monitoring::qualification::detail
