#pragma once

#include "epycbench/monitoring/host_runner_context_session.hpp"

#include <optional>
#include "platform_linux/topology_source_io.hpp"

namespace epycbench::monitoring::qualification {

class detail_HostQualificationPlanAccess;

enum class TypedSysnameDisposition { linux, not_linux };

struct detail_HostObservedServiceCore final {
    topology::StableCoreId stable_core;
    std::vector<topology::StableProcessingUnitId> stable_pus;
    std::vector<core::LinuxCpuId> sibling_cpus;
};

enum class detail_HostObservationFailureKind {
    expected_error,
    contained_exception,
    contained_unknown_failure
};

struct detail_HostObservationFailure final {
    detail_HostObservationFailureKind kind;
    std::optional<core::Error> underlying;
};

enum class detail_HostObservationConstructionInjection {
    none,
    standard_exception,
    unknown_exception
};

class detail_HostRevalidationObservation final {
  public:
    [[nodiscard]] static auto from_context(runtime::RunnerContext) noexcept
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
    [[nodiscard]] static auto from_context_fallible(runtime::RunnerContext)
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
    [[nodiscard]] static auto from_context_for_testing(
        runtime::RunnerContext,detail_HostObservationConstructionInjection) noexcept
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
    [[nodiscard]] static auto create_for_testing(
        runtime::RunnerContext, core::CpuSet, core::NodeSet,
        std::vector<detail_HostObservedServiceCore>) noexcept
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
  private:
    [[nodiscard]] static auto from_context_impl(runtime::RunnerContext,
        detail_HostObservationConstructionInjection)
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
    detail_HostRevalidationObservation(runtime::RunnerContext context, core::CpuSet cpus,
      core::NodeSet nodes,std::vector<detail_HostObservedServiceCore> service) noexcept
      : context_{std::move(context)},effective_cpus_{std::move(cpus)},
        effective_nodes_{std::move(nodes)},service_{std::move(service)} {}
    runtime::RunnerContext context_;
    core::CpuSet effective_cpus_;
    core::NodeSet effective_nodes_;
    std::vector<detail_HostObservedServiceCore> service_;
    friend class RunnerContextAcquisitionSession;
    friend class detail_RunnerContextAcquisitionAccess;
};

class RunnerContextAcquisitionOperations {
  public:
    RunnerContextAcquisitionOperations() noexcept = default;
    [[nodiscard]] static auto
    create(void* state, void (*destroy)(void*) noexcept,
           std::expected<runtime::RunnerContext, core::Error> (*acquire_context)(void*),
           std::expected<TypedSysnameDisposition, core::Error> (*observe_platform)(void*),
           const kernel::KernelDescriptor* (*find_descriptor)(void*, kernel::KernelId),
           std::expected<detail_HostRevalidationObservation,
                         detail_HostObservationFailure> (*reobserve)(void*),
           std::expected<void, core::Error> (*before_registration)(void*) = nullptr) noexcept
        -> std::expected<RunnerContextAcquisitionOperations, core::Error>;
    RunnerContextAcquisitionOperations(const RunnerContextAcquisitionOperations&) = delete;
    RunnerContextAcquisitionOperations(RunnerContextAcquisitionOperations&& other) noexcept;
    auto operator=(const RunnerContextAcquisitionOperations&)
        -> RunnerContextAcquisitionOperations& = delete;
    auto operator=(RunnerContextAcquisitionOperations&&)
        -> RunnerContextAcquisitionOperations& = delete;
    ~RunnerContextAcquisitionOperations() noexcept;

  private:
    RunnerContextAcquisitionOperations(
        void* state, void (*destroy)(void*) noexcept,
        std::expected<runtime::RunnerContext, core::Error> (*acquire_context)(void*),
        std::expected<TypedSysnameDisposition, core::Error> (*observe_platform)(void*),
        const kernel::KernelDescriptor* (*find_descriptor)(void*, kernel::KernelId),
        std::expected<detail_HostRevalidationObservation,
                      detail_HostObservationFailure> (*reobserve)(void*),
        std::expected<void, core::Error> (*before_registration)(void*)) noexcept;
    void* state_{};
    void (*destroy_)(void*) noexcept {};
    std::expected<runtime::RunnerContext, core::Error> (*acquire_context_)(void*){};
    std::expected<TypedSysnameDisposition, core::Error> (*observe_platform_)(void*){};
    const kernel::KernelDescriptor* (*find_descriptor_)(void*, kernel::KernelId){};
    std::expected<detail_HostRevalidationObservation,
                  detail_HostObservationFailure> (*reobserve_)(void*){};
    std::expected<void, core::Error> (*before_registration_)(void*){};
    // Borrowed from the stable composition owner, which outlives these operations.
    // Operations never destroy or replace an already engaged primary failure.
    std::optional<core::Error>* raw_failure_sink_{};
    friend class detail_RunnerContextAcquisitionFactory;
    friend class detail_RunnerContextAcquisitionAccess;
    friend class RunnerContextAcquisitionSession;
    friend class detail_HostQualificationPlanAccess;
};

class detail_RunnerContextAcquisitionAccess {
  public:
    [[nodiscard]] static auto input_template_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<std::reference_wrapper<const MonitoringHostInputTemplateAuthority>,
                         core::Error>;
    [[nodiscard]] static auto runner_context_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<std::reference_wrapper<const runtime::RunnerContext>, core::Error>;
    [[nodiscard]] static auto prerequisites_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<std::reference_wrapper<const MonitoringHostPrerequisiteCapabilities>,
                         core::Error>;
    [[nodiscard]] static auto production_operations(std::optional<core::Error>* raw_failure = nullptr,
        platform_linux::detail::PreparedTopologySourceIO* prepared = nullptr,
        const std::uint64_t* original_main_invocation_end = nullptr)
        -> std::expected<RunnerContextAcquisitionOperations, core::Error>;
    // These delegates borrow the same complete operations owner. Exceptions pass
    // through so the enclosing original-main guard can observe the post-call mask.
    [[nodiscard]] static auto acquire_context(RunnerContextAcquisitionOperations&)
        -> std::expected<runtime::RunnerContext, core::Error>;
    [[nodiscard]] static auto observe_platform(RunnerContextAcquisitionOperations&)
        -> std::expected<TypedSysnameDisposition, core::Error>;
    [[nodiscard]] static auto find_descriptor(RunnerContextAcquisitionOperations&,
                                               kernel::KernelId)
        -> const kernel::KernelDescriptor*;
    [[nodiscard]] static auto reobserve(RunnerContextAcquisitionOperations&)
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>;
    [[nodiscard]] static auto before_registration(RunnerContextAcquisitionOperations&)
        -> std::expected<void, core::Error>;
    static void attach_raw_failure_sink(RunnerContextAcquisitionOperations&,
                                         std::optional<core::Error>*) noexcept;
    [[nodiscard]] static auto acquire_with(MonitoringHostInputTemplateAuthority input_template,
                                           RunnerContextAcquisitionOperations operations) noexcept
        -> std::expected<RunnerContextAcquisitionSession, core::Error>;
    [[nodiscard]] static auto acquire_with_fallible(MonitoringHostInputTemplateAuthority,
        RunnerContextAcquisitionOperations)
        -> std::expected<RunnerContextAcquisitionSession, core::Error>;
    [[nodiscard]] static auto revalidate_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<HostRevalidationDisposition, core::Error>;
};

} // namespace epycbench::monitoring::qualification
