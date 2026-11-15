#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "epycbench/runtime/runner_control_reader.hpp"

#include <expected>
#include <span>
#include <string_view>

namespace epycbench::runtime {

enum class RunnerInvocationKind {
    public_cli,
    valid_runner,
    invalid_runner,
};

[[nodiscard]] auto classify_runner_invocation(
    std::span<const std::string_view> arguments) noexcept -> RunnerInvocationKind;

struct RunnerSession {
    void* context;
    std::expected<void, core::Error> (*run)(void* context, RunnerControlReadCapability& control,
                                            ipc::ByteWriter& events);
};

[[nodiscard]] auto run_runner_internal_mode(
    std::span<const std::string_view> arguments, RunnerSession session) -> int;

} // namespace epycbench::runtime
