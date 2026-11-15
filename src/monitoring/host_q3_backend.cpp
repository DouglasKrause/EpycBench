#include "monitoring/host_q3_backend.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q3_authority_session.hpp"
#include "monitoring/host_q3_temperature_evidence_json.hpp"
#include "monitoring/host_q3_temperature_evidence_value.hpp"
#include "monitoring/host_q3_temperature_production_adapter.hpp"
#include "monitoring/host_running_build_attestation_value.hpp"
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::ordered_json;
thread_local void (*serialization_fault)(std::size_t){};
void serialization_boundary(std::size_t point) {
    if (serialization_fault)
        serialization_fault(point);
}
auto token(MonitoringHostQ3BackendPhase p) -> std::string_view {
    switch (p) {
    case MonitoringHostQ3BackendPhase::repository:
        return "repository";
    case MonitoringHostQ3BackendPhase::collection_plan:
        return "collection_plan";
    case MonitoringHostQ3BackendPhase::predecessor:
        return "predecessor";
    case MonitoringHostQ3BackendPhase::build_attestation:
        return "build_attestation";
    case MonitoringHostQ3BackendPhase::source_discovery:
        return "source_discovery";
    case MonitoringHostQ3BackendPhase::oracle:
        return "oracle";
    case MonitoringHostQ3BackendPhase::source_read:
        return "source_read";
    case MonitoringHostQ3BackendPhase::agreement:
        return "agreement";
    case MonitoringHostQ3BackendPhase::clock:
        return "clock";
    case MonitoringHostQ3BackendPhase::deadline:
        return "deadline";
    case MonitoringHostQ3BackendPhase::capacity:
        return "capacity";
    case MonitoringHostQ3BackendPhase::finalization:
        return "finalization";
    case MonitoringHostQ3BackendPhase::evidence_construction:
        return "evidence_construction";
    }
    std::terminate();
}
auto stage(MonitoringHostQ3BackendPhase p, const core::Error* underlying = nullptr)
    -> MonitoringHostQ3BackendFailure {
    core::Error e{.category = core::ErrorCategory::monitoring,
                  .code = core::StableErrorCode{"monitoring.q3.backend.stage_failure"},
                  .message = {},
                  .operation = "run_monitoring_host_q3_backend",
                  .os_error = underlying ? underlying->os_error : std::nullopt,
                  .context = {{"classification", token(p)}},
                  .severity = core::ErrorSeverity::error,
                  .continuation = core::ContinuationPolicy::abort_command};
    MonitoringHostQ3BackendFailure out{p, std::move(e)};
    if (underlying) {
        const auto& code = underlying->code.value();
        if (!code.empty() && code.size() <= out.underlying_code.size()) {
            std::copy(code.begin(), code.end(), out.underlying_code.begin());
            out.underlying_code_size = code.size();
        }
        if (p == MonitoringHostQ3BackendPhase::repository &&
            underlying->code.value() == "monitoring.q1_plan_authority.repository_rejected" &&
            underlying->context.is_object()) {
            auto found = underlying->context.find("classification");
            constexpr std::array allowed{
                "repository_authority", "git_executable", "git_identity", "git_metadata",
                "git_config",           "git_result",     "git_poll",     "git_read",
                "git_output_limit",     "git_setup",      "git_exit",     "git_spawn",
                "repository_dirty",     "tracked_entry"};
            if (found != underlying->context.end() && found->is_string()) {
                const auto& value = found->get_ref<const std::string&>();
                if (std::ranges::find(allowed, value) != allowed.end() &&
                    value.size() <= out.repository_classification.size()) {
                    std::copy(value.begin(), value.end(), out.repository_classification.begin());
                    out.repository_classification_size = value.size();
                }
            }
        }
    }
    return out;
}
auto authority_phase(MonitoringQ3AuthorityStage p) {
    switch (p) {
    case MonitoringQ3AuthorityStage::repository:
        return MonitoringHostQ3BackendPhase::repository;
    case MonitoringQ3AuthorityStage::collection_plan:
        return MonitoringHostQ3BackendPhase::collection_plan;
    case MonitoringQ3AuthorityStage::predecessor:
        return MonitoringHostQ3BackendPhase::predecessor;
    case MonitoringQ3AuthorityStage::build_attestation:
        return MonitoringHostQ3BackendPhase::build_attestation;
    case MonitoringQ3AuthorityStage::final_recheck:
    case MonitoringQ3AuthorityStage::retirement:
        return MonitoringHostQ3BackendPhase::finalization;
    }
    std::terminate();
}
auto collection_phase(MonitoringQ3CollectionStage p) {
    switch (p) {
    case MonitoringQ3CollectionStage::source_discovery:
        return MonitoringHostQ3BackendPhase::source_discovery;
    case MonitoringQ3CollectionStage::oracle:
        return MonitoringHostQ3BackendPhase::oracle;
    case MonitoringQ3CollectionStage::source_read:
        return MonitoringHostQ3BackendPhase::source_read;
    case MonitoringQ3CollectionStage::agreement:
        return MonitoringHostQ3BackendPhase::agreement;
    case MonitoringQ3CollectionStage::clock:
        return MonitoringHostQ3BackendPhase::clock;
    case MonitoringQ3CollectionStage::deadline:
        return MonitoringHostQ3BackendPhase::deadline;
    case MonitoringQ3CollectionStage::capacity:
        return MonitoringHostQ3BackendPhase::capacity;
    case MonitoringQ3CollectionStage::finalization:
        return MonitoringHostQ3BackendPhase::finalization;
    }
    std::terminate();
}
void retain(MonitoringHostQ3BackendFailure& out, core::Error e) {
    if (out.secondary_count < out.secondary.size())
        out.secondary[out.secondary_count++] = std::move(e);
    else
        out.cleanup_overflowed = true;
}
void project(MonitoringHostQ3BackendFailure& out, const core::Error* underlying) noexcept {
    if (!underlying)
        return;
    if (underlying->os_error && underlying->os_error->value > 0) {
        out.primary.os_error.emplace();
        out.primary.os_error->value = underlying->os_error->value;
    }
    const auto& code = underlying->code.value();
    constexpr std::array allowed_codes{"monitoring.q3.repository_invalid",
                                       "monitoring.q3.collection_plan_invalid",
                                       "monitoring.q3.predecessor_invalid",
                                       "monitoring.q3.build_attestation_invalid",
                                       "linux.hwmon_temperature.discovery_failed",
                                       "linux.hwmon_temperature.ambiguous_sensor",
                                       "linux.hwmon_temperature.read_failed",
                                       "linux.hwmon_temperature.invalid_value",
                                       "monitoring.q3.oracle_open_failed",
                                       "monitoring.q3.oracle_read_failed",
                                       "monitoring.q3.oracle_identity_changed",
                                       "monitoring.q3.parser_disagreement",
                                       "monitoring.q3.clock_failed",
                                       "monitoring.q3.deadline",
                                       "monitoring.q3.capacity",
                                       "monitoring.q3.finalization_failed",
                                       "monitoring.q3.evidence_invalid"};
    std::string_view projected;
    if (std::ranges::find(allowed_codes, code) != allowed_codes.end())
        projected = code;
    else
        switch (out.phase) {
        case MonitoringHostQ3BackendPhase::repository:
            projected = allowed_codes[0];
            break;
        case MonitoringHostQ3BackendPhase::collection_plan:
            projected = allowed_codes[1];
            break;
        case MonitoringHostQ3BackendPhase::predecessor:
            projected = allowed_codes[2];
            break;
        case MonitoringHostQ3BackendPhase::build_attestation:
            projected = allowed_codes[3];
            break;
        case MonitoringHostQ3BackendPhase::source_discovery:
            projected = allowed_codes[4];
            break;
        case MonitoringHostQ3BackendPhase::source_read:
            projected = allowed_codes[6];
            break;
        case MonitoringHostQ3BackendPhase::agreement:
            projected = allowed_codes[11];
            break;
        case MonitoringHostQ3BackendPhase::clock:
            projected = allowed_codes[12];
            break;
        case MonitoringHostQ3BackendPhase::deadline:
            projected = allowed_codes[13];
            break;
        case MonitoringHostQ3BackendPhase::capacity:
            projected = allowed_codes[14];
            break;
        case MonitoringHostQ3BackendPhase::finalization:
            projected = allowed_codes[15];
            break;
        case MonitoringHostQ3BackendPhase::evidence_construction:
            projected = allowed_codes[16];
            break;
        case MonitoringHostQ3BackendPhase::oracle:
            break;
        }
    if (!projected.empty()) {
        std::copy(projected.begin(), projected.end(), out.underlying_code.begin());
        out.underlying_code_size = projected.size();
    }
    if (out.phase != MonitoringHostQ3BackendPhase::repository ||
        code != "monitoring.q1_plan_authority.repository_rejected" ||
        !underlying->context.is_object())
        return;
    auto found = underlying->context.find("classification");
    constexpr std::array allowed{
        "repository_authority", "git_executable", "git_identity", "git_metadata",
        "git_config",           "git_result",     "git_poll",     "git_read",
        "git_output_limit",     "git_setup",      "git_exit",     "git_spawn",
        "repository_dirty",     "tracked_entry"};
    if (found == underlying->context.end() || !found->is_string())
        return;
    const auto& value = found->get_ref<const std::string&>();
    if (std::ranges::find(allowed, value) == allowed.end() ||
        value.size() > out.repository_classification.size())
        return;
    std::copy(value.begin(), value.end(), out.repository_classification.begin());
    out.repository_classification_size = value.size();
}
auto safe_error(const core::Error& e, std::optional<std::string_view> classification = std::nullopt)
    -> Json {
    Json out{{"category", core::detail::enum_name(e.category, core::detail::error_category_names)},
             {"code", classification ? "monitoring.q3.backend.stage_failure" : e.code.value()},
             {"operation", classification ? "run_monitoring_host_q3_backend" : e.operation},
             {"severity", "error"},
             {"continuation", "abort_command"}};
    if (classification)
        out["classification"] = *classification;
    if (e.os_error && e.os_error->value > 0)
        out["os_error"] = e.os_error->value;
    out["context"] = Json::object();
    return out;
}
} // namespace
auto validate_monitoring_host_q3_backend_success(std::string_view bytes) noexcept -> bool {
    try {
        if (bytes.empty() || bytes.size() > 1048576U || bytes.back() != '\n')
            return false;
        auto root = Json::parse(bytes.begin(), bytes.end() - 1);
        auto canonical = core::canonicalize_json(root);
        if (!canonical || *canonical + "\n" != bytes || !root.is_object() || root.size() != 3U ||
            root.value("schema", "") != "epycbench.monitoring-host-q3-backend-result/v1")
            return false;
        auto evidence = core::canonicalize_json(root.at("evidence"));
        if (!evidence)
            return false;
        evidence->push_back('\n');
        auto accepted = validate_monitoring_q3_temperature_evidence_json(*evidence);
        if (!accepted)
            return false;
        const auto& commit = accepted->binding.repository_commit;
        auto build = attest_monitoring_running_build_value(root.at("build_manifest"), commit);
        if (!build)
            return false;
        auto encoded_build = core::canonicalize_json(*build),
             input_build = core::canonicalize_json(root.at("build_manifest"));
        return encoded_build && input_build && *encoded_build == *input_build &&
               root.at("build_manifest").at("git").value("dirty", true) == false &&
               accepted->binding.build_identity ==
                   "sha256:" + root.at("build_manifest").value("build_identity", "");
    } catch (...) {
        return false;
    }
}
auto MonitoringHostQ3BackendBody::run(const std::filesystem::path& root, std::string_view qid,
                                      Operations operations) noexcept
    -> std::expected<MonitoringHostQ3BackendResult, MonitoringHostQ3BackendFailure> {
    MonitoringHostQ3BackendFailure fallback;
    fallback.phase = MonitoringHostQ3BackendPhase::finalization;
    fallback.terminal_containment = true;
    MonitoringHostQ3BackendFailure terminal;
    terminal.phase = MonitoringHostQ3BackendPhase::finalization;
    terminal.terminal_containment = true;
    std::array<MonitoringHostQ3BackendFailure, 13> stock;
    try {
        if (operations.fault)
            operations.fault(operations.context, 0U);
        for (std::size_t i{}; i < stock.size(); ++i) {
            if (operations.fault)
                operations.fault(operations.context, 1U + i);
            stock[i] = stage(static_cast<MonitoringHostQ3BackendPhase>(i));
        }
    } catch (...) {
        return std::unexpected{std::move(fallback)};
    }
    try {
        const auto take = [&](MonitoringHostQ3BackendPhase phase,
                              const core::Error* underlying = nullptr) {
            auto value = std::move(stock[static_cast<std::size_t>(phase)]);
            if (operations.fault)
                operations.fault(operations.context, 100U + static_cast<std::size_t>(phase));
            project(value, underlying);
            return value;
        };
        auto ready = operations.authenticate_and_finalize(operations.context, root, qid);
        if (!ready) {
            if (ready.error().authored)
                return MonitoringHostQ3BackendResult{
                    MonitoringHostQ3BackendAuthored{{std::move(*ready.error().authored)}}};
            for (std::size_t i{}; i < ready.error().cleanup.count; ++i)
                retain(terminal, std::move(*ready.error().cleanup.failures[i]));
            terminal.cleanup_overflowed |= ready.error().cleanup.overflowed;
            auto out = take(authority_phase(ready.error().stage), &ready.error().primary);
            for (std::size_t i{}; i < terminal.secondary_count; ++i)
                retain(out, std::move(*terminal.secondary[i]));
            out.cleanup_overflowed |= terminal.cleanup_overflowed;
            return std::unexpected{std::move(out)};
        }
        auto collection = operations.collect(operations.context, *ready);
        if (!collection) {
            auto out =
                take(collection_phase(collection.error().stage), &collection.error().primary);
            for (std::size_t i{}; i < collection.error().cleanup_count; ++i)
                retain(out, std::move(*collection.error().cleanup[i]));
            out.cleanup_overflowed |= collection.error().cleanup_overflowed;
            return std::unexpected{std::move(out)};
        }
        auto evidence = operations.build(operations.context, *ready, *collection);
        if (!evidence)
            return std::unexpected{
                take(MonitoringHostQ3BackendPhase::evidence_construction, &evidence.error())};
        auto evidence_bytes = operations.encode(operations.context, *evidence);
        if (!evidence_bytes)
            return std::unexpected{
                take(MonitoringHostQ3BackendPhase::evidence_construction, &evidence_bytes.error())};
        if (evidence_bytes->size() > 524288U ||
            !operations.validate(operations.context, *evidence_bytes))
            return std::unexpected{take(MonitoringHostQ3BackendPhase::evidence_construction)};
        auto bytes =
            operations.assemble(operations.context, ready->build_attestation(), *evidence_bytes);
        if (!bytes && bytes.error().kind == AssemblyFailureKind::terminal_containment)
            return std::unexpected{std::move(terminal)};
        if (!bytes)
            return std::unexpected{
                take(MonitoringHostQ3BackendPhase::evidence_construction,
                     bytes.error().ordinary ? &*bytes.error().ordinary : nullptr)};
        if (bytes->size() > 1048576U)
            return std::unexpected{take(MonitoringHostQ3BackendPhase::capacity)};
        return MonitoringHostQ3BackendResult{MonitoringHostQ3BackendBytes{std::move(*bytes)}};
    } catch (...) {
        return std::unexpected{std::move(terminal)};
    }
}
auto MonitoringHostQ3BackendBody::assemble(std::string_view build,
                                           std::string_view evidence) noexcept
    -> std::expected<std::string, AssemblyFailure> {
    return assemble_with(build, evidence, nullptr, nullptr);
}
auto MonitoringHostQ3BackendBody::assemble_with(std::string_view build, std::string_view evidence,
                                                void* context,
                                                void (*fault)(void*, std::size_t)) noexcept
    -> std::expected<std::string, AssemblyFailure> {
    AssemblyFailure terminal;
    AssemblyFailure failure;
    try {
        if (fault)
            fault(context, 100U);
        failure.kind = AssemblyFailureKind::ordinary_validation;
        failure.ordinary.emplace(
            stage(MonitoringHostQ3BackendPhase::evidence_construction).primary);
        if (fault)
            fault(context, 101U);
    } catch (...) {
        return std::unexpected{std::move(terminal)};
    }
    try {
        if (fault)
            fault(context, 0U);
        if (!validate_monitoring_q3_temperature_evidence_json(evidence))
            return std::unexpected{std::move(failure)};
        if (fault)
            fault(context, 1U);
        auto build_json = Json::parse(build);
        if (fault)
            fault(context, 2U);
        auto evidence_json = Json::parse(evidence.begin(), evidence.end() - 1);
        if (fault)
            fault(context, 3U);
        Json wrapper{{"schema", "epycbench.monitoring-host-q3-backend-result/v1"},
                     {"build_manifest", std::move(build_json)},
                     {"evidence", std::move(evidence_json)}};
        if (fault)
            fault(context, 4U);
        auto bytes = core::canonicalize_json(wrapper);
        if (!bytes)
            return std::unexpected{std::move(failure)};
        bytes->push_back('\n');
        if (fault)
            fault(context, 5U);
        if (!validate_monitoring_host_q3_backend_success(*bytes))
            return std::unexpected{std::move(failure)};
        return std::move(*bytes);
    } catch (...) {
        return std::unexpected{std::move(terminal)};
    }
}

auto run_monitoring_host_q3_backend(const std::filesystem::path& root, std::string_view qid,
                                    runtime::ControllerProcessProductionAuthority&) noexcept
    -> std::expected<MonitoringHostQ3BackendResult, MonitoringHostQ3BackendFailure> {
    struct Production {
        static auto authority(void*, const std::filesystem::path& path, std::string_view id)
            -> std::expected<MonitoringQ3SourceReady, MonitoringQ3AuthorityFailure> {
            auto session = MonitoringQ3AuthoritySession::authenticate(path, id);
            if (!session)
                return std::unexpected{std::move(session.error())};
            auto ready = session->finalize_before_source();
            if (!ready)
                return std::unexpected{std::move(ready.error())};
            return std::move(*ready);
        }
        static auto collect(void*, const MonitoringQ3SourceReady& r) {
            return MonitoringQ3TemperatureProductionAdapter::collect(r);
        }
        static auto build(void*, const MonitoringQ3SourceReady& r,
                          const MonitoringQ3TemperatureCollection& c) {
            return MonitoringQ3TemperatureEvidenceBuilder::build(r, c);
        }
        static auto encode(void*, const MonitoringQ3TemperatureEvidenceValue& e) {
            return encode_monitoring_q3_temperature_evidence_json(e);
        }
        static auto validate(void*, std::string_view b) {
            return static_cast<bool>(validate_monitoring_q3_temperature_evidence_json(b));
        }
        static auto assemble(void*, std::string_view build, std::string_view evidence) {
            return MonitoringHostQ3BackendBody::assemble(build, evidence);
        }
    };
    return MonitoringHostQ3BackendBody::run(root, qid,
                                            {nullptr, Production::authority, Production::collect,
                                             Production::build, Production::encode,
                                             Production::validate, Production::assemble, nullptr});
}
auto monitoring_host_q3_usage_failure() noexcept -> MonitoringHostQ3BackendFailure {
    MonitoringHostQ3BackendFailure out;
    out.usage_failure = true;
    return out;
}
auto serialize_monitoring_host_q3_backend_failure(const MonitoringHostQ3BackendFailure& v) noexcept
    -> std::expected<std::string, core::Error> {
    try {
        if (v.usage_failure) {
            if (v.terminal_containment || v.secondary_count != 0U || v.cleanup_overflowed ||
                v.underlying_code_size != 0U || v.repository_classification_size != 0U)
                return std::unexpected{core::Error{}};
            serialization_boundary(0U);
            Json root{{"schema", "epycbench.monitoring-host-q3-backend-error/v1"},
                      {"failure",
                       {{"category", "usage"},
                        {"code", "monitoring.q3.backend.usage"},
                        {"operation", "run_monitoring_host_q3_backend"},
                        {"severity", "error"},
                        {"continuation", "abort_command"},
                        {"context", Json::object()}}},
                      {"secondary_failures", Json::array()}};
            serialization_boundary(1U);
            auto bytes = core::canonicalize_json(root);
            if (!bytes || bytes->size() + 1U > 65536U)
                return std::unexpected{core::Error{}};
            serialization_boundary(2U);
            bytes->push_back('\n');
            return bytes;
        }
        if (static_cast<unsigned>(v.phase) >
                static_cast<unsigned>(MonitoringHostQ3BackendPhase::evidence_construction) ||
            v.secondary_count > v.secondary.size() ||
            v.underlying_code_size > v.underlying_code.size() ||
            v.repository_classification_size > v.repository_classification.size())
            return std::unexpected{core::Error{}};
        if (v.cleanup_overflowed && v.secondary_count != v.secondary.size())
            return std::unexpected{core::Error{}};
        for (std::size_t i{}; i < v.secondary_count; ++i)
            if (!v.secondary[i])
                return std::unexpected{core::Error{}};
        if (v.terminal_containment) {
            if (v.underlying_code_size != 0U || v.repository_classification_size != 0U)
                return std::unexpected{core::Error{}};
            serialization_boundary(0U);
            Json secondary = Json::array();
            for (std::size_t i{}; i < v.secondary_count; ++i)
                if (v.secondary[i])
                    secondary.push_back(safe_error(*v.secondary[i]));
            if (v.cleanup_overflowed)
                secondary.push_back(safe_error(
                    core::Error{.category = core::ErrorCategory::monitoring,
                                .code = core::StableErrorCode{"monitoring.q3.cleanup_overflow"},
                                .message = {},
                                .operation = "run_monitoring_host_q3_backend",
                                .os_error = std::nullopt,
                                .context = Json::object(),
                                .severity = core::ErrorSeverity::error,
                                .continuation = core::ContinuationPolicy::abort_command}));
            serialization_boundary(1U);
            Json root{{"schema", "epycbench.monitoring-host-q3-backend-error/v1"},
                      {"failure",
                       {{"category", "monitoring"},
                        {"code", "monitoring.q3.backend.terminal_containment"},
                        {"operation", "run_monitoring_host_q3_backend"},
                        {"severity", "error"},
                        {"continuation", "abort_command"},
                        {"classification", "finalization"},
                        {"context", Json::object()}}},
                      {"secondary_failures", std::move(secondary)}};
            serialization_boundary(2U);
            auto bytes = core::canonicalize_json(root);
            if (!bytes)
                return std::unexpected{core::Error{}};
            serialization_boundary(3U);
            bytes->push_back('\n');
            return bytes;
        }
        if (v.primary.code.value() != "monitoring.q3.backend.stage_failure")
            return std::unexpected{core::Error{}};
        serialization_boundary(0U);
        Json secondary = Json::array();
        for (std::size_t i{}; i < v.secondary_count; ++i)
            if (v.secondary[i])
                secondary.push_back(safe_error(*v.secondary[i]));
        if (v.cleanup_overflowed)
            secondary.push_back(safe_error(
                core::Error{.category = core::ErrorCategory::monitoring,
                            .code = core::StableErrorCode{"monitoring.q3.cleanup_overflow"},
                            .message = {},
                            .operation = "run_monitoring_host_q3_backend",
                            .os_error = std::nullopt,
                            .context = Json::object(),
                            .severity = core::ErrorSeverity::error,
                            .continuation = core::ContinuationPolicy::abort_command}));
        serialization_boundary(1U);
        auto primary = safe_error(v.primary, std::optional<std::string_view>{token(v.phase)});
        constexpr std::array allowed_codes{"monitoring.q3.repository_invalid",
                                           "monitoring.q3.collection_plan_invalid",
                                           "monitoring.q3.predecessor_invalid",
                                           "monitoring.q3.build_attestation_invalid",
                                           "linux.hwmon_temperature.discovery_failed",
                                           "linux.hwmon_temperature.ambiguous_sensor",
                                           "linux.hwmon_temperature.read_failed",
                                           "linux.hwmon_temperature.invalid_value",
                                           "monitoring.q3.oracle_open_failed",
                                           "monitoring.q3.oracle_read_failed",
                                           "monitoring.q3.oracle_identity_changed",
                                           "monitoring.q3.parser_disagreement",
                                           "monitoring.q3.clock_failed",
                                           "monitoring.q3.deadline",
                                           "monitoring.q3.capacity",
                                           "monitoring.q3.finalization_failed",
                                           "monitoring.q3.evidence_invalid"};
        Json context = Json::object();
        const std::string_view underlying{v.underlying_code.data(), v.underlying_code_size};
        if (!underlying.empty()) {
            if (std::ranges::find(allowed_codes, underlying) == allowed_codes.end())
                return std::unexpected{core::Error{}};
            context["underlying_code"] = underlying;
        }
        constexpr std::array repository_allowed{
            "repository_authority", "git_executable", "git_identity", "git_metadata",
            "git_config",           "git_result",     "git_poll",     "git_read",
            "git_output_limit",     "git_setup",      "git_exit",     "git_spawn",
            "repository_dirty",     "tracked_entry"};
        const std::string_view repository{v.repository_classification.data(),
                                          v.repository_classification_size};
        if (!repository.empty()) {
            if (v.phase != MonitoringHostQ3BackendPhase::repository ||
                std::ranges::find(repository_allowed, repository) == repository_allowed.end())
                return std::unexpected{core::Error{}};
            context["repository_classification"] = repository;
        }
        primary["context"] = std::move(context);
        serialization_boundary(2U);
        Json root{{"schema", "epycbench.monitoring-host-q3-backend-error/v1"},
                  {"failure", std::move(primary)},
                  {"secondary_failures", std::move(secondary)}};
        serialization_boundary(3U);
        auto bytes = core::canonicalize_json(root);
        if (!bytes || bytes->size() + 1U > 65536U)
            return std::unexpected{stage(MonitoringHostQ3BackendPhase::capacity).primary};
        serialization_boundary(4U);
        bytes->push_back('\n');
        return bytes;
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
auto serialize_monitoring_host_q3_authored(const MonitoringHostQ3BackendAuthored& v) noexcept
    -> std::expected<std::string, core::Error> {
    try {
        if (v.diagnostics.empty() || v.diagnostics.size() > 32U)
            return std::unexpected{stage(MonitoringHostQ3BackendPhase::capacity).primary};
        constexpr std::array allowed{
            "monitoring.q3.plan.schema",        "monitoring.q3.plan.structure",
            "monitoring.q3.plan.value",         "monitoring.q3.plan.canonical",
            "monitoring.q3.evidence.structure", "monitoring.q3.evidence.value",
            "monitoring.q3.evidence.canonical", "monitoring.q3.evidence.privacy"};
        serialization_boundary(0U);
        Json records = Json::array();
        for (const auto& d : v.diagnostics) {
            const auto valid_pointer = [](std::string_view pointer) noexcept {
                if (pointer.empty())
                    return true;
                if (pointer.front() != '/')
                    return false;
                for (std::size_t i{}; i < pointer.size(); ++i)
                    if (pointer[i] == '~' &&
                        (i + 1U == pointer.size() || (pointer[++i] != '0' && pointer[i] != '1')))
                        return false;
                return true;
            };
            const bool any = d.byte_offset || d.line || d.column;
            if (std::ranges::find(allowed, d.code) == allowed.end() ||
                d.json_pointer.size() > 4096U || !valid_pointer(d.json_pointer) ||
                any != static_cast<bool>(d.byte_offset && d.line && d.column) ||
                (d.line && *d.line == 0U) || (d.column && *d.column == 0U))
                return std::unexpected{
                    stage(MonitoringHostQ3BackendPhase::evidence_construction).primary};
            Json location = nullptr;
            if (any)
                location = {
                    {"byte_offset", *d.byte_offset}, {"line", *d.line}, {"column", *d.column}};
            serialization_boundary(1U);
            records.push_back({{"code", d.code},
                               {"json_pointer", d.json_pointer},
                               {"location", std::move(location)},
                               {"safe_context", Json::object()}});
        }
        serialization_boundary(2U);
        Json root{{"schema", "epycbench.monitoring-host-q3-backend-authored/v1"},
                  {"diagnostics", std::move(records)}};
        serialization_boundary(3U);
        auto bytes = core::canonicalize_json(root);
        if (!bytes || bytes->size() + 1U > 65536U)
            return std::unexpected{stage(MonitoringHostQ3BackendPhase::capacity).primary};
        serialization_boundary(4U);
        bytes->push_back('\n');
        return bytes;
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
void MonitoringHostQ3BackendTestAccess::set_serialization_fault(void (*fault)(std::size_t)) {
    serialization_fault = fault;
}
void MonitoringHostQ3BackendTestAccess::reset_serialization_fault() noexcept {
    serialization_fault = nullptr;
}
} // namespace epycbench::monitoring::qualification::detail
