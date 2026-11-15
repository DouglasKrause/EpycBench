#pragma once
#include "monitoring/host_frozen_process_control.hpp"
#include "monitoring/host_runner_context_session_access.hpp"

namespace epycbench::monitoring::qualification::detail {
struct FrozenSourceSessionStockProbe final {
    bool identity{}, projection{}, immutable_first{}, control_stocks{}, composition_stocks{},
        charged{};
};
struct FrozenSourceLinuxIOProbe final {
    bool unpublished_hidden{}, pending_hidden{}, executing_hidden{}, published{}, immutable{}, charged{};
    std::optional<platform_linux::detail::TopologySourceFailure> primary;
    std::array<platform_linux::detail::TopologySourceCloseFact, 6> cleanup{};
    std::size_t cleanup_count{};
};
enum class FrozenSourceInitialCallProbeMode : unsigned char {
    pre_expired, pre_cancelled, timely_core, timely_core_mask, late_core,
    late_standard, late_unknown, late_core_standard, late_core_unknown,
    timely_standard, timely_unknown, late_success
};
struct FrozenSourceInitialCallProbe final {
    bool provider_called{}, guard_before_provider{}, primary_guard{}, lower_primary{}, lower_late{};
    bool actual_identity{}, actual_errno{}, standard{}, unknown{}, immutable{}, charged{};
    unsigned mask_checks{};
    std::optional<FrozenSourceControlReason> reason;
};
enum class FrozenSourceRegistrationProbeMode : unsigned char {
    cancel_before_hook,
    expire_before_hook,
    cancel_after_hook,
    expire_after_hook,
    core_projection_denial,
    post_mask_refusal
};
struct FrozenSourceRegistrationProbe final {
    unsigned hook_calls{}, mask_checks{}, guard_checks{};
    bool nested{}, not_committed{}, original_token{}, original_end{}, distinct_stocks{};
    bool outer_guard{}, lower_guard{}, actual_identity{}, actual_errno{}, lower_primary{},
        lower_late{};
    bool immutable{}, charged{}, projection_denied{};
    std::optional<FrozenSourceControlReason> reason;
};
struct FrozenSourceRawOrderProbe final {
    bool unpublished_hidden{}, pending_hidden{}, primary{}, secondary{}, immutable{}, charged{};
};
[[nodiscard]] auto probe_frozen_source_raw_failure_order(bool timeout_first, core::Error,
    platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
    void*, void (*)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceRawOrderProbe, FrozenSourceControlReason>;
// Pure actual-State retention probe. No affinity, native owner or capability is acquired.
[[nodiscard]] auto probe_frozen_source_session_stocks(const RunnerContextAcquisitionSession&,
    bool unknown, platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
    void*, void (*)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceSessionStockProbe, FrozenSourceControlReason>;
}

#if defined(EPYCBENCH_HOST_FROZEN_COLLECTION_TEST)
namespace epycbench::monitoring::qualification::detail {
// Explicit synthetic native ownership probe. It forwards real main/control
// preparation and mailbox mechanics, but cannot prepare a source Collection,
// activate a Run or establish producer/stage qualification.
class FrozenSourceControlTestAccess final {
  public:
    [[nodiscard]] static auto collection_inputs(const FrozenSourceControlOwner& owner) noexcept {
        return owner.collection_inputs_for_testing();
    }
    [[nodiscard]] static auto acquire_collection(HostFrozenSourceAdmission admission,
        MonitoringHostInputTemplateAuthority input, RunnerContextAcquisitionOperations operations,
        runtime::ProductionRequestIdSource& ids, const HostFrozenSourceLaunchBinding& binding,
        std::uint8_t phase, std::size_t workers, const FrozenProcessRecord& grant,
        platform_linux::host_work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
        std::size_t capacity) noexcept {
        return FrozenSourceControlOwner::acquire_impl(std::move(input),
            std::optional{std::move(operations)}, phase, workers, grant, ledger, cancellation,
            capacity, nullptr, std::optional<HostFrozenSourceAdmission>{std::move(admission)},
            &ids, &binding);
    }
    [[nodiscard]] static auto start_collection(FrozenSourceControlOwner& owner,
        std::span<const HostFrozenCollectionTestAccess::Endpoint> endpoints,
        HostFrozenCollectionDependencies dependencies, core::LinuxCpuId actual_allowed_control_cpu) noexcept {
        return owner.start_owned_synthetic_for_testing(endpoints, std::move(dependencies),
                                                      actual_allowed_control_cpu);
    }
    // Pure actual Session registration inside the State's nested composition
    // guard; injected leaves acquire no native/source execution authority.
    [[nodiscard]] static auto registration(FrozenSourceRegistrationProbeMode mode,
        MonitoringHostInputTemplateAuthority input, RunnerContextAcquisitionOperations operations,
        const void* expected_context, platform_linux::host_work::ResourceLedger& ledger,
        runtime::RunnerCancellation& cancellation, void* context,
        void (*denial)(void*, bool) noexcept) noexcept {
        return FrozenSourceControlOwner::registration_for_testing(mode, std::move(input),
            std::move(operations), expected_context, ledger, cancellation, context, denial);
    }
    // Pure actual-State initial-call decision with synthetic clock/mask leaves;
    // production uses this same body with its actual token, RAW clock and mask.
    [[nodiscard]] static auto initial_call(FrozenSourceInitialCallProbeMode mode, core::Error actual,
        platform_linux::host_work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
        void* context, void (*denial)(void*, bool) noexcept) noexcept {
        return FrozenSourceControlOwner::initial_call_for_testing(mode, std::move(actual), ledger,
                                                                 cancellation, context, denial);
    }
    // Pure actual-State ownership/publication probe. Its post-main result is
    // synthetic; it provides no native placement or production authority.
    [[nodiscard]] static auto linux_io_publication(platform_linux::detail::PreparedTopologySourceIO owner,
        platform_linux::host_work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
        void* context, void (*invoke)(void*, platform_linux::detail::PreparedTopologySourceIO&, std::optional<core::Error>&),
        bool (*post_main)(void*) noexcept, void (*denial)(void*, bool) noexcept) noexcept {
        return FrozenSourceControlOwner::linux_io_publication_for_testing(std::move(owner), ledger, cancellation,
                                                                        context, invoke, post_main, denial);
    }
    [[nodiscard]] static auto raw_failure_order(bool timeout_first, core::Error actual,
        platform_linux::host_work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
        void* context, void (*denial)(void*, bool) noexcept) noexcept {
        return probe_frozen_source_raw_failure_order(timeout_first, std::move(actual), ledger,
                                                    cancellation, context, denial);
    }
    [[nodiscard]] static auto session_stocks(const RunnerContextAcquisitionSession& session,
        bool unknown, platform_linux::host_work::ResourceLedger& ledger,
        runtime::RunnerCancellation& cancellation, void* context,
        void (*denial)(void*, bool) noexcept) noexcept {
        return probe_frozen_source_session_stocks(session, unknown, ledger, cancellation, context, denial);
    }
    [[nodiscard]] static auto acquire_with_operations(MonitoringHostInputTemplateAuthority input,
        RunnerContextAcquisitionOperations operations, std::uint8_t phase, std::size_t workers,
        const FrozenProcessRecord& grant, platform_linux::host_work::ResourceLedger& ledger,
        runtime::RunnerCancellation& cancellation, std::size_t capacity) noexcept {
        return FrozenSourceControlOwner::acquire_impl(std::move(input),
            std::optional{std::move(operations)}, phase, workers, grant, ledger, cancellation, capacity);
    }
    [[nodiscard]] static auto enqueue_probe(FrozenSourceControlOwner& owner,
        const FrozenProcessRecord& permit) noexcept { return owner.enqueue_probe(permit); }
    [[nodiscard]] static auto probe_result(const FrozenSourceControlOwner& owner) noexcept {
        return owner.probe_result();
    }
    [[nodiscard]] static auto source_refused(const FrozenSourceControlOwner& owner) noexcept {
        return owner.source_refused();
    }
    // Borrow only before this synthetic owner's control/inputs exist. The
    // original-main provider may retain a cause; production code publishes it.
    [[nodiscard]] static auto bind_main_failure_sink(FrozenSourceControlOwner& owner,
        std::optional<core::Error>** destination) noexcept {
        return owner.bind_main_failure_sink_for_testing(destination);
    }
    [[nodiscard]] static auto prepare_state_storage(platform_linux::host_work::ResourceLedger& ledger,
        runtime::RunnerCancellation& cancellation) noexcept {
        return FrozenSourceControlOwner::prepare_state_storage_for_testing(ledger, cancellation);
    }
    // Pure result-boundary injection into the actual State's retention paths;
    // no native call, identity, source capability, Session or Run is acquired.
    [[nodiscard]] static auto retain_failure_facts(platform_linux::host_work::ResourceLedger& ledger,
        runtime::RunnerCancellation& cancellation, FrozenSourceControlFailureProbeFirst first,
        void* context, void (*allocation_denial)(void*, bool) noexcept) noexcept {
        return FrozenSourceControlOwner::retain_failure_facts_for_testing(
            ledger, cancellation, first, context, allocation_denial);
    }
};
} // namespace epycbench::monitoring::qualification::detail
#endif
