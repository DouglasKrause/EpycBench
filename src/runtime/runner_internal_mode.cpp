#include "epycbench/runtime/runner_internal_mode.hpp"

#include "runtime/platform_ipc_adapters.hpp"
#include "runtime/runner_internal_mode_access.hpp"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::runtime {
namespace detail {
namespace {

inline constexpr int runner_control_descriptor = 3;
inline constexpr int runner_event_descriptor = 4;

struct RunnerTransport {
    platform_linux::LocalIpcEndpoint control_reader;
    platform_linux::LocalIpcEndpoint event_writer;
};

struct RunnerOutcome {
    std::optional<core::Error> primary;
    std::optional<core::Error> event_close;
    std::optional<core::Error> control_close;
};

[[nodiscard]] auto runner_error(std::string_view code, std::string_view message,
                                std::string_view operation, std::string_view classification,
                                std::string_view phase,
                                std::optional<std::string_view> channel = std::nullopt,
                                const core::Error* underlying = nullptr) -> core::Error {
    auto context = nlohmann::json{
        {"classification", classification},
        {"runner_phase", phase},
        {"component", "runner_internal_mode"},
    };
    if (channel) {
        context["channel"] = *channel;
    }
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
    }
    std::optional<core::OsError> os_error;
    if (underlying != nullptr && underlying->os_error) {
        os_error = core::OsError{
            .value = underlying->os_error->value,
            .domain = "native",
            .message = "",
        };
    }
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto contained_error(std::string_view phase, bool unknown) -> core::Error {
    return runner_error(unknown ? "runtime.runner.contained_unknown_failure"
                                : "runtime.runner.contained_exception",
                        "Runner internal mode contained a failure",
                        "run_runner_internal_mode", unknown ? "contained_unknown_failure"
                                                             : "contained_exception",
                        phase);
}

[[nodiscard]] auto adopt_endpoint(RunnerEndpointFactory factory, int descriptor,
                                  std::string_view channel)
    -> std::expected<platform_linux::LocalIpcEndpoint, core::Error> {
    try {
        auto endpoint = factory.adopt(factory.context, descriptor);
        if (!endpoint) {
            return std::unexpected{runner_error(
                "runtime.runner.transport_setup_failed", "Runner endpoint adoption failed",
                "run_runner_internal_mode", "endpoint_adoption_failed", "startup", channel,
                &endpoint.error())};
        }
        return std::move(*endpoint);
    } catch (const std::exception&) {
        return std::unexpected{contained_error("startup", false)};
    } catch (...) {
        return std::unexpected{contained_error("startup", true)};
    }
}

[[nodiscard]] auto close_error(const core::Error& underlying, std::string_view channel,
                               std::string_view classification) -> core::Error {
    return runner_error("runtime.runner.cleanup_failed", "Runner endpoint cleanup failed",
                        "run_runner_internal_mode", classification, "cleanup", channel,
                        &underlying);
}

void render_once(const InternalErrorRenderer& renderer, const core::Error& error) noexcept {
    if (renderer.render != nullptr) {
        renderer.render(renderer.context, error);
    }
}

auto production_adopt(void*, int descriptor)
    -> std::expected<platform_linux::LocalIpcEndpoint, core::Error> {
    return platform_linux::LocalIpcEndpoint::adopt(descriptor);
}

auto production_capability(void*, platform_linux::LocalIpcEndpoint& endpoint)
    -> std::expected<RunnerControlReadCapability, core::Error> {
    return make_runner_control_read_capability(endpoint);
}

void production_render(void*, const core::Error& error) noexcept {
    try {
        std::string_view classification{"runner_failure"};
        if (error.context.is_object()) {
            const auto item = error.context.find("classification");
            if (item != error.context.end() && item->is_string()) {
                classification = item->get_ref<const std::string&>();
            }
        }
        static_cast<void>(std::fprintf(stderr, "%s: %.*s\n", error.code.value().c_str(),
                                       static_cast<int>(classification.size()),
                                       classification.data()));
    } catch (...) {
    }
}

} // namespace

auto run_runner_internal_mode_with(std::span<const std::string_view> arguments,
                                   RunnerSession session, RunnerEndpointFactory endpoints,
                                   RunnerControlCapabilityFactory capabilities,
                                   InternalErrorRenderer renderer) -> int {
    try {
        if (classify_runner_invocation(arguments) != RunnerInvocationKind::valid_runner) {
            const auto error = runner_error(
                "runtime.runner.invalid_invocation", "Invalid runner internal invocation",
                "run_runner_internal_mode", "invalid_internal_mode", "invocation");
            render_once(renderer, error);
            return 2;
        }
        if (session.run == nullptr) {
            const auto error = runner_error(
                "runtime.runner.session_failed", "Runner session callback is unavailable",
                "run_runner_internal_mode", "session_failed", "session");
            render_once(renderer, error);
            return 1;
        }

        auto control = adopt_endpoint(endpoints, runner_control_descriptor, "control");
        if (!control) {
            render_once(renderer, control.error());
            return 1;
        }
        auto events = adopt_endpoint(endpoints, runner_event_descriptor, "event");
        if (!events) {
            static_cast<void>(control->close());
            render_once(renderer, events.error());
            return 1;
        }

        RunnerTransport transport{.control_reader = std::move(*control),
                                  .event_writer = std::move(*events)};
        EndpointByteWriter writer{transport.event_writer};
        RunnerOutcome outcome;
        {
            auto capability = [&]() -> std::expected<RunnerControlReadCapability, core::Error> {
                try {
                    if (capabilities.create == nullptr)
                        return std::unexpected{contained_error("startup", false)};
                    return capabilities.create(capabilities.context, transport.control_reader);
                } catch (const std::exception&) {
                    return std::unexpected{contained_error("startup", false)};
                } catch (...) {
                    return std::unexpected{contained_error("startup", true)};
                }
            }();
            if (!capability) {
                outcome.primary = runner_error(
                    "runtime.runner.transport_setup_failed",
                    "Runner control capability construction failed", "run_runner_internal_mode",
                    "control_capability_creation_failed", "startup", "control",
                    &capability.error());
            } else {
                try {
                    auto session_result = session.run(session.context, *capability, writer);
                    if (!session_result) {
                        outcome.primary = runner_error(
                            "runtime.runner.session_failed", "Runner session failed",
                            "run_runner_internal_mode", "session_failed", "session",
                            std::nullopt, &session_result.error());
                    }
                } catch (const std::exception&) {
                    outcome.primary = contained_error("session", false);
                } catch (...) {
                    outcome.primary = contained_error("session", true);
                }
            }
        }

        auto event_close = transport.event_writer.close();
        if (!event_close) {
            outcome.event_close = close_error(event_close.error(), "event",
                                              "event_close_failed");
        }
        auto control_close = transport.control_reader.close();
        if (!control_close) {
            outcome.control_close = close_error(control_close.error(), "control",
                                                "control_close_failed");
        }

        const core::Error* reported = nullptr;
        if (outcome.primary) {
            reported = &*outcome.primary;
        } else if (outcome.event_close) {
            reported = &*outcome.event_close;
        } else if (outcome.control_close) {
            reported = &*outcome.control_close;
        }
        if (reported != nullptr) {
            render_once(renderer, *reported);
            return 1;
        }
        return 0;
    } catch (const std::exception&) {
        const auto error = contained_error("boundary", false);
        render_once(renderer, error);
        return 1;
    } catch (...) {
        const auto error = contained_error("boundary", true);
        render_once(renderer, error);
        return 1;
    }
}

} // namespace detail

auto classify_runner_invocation(std::span<const std::string_view> arguments) noexcept
    -> RunnerInvocationKind {
    if (arguments.size() < 2 || arguments[1] != "__runner") {
        return RunnerInvocationKind::public_cli;
    }
    return arguments.size() == 2 ? RunnerInvocationKind::valid_runner
                                 : RunnerInvocationKind::invalid_runner;
}

auto run_runner_internal_mode(std::span<const std::string_view> arguments,
                              RunnerSession session) -> int {
    return detail::run_runner_internal_mode_with(
        arguments, session,
        detail::RunnerEndpointFactory{.context = nullptr, .adopt = detail::production_adopt},
        detail::RunnerControlCapabilityFactory{.context = nullptr,
                                               .create = detail::production_capability},
        detail::InternalErrorRenderer{.context = nullptr, .render = detail::production_render});
}

} // namespace epycbench::runtime
