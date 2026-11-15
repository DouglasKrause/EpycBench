#pragma once

#include "cli/benchmark_run_composition.hpp"
#include "platform_linux/benchmark_run_input.hpp"
#include "results/preparation_observation.hpp"

namespace epycbench::cli::benchmark_run_command {
enum class Error { capacity };
enum class ArgumentError { usage };
struct Arguments {
    std::string_view scenario_path;
    scenario::ScenarioFormat format{scenario::ScenarioFormat::yaml};
    bool help{};
    bool demo{};
};
// Arguments after the literal `run`; all views borrow the original argument
// storage. Parsing performs no allocation, acquisition or environment access.
[[nodiscard]] auto parse_arguments(std::span<const std::string_view>) noexcept
    -> std::expected<Arguments, ArgumentError>;
// The built-in demo changes only source acquisition, not Run admission/execution.
[[nodiscard]] constexpr auto demo_arguments() noexcept -> Arguments {
    return {"builtin-demo.yaml",scenario::ScenarioFormat::yaml,false,true};
}
[[nodiscard]] auto read_scenario(const Arguments&,
    platform_linux::benchmark_run_input::FileOperations,std::string_view demo_directory={})
    -> platform_linux::benchmark_run_input::TextRead;
[[nodiscard]] auto run_demo(runtime::ControllerProcessProductionAuthority&,
    std::ostream&,std::ostream&) noexcept -> int;
enum class DependencyMode { native, supplied };
struct SuppliedKernelPolicy {
    std::string_view root;
    // Explicit bandwidth, latency order; not the numeric order of Family.
    std::array<core::Sha256Digest,2> approval_digests;
    // Explicit supplied route; omitted preserves existing legacy fixtures.
    results::kernel_approval::ReviewRoute review_route{results::kernel_approval::ReviewRoute::human_approval};
};
enum class KernelInputError { admission, unconfigured, acquisition, digest, owner };
struct KernelSelection {
    std::string_view root;
    std::array<char,64> directory;
    core::Sha256Digest expected;
    DependencyMode mode;
    results::kernel_approval::ReviewRoute review_route;
};
// Historical supplied evidence only; returned views borrow the supplied policy.
// Native runs do not select or acquire review manifests.
[[nodiscard]] auto select_kernel_evidence(DependencyMode,const SuppliedKernelPolicy*,
    results::kernel_report::Family,const platform_linux::benchmark_run_input::FileOperations&)
    -> std::expected<KernelSelection,KernelInputError>;
struct KernelEvidence {
    std::array<platform_linux::benchmark_run_input::TextRead,5> files;
    std::array<std::optional<results::kernel_report::Owner>,2> reports;
    std::optional<results::kernel_approval::Owner> approval;
    std::optional<KernelInputError> failure;
    std::optional<core::Error> producer_error;
    std::array<core::Error,4> cleanup_errors;
    std::size_t cleanup_count{};
    std::size_t source_bytes{};
    bool cancelled{};
};
// Ordinary preparation policy only; these values do not grant producer authority.
struct ProducerLimits {
    results::final_document::Limits serialization;
    scenario::ScenarioResolutionLimits resolution;
    results::authored_source::Limits source;
    results::build_manifest::Limits build;
    results::host_assembly::Limits host;
    results::kernel_report::Limits report;
    results::kernel_approval::Limits approval;
    results::implementation_selection::Limits selection;
    results::build_object_association::Limits association;
    results::execution_plan::Limits plan;
    results::process_subject::Limits session;
};
// Existing parent phase equation I+O+T. The caller supplies actual already-admitted
// input stock; configured child reservations remain separate from payload bytes.
[[nodiscard]] auto preparation_phase(std::size_t input, std::size_t output,
    std::size_t fixed, std::size_t rows, std::size_t cell,
    std::span<const std::size_t> child_reservations)
    -> std::expected<std::size_t, Error>;
// Same closed input ceiling used by the factory. Checks happen before callers
// invoke a parent; this is logical producer input, not total process storage.
[[nodiscard]] auto producer_input_limit() -> std::expected<std::size_t,Error>;
[[nodiscard]] auto preparation_inputs(std::span<const std::size_t>,std::size_t ceiling)
    -> std::expected<std::size_t,Error>;
[[nodiscard]] auto producer_limits(std::size_t live_input_bytes = 0)
    -> std::expected<ProducerLimits, Error>;
// Closed ordinary caller limits, derived from the same accepted serialization
// profile and the existing C3 storage equations; not an allocator/RSS budget.
[[nodiscard]] auto run_limits() -> std::expected<benchmark_run_composition::RunLimits,Error>;
// Parses before selecting any native acquisition. Tests invoke only help/usage;
// actual command execution is exercised through execute with supplied operations.
[[nodiscard]] auto run(int,char*[],runtime::ControllerProcessProductionAuthority&,
    std::ostream&,std::ostream&) noexcept -> int;
// Maps this image's generated build fields directly into the existing owner.
[[nodiscard]] auto retain_current_build(results::build_manifest::Limits)
    -> std::expected<results::build_manifest::Owner,core::Error>;
struct HostInputOperations {
    platform_linux::benchmark_run_input::FileOperations files;
    platform_linux::benchmark_run_input::HostOperations host;
    void* clock_context{};
    std::expected<results::journal::Timestamp,platform_linux::benchmark_run_input::Failure>
        (*clock)(void*){};
    platform_linux::detail::ControllerSignalScope* cancellation{};
};
struct HostEvidence {
    results::host_assembly::Categories categories;
    std::vector<results::process_subject::FailureFact> failures;
    std::vector<results::run_creation::Binding> bindings;
    std::optional<core::Error> retention_failure;
    std::size_t source_bytes{};
    bool cancelled{};
};
// Existing owning children and their creation bindings; no acceptance capability.
[[nodiscard]] auto acquire_host(const results::journal::RunId&,const runtime::RunnerContext&,
    HostInputOperations,const ProducerLimits&) -> HostEvidence;
struct SessionPreparation {
    std::optional<results::process_subject::Session> session;
    // Retains the original input if Session admission refuses it.
    std::optional<results::run_creation::Input> uncommitted_creation;
    std::optional<core::Error> failure;
    std::size_t source_bytes{};
    bool cancelled{};
};
[[nodiscard]] auto prepare_session(const results::journal::RunId&,const results::journal::Timestamp&,
    results::authored_source::SourceBundle&,const runtime::RunnerContext&,HostInputOperations,
    const ProducerLimits&) -> SessionPreparation;
// All original bytes and failed lower dispositions survive. The shared debit is
// charged before the next read and spans both families; no failed acquisition is
// replaced with a deliberately malformed retainer input.
[[nodiscard]] auto acquire_kernel_evidence(DependencyMode,const SuppliedKernelPolicy*,
    results::kernel_report::Family,const platform_linux::benchmark_run_input::FileOperations&,
    const ProducerLimits&,std::size_t& aggregate_source_bytes,
    platform_linux::detail::ControllerSignalScope* cancellation=nullptr) -> KernelEvidence;
[[nodiscard]] auto acquire_kernel_evidence(results::preparation_observation::Adapter&,
    const results::journal::Timestamp&,KernelEvidence&,DependencyMode,const SuppliedKernelPolicy*,
    results::kernel_report::Family,const platform_linux::benchmark_run_input::FileOperations&,
    const ProducerLimits&,std::size_t& aggregate_source_bytes,
    platform_linux::detail::ControllerSignalScope* cancellation=nullptr)
    -> std::expected<void,results::preparation_observation::Error>;
struct ControllerCheckOperations {
    platform_linux::LinuxThreadId thread;
    std::size_t page_bytes{};
    std::span<const core::LinuxCpuId> present{},selected{};
    platform_linux::detail::BoundedTaskStatOperations page_faults{};
    platform_linux::detail::BoundedPerfContextSwitchOperations context_switches{};
    platform_linux::detail::BoundedSysfsOperations temperature{},frequency{};
    void* clock_context{};
    std::expected<results::journal::Timestamp,platform_linux::benchmark_run_input::Failure>
        (*clock)(void*){};
    std::size_t source_text_bytes{16U*1024U*1024U};
    platform_linux::detail::ControllerSignalScope* cancellation{};
};
using ControllerObservation=std::variant<std::monostate,
    platform_linux::benchmark_run_input::PageFaultRead,
    platform_linux::benchmark_run_input::PerfCapability,
    platform_linux::benchmark_run_input::TemperatureRead,
    platform_linux::benchmark_run_input::FrequencyRead>;
struct ControllerChecks {
    ControllerChecks() = default;
    ControllerChecks(ControllerChecks&&) noexcept = default;
    auto operator=(ControllerChecks&&) noexcept -> ControllerChecks&;
    ControllerChecks(const ControllerChecks&) = delete;
    auto operator=(const ControllerChecks&) -> ControllerChecks& = delete;
    ~ControllerChecks();
    std::array<ControllerObservation,4> observations;
    std::array<std::optional<results::journal::Timestamp>,4> observed_at;
    // Fixed stock before acquisition. Entries [0,frequency_attempts) associate
    // exactly with FrequencyRead::domains in actual attempt order. A failed read
    // or clock leaves its timestamp absent; earlier entries remain unchanged.
    std::array<std::optional<results::journal::Timestamp>,1024> frequency_observed_at;
    std::size_t frequency_attempts{};
    std::optional<monitoring::MonitoringChannel> failed_channel;
    platform_linux::benchmark_run_input::Outcome admission;
    std::optional<results::validation_event::Owner> assessment;
    std::optional<core::Error> assessment_error;
    std::size_t source_bytes{};
    bool cancelled{};
    nlohmann::json snapshot;
    std::vector<core::Error> captured_errors;
    std::size_t retained_bytes{};
};
// Actual controller-source checks only. Neither this result nor its existing
// assessment owner establishes worker readiness or hardware qualification.
[[nodiscard]] auto controller_checks(const results::journal::RunId&,std::uint64_t ordinal,
    const monitoring::MonitoringExecutionPolicy&,const ControllerCheckOperations&,
    results::validation_event::Limits) -> ControllerChecks;
[[nodiscard]] auto capture_controller_checks(ControllerChecks&,const ProducerLimits&)
    -> results::preparation_observation::CheckCallResult;
struct CommandDependencies {
    DependencyMode mode;
    const SuppliedKernelPolicy* kernel;
    HostInputOperations host;
    ControllerCheckOperations checks;
    results::preparation_observation::TimestampSource availability;
    benchmark_run_composition::RunDependencies run;
};
// The caller owns the admitted context, identity and process resources. Every
// preparation child and the Stage-B handoff use these same owners.
[[nodiscard]] auto execute(const Arguments&,const runtime::RunnerContext&,
    runtime::ControllerProcessProductionAuthority&,const platform_linux::detail::RunStartIdentity&,
    platform_linux::detail::ControllerSignalScope&,platform_linux::detail::CurrentProcessImage&,
    std::string_view xdg_runtime,benchmark_run_composition::RunLimits,CommandDependencies)
    -> benchmark_run_composition::RunReport;
}
