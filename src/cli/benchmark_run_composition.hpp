#pragma once

#include "platform_linux/run_start_identity.hpp"
#include "results/process_subject_admission.hpp"
#include "results/filesystem_journal.hpp"
#include "results/final_document.hpp"
#include "results/native_lifecycle.hpp"
#include "results/preparation_observation.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include <iosfwd>

namespace epycbench::cli::benchmark_run_composition {
enum class Error { identity, output_policy, capacity, source, journal, execution, finalization };
struct Output {
    std::string journal_root, final_root, final_leaf;
};
struct Failure {
    Error kind{};
    std::optional<results::filesystem_journal::Failure> journal{};
    std::optional<results::final_document::Failure> final{};
};
struct RetainedReferences {
    std::uint64_t creation{},topology{},normalization{},planning{},preflight{};
};
struct FinalReport {
    std::optional<Failure> primary;
    results::filesystem_journal::Receipt writer;
    std::optional<results::filesystem_journal::Receipt> recovery;
    std::optional<results::filesystem_journal::Failure> recovery_failure;
    std::optional<results::final_document::Failure> final_failure;
    std::optional<results::final_document::Publication> publication;
};
struct PairReport {
    std::optional<runtime::detail::NativePairwiseFailure> preparation;
    std::optional<runtime::detail::NativePairwiseOutcome> execution;
    std::optional<Error> composition;
    std::optional<results::native_lifecycle::Error> journal;
    std::optional<results::producer_progress::ProgressFailure> progress_failure;
};
[[nodiscard]] auto execute_pair(results::filesystem_journal::Writer&,
    const results::journal::RunId&,const scenario::ScenarioDigestedPlan&,
    const results::execution_plan::Owner&,std::size_t pair_ordinal,RetainedReferences,
    const runtime::AdmittedPairwiseProductionInputs&,
    const runtime::detail::PairwiseHostCapacityRequestTranscript&,const runtime::RunnerContext&,
    runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
    runtime::ControllerLifecycleConfig,runtime::detail::NativePairwiseDependencies,
    results::block_evidence::ContextLimits,results::producer_progress::TimestampSource,
    results::producer_progress::Limits,core::JcsLimits) -> PairReport;
// Ordinary preparation only. The view is the actual retained resolved Scenario;
// returned strings grant no filesystem, producer or execution authority.
[[nodiscard]] auto prepare_output(const nlohmann::ordered_json&, std::string_view xdg_runtime,
    const platform_linux::detail::RunStartIdentity&) -> std::expected<Output,Error>;
[[nodiscard]] auto validate_identity(const results::process_subject::Session&,
    const platform_linux::detail::RunStartIdentity&) noexcept -> std::expected<void,Error>;
// Appends actual retained children into an empty progress-profile Writer. A failure leaves its
// already accepted prefix intact; there is no rollback or replay on that writer.
[[nodiscard]] auto append_retained(results::filesystem_journal::Writer&,
    const results::process_subject::Session&,const results::execution_plan::Owner&,
    const results::event_profile::Limits&) -> std::expected<RetainedReferences,Failure>;
// Binds already persisted observed preparation to the same original owners;
// never appends genesis, slices a terminal ledger or recreates preparation.
[[nodiscard]] auto prepared_references(const results::filesystem_journal::Writer&,
    const results::process_subject::Session&,const results::execution_plan::Owner&,
    const results::event_profile::Limits&) -> std::expected<RetainedReferences,Failure>;
[[nodiscard]] auto close_completed(results::filesystem_journal::Writer&,
    const results::journal::Timestamp&,results::preparation_observation::Adapter* = nullptr)
    -> std::expected<results::filesystem_journal::Receipt,Failure>;
[[nodiscard]] auto prepare_request_facts(const results::block_evidence::ExpectedPairContext&,
    const runtime::AdmittedPairwiseProductionInputs&,
    const runtime::detail::PairwiseHostCapacityRequestTranscript&,
    const runtime::wire_v1::detail::AuthoredCapacityOffer&,
    const results::execution_plan::Owner&,std::size_t pair_ordinal,
    RetainedReferences,const results::event_profile::Limits&,bool actual_preparation=false) -> std::expected<std::string,Error>;
// Called only after exact runner retirement. Uses the real C3 filesystem and C4
// immutable publication owners. Failed/uncertain files are never deleted here.
[[nodiscard]] auto finalize(results::filesystem_journal::Writer&,const Output&,
    const results::journal::RunId&,const results::final_document::FinalizationFacts&,
    results::filesystem_journal::Limits,results::final_document::Limits,
    std::optional<platform_linux::journal_file::Limits> publication_limits = std::nullopt) -> FinalReport;
struct RunLimits {
    std::size_t pair_reports{}, scenario_bytes{}, total_written_bytes{};
    results::filesystem_journal::Limits journal;
    results::final_document::Limits final;
    results::block_evidence::ContextLimits context;
    results::producer_progress::Limits progress;
    core::JcsLimits analysis;
};
struct FinalIdentityOperations {
    platform_linux::detail::ClockOperations clock;
    platform_linux::detail::EntropyOperations entropy;
};
struct RunDependencies {
    runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::LowerOperations capacity;
    runtime::detail::NativePairwiseDependencies controller;
    results::producer_progress::TimestampSource timestamp;
    // An explicit supplied table never falls back to native operations.
    std::optional<FinalIdentityOperations> final_identity;
};
enum class ExecutionDisposition { not_started, completed, failed, interrupted };
struct RunReport {
    std::array<char,36> run_id{};
    std::optional<results::filesystem_journal::Receipt> journal;
    std::optional<Failure> primary;
    std::vector<std::optional<PairReport>> pairs;
    std::optional<FinalReport> final;
    std::optional<platform_linux::detail::ImageFailure> image_failure;
    std::optional<platform_linux::detail::SignalScopeFailure> signal_failure;
    platform_linux::detail::SignalScopeClose signal;
    ExecutionDisposition execution{ExecutionDisposition::not_started};
    int exit_code{};
    bool secondary_failure{};
};
// Renders the bounded P2 summary completely before touching either sink.
// Returned status preserves an earlier primary if the selected sink fails.
[[nodiscard]] auto write_summary(const RunReport&,std::ostream& output,std::ostream& errors) noexcept -> int;
[[nodiscard]] auto execute(const results::process_subject::Session&,
    const results::execution_plan::Owner&,const scenario::ScenarioDigestedPlan&,
    const runtime::RunnerContext&,runtime::ControllerProcessProductionAuthority&,
    const platform_linux::detail::RunStartIdentity&,platform_linux::detail::ControllerSignalScope&,
    platform_linux::detail::CurrentProcessImage&,std::string_view xdg_runtime,
    RunLimits,RunDependencies) -> RunReport;
// Borrows the existing preparation Writer/observer until close. The caller keeps
// both objects at stable addresses; this consumes their use, not their ownership.
[[nodiscard]] auto execute(const results::process_subject::Session&,
    const results::execution_plan::Owner&,const scenario::ScenarioDigestedPlan&,
    const runtime::RunnerContext&,runtime::ControllerProcessProductionAuthority&,
    const platform_linux::detail::RunStartIdentity&,platform_linux::detail::ControllerSignalScope&,
    platform_linux::detail::CurrentProcessImage&,std::string_view xdg_runtime,
    RunLimits,RunDependencies,results::filesystem_journal::Writer&,
    results::preparation_observation::Adapter&) -> RunReport;

}
