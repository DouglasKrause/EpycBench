#pragma once

#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_frozen_process_control.hpp"
#include "monitoring/host_frozen_source_admission.hpp"
#include "monitoring/host_frozen_evidence.hpp"

namespace epycbench::monitoring::qualification::detail {
// The same finite pre-preparation QID decision used by the production
// governed entry. Tests exercise its accepted and refused boundary directly.
[[nodiscard]] auto host_frozen_source_governed_entry_failure(bool invalid_clock,
    bool invalid_root, bool arguments_owned, std::string_view qualification_id) noexcept
    -> std::optional<HostFrozenSourceLaunchReason>;
template <typename Refuse, typename Proceed>
auto dispatch_host_frozen_source_governed_entry(bool invalid_clock, bool invalid_root,
    bool arguments_owned, std::string_view qualification_id, Refuse&& refuse,
    Proceed&& proceed) -> bool {
    if (const auto failure = host_frozen_source_governed_entry_failure(
            invalid_clock, invalid_root, arguments_owned, qualification_id)) {
        refuse(*failure);
        return false;
    }
    return proceed();
}
// The collector's fixed retirement-only diagnostic precedes its ordinary tag.
// It carries no paths, messages, owners, or successful authority.
[[nodiscard]] auto host_frozen_source_retirement_tag(const MonitoringQ4AuthorityFailure&) noexcept
    -> std::array<char, 50>;
// Final-recheck scalar primary/detail facts precede the parent's 50-byte cut.
// Retirement keeps its original tag; no error text or input is projected.
[[nodiscard]] auto host_frozen_source_authority_tag(const MonitoringQ4AuthorityFailure&) noexcept
    -> std::array<char, 50>;
// Headroom's already-owned primary and ordered cleanup fit before the same cut.
[[nodiscard]] auto host_frozen_source_headroom_tag(const platform_linux::host_work::Failure&,
    unsigned preparation_step, HostFrozenSourceLaunchReason outer_primary) noexcept
    -> std::array<char, 50>;
// Admission's first binding rejection and optional existing lower code precede
// the same parent cut. This projects quiescent facts, never authority or text.
[[nodiscard]] auto host_frozen_source_admission_tag(HostFrozenAdmissionReason,
    HostFrozenBindingCheckpoint, const core::Error*, unsigned preparation_step,
    HostFrozenSourceLaunchReason outer_primary) noexcept -> std::array<char, 50>;
// Executable-local dependencies borrow one fixture stock which outlives the
// actual State, Session, Control thread, payloads and confirmed retirement.
// The production entry never accepts this table or selects it from argv/env.
// Admission still authenticates the actual private repository and Build; only
// host leaves and evidence domain are explicitly synthetic.
struct HostFrozenSourceSyntheticDispatcher final {
    void* context{};
    MonitoringRunningBuildAuthority::Operations build;
    std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> (*admit)(void*,
        const HostFrozenSourceLaunchBinding&, const std::filesystem::path&, std::string_view,
        const HostFrozenSourceRetainedInput*) noexcept{};
    std::expected<FrozenSourceControlOwner, FrozenSourceControlReason> (*acquire)(void*,
        HostFrozenSourceAdmission, MonitoringHostInputTemplateAuthority,
        runtime::ProductionRequestIdSource&, const HostFrozenSourceLaunchBinding&,
        const FrozenProcessRecord&, platform_linux::host_work::ResourceLedger&,
        runtime::RunnerCancellation&) noexcept{};
    std::expected<void, FrozenSourceControlReason> (*start)(void*, FrozenSourceControlOwner&) noexcept{};
    std::expected<HostFrozenCellEvidence, HostFrozenEvidenceFailure> (*cell)(void*,
        const HostFrozenCell&, const MonitoringQ1ValidatedPublicValues&,
        const MonitoringRunningBuildAuthority&, std::size_t) noexcept{};
    std::expected<HostFrozenTerminationEvidence, HostFrozenEvidenceFailure> (*termination)(void*,
        const HostFrozenCollection&, const HostFrozenCollectionFailure*,
        const MonitoringQ1ValidatedPublicValues&, const MonitoringRunningBuildAuthority&,
        std::size_t) noexcept{};
};
[[nodiscard]] auto run_host_frozen_synthetic_collector(int, char*[],
    runtime::ControllerProcessProductionAuthority&, runtime::RunnerCancellation&,
    HostFrozenSourceLaunchClock&, std::uint64_t, const HostFrozenSourceSyntheticDispatcher&) noexcept -> int;
[[nodiscard]] auto run_host_frozen_synthetic_outer(int, char*[],
    runtime::ControllerProcessProductionAuthority&, runtime::RunnerCancellation&,
    HostFrozenSourceLaunchClock&, std::uint64_t, const HostFrozenSourceSyntheticDispatcher&,
    const char* fixture_setup, bool inject_cleanup_wait_failure = false) noexcept -> int;
// Direct observation of the same private dispatcher report/retirement body.
// Injected IO owns only scripted descriptor facts; this yields no invocation,
// producer or publication capability and is never selected by argv/environment.
struct HostFrozenSourceGovernedReportTestObservation final {
    bool restoration{}, report_eof{}, retired{}, cancelled{}, selected_cancel{};
    std::optional<HostFrozenSourceBindingFact> primary;
    std::size_t secondary_count{};
    std::optional<std::uint64_t> restored_at_ns, elapsed_ns, exposure_ns;
    bool timing_projected{};
};
[[nodiscard]] auto observe_host_frozen_source_governed_report_for_test(
    const HostFrozenSourceGovernedHandoff&, runtime::RunnerCancellation&, HostFrozenSourceLaunchClock&,
    platform_linux::detail::TopologySourceOperations,
    platform_linux::host_work::SourceLaunchStreamOperations,
    platform_linux::host_work::GuardOperations, std::uint64_t original_observation) noexcept
    -> HostFrozenSourceGovernedReportTestObservation;
#if defined(EPYCBENCH_HOST_FROZEN_COLLECTION_TEST)
struct HostFrozenSourceLaunchDispatcherTestAccess final {
    [[nodiscard]] static auto collect(int argc, char* argv[],
        runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& token,
        HostFrozenSourceLaunchClock& clock, std::uint64_t main_origin,
        const HostFrozenSourceSyntheticDispatcher& dependencies) noexcept -> int {
        return run_host_frozen_synthetic_collector(argc, argv, authority, token, clock,
                                                  main_origin, dependencies);
    }
    [[nodiscard]] static auto invoke(int argc, char* argv[],
        runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& token,
        HostFrozenSourceLaunchClock& clock, std::uint64_t main_origin,
        const HostFrozenSourceSyntheticDispatcher& dependencies, const char* fixture_setup,
        bool inject_cleanup_wait_failure = false) noexcept -> int {
        return run_host_frozen_synthetic_outer(argc, argv, authority, token, clock,
            main_origin, dependencies, fixture_setup, inject_cleanup_wait_failure);
    }
};
#endif
} // namespace epycbench::monitoring::qualification::detail
