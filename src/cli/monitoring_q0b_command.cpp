#include "cli/monitoring_q0b_command.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/monitoring/q0b_backend_result.hpp"
#include "monitoring/host_q0b_backend_driver.hpp"
#include "monitoring/host_q0b_governed_work.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "cli/monitoring_q0b_command_test_access.hpp"
#include "epycbench/runtime/linux_pairwise_production_host_operations_provider.hpp"
#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include "runtime/controller_process_production_authority.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

namespace epycbench::cli {
namespace {
using monitoring::qualification::Q0bBackendFailure;
using monitoring::qualification::Q0bBackendResult;
using Json = nlohmann::ordered_json;
thread_local detail::HostQ0bBackendDispatch host_dispatch{};

constexpr std::string_view fixed_transport_failure =
    "{\"failure\":{\"category\":\"monitoring\",\"classification\":\"contained_transport_failure\","
    "\"code\":\"monitoring.q0b.backend.transport_failure\",\"context\":{},"
    "\"continuation\":\"abort_command\",\"operation\":\"monitoring.q0b.backend.transport\","
    "\"severity\":\"error\"},\"schema\":\"epycbench.q0b-backend-error/v1\","
    "\"secondary_failures\":[]}\n";

auto write_fixed_transport_failure(std::ostream& error) noexcept -> int {
    try { error << fixed_transport_failure; } catch (...) {}
    return 1;
}

auto canonical_line(const Json& value) -> std::expected<std::string, core::Error> {
    auto canonical = core::canonicalize_json(value);
    if (!canonical) return std::unexpected{canonical.error()};
    canonical->push_back('\n');
    return canonical;
}

auto usage_failure() -> Q0bBackendFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q0b.backend.usage"},
             .message = "Invalid internal Q0b backend arguments",
             .operation = "monitoring.q0b.backend.arguments",
             .os_error = std::nullopt,
             .context = {{"classification", "invalid_arguments"}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}, {}};
}

auto transport_failure(std::string classification) -> Q0bBackendFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q0b.backend.transport_failure"},
             .message = "Internal Q0b backend result transport failed",
             .operation = "monitoring.q0b.backend.transport",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}, {}};
}

auto collected_at() -> std::string {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
                                      std::chrono::system_clock::now()));
}
auto valid_host_qid(std::string_view value)->bool{
 constexpr std::string_view prefix="MON-007.host-v2.";if(!value.starts_with(prefix))return false;
 const auto suffix=value.substr(prefix.size());if(suffix.empty()||suffix.size()>64U||
   suffix.front()=='-'||suffix.back()=='-')return false;
 return std::ranges::all_of(suffix,[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';});
}
} // namespace

auto render_monitoring_q0b_result(
    std::expected<Q0bBackendResult, Q0bBackendFailure> result,
    std::ostream& output, std::ostream& error) noexcept -> int {
    try {
        Json envelope;
        int status{};
        if (!result) {
            auto projected = monitoring::qualification::project_q0b_backend_error(
                monitoring::qualification::Q0bC0DriverFailure{
                    result.error().primary, result.error().secondary});
            if (!projected) return write_fixed_transport_failure(error);
            envelope = std::move(*projected);
            status = result.error().primary.code.value() == "monitoring.q0b.backend.usage" ? 2 : 1;
        } else if (const auto* diagnostics =
                       std::get_if<monitoring::qualification::Q0bBackendAuthoredDiagnostics>(
                           &*result)) {
            auto projected = monitoring::qualification::project_q0b_backend_authored_diagnostics(
                diagnostics->diagnostics);
            if (!projected) {
                auto outer = monitoring::qualification::project_q0b_backend_error(projected.error());
                if (!outer) return write_fixed_transport_failure(error);
                envelope = std::move(*outer);
                status = 1;
            } else {
                envelope = std::move(*projected);
                status = 6;
            }
        } else {
            auto line = canonical_line(std::get<Json>(*result));
            if (!line || line->size() > 8U * 1024U * 1024U)
                return write_fixed_transport_failure(error);
            output << *line;
            if (!output) return write_fixed_transport_failure(error);
            return 0;
        }
        auto line = canonical_line(envelope);
        if (!line || line->size() > 1024U * 1024U)
            return write_fixed_transport_failure(error);
        error << *line;
        if (!error) return 1;
        return status;
    } catch (...) {
        return write_fixed_transport_failure(error);
    }
}

auto run_monitoring_q0b_command(
    int argc, char* argv[], runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output, std::ostream& error) noexcept -> int {
    if (argc != 6 || std::string_view{argv[1]} != "__monitoring-q0b-c0-characterize" ||
        std::string_view{argv[2]} != "--repository-root" ||
        std::string_view{argv[4]} != "--qualification-id" ||
        std::string_view{argv[3]}.empty() || std::string_view{argv[5]}.empty())
        return render_monitoring_q0b_result(
            std::unexpected<Q0bBackendFailure>{usage_failure()}, output, error);
    try {
        auto provider = runtime::make_linux_pairwise_production_host_operations_provider();
        auto adapter = provider.adapter();
        auto operations = adapter.calibration_operations();
        if (!operations)
            return render_monitoring_q0b_result(
                std::unexpected<Q0bBackendFailure>{{operations.error(), {}}}, output, error);
        monitoring::qualification::Q0bBackendDependencies dependencies{
            .runner_context = runtime::production_runner_context_provider(),
            .operations = std::move(*operations)};
        return render_monitoring_q0b_result(
            monitoring::qualification::run_q0b_backend_driver(
                argv[3], argv[5], collected_at(), authority, dependencies),
            output, error);
    } catch (...) {
        return render_monitoring_q0b_result(
            std::unexpected<Q0bBackendFailure>{
                transport_failure("uncaught_internal_exception")},
            output, error);
    }
}

auto run_monitoring_host_q0b_command(
 int argc,char* argv[],runtime::ControllerProcessProductionAuthority& authority,
 std::ostream& output,std::ostream& error) noexcept->int{
 const auto render_failure=[&](Q0bBackendFailure failure) noexcept {
   const auto fallback=[&]() noexcept {try{error<<fixed_transport_failure;error.flush();}catch(...){}return 1;};
   try{
    auto projected=monitoring::qualification::project_q0b_backend_error(
      monitoring::qualification::Q0bC0DriverFailure{failure.primary,failure.secondary});
    if(!projected)return fallback();
    auto line=canonical_line(*projected);
    if(!line||line->size()>1024U*1024U)return fallback();
    const auto status=failure.primary.code.value()=="monitoring.q0b.backend.usage"?2:1;
    try{error<<*line;error.flush();return error?status:1;}catch(...){return 1;}
   }catch(...){return fallback();}
 };
 if(argc!=6||std::string_view{argv[1]}!="__monitoring-host-q0b-c0-characterize"||
    std::string_view{argv[2]}!="--repository-root"||std::string_view{argv[3]}.empty()||
    std::string_view{argv[4]}!="--qualification-id"||!valid_host_qid(argv[5]))
   return render_failure(usage_failure());
 auto result=host_dispatch?host_dispatch(argv[3],argv[5],authority):
   monitoring::qualification::detail::run_host_q0b_backend_driver(argv[3],argv[5],authority);
 if(!result)return render_failure(std::move(result.error()));
 try{
   const auto& bytes=result->canonical_json();
   if(bytes.size()>10U*1024U*1024U)return render_failure(
     transport_failure("success_payload_too_large"));
   output<<bytes;output.flush();if(!output)return 1;return 0;
 }catch(...){return 1;}
}

namespace {
namespace host = monitoring::qualification::detail;
namespace work = platform_linux::host_work;
namespace native = platform_linux::detail;
constexpr std::string_view governed_token = "__monitoring-host-q0b-governed-characterize";

template<class Integer>
auto canonical_integer(std::string_view text, Integer& output) noexcept -> bool {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    if (!std::ranges::all_of(text, [](char digit) { return digit >= '0' && digit <= '9'; })) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
struct GovernedArguments final {
    int projection{}, parent{};
    std::uint64_t origin{}, end{};
};
auto governed_arguments(int argc, char* argv[], std::uint64_t main_entry) noexcept
    -> std::optional<GovernedArguments> {
    if (argc != 14 || !argv) return std::nullopt;
    for (int index = 0; index < argc; ++index) if (!argv[index]) return std::nullopt;
    if (std::string_view{argv[1]} != governed_token ||
        std::string_view{argv[2]} != "--repository-root" || std::string_view{argv[3]}.empty() ||
        std::string_view{argv[3]}.size() > 4096 ||
        std::string_view{argv[4]} != "--qualification-id" || !valid_host_qid(argv[5]) ||
        std::string_view{argv[6]} != "--projection-fd" ||
        std::string_view{argv[8]} != "--expected-parent" ||
        std::string_view{argv[10]} != "--origin-raw-ns" ||
        std::string_view{argv[12]} != "--end-raw-ns") return std::nullopt;
    GovernedArguments result;
    if (!canonical_integer(argv[7], result.projection) || result.projection < 3 || result.projection >= 1024 ||
        !canonical_integer(argv[9], result.parent) || result.parent <= 0 ||
        !canonical_integer(argv[11], result.origin) || result.origin == 0 ||
        !canonical_integer(argv[13], result.end) || result.end <= result.origin ||
        result.end - result.origin > UINT64_C(300000000000) ||
        main_entry < result.origin || main_entry >= result.end) return std::nullopt;
    return result;
}
using HostResult = std::expected<monitoring::qualification::MonitoringHostQ0bBackendResult, Q0bBackendFailure>;
auto render_host_result(HostResult result, std::ostream& output, std::ostream& error) noexcept -> int {
    const auto fallback = [&]() noexcept {
        try { error << fixed_transport_failure; error.flush(); } catch (...) {}
        return 1;
    };
    try {
        if (!result) {
            auto projected = monitoring::qualification::project_q0b_backend_error(
                monitoring::qualification::Q0bC0DriverFailure{result.error().primary, result.error().secondary});
            if (!projected) return fallback();
            auto line = canonical_line(*projected);
            if (!line || line->size() > 1024U * 1024U) return fallback();
            const auto status = result.error().primary.code.value() == "monitoring.q0b.backend.usage" ? 2 : 1;
            error << *line; error.flush(); return error ? status : 1;
        }
        const auto& bytes = result->canonical_json();
        if (bytes.size() > 10U * 1024U * 1024U)
            return render_host_result(std::unexpected{transport_failure("success_payload_too_large")}, output, error);
        output << bytes; output.flush(); return output ? 0 : 1;
    } catch (...) { return fallback(); }
}
auto command_failure(std::string classification, std::optional<native::RawStatusResult> raw = std::nullopt)
    -> Q0bBackendFailure {
    auto result = transport_failure(std::move(classification));
    if (raw) {
        result.primary.message += " (native result " + std::to_string(raw->value) + ")";
        if (raw->error_number > 0)
            result.primary.os_error = core::OsError::capture(std::error_code{raw->error_number, std::generic_category()});
    }
    return result;
}
auto signal_failure(std::string classification, const native::SignalScopeDiagnostic& fact) -> Q0bBackendFailure {
    auto result = command_failure(std::move(classification));
    result.primary.context["underlying_code"] = "monitoring.q0b.signal." +
        std::to_string(static_cast<unsigned>(fact.stage)) + "." + std::to_string(static_cast<unsigned>(fact.reason));
    if (fact.error_number > 0)
        result.primary.os_error = core::OsError::capture(std::error_code{fact.error_number, std::generic_category()});
    return result;
}
void append_command_failure(HostResult& result, Q0bBackendFailure failure) {
    if (result) result = std::unexpected{std::move(failure)};
    else result.error().secondary.push_back(std::move(failure.primary));
}
void close_projection(HostResult& result, platform_linux::FileDescriptor& descriptor) {
    if (!descriptor.active()) return;
    const auto closed = native::DescriptorAccess::close_raw(descriptor);
    if (closed.value != 0 || closed.error_number != 0)
        append_command_failure(result, command_failure("projection_close", closed));
}
auto governed_body(int argc, char* argv[], runtime::ControllerProcessProductionAuthority& authority,
    platform_linux::FileDescriptor& descriptor, host::HostQ0bBackendDependencies dependencies,
    host::HostQ0bGovernedLeaves leaves, work::GuardOperations guard, std::uint64_t main_entry,
    std::uint64_t& last_accepted, host::HostQ0bWorkSnapshot* observed = nullptr) -> HostResult {
    const auto arguments = governed_arguments(argc, argv, main_entry);
    if (!arguments || !descriptor.active() || descriptor.native_handle() != arguments->projection)
        return std::unexpected{usage_failure()};
    auto owner = host::HostQ0bGovernedWork::prepare_governed(arguments->parent, descriptor,
        dependencies, leaves, {guard, arguments->origin, arguments->end, main_entry});
    if (!owner) {
        const auto classification = owner.error() == host::HostQ0bWorkReason::clock ? "clock" :
            owner.error() == host::HostQ0bWorkReason::deadline ? "deadline" :
            owner.error() == host::HostQ0bWorkReason::cancelled ? "cancelled" : "governed_preparation";
        return std::unexpected{command_failure(classification)};
    }
    auto result = owner->run_once(argv[3], argv[5], authority);
    const auto actual = owner->snapshot();
    last_accepted = actual.last_accepted_raw_ns;
    if (observed) *observed = actual;
    return result;
}
void finalize_governed(HostResult& result, platform_linux::FileDescriptor& descriptor,
    const std::optional<GovernedArguments>& arguments, work::GuardOperations guard,
    std::uint64_t& last_accepted, native::ControllerSignalScope*& signal_owner,
    runtime::RunnerCancellation* cancellation) {
    close_projection(result, descriptor);
    if (auto* active = std::exchange(signal_owner, nullptr)) {
        const auto restored = active->close();
        const auto observed = restored ? restored->observed : restored.error().facts.observed;
        if (observed != native::ControllerSignal::none && cancellation)
            static_cast<void>(cancellation->try_request(observed == native::ControllerSignal::interrupt
                ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
        if (!restored) {
            append_command_failure(result, signal_failure("signal_restore", restored.error().primary));
            for (std::size_t index = 0; index < restored.error().cleanup_count; ++index)
                append_command_failure(result, signal_failure("signal_restore", restored.error().cleanup[index]));
        }
    }
    // Use the actual owner's accepted history across consuming cleanup. Neither
    // cleanup nor the final sample starts a new interval or replaces that history.
    if (!result || !arguments) return;
    if (!guard.now || !guard.cancelled) {
        result = std::unexpected{command_failure("invalid_guard")}; return;
    }
    if (guard.cancelled(guard.context)) {
        result = std::unexpected{command_failure("cancelled")}; return;
    }
    std::uint64_t now{};
    const auto checked = guard.now(guard.context, &now);
    const auto actual = guard.actual_raw_now ? *guard.actual_raw_now : checked;
    if (checked.value != 0 || checked.error_number != 0 || now < arguments->origin || now < last_accepted)
        result = std::unexpected{command_failure("clock", actual)};
    else if (guard.cancelled(guard.context)) result = std::unexpected{command_failure("cancelled")};
    else if (now >= arguments->end) result = std::unexpected{command_failure("deadline", actual)};
    else last_accepted = now;
}
struct GovernedCommandGuard final {
    work::GuardOperations clock;
    runtime::RunnerCancellation& cancellation;
    native::ControllerSignalScope& signals;
    auto operations() noexcept -> work::GuardOperations {
        return {this, [](void* raw, std::uint64_t* output) noexcept -> native::RawStatusResult {
            auto& owner = *static_cast<GovernedCommandGuard*>(raw);
            return owner.clock.now(owner.clock.context, output);
        }, [](void* raw) noexcept {
            auto& owner = *static_cast<GovernedCommandGuard*>(raw);
            const auto signal = owner.signals.observe();
            if (signal != native::ControllerSignal::none)
                static_cast<void>(owner.cancellation.try_request(signal == native::ControllerSignal::interrupt
                    ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
            return owner.cancellation.requested();
        }, clock.actual_raw_now};
    }
};
} // namespace

auto is_monitoring_host_q0b_governed_command(int argc, char* argv[]) noexcept -> bool {
    return argc > 1 && argv && argv[1] && std::string_view{argv[1]} == governed_token;
}
auto run_monitoring_host_q0b_governed_command(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& cancellation,
    monitoring::qualification::detail::HostFrozenSourceLaunchClock& clock, std::uint64_t main_entry,
    std::ostream& output, std::ostream& error) noexcept -> int {
    // Adopt the known inherited role before any signal/metadata/discovery work.
    int number{};
    if (argc < 8 || !argv || !argv[6] || !argv[7] ||
        std::string_view{argv[6]} != "--projection-fd" || !canonical_integer(argv[7], number) ||
        number < 3 || number >= 1024) {
        try { return render_host_result(std::unexpected{usage_failure()}, output, error); }
        catch (...) { return write_fixed_transport_failure(error); }
    }
    auto descriptor = platform_linux::FileDescriptor::adopt(number);
    if (!descriptor) return write_fixed_transport_failure(error);
    native::ControllerSignalScope signals;
    native::ControllerSignalScope* signal_owner{};
    auto last_accepted = main_entry;
    const auto guard = host::production_host_frozen_source_launch_io(clock).guard;
    try {
        HostResult result = std::unexpected{usage_failure()};
        const auto arguments = governed_arguments(argc, argv, main_entry);
        if (arguments && clock.cancellation == &cancellation) {
            const auto acquired = signals.acquire();
            if (!acquired) {
                result = std::unexpected{signal_failure("signal_admission", acquired.error().primary)};
                for (std::size_t index = 0; index < acquired.error().cleanup_count; ++index)
                    append_command_failure(result, signal_failure("signal_restore", acquired.error().cleanup[index]));
            } else {
                signal_owner = &signals;
                GovernedCommandGuard owner{guard,
                    cancellation, signals};
                result = governed_body(argc, argv, authority, *descriptor,
                    host::production_host_q0b_backend_dependencies(), host::production_host_q0b_governed_leaves(),
                    owner.operations(), main_entry, last_accepted);
            }
        }
        finalize_governed(result, *descriptor, arguments, guard, last_accepted, signal_owner, &cancellation);
        return render_host_result(std::move(result), output, error);
    } catch (...) {
        static_cast<void>(native::DescriptorAccess::close_raw(*descriptor));
        if (auto* active = std::exchange(signal_owner, nullptr)) static_cast<void>(active->close());
        return write_fixed_transport_failure(error);
    }
}

auto detail::MonitoringQ0bCommandTestAccess::run_governed(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, platform_linux::FileDescriptor& descriptor,
    host::HostQ0bBackendDependencies dependencies, host::HostQ0bGovernedLeaves leaves,
    work::GuardOperations guard, std::uint64_t main_entry, std::ostream& output, std::ostream& error,
    host::HostQ0bWorkSnapshot* observed, native::ControllerSignalScope* signal_owner,
    runtime::RunnerCancellation* cancellation) noexcept -> int {
    auto last_accepted = main_entry;
    try {
        auto result = governed_body(argc, argv, authority, descriptor, dependencies, leaves, guard, main_entry, last_accepted, observed);
        finalize_governed(result, descriptor, governed_arguments(argc, argv, main_entry), guard,
            last_accepted, signal_owner, cancellation);
        return render_host_result(std::move(result), output, error);
    } catch (...) {
        static_cast<void>(native::DescriptorAccess::close_raw(descriptor));
        if (auto* active = std::exchange(signal_owner, nullptr)) static_cast<void>(active->close());
        return write_fixed_transport_failure(error);
    }
}

void detail::MonitoringQ0bCommandTestAccess::set_host_dispatch(
 detail::HostQ0bBackendDispatch value) noexcept{host_dispatch=value;}
void detail::MonitoringQ0bCommandTestAccess::reset() noexcept{host_dispatch=nullptr;}

} // namespace epycbench::cli
