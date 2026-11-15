#pragma once

#include "runtime/finalized_evidence_wire_startup_codec.hpp"
#include "runtime/pairwise_v2_native_lifecycle.hpp"
#include "runtime/runner_internal_mode_access.hpp"

namespace epycbench::runtime::detail {
enum class StageBEntryFailure : unsigned char { identity, overflow, allocation };
// Deterministic evidence binding, never an authentication or spawn capability.
[[nodiscard]] auto stage_b_startup_session(std::string_view run_uuid,
    std::uint64_t canonical_pair_ordinal) noexcept
    -> std::expected<wire_v1::detail::StartupSessionId, StageBEntryFailure>;
[[nodiscard]] auto stage_b_logical_child_token(std::uint64_t canonical_pair_ordinal) noexcept
    -> std::expected<std::uint64_t, StageBEntryFailure>;
struct StageBRunnerEntryDependencies final {
    RunnerEndpointFactory endpoints;
    RunnerContextProvider runner_context;
    ControllerClock clock;
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations authority;
    void* backend_context{};
    NativePreparedProductionDependencies (*backend)(void*){};
};
struct StageBRunnerEntryOutcome final {
    int exit_code{1};
    std::optional<NativePairwiseFailure> primary;
    bool cleanup_failure{};
};
[[nodiscard]] auto run_stage_b_runner_with(std::span<const std::string_view>,
    StageBRunnerEntryDependencies) noexcept -> StageBRunnerEntryOutcome;
// Compiled native factory selection. Tests call only the injected entry above.
[[nodiscard]] auto run_stage_b_runner() noexcept -> int;
} // namespace epycbench::runtime::detail
