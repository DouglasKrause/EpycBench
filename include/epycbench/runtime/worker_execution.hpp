#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/core/id_set.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"
#include "epycbench/runtime/runner_control_monitor.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <vector>

namespace epycbench::runtime {

enum class WorkerObservationPolicy { none, before_and_after };
enum class WorkerCancellationPolicy { observe, cleanup_insensitive };

struct WorkerReadyEvidence {
    std::size_t worker_index;
    core::WorkerId worker_id;
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
    core::LinuxCpuId requested_cpu;
    platform_linux::LinuxThreadId tid;
    core::CpuSet observed_affinity;
    core::LinuxCpuId observed_cpu;
};

struct WorkerExecutionContext {
    std::size_t worker_index;
    core::WorkerId worker_id;
    core::ProcessingUnitId processing_unit;
    core::CoreId core;
    core::LinuxCpuId requested_cpu;
    const WorkerReadyEvidence& readiness;
};

struct WorkerExecutionControl {
    RunnerCancellation& runner_cancellation;
    std::stop_token team_stop;
    std::stop_token command_stop;

    void signal_timed_completion() const noexcept {
        if (signal_timed_completion_ != nullptr)
            signal_timed_completion_(timed_completion_context_);
    }

    using TimedCompletionFunction = void (*)(void*) noexcept;

    WorkerExecutionControl(RunnerCancellation& cancellation_value, std::stop_token team_token,
                           std::stop_token command_token, void* timed_context = nullptr,
                           TimedCompletionFunction signal_function = nullptr) noexcept
        : runner_cancellation{cancellation_value}, team_stop{team_token},
          command_stop{command_token}, timed_completion_context_{timed_context},
          signal_timed_completion_{signal_function} {}

  private:
    void* timed_completion_context_{};
    TimedCompletionFunction signal_timed_completion_{};
};

struct WorkerOperation {
    void* context{};
    std::expected<void, core::Error> (*run)(void*, const WorkerExecutionContext&,
                                            const WorkerExecutionControl&){};
};

struct WorkerCommandResult {
    core::WorkerId worker_id;
    std::expected<void, core::Error> result;
    std::optional<core::LinuxCpuId> cpu_before;
    std::optional<core::LinuxCpuId> cpu_after;
    bool migrated{};
    bool cancellation_observed{};
};

struct WorkerTeamCommandOutcome {
    std::vector<WorkerCommandResult> workers;

    [[nodiscard]] auto succeeded() const noexcept -> bool;
    [[nodiscard]] auto cancelled() const noexcept -> bool;
};

} // namespace epycbench::runtime
