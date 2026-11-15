#include "monitoring/host_q2_diagnostic_transport.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto safe(std::string_view value) noexcept -> bool {
    if (value.empty() || value.size() > 128U || value.front() < 'a' || value.front() > 'z')
        return false;
    for (char c : value.substr(1))
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}
auto context_value_ok(std::string_view key, std::string_view value) noexcept -> bool {
    if (key == "underlying_code")
        return safe(value);
    if (key == "channel")
        return value == "control" || value == "event";
    if (key == "protocol_phase")
        return value == "framing" || value == "encoding" || value == "decoding" ||
               value == "state_validation";
    if (key == "runner_phase")
        return value == "startup" || value == "placement" || value == "cleanup";
    if (key == "verification_phase")
        return value == "before" || value == "after";
    if (key == "frame_size_category")
        return value == "zero" || value == "within_limit" || value == "over_limit";
    if (key == "sequence_category")
        return value == "gap" || value == "duplicate" || value == "reversal" ||
               value == "unexpected";
    if (key == "expected_message" || key == "received_message")
        return value == "placement_request" || value == "cancel_request" ||
               value == "request_accepted" || value == "placement_succeeded" ||
               value == "request_cancelled" || value == "request_failed" || value == "stream_end" ||
               (key == "received_message" && value == "unknown");
    if (key == "component")
        return value == "ipc" || value == "affinity" || value == "mapping" ||
               value == "numa_bind" || value == "initialization" || value == "page_query" ||
               value == "threshold" || value == "read_only" || value == "worker" ||
               value == "service" || value == "runner";
    return false;
}
auto repository_classification_ok(std::string_view value) noexcept -> bool {
    constexpr std::array allowed{"repository_authority","git_executable","git_identity","git_metadata","git_config",
      "git_result","git_poll","git_read","git_output_limit","git_setup","git_exit",
      "git_spawn","repository_dirty","tracked_entry"};
    return std::ranges::find(allowed,value)!=allowed.end();
}
auto required_event_ok(const MonitoringQ2RequiredEventDiagnostic& value) noexcept -> bool {
    // These are the existing fixed Q2 plan bounds, not larger diagnostic limits.
    if ((value.kind != MonitoringQ2TrialKind::voluntary &&
         value.kind != MonitoringQ2TrialKind::preemption) ||
        value.ordinal >= 4096U || value.scoped_records > 1048576U ||
        value.voluntary_count > value.scoped_records ||
        value.involuntary_count > value.scoped_records - value.voluntary_count)
        return false;
    return value.kind == MonitoringQ2TrialKind::voluntary ? value.voluntary_count == 0U
                                                        : value.involuntary_count == 0U;
}
void append_required_event(MonitoringQ2DiagnosticBuffer& out,
                           const MonitoringQ2RequiredEventDiagnostic& value) noexcept {
    out.add(",\"required_event\":{\"involuntary_count\":");
    out.number(value.involuntary_count);
    out.add(",\"kind\":\"");
    out.add(value.kind == MonitoringQ2TrialKind::voluntary ? "voluntary" : "preemption");
    out.add("\",\"ordinal\":");
    out.number(value.ordinal);
    out.add(",\"scoped_records\":");
    out.number(value.scoped_records);
    out.add(",\"voluntary_count\":");
    out.number(value.voluntary_count);
    out.add(",\"worker_id\":");
    out.number(static_cast<std::uint64_t>(value.worker.value()));
    out.add("}");
}
} // namespace
void MonitoringQ2DiagnosticBuffer::add(std::string_view value) noexcept {
    if (failed || value.size() > bytes.size() - size) {
        failed = true;
        return;
    }
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
    size += value.size();
}
void MonitoringQ2DiagnosticBuffer::number(int value) noexcept {
    std::array<char, 32> text{};
    auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        failed = true;
        return;
    }
    add({text.data(), static_cast<std::size_t>(result.ptr - text.data())});
}
void MonitoringQ2DiagnosticBuffer::number(std::uint64_t value) noexcept {
    std::array<char, 32> text{};
    auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        failed = true;
        return;
    }
    add({text.data(), static_cast<std::size_t>(result.ptr - text.data())});
}
auto monitoring_q2_phase_name(MonitoringHostQ2BackendPhase value) noexcept -> std::string_view {
    using P = MonitoringHostQ2BackendPhase;
    switch (value) {
    case P::repository:
        return "repository";
    case P::collection_plan:
        return "collection_plan";
    case P::predecessor:
        return "predecessor";
    case P::build_attestation:
        return "build_attestation";
    case P::permission:
        return "permission";
    case P::session:
        return "session";
    case P::worker_admission:
        return "worker_admission";
    case P::source_preparation:
        return "source_preparation";
    case P::voluntary_event:
        return "voluntary_event";
    case P::preemption_event:
        return "preemption_event";
    case P::drain:
        return "drain";
    case P::association:
        return "association";
    case P::clock:
        return "clock";
    case P::completeness:
        return "completeness";
    case P::finalization:
        return "finalization";
    case P::evidence_construction:
        return "evidence_construction";
    case P::usage:
        return "usage";
    }
    std::terminate();
}
void append_monitoring_q2_projected_failure(MonitoringQ2DiagnosticBuffer& out,
                                            std::string_view classification,
                                            std::string_view underlying, int native,
                                            std::string_view repository_classification) noexcept {
    if (!safe(underlying)) {
        underlying = "monitoring.q2.contained_failure";
    }
    out.add("{\"category\":\"monitoring\",\"classification\":\"");
    out.add(classification);
    out.add("\",\"code\":\"monitoring.q2.backend.stage_failure\",\"context\":{");
    if (repository_classification_ok(repository_classification)) {
        out.add("\"repository_classification\":\"");
        out.add(repository_classification);
        out.add("\",");
    }
    out.add("\"underlying_code\":\"");
    out.add(underlying);
    out.add(
        "\"},\"continuation\":\"abort_command\",\"operation\":\"run_monitoring_host_q2_backend\"");
    if (native > 0) {
        out.add(",\"os_error\":");
        out.number(native);
    }
    out.add(",\"severity\":\"error\"}");
}
void append_monitoring_q2_factual_failure(MonitoringQ2DiagnosticBuffer& out,
                                          const core::Error& value) noexcept {
    const auto category =
        core::detail::enum_name(value.category, core::detail::error_category_names);
    const auto severity =
        core::detail::enum_name(value.severity, core::detail::error_severity_names);
    const auto continuation =
        core::detail::enum_name(value.continuation, core::detail::continuation_policy_names);
    const auto& code = value.code.value();
    std::string_view classification;
    if (value.context.is_object()) {
        for (const auto& [key, item] : value.context.items()) {
            if (key != "classification")
                continue;
            if (!item.is_string()) {
                out.failed = true;
                return;
            }
            classification = item.get_ref<const std::string&>();
            break;
        }
    }
    if (!safe(code) || value.operation.size() > 64U || !safe(value.operation) ||
        (!classification.empty() && (classification.size() > 64U || !safe(classification))) ||
        !value.context.is_object() || value.severity != core::ErrorSeverity::error ||
        value.continuation != core::ContinuationPolicy::abort_command ||
        (value.os_error && value.os_error->value <= 0)) {
        out.failed = true;
        return;
    }
    out.add("{\"category\":\"");
    out.add(category);
    if (!classification.empty()) {
        out.add("\",\"classification\":\"");
        out.add(classification);
    }
    out.add("\",\"code\":\"");
    out.add(code);
    out.add("\",\"context\":{");
    bool comma = false;
    for (const auto& [key, item] : value.context.items()) {
        if (key == "classification")
            continue;
        if (!item.is_string()) {
            out.failed = true;
            return;
        }
        const auto& text = item.get_ref<const std::string&>();
        if (!context_value_ok(key, text)) {
            out.failed = true;
            return;
        }
        if (comma)
            out.add(",");
        out.add("\"");
        out.add(key);
        out.add("\":\"");
        out.add(text);
        out.add("\"");
        comma = true;
    }
    out.add("},\"continuation\":\"");
    out.add(continuation);
    out.add("\",\"operation\":\"");
    out.add(value.operation);
    if (value.os_error) {
        out.add("\",\"os_error\":");
        out.number(value.os_error->value);
        out.add(",\"severity\":\"");
    } else
        out.add("\",\"severity\":\"");
    out.add(severity);
    out.add("\"}");
}
void append_monitoring_q2_native_cleanup(MonitoringQ2DiagnosticBuffer& out, std::string_view code,
                                         std::string_view classification, int native) noexcept {
    out.add("{\"category\":\"monitoring\",\"classification\":\"");
    out.add(classification);
    out.add("\",\"code\":\"");
    out.add(code);
    out.add("\",\"context\":{},\"continuation\":\"abort_command\",\"operation\":\"");
    out.add(code == "monitoring.q2.backend.cleanup_overflow" ? "run_monitoring_host_q2_backend"
                                                             : "monitoring_q2_native_cleanup");
    out.add("\"");
    if (native > 0) {
        out.add(",\"os_error\":");
        out.number(native);
    }
    out.add(",\"severity\":\"error\"}");
}
void finish_monitoring_q2_diagnostic_buffer(MonitoringQ2DiagnosticBuffer& out,
                                            std::string_view classification,
                                            std::string_view underlying, int native) noexcept {
    if (!out.failed)
        return;
    out.failed = false;
    out.size = 0;
    out.add("{\"failure\":");
    append_monitoring_q2_projected_failure(out, classification, underlying, native);
    out.add(",\"schema\":\"epycbench.monitoring-host-q2-backend-error/v1\","
            "\"secondary_failures\":[]}\n");
}
auto write_monitoring_q2_diagnostic_buffer(
    const MonitoringQ2DiagnosticBuffer& out,
    MonitoringQ2DiagnosticWriteOperations operations) noexcept -> bool {
    if (out.failed || !operations.write)
        return false;
    std::size_t offset{};
    std::size_t interruptions{};
    while (offset < out.size) {
        const auto remaining = out.size - offset;
        errno = 0;
        const auto count =
            operations.write(operations.context, 2, out.bytes.data() + offset, remaining);
        const auto write_error = errno;
        if (count > 0 && static_cast<std::size_t>(count) <= remaining) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && write_error == EINTR && interruptions++ < 64U)
            continue;
        return false;
    }
    return true;
}
auto serialize_monitoring_host_q2_failure(const MonitoringHostQ2BackendFailure& value,
                                          MonitoringQ2DiagnosticBuffer& out) noexcept -> bool {
    if (value.phase == MonitoringHostQ2BackendPhase::usage) {
        out.add("{\"failure\":{\"category\":\"usage\",\"code\":\"monitoring.q2.backend.usage\","
                "\"context\":{},\"continuation\":\"abort_command\",\"operation\":\"run_monitoring_"
                "host_q2_backend\",\"severity\":\"error\"},\"schema\":\"epycbench.monitoring-host-"
                "q2-backend-error/v1\",\"secondary_failures\":[]}\n");
        return !out.failed;
    }
    std::string_view underlying = value.primary.code.value();
    std::string_view repository_classification;
    if (value.underlying_code_size > 0 &&
        value.underlying_code_size <= value.underlying_code.size())
        underlying = std::string_view{value.underlying_code.data(), value.underlying_code_size};
    if (value.primary.context.is_object()) {
        for (const auto& [key, item] : value.primary.context.items()) {
            if (key == "underlying_code" && item.is_string()) {
                underlying = item.get_ref<const std::string&>();
                break;
            }
        }
    }
    if (value.phase == MonitoringHostQ2BackendPhase::repository &&
        value.repository_classification_size > 0 &&
        value.repository_classification_size <= value.repository_classification.size())
        repository_classification = std::string_view{value.repository_classification.data(),
                                                     value.repository_classification_size};
    out.add("{\"failure\":");
    append_monitoring_q2_projected_failure(out, monitoring_q2_phase_name(value.phase), underlying,
                                           value.primary.os_error ? value.primary.os_error->value
                                                                  : 0,
                                           repository_classification);
    if (value.phase == MonitoringHostQ2BackendPhase::completeness &&
        underlying == "monitoring.q2.collector.required_event" && value.required_event &&
        required_event_ok(*value.required_event))
        append_required_event(out, *value.required_event);
    out.add(
        ",\"schema\":\"epycbench.monitoring-host-q2-backend-error/v1\",\"secondary_failures\":[");
    const bool overflow =
        value.cleanup_overflowed || value.secondary_count > value.secondary.size();
    const auto limit =
        overflow ? std::min(value.secondary_count, value.secondary.size()) : value.secondary_count;
    for (std::size_t i{}; i < limit; ++i) {
        if (i)
            out.add(",");
        if (!value.secondary[i]) {
            out.failed = true;
            break;
        }
        append_monitoring_q2_factual_failure(out, *value.secondary[i]);
    }
    if (overflow) {
        if (limit)
            out.add(",");
        append_monitoring_q2_native_cleanup(out, "monitoring.q2.backend.cleanup_overflow",
                                            "finalization", 0);
    }
    out.add("]}\n");
    finish_monitoring_q2_diagnostic_buffer(
        out, monitoring_q2_phase_name(value.phase), underlying,
        value.primary.os_error && value.primary.os_error->value > 0 ? value.primary.os_error->value
                                                                    : 0);
    return !out.failed;
}
} // namespace epycbench::monitoring::qualification::detail
