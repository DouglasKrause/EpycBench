#pragma once

#include "epycbench/runtime/runner_control_reader.hpp"
#include "epycbench/runtime/runner_execution_backend.hpp"
#include "epycbench/runtime/runner_internal_mode.hpp"

#include <expected>
#include <memory>

namespace epycbench::runtime {

struct RunnerControlMonitorFactory {
    void* context;
    // Invoked only after contextual admission. Ownership of the already-advanced control state
    // transfers exactly once to the bound monitor implementation.
    std::expected<RunnerControlMonitor, core::Error> (*create)(
        void*, const AdmittedPlacementRequest&, std::unique_ptr<ipc::RunnerControlState>,
        RunnerControlReadCapability&, RunnerCancellation&);
};

class RunnerStateMachine {
  public:
    [[nodiscard]] static auto create(RunnerContextProvider context_provider,
                                     RunnerControlMonitorFactory monitor_factory,
                                     RunnerExecutionBackend execution_backend) noexcept
        -> std::expected<RunnerStateMachine, core::Error>;

    RunnerStateMachine(RunnerStateMachine&& other) noexcept;
    RunnerStateMachine(const RunnerStateMachine&) = delete;
    auto operator=(const RunnerStateMachine&) -> RunnerStateMachine& = delete;
    auto operator=(RunnerStateMachine&&) -> RunnerStateMachine& = delete;
    ~RunnerStateMachine() noexcept = default;

    [[nodiscard]] auto run(RunnerControlReadCapability& control, ipc::ByteWriter& events)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto session() noexcept -> RunnerSession;

  private:
    enum class State { created, running, completed, failed, moved_from };

    RunnerStateMachine(RunnerContextProvider context_provider,
                       RunnerControlMonitorFactory monitor_factory,
                       RunnerExecutionBackend execution_backend) noexcept
        : context_provider_{context_provider}, monitor_factory_{monitor_factory},
          execution_backend_{execution_backend} {}

    RunnerContextProvider context_provider_;
    RunnerControlMonitorFactory monitor_factory_;
    RunnerExecutionBackend execution_backend_;
    State state_{State::created};
};

} // namespace epycbench::runtime
