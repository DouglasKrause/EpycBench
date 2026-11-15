#pragma once

#include "epycbench/runtime/admitted_placement_request.hpp"
#include "epycbench/runtime/runner_control_monitor.hpp"

#include <expected>
#include <variant>

namespace epycbench::runtime {

struct RunnerPlacementSucceeded { ipc::PlacementProof proof; };
struct RunnerPlacementCancelled { ipc::RunnerPhase phase; };
struct RunnerPlacementFailed { core::Error error; };
using RunnerExecutionOutcome =
    std::variant<RunnerPlacementSucceeded, RunnerPlacementCancelled, RunnerPlacementFailed>;

struct RunnerExecutionBackend {
    void* context;
    // The callback is synchronous. It must not retain either borrowed argument and has no
    // control/event transport authority.
    std::expected<RunnerExecutionOutcome, core::Error> (*execute)(
        void*, const AdmittedPlacementRequest&, RunnerCancellation&);
};

} // namespace epycbench::runtime
