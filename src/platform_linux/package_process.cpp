#include "platform_linux/package_process.hpp"

#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/current_process_image.hpp"
#include "runtime/controller_lifecycle_policy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>
#include <sys/prctl.h>
#include <unistd.h>

namespace epycbench::platform_linux::package_process {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;
using SignalScope = detail::ControllerSignalScope;
constexpr std::size_t message_budget = 16U*1024U*1024U;
constexpr std::array<std::string_view,2> arguments{"epycbench","__package-runner"};

auto invalid(std::string message, Json context = Json::object()) -> core::Error {
    return {core::ErrorCategory::runtime,core::StableErrorCode{"runtime.package.runner"},
        std::move(message),"package_process",{},std::move(context),core::ErrorSeverity::error,
        core::ContinuationPolicy::abort_command};
}

auto controller_signal(const SignalScope* signals) noexcept -> int {
    const auto observed = signals ? signals->observe() : detail::ControllerSignal::none;
    return observed == detail::ControllerSignal::interrupt ? SIGINT :
           observed == detail::ControllerSignal::terminate ? SIGTERM : 0;
}

auto cancelled(int signal, std::string_view phase) -> core::Error {
    return {core::ErrorCategory::runtime, core::StableErrorCode{"runtime.package.cancelled"},
        "Package Run interrupted by Controller signal", "package_process",
        {}, {{"signal",signal},{"phase",phase}}, core::ErrorSeverity::error,
        core::ContinuationPolicy::abort_command};
}

auto poll_wait(Clock::time_point deadline, const SignalScope* signals) -> Duration {
    const auto remaining = std::chrono::ceil<Duration>(deadline-Clock::now());
    return signals ? std::min(remaining,runtime::detail::controller_lifecycle_policy_v1().observation_quantum)
                   : remaining;
}

bool interrupted_poll(const core::Error& error) noexcept {
    return error.os_error && error.os_error->value == EINTR;
}

// Closing the writing endpoint delimits one bounded message. Both directions have
// distinct sockets, so a short read or write cannot discard the rest of a frame.
auto send(LocalIpcEndpoint& endpoint,std::string_view input, Clock::time_point deadline,
          const SignalScope* signals = nullptr)
    -> std::expected<void,core::Error> {
    if (input.size()>message_budget) return std::unexpected{invalid("Package IPC message exceeds 16 MiB")};
    auto bytes=std::as_bytes(std::span(input));
    while (!bytes.empty()) {
        if (const auto signal=controller_signal(signals); signal)
            return std::unexpected{cancelled(signal,"dispatch")};
        const auto wait=poll_wait(deadline,signals).count();
        if (wait<=0) return std::unexpected{invalid("Package IPC write timed out")};
        auto ready=endpoint.observe_writable(wait);
        if (!ready) {
            if (interrupted_poll(ready.error())) continue;
            return std::unexpected{ready.error()};
        }
        if (ready->timed_out) continue;
        if (!ready->writable) return std::unexpected{invalid("Package IPC peer stopped accepting input")};
        auto sent=endpoint.send(bytes.first(std::min(bytes.size(),std::size_t{4096})));
        if (!sent) return std::unexpected{sent.error()};
        if (*sent==0) return std::unexpected{invalid("Package IPC write made no progress")};
        bytes=bytes.subspan(*sent);
    }
    return endpoint.close();
}

auto read(LocalIpcEndpoint& endpoint,Clock::time_point deadline,const SignalScope* signals = nullptr)
    -> std::expected<Json,core::Error> {
    std::string text;
    std::array<std::byte,8192> buffer;
    for (;;) {
        if (const auto signal=controller_signal(signals); signal)
            return std::unexpected{cancelled(signal,"execution")};
        const auto wait=poll_wait(deadline,signals).count();
        if (wait<=0) return std::unexpected{invalid("Package Runner exceeded its execution deadline")};
        auto ready=endpoint.readiness(wait);
        if (!ready) {
            if (interrupted_poll(ready.error())) continue;
            return std::unexpected{ready.error()};
        }
        if (ready->timed_out) continue;
        auto count=endpoint.receive(buffer);
        if (!count) return std::unexpected{count.error()};
        if (!*count) break;
        if (*count>message_budget-text.size()) return std::unexpected{invalid("Package IPC message exceeds 16 MiB")};
        text.append(reinterpret_cast<const char*>(buffer.data()),*count);
    }
    auto result=Json::parse(text,nullptr,false);
    if (!result.is_object()) return std::unexpected{invalid("Incomplete package Runner reply")};
    return result;
}

struct Retirement {
    ChildProcess& child;
    ~Retirement() {
        if (child.active()) {
            static_cast<void>(child.send_signal(ProcessSignal::kill));
            static_cast<void>(child.wait());
        }
    }
};

auto observe_exit(ChildProcess& child, Clock::time_point deadline, Duration quantum,
                  const SignalScope* signals = nullptr)
    -> std::expected<std::optional<ProcessExitStatus>,core::Error> {
    for (;;) {
        if (controller_signal(signals)) return std::nullopt;
        auto status = child.try_wait();
        if (!status || status->has_value()) return status;
        const auto remaining = std::chrono::ceil<Duration>(deadline - Clock::now());
        if (remaining <= Duration::zero()) return std::nullopt;
        std::this_thread::sleep_for(std::min(quantum, remaining));
    }
}
} // namespace

auto collect(const Json& request, const SignalScope* signals) -> Block {
    if (const auto signal=controller_signal(signals); signal)
        return std::unexpected{cancelled(signal,"before_dispatch")};
    const auto limits=runtime::detail::controller_lifecycle_policy_v1();
    const auto admission_deadline=Clock::now()+limits.startup_timeout;
    const auto operation=request.value("operation","");
    const bool startup_only=operation=="inspect" || operation=="validate";
    auto image=detail::CurrentProcessImage::acquire();
    if (!image) return std::unexpected{invalid("Cannot bind the running executable for the package Runner")};
    auto channels=create_local_ipc_channels();
    if (!channels) return std::unexpected{channels.error()};
    auto child=image->spawn(arguments,channels->runner);
    if (!child) return std::unexpected{invalid("Cannot launch the bound package Runner",{{"errno",child.error().error_number}})};
    Retirement retire{*child};
    if (!channels->runner.control_reader.close() || !channels->runner.event_writer.close())
        return std::unexpected{invalid("Cannot release child endpoints in Controller")};
    const auto retire_cancelled=[&](int signal,std::string_view phase) -> Block {
        if (child->active()) {
            static_cast<void>(child->send_signal(ProcessSignal::terminate));
            static_cast<void>(observe_exit(*child,Clock::now()+limits.cancellation_grace,limits.observation_quantum));
        }
        return std::unexpected{cancelled(signal,phase)};
    };
    const auto sent=send(channels->controller.control_writer,request.dump(),admission_deadline,signals);
    if (const auto signal=controller_signal(signals); signal) return retire_cancelled(signal,"dispatch");
    if (!sent) return std::unexpected{sent.error()};
    const auto execution_deadline=startup_only ? admission_deadline : Clock::now()+limits.execution_timeout;
    auto reply=read(channels->controller.event_reader,execution_deadline,signals);
    if (const auto signal=controller_signal(signals); signal) return retire_cancelled(signal,"execution");
    // The EOF from a dying worker may arrive before its wait status. Always reap
    // it, including after a reply, within the normal Controller grace periods.
    const auto retirement_deadline=startup_only ? admission_deadline :
        Clock::now()+(reply ? limits.completion_grace : limits.termination_grace);
    auto status=observe_exit(*child,retirement_deadline,limits.observation_quantum,signals);
    if (const auto signal=controller_signal(signals); signal) return retire_cancelled(signal,"retirement");
    if (!status) return std::unexpected{status.error()};
    if (!*status) {
        if (!reply) return std::unexpected{reply.error()};
        return std::unexpected{invalid("Package Runner failed to exit before the completion deadline")};
    }
    if (const auto* signal=std::get_if<ProcessSignaled>(&**status))
        return std::unexpected{invalid("Package Runner terminated by signal",{{"signal",signal->signal},{"core_dumped",signal->core_dumped}})};
    const auto exit_code=std::get<ProcessExited>(**status).code;
    if (exit_code!=0)
        return std::unexpected{invalid("Package Runner exited unsuccessfully",{{"exit_code",exit_code},{"reply_received",reply.has_value()}})};
    if (!reply) return std::unexpected{reply.error()};
    if (reply->contains("failure")) {
        try {
            return std::unexpected{reply->at("failure").get<core::Error>()};
        } catch (const std::exception&) {
            return std::unexpected{invalid("Package Runner returned malformed error evidence")};
        }
    }
    return reply;
}

int serve(Execute execute) {
    const auto parent=::getppid();
    if (parent<=1 || ::prctl(PR_SET_PDEATHSIG,SIGKILL)!=0 || ::getppid()!=parent) return 1;
    auto control=LocalIpcEndpoint::adopt(3);
    auto event=LocalIpcEndpoint::adopt(4);
    if (!control || !event) return 1;
    const auto timeout=runtime::detail::controller_lifecycle_policy_v1().startup_timeout;
    auto request=read(*control,Clock::now()+timeout);
    if (!request) return 1;
    Json reply;
    try {
        auto outcome=execute(*request);
        if (outcome) reply=std::move(*outcome);
        else reply={{"failure",outcome.error()}};
    } catch (const std::exception& e) {
        reply={{"failure",invalid(e.what())}};
    }
    return send(*event,reply.dump(),Clock::now()+timeout) ? 0 : 1;
}
}
