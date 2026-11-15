#include "monitoring/host_q4_backend.hpp"
#include "monitoring/host_q4_authority_session.hpp"
#include "monitoring/host_q4_backend_transport.hpp"
#include "monitoring/host_q4_kernel_maximum_authority.hpp"
#include "monitoring/host_q4_source_ready.hpp"
#include "epycbench/platform_linux/q4_capacity_control.hpp"

namespace epycbench::monitoring::qualification::detail {
namespace {
auto terminal_error() -> core::Error { return {}; }
auto terminal_failure() -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure value;
    value.terminal_containment = true;
    return value;
}
auto valid_cleanup(const core::Error& value) noexcept -> bool {
    return std::ranges::any_of(core::detail::error_category_names,
                               [&](const auto& item) { return item.first == value.category; }) &&
           !value.code.value().empty() && value.code.value().size() <= 128U &&
           value.code.value().front() >= 'a' && value.code.value().front() <= 'z' &&
           std::ranges::all_of(value.code.value(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                      c == '.' || c == '-';
           }) &&
           !value.operation.empty() && value.operation.size() <= 64U &&
           value.operation.front() >= 'a' && value.operation.front() <= 'z' &&
           std::ranges::all_of(value.operation, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                      c == '.' || c == '-';
           }) &&
           value.severity == core::ErrorSeverity::error &&
           value.continuation == core::ContinuationPolicy::abort_command &&
           (!value.os_error || value.os_error->value >= 1);
}
auto valid_overflow(const core::Error& value) noexcept -> bool {
    return value.category == core::ErrorCategory::monitoring &&
           value.code.value() == "monitoring.q4.cleanup_overflow" &&
           value.operation == "run_monitoring_host_q4_backend" && !value.os_error &&
           value.context.empty() && value.severity == core::ErrorSeverity::error &&
           value.continuation == core::ContinuationPolicy::abort_command;
}
auto phase(MonitoringQ4ProjectedAuthorityPhase value) noexcept -> std::optional<MonitoringHostQ4BackendPhase> {
    using P = MonitoringQ4ProjectedAuthorityPhase;
    switch (value) {
    case P::repository: return MonitoringHostQ4BackendPhase::repository;
    case P::collection_plan: return MonitoringHostQ4BackendPhase::collection_plan;
    case P::predecessor: return MonitoringHostQ4BackendPhase::predecessor;
    case P::build_attestation: return MonitoringHostQ4BackendPhase::build_attestation;
    case P::finalization: return MonitoringHostQ4BackendPhase::finalization;
    }
    return std::nullopt;
}
auto authority_failure(const MonitoringQ4AuthorityFailure& value)
    -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure out;
    if (!value.projected) return terminal_failure();
    const auto& projected = *value.projected;
    if (projected.primary_kind == MonitoringQ4ProjectedPrimaryKind::terminal_containment) {
        if (projected.primary) return terminal_failure();
        out.terminal_containment = true;
    } else if (projected.primary_kind != MonitoringQ4ProjectedPrimaryKind::stage_failure ||
               !projected.primary ||
               projected.primary->code.value()!="monitoring.q4.backend.stage_failure" ||
               projected.primary->operation!="run_monitoring_host_q4_backend" ||
               projected.primary->category!=core::ErrorCategory::monitoring ||
               projected.primary->severity!=core::ErrorSeverity::error ||
               projected.primary->continuation!=core::ContinuationPolicy::abort_command ||
               !projected.primary->context.is_object()||
               (projected.primary->os_error&&projected.primary->os_error->value<1)) {
        return terminal_failure();
    } else {
        const auto projected_phase=phase(projected.phase);
        if(!projected_phase)return terminal_failure();
        out.phase = *projected_phase;
        const auto& context = projected.primary->context;
        const bool repository=out.phase==MonitoringHostQ4BackendPhase::repository;
        if(context.size()!=(repository&&context.contains("repository_classification")?2U:1U)||
           !context.contains("underlying_code")||
           !context.at("underlying_code").is_string()||
           (context.contains("repository_classification")&&
            !context.at("repository_classification").is_string())||
           (context.contains("repository_classification")&&!repository)){
            return terminal_failure();
        }
        if (context.is_object() && context.contains("underlying_code") &&
            context.at("underlying_code").is_string())
            out.underlying_code = context.at("underlying_code").get<std::string>();
        if(repository&&context.contains("repository_classification")&&
           context.at("repository_classification").is_string())
            out.repository_classification=context.at("repository_classification").get<std::string>();
        if(out.underlying_code.empty())return terminal_failure();
        if (projected.primary->os_error)
            out.native_error = projected.primary->os_error->value;
    }
    if(projected.cleanup_count>projected.cleanup.size())return terminal_failure();
    const auto count = projected.cleanup_count;
    for (std::size_t index{}; index < count; ++index) {
        if (!projected.cleanup[index]) return terminal_failure();
        if (projected.cleanup[index]->code.value() == "monitoring.q4.cleanup_overflow") {
            if(index+1U!=count||index!=31U||out.cleanup_count!=31U||
               !valid_overflow(*projected.cleanup[index]))return terminal_failure();
            out.cleanup_overflowed = true;
            continue;
        }
        if (!valid_cleanup(*projected.cleanup[index])) return terminal_failure();
        if (out.cleanup_count == out.cleanup.size()) return terminal_failure();
        out.cleanup[out.cleanup_count++] = projected.cleanup[index];
    }
    return out;
}
auto kernel_reason_valid(const MonitoringQ4KernelMaximumFailure& value) noexcept -> bool {
    using K = MonitoringQ4KernelMaximumFailureKind;
    using R = MonitoringQ4CapacityReason;
    using F = MonitoringQ4CapacityReasonFamily;
    const auto* entry = monitoring_q4_capacity_reason_entry(value.reason);
    if (value.primary == K::close || value.primary == K::contained || value.primary == K::inactive)
        return value.reason == R::none;
    if (value.reason == R::unspecified) return true;
    if (!entry) return false;
    switch (value.primary) {
    case K::open: return value.reason == R::kernel_retain_open;
    case K::held_stat: return value.reason == R::kernel_retain_held_stat || value.reason == R::kernel_after_held_stat;
    case K::named_stat: return value.reason == R::kernel_retain_named_stat || value.reason == R::kernel_after_named_stat;
    case K::read: return value.reason == R::kernel_body_read || value.reason == R::kernel_probe_read;
    case K::read_contract: return value.reason == R::kernel_body_overreport || value.reason == R::kernel_probe_overreport;
    case K::byte_limit: return value.reason == R::kernel_probe_not_eof;
    case K::invalid_value:
        return entry->family == F::adapter || entry->family == F::linux ||
               entry->code.starts_with("monitoring.q4.capacity.kernel.value.");
    case K::identity:
        switch (value.reason) {
        case R::kernel_retain_type:
        case R::kernel_retain_links:
        case R::kernel_retain_device:
        case R::kernel_retain_inode:
        case R::kernel_retain_owner:
        case R::kernel_retain_mode:
        case R::kernel_retain_identity_links:
        case R::kernel_after_type:
        case R::kernel_after_links:
        case R::kernel_after_held_device:
        case R::kernel_after_held_inode:
        case R::kernel_after_held_owner:
        case R::kernel_after_held_mode:
        case R::kernel_after_held_links:
        case R::kernel_after_named_device:
        case R::kernel_after_named_inode:
        case R::kernel_after_named_owner:
        case R::kernel_after_named_mode:
        case R::kernel_after_named_links:
            return true;
        default: return false;
        }
    default: return false;
    }
}
auto ready_reason_valid(MonitoringQ4SourceReadyFailure value) noexcept -> bool {
    using K = MonitoringQ4SourceReadyFailureKind;
    using R = MonitoringQ4CapacityReason;
    using F = MonitoringQ4CapacityReasonFamily;
    if (value.kind == K::inactive || value.kind == K::contained || value.kind == K::plan_invalid)
        return value.reason == R::none;
    if (value.reason == R::unspecified) return true;
    const auto* entry = monitoring_q4_capacity_reason_entry(value.reason);
    if (!entry) return false;
    switch (value.kind) {
    case K::page_query_failed: return value.reason == R::ready_page_query;
    case K::page_size_mismatch: return value.reason == R::ready_page_mismatch;
    case K::admitted_cpus_invalid:
        return value.reason == R::ready_cpus_empty || value.reason == R::ready_cpu_range || value.reason == R::ready_cpu_order;
    case K::allocation:
        return (entry->family == F::ready && entry->code.ends_with(".allocation")) ||
               (entry->family == F::digits && entry->code.ends_with(".allocation_scope"));
    case K::capacity:
        return monitoring_q4_capacity_reason_matches(value.reason,
            {F::structural,F::inputs,F::digits,F::collection,F::pass,F::unsupported,F::interval,F::success}) &&
            !entry->code.ends_with(".allocation_scope");
    default: return false;
    }
}
auto kernel_failure(const MonitoringQ4KernelMaximumFailure& value)
    -> MonitoringHostQ4BackendFailure {
    using K = MonitoringQ4KernelMaximumFailureKind;
    MonitoringHostQ4BackendFailure out;
    if (!kernel_reason_valid(value)) return terminal_failure();
    switch(value.primary){
    case K::contained:out.terminal_containment=true;break;
    case K::inactive:return terminal_failure();
    case K::close:
        out.phase = MonitoringHostQ4BackendPhase::finalization;
        out.underlying_code = "monitoring.q4.finalization_failed";
        break;
    case K::open:case K::held_stat:case K::named_stat:case K::identity:case K::read:
    case K::read_contract:case K::byte_limit:case K::invalid_value:
        out.phase = MonitoringHostQ4BackendPhase::capacity;
        out.underlying_code = *monitoring_q4_capacity_reason_code(value.reason);
        break;
    default:out.terminal_containment=true;return out;
    }
    if (value.primary == K::contained &&
        (value.native_error != 0 || value.primary_uncertain)) return terminal_failure();
    if(value.native_error<0||value.primary_uncertain!=(value.primary==K::close)){
        out=MonitoringHostQ4BackendFailure{};out.terminal_containment=true;return out;
    }
    if (value.native_error > 0) out.native_error = value.native_error;
    if (value.cleanup) {
        if (value.primary == K::close) return terminal_failure();
        if(value.cleanup->kind!=K::close||!value.cleanup->uncertain){
            out=MonitoringHostQ4BackendFailure{};out.terminal_containment=true;return out;
        }
        if(value.cleanup->native_error<0){out=MonitoringHostQ4BackendFailure{};out.terminal_containment=true;return out;}
        out.cleanup[0] = core::Error{.category=core::ErrorCategory::monitoring,
            .code=core::StableErrorCode{"monitoring.q4.finalization_failed"}, .message={},
            .operation="retire_monitoring_q4_kernel_maximum",
            .os_error=value.cleanup->native_error > 0 ? std::optional{core::OsError{value.cleanup->native_error,"errno",""}} : std::nullopt,
            .context={}, .severity=core::ErrorSeverity::error,
            .continuation=core::ContinuationPolicy::abort_command};
        out.cleanup_count = 1;
    }
    return out;
}
auto source_ready_failure(MonitoringQ4SourceReadyFailure value)
    -> MonitoringHostQ4BackendFailure {
    MonitoringHostQ4BackendFailure out;
    using F=MonitoringQ4SourceReadyFailureKind;
    if (!ready_reason_valid(value)) return terminal_failure();
    switch(value.kind){
    case F::contained:case F::inactive:out.terminal_containment=true;break;
    case F::plan_invalid:
        out.phase = MonitoringHostQ4BackendPhase::collection_plan;
        out.underlying_code = "monitoring.q4.collection_plan_invalid";
        break;
    case F::page_query_failed:case F::page_size_mismatch:case F::admitted_cpus_invalid:
    case F::capacity:case F::allocation:
        out.phase = MonitoringHostQ4BackendPhase::capacity;
        out.underlying_code = *monitoring_q4_capacity_reason_code(value.reason);
        break;
    default:out.terminal_containment=true;break;
    }
    return out;
}
auto output(MonitoringHostQ4BackendOutputKind kind,
            std::expected<std::string, core::Error> bytes, std::uint64_t limit = 792302U)
    -> std::expected<MonitoringHostQ4BackendOutput, core::Error> {
    if (!bytes) return std::unexpected{std::move(bytes.error())};
    return MonitoringHostQ4BackendOutput{kind, std::move(*bytes), limit};
}
auto authored(const MonitoringQ4AuthorityFailure& failure)
    -> std::optional<MonitoringHostQ4BackendAuthored> {
    if (failure.stage != MonitoringQ4AuthorityStage::plan_schema) return std::nullopt;
    const auto* binding = std::get_if<MonitoringQ4GovernedPlanSchemaBindingFailure>(&failure.detail);
    if (!binding) return std::nullopt;
    const auto* diagnostic = std::get_if<MonitoringGovernedPlanAuthoredDiagnostic>(binding);
    if (!diagnostic) return std::nullopt;
    return MonitoringHostQ4BackendAuthored{{*diagnostic}};
}
} // namespace
auto project_monitoring_host_q4_capacity_control_failure(
    const platform_linux::Q4CapacityControlResult& result)
    -> MonitoringHostQ4BackendFailure {
    const auto native_error = result.native_error;
    const auto cleanup_error = result.cleanup_native_error;
    MonitoringHostQ4BackendFailure failure;
    const bool sole_close = result.close_failed && !result.primary_failure;
    using L = platform_linux::Q4CapacityControlReason;
    using R = MonitoringQ4CapacityReason;
    R reason = R::none;
    switch (result.reason) {
    case L::none: break;
    case L::unspecified: reason = R::unspecified; break;
    case L::ack_exception_scope: reason = R::control_ack_exception_scope; break;
    case L::ack_canonical: reason = R::control_ack_canonical; break;
    case L::ack_mismatch: reason = R::control_ack_mismatch; break;
    case L::compare_exception_scope: reason = R::control_compare_exception_scope; break;
    case L::descriptor: reason = R::control_descriptor; break;
    case L::evidence_zero: reason = R::control_evidence_zero; break;
    case L::preamble_exception_scope: reason = R::control_preamble_exception_scope; break;
    case L::preamble_canonical: reason = R::control_preamble_canonical; break;
    case L::preamble_limit: reason = R::control_preamble_limit; break;
    case L::read_deadline: reason = R::control_read_deadline; break;
    case L::read_error: reason = R::control_read_error; break;
    case L::read_events: reason = R::control_read_events; break;
    case L::read_exception_scope: reason = R::control_read_exception_scope; break;
    case L::read_limit: reason = R::control_read_limit; break;
    case L::read_poll_error: reason = R::control_read_poll_error; break;
    case L::read_poll_timeout: reason = R::control_read_poll_timeout; break;
    case L::read_premature_eof: reason = R::control_read_premature_eof; break;
    case L::read_trailing: reason = R::control_read_trailing; break;
    case L::shutdown_deadline: reason = R::control_shutdown_deadline; break;
    case L::shutdown_error: reason = R::control_shutdown_error; break;
    case L::shutdown_exception_scope: reason = R::control_shutdown_exception_scope; break;
    case L::stdout_short: reason = R::control_stdout_short; break;
    case L::timeout_zero: reason = R::control_timeout_zero; break;
    case L::wire_exception_scope: reason = R::control_wire_exception_scope; break;
    case L::write_deadline: reason = R::control_write_deadline; break;
    case L::write_events: reason = R::control_write_events; break;
    case L::write_exception_scope: reason = R::control_write_exception_scope; break;
    case L::write_poll_error: reason = R::control_write_poll_error; break;
    case L::write_poll_timeout: reason = R::control_write_poll_timeout; break;
    case L::write_send: reason = R::control_write_send; break;
    case L::write_zero: reason = R::control_write_zero; break;
    default: return terminal_failure();
    }
    if (result.acknowledged || native_error < 0 || cleanup_error < 0 ||
        (sole_close ? (reason != R::none || native_error != 0 || cleanup_error == 0)
                    : (!result.primary_failure || !monitoring_q4_capacity_reason_code(reason))))
        return terminal_failure();
    failure.phase=sole_close?MonitoringHostQ4BackendPhase::finalization:MonitoringHostQ4BackendPhase::capacity;
    failure.underlying_code = sole_close ? std::string_view{"monitoring.q4.finalization_failed"}
                                         : *monitoring_q4_capacity_reason_code(reason);
    if(sole_close)failure.native_error=cleanup_error;else if(native_error>0)failure.native_error=native_error;
    if(!sole_close&&cleanup_error>0){failure.cleanup[0]=core::Error{.category=core::ErrorCategory::monitoring,.code=core::StableErrorCode{"monitoring.q4.capacity_control_close_failed"},.message={},.operation="close_q4_capacity_control",.os_error=core::OsError{cleanup_error,"errno",""},.context={},.severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};failure.cleanup_count=1;}
    return failure;
}

auto run_monitoring_host_q4_backend_stages(MonitoringHostQ4BackendStages operations) noexcept
    ->std::expected<MonitoringHostQ4BackendOutput,core::Error>{
    try{
        if(!operations.context)return std::unexpected{terminal_error()};
        for(const auto stage:operations.stage){if(!stage)return std::unexpected{terminal_error()};auto result=stage(operations.context);if(!result)return std::unexpected{std::move(result.error())};if(std::holds_alternative<std::monostate>(*result))continue;if(auto*f=std::get_if<MonitoringQ4AuthorityFailure>(&*result)){if(auto a=authored(*f))return output(MonitoringHostQ4BackendOutputKind::authored,serialize_monitoring_host_q4_backend_authored_pre_source(*a));return output(MonitoringHostQ4BackendOutputKind::failure,serialize_monitoring_host_q4_backend_failure_pre_source(authority_failure(*f)));}if(auto*f=std::get_if<MonitoringQ4KernelMaximumFailure>(&*result))return output(MonitoringHostQ4BackendOutputKind::failure,serialize_monitoring_host_q4_backend_failure_pre_source(kernel_failure(*f)));if(auto*f=std::get_if<MonitoringQ4SourceReadyFailure>(&*result))return output(MonitoringHostQ4BackendOutputKind::failure,serialize_monitoring_host_q4_backend_failure_pre_source(source_ready_failure(*f)));if(auto*out=std::get_if<MonitoringHostQ4BackendOutput>(&*result))return std::move(*out);return std::unexpected{terminal_error()};}
        return std::unexpected{terminal_error()};
    }catch(...){return std::unexpected{terminal_error()};}
}
auto project_monitoring_host_q4_adapter_result(MonitoringQ4SourceReady&ready,
    std::expected<std::string,MonitoringHostQ4BackendFailure> value) noexcept
    ->std::expected<MonitoringHostQ4BackendOutput,core::Error>{
    try{if(value&&!validate_monitoring_host_q4_backend_result(ready,*value))return std::unexpected{terminal_error()};return value?output(MonitoringHostQ4BackendOutputKind::success,
        std::expected<std::string,core::Error>{std::move(*value)},ready.capacity().success_stdout_bytes().value):
        output(MonitoringHostQ4BackendOutputKind::failure,
            serialize_monitoring_host_q4_backend_failure(ready,value.error()),
            ready.capacity().backend_stderr_bytes().value);}catch(...){return std::unexpected{terminal_error()};}
}
auto run_monitoring_host_q4_backend(const std::filesystem::path& root, std::string_view qid,
                                    runtime::ControllerProcessProductionAuthority&,int capacity_control_fd) noexcept
    -> std::expected<MonitoringHostQ4BackendOutput, core::Error> {
    try {
        struct State{const std::filesystem::path*root;std::string_view qid;int capacity_control_fd;std::optional<MonitoringQ4AuthoritySession>session;std::optional<MonitoringQ4KernelMaximumReady>kernel_ready;std::optional<MonitoringQ4KernelMaximumAuthority>maximum;std::optional<MonitoringQ4AuthenticatedKernelMaximum>authenticated;std::optional<MonitoringQ4SourceReady>ready;};
        struct Production{
          static auto auth(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);auto v=MonitoringQ4AuthoritySession::authenticate(*s.root,s.qid);if(!v)return MonitoringHostQ4BackendStageResult{std::move(v.error())};s.session.emplace(std::move(*v));return std::monostate{};}
          static auto finalize(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.session)return std::unexpected{terminal_error()};auto v=s.session->finalize_before_kernel_max();if(!v)return MonitoringHostQ4BackendStageResult{std::move(v.error())};s.kernel_ready.emplace(std::move(*v));return std::monostate{};}
          static auto retain(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.kernel_ready)return std::unexpected{terminal_error()};auto v=MonitoringQ4KernelMaximumAuthority::retain(*s.kernel_ready);if(!v)return MonitoringHostQ4BackendStageResult{v.error()};s.maximum.emplace(std::move(*v));return std::monostate{};}
          static auto authenticate_max(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.maximum)return std::unexpected{terminal_error()};auto v=s.maximum->authenticate_and_retire();if(!v)return MonitoringHostQ4BackendStageResult{v.error()};s.authenticated.emplace(std::move(*v));return std::monostate{};}
          static auto source_ready(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.kernel_ready||!s.authenticated)return std::unexpected{terminal_error()};auto transition=MonitoringQ4SourceReadyTransition::begin(std::move(*s.kernel_ready));auto v=transition.finalize(std::move(*s.authenticated));if(!v)return MonitoringHostQ4BackendStageResult{v.error()};s.ready.emplace(std::move(*v));return std::monostate{};}
          static auto capacity(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.ready)return std::unexpected{terminal_error()};const auto result=platform_linux::q4_capacity_control_exchange(s.capacity_control_fd,s.ready->capacity().evidence_bytes().value,s.ready->capacity().success_stdout_bytes().value,5000U);if(result.acknowledged)return std::monostate{};auto failure=project_monitoring_host_q4_capacity_control_failure(result);auto bytes=serialize_monitoring_host_q4_backend_failure(*s.ready,failure);if(!bytes)return std::unexpected{terminal_error()};return MonitoringHostQ4BackendStageResult{MonitoringHostQ4BackendOutput{MonitoringHostQ4BackendOutputKind::failure,std::move(*bytes),s.ready->capacity().backend_stderr_bytes().value}};}
          static auto adapter(void*raw)->std::expected<MonitoringHostQ4BackendStageResult,core::Error>{auto&s=*static_cast<State*>(raw);if(!s.ready)return std::unexpected{terminal_error()};auto out=project_monitoring_host_q4_adapter_result(*s.ready,run_monitoring_host_q4_backend_adapter(*s.ready));if(!out)return std::unexpected{std::move(out.error())};return MonitoringHostQ4BackendStageResult{std::move(*out)};}
        };
        State state{&root,qid,capacity_control_fd,{},{},{},{},{}};
        return run_monitoring_host_q4_backend_stages({&state,{Production::auth,Production::finalize,Production::retain,Production::authenticate_max,Production::source_ready,Production::capacity,Production::adapter}});
    } catch (...) { return std::unexpected{terminal_error()}; }
}
} // namespace epycbench::monitoring::qualification::detail
