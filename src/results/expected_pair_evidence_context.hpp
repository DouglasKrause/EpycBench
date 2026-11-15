#pragma once

#include "epycbench/results/journal_records.hpp"
#include "results/process_subject_facts.hpp"
#include "epycbench/runtime/admitted_placement_request.hpp"
#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"
#include "runtime/finalized_evidence_wire_capacity.hpp"

#include <memory>
#include <variant>
#include "epycbench/runtime/production_admission.hpp"

namespace epycbench::results::event_profile::detail { struct PairContextDecoder; }
namespace epycbench::results::block_evidence {
class Component;
class Accumulator;
class PairPrefix;
enum class ErrorCode : std::uint8_t {
    capacity, identity, malformed, state, sequence, association, reconstruction,
    projection, allocation_failure, diagnostic_overflow
};
enum class Phase : std::uint8_t { context, parse, reconstruct, project, event, handoff };
struct ComponentDiagnostic final {
    std::array<char, 512> identity{};
    std::size_t identity_size{};
    std::array<std::uint64_t, 4> ordinals{};
    std::size_t ordinal_count{};
};
struct Error final {
    ErrorCode code;
    Phase phase;
    std::size_t limit{}, observed{};
    std::uint8_t detail_code{};
    ComponentDiagnostic diagnostic{};
};
struct ContextLimits final {
    std::size_t scenario_bytes, resolved_plan_bytes, nodes, strings, depth, workers;
    std::size_t capacity_elements, identity_bytes, retained_bytes, simultaneous_bytes;
};
struct ContextUsage final {
    std::size_t retained_bytes{}, simultaneous_bytes{}, identity_bytes{}, capacity_elements{};
};
struct ExpectedMemoryCell final {
    runtime::ipc::PlacementRegionRequest request;
    std::vector<core::NumaNodeId> resolved_nodes;
};
struct ExpectedMonitoringPreparation final {
    monitoring::MonitoringExecutionPolicy policy;
    std::optional<runtime::ServiceThreadAssignment> assignment;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    core::Sha256Digest scenario;
};
struct ExpectedCompletePreparation final {
    runtime::AdmittedMeasurementInputs measurement;
    runtime::StatisticsProductionPolicy statistics;
    std::uint64_t first_request, second_request, comparison_seed;
};
class ExpectedPairContext final {
  public:
    struct State;
    [[nodiscard]] static auto prepare(const journal::RunId&,
        const scenario::ScenarioDigestedPlan&, const runtime::AdmittedPairwiseProductionRequest&,
        const runtime::wire_v1::CalculatedEvidenceCapacity&, ContextLimits)
        -> std::expected<ExpectedPairContext, Error>;
    [[nodiscard]] static auto prepare(const journal::RunId&, const scenario::ScenarioDigestedPlan&,
        const runtime::AdmittedPairwiseProductionInputs&, const runtime::wire_v1::CalculatedEvidenceCapacity&,
        ContextLimits) -> std::expected<ExpectedPairContext, Error>;
    [[nodiscard]] auto complete_preparation() const noexcept -> const ExpectedCompletePreparation*;
    [[nodiscard]] auto monitoring_preparation() const noexcept -> const ExpectedMonitoringPreparation*;
    [[nodiscard]] auto control_assignment() const noexcept -> const runtime::ServiceThreadAssignment*;
    ExpectedPairContext(ExpectedPairContext&&) noexcept;
    auto operator=(ExpectedPairContext&&) noexcept -> ExpectedPairContext&;
    ~ExpectedPairContext();
    ExpectedPairContext(const ExpectedPairContext&) = delete;
    auto operator=(const ExpectedPairContext&) -> ExpectedPairContext& = delete;
    [[nodiscard]] auto available() const noexcept -> bool { return bool(state_); }
    [[nodiscard]] auto run() const noexcept -> const journal::RunId&;
    [[nodiscard]] auto provenance() const noexcept -> const runtime::PairwiseRunProvenance&;
    [[nodiscard]] auto cpus() const noexcept -> std::span<const core::LinuxCpuId>;
    [[nodiscard]] auto workers() const noexcept -> std::span<const core::WorkerId>;
    [[nodiscard]] auto kernel(bool second) const noexcept -> const runtime::AdmittedKernelExecutionInputs&;
    [[nodiscard]] auto assignments(bool second) const noexcept -> std::span<const runtime::ipc::PlacementCpuAssignment>;
    [[nodiscard]] auto memory_requests(bool second) const noexcept -> std::span<const ExpectedMemoryCell>;
    [[nodiscard]] auto capacity() const noexcept -> const runtime::wire_v1::CalculatedEvidenceCapacity&;
    [[nodiscard]] auto usage() const noexcept -> ContextUsage;
    [[nodiscard]] auto exactly_matches(const ExpectedPairContext&) const noexcept -> bool;
  private:
    static auto prepare_impl(const journal::RunId&, const scenario::ScenarioDigestedPlan&,
        const runtime::AdmittedPairwiseProductionRequest&, const runtime::wire_v1::CalculatedEvidenceCapacity&,
        ContextLimits, const runtime::ServiceThreadAssignment*, const runtime::AdmittedPairwiseProductionInputs* = nullptr) -> std::expected<ExpectedPairContext, Error>;
    explicit ExpectedPairContext(std::shared_ptr<const State>) noexcept;
    std::shared_ptr<const State> state_;
    friend class PairFactsLease;
    friend class Component;
    friend class Accumulator;
    friend class PairPrefix;
};
// Decoded backing contains only retained factual values, never an admitted request.
struct DecodedPairFacts final {
    journal::RunId run;
    runtime::PairwiseRunProvenance provenance;
    runtime::ipc::PlacementKernelExecutionInputs first, second;
    runtime::wire_v1::CalculatedEvidenceCapacity capacity;
    std::vector<core::LinuxCpuId> cpus;
    std::vector<core::WorkerId> workers;
    std::vector<runtime::ipc::PlacementCpuAssignment> first_assignments, second_assignments;
    std::vector<ExpectedMemoryCell> first_memory, second_memory;
    ContextUsage usage;
    std::optional<runtime::ServiceThreadAssignment> control;
    std::optional<ExpectedMonitoringPreparation> monitoring;
    std::optional<ExpectedCompletePreparation> preparation;
    process_subject::Domain domain{process_subject::Domain::synthetic_execution};
};
struct PairFactsView final {
    const journal::RunId& run;
    const runtime::PairwiseRunProvenance& provenance;
    const runtime::ipc::PlacementKernelExecutionInputs &first, &second;
    const runtime::wire_v1::CalculatedEvidenceCapacity& capacity;
    std::span<const core::LinuxCpuId> cpus;
    std::span<const core::WorkerId> workers;
    std::span<const runtime::ipc::PlacementCpuAssignment> first_assignments, second_assignments;
    std::span<const ExpectedMemoryCell> first_memory, second_memory;
    const runtime::ServiceThreadAssignment* control;
    const ExpectedMonitoringPreparation* monitoring;
    const ExpectedCompletePreparation* preparation;
    ContextUsage usage;
};
class PairFactsLease final {
  public:
    enum class Origin { actual_context, decoded_evidence };
    PairFactsLease(ExpectedPairContext&&) noexcept;
    PairFactsLease(const ExpectedPairContext&) noexcept;
    PairFactsLease(PairFactsLease&&) noexcept = default;
    auto operator=(PairFactsLease&&) noexcept -> PairFactsLease& = default;
    PairFactsLease(const PairFactsLease&) = delete;
    auto operator=(const PairFactsLease&) -> PairFactsLease& = delete;
    [[nodiscard]] auto available() const noexcept -> bool;
    [[nodiscard]] auto origin() const noexcept -> Origin;
    [[nodiscard]] auto decoded_domain() const noexcept -> std::optional<process_subject::Domain> {
        const auto* decoded=std::get_if<std::shared_ptr<const DecodedPairFacts>>(&value_);
        return decoded&&*decoded?std::optional{(*decoded)->domain}:std::nullopt;
    }
    [[nodiscard]] auto actual_context() const noexcept -> const ExpectedPairContext*;
    [[nodiscard]] auto same_owner(const PairFactsLease&) const noexcept -> bool;
    [[nodiscard]] auto retain() const noexcept -> PairFactsLease;
    [[nodiscard]] auto view() const noexcept -> PairFactsView;
    [[nodiscard]] auto run() const noexcept -> const journal::RunId&;
    [[nodiscard]] auto provenance() const noexcept -> const runtime::PairwiseRunProvenance&;
    [[nodiscard]] auto kernel(bool) const noexcept -> const runtime::ipc::PlacementKernelExecutionInputs&;
    [[nodiscard]] auto capacity() const noexcept -> const runtime::wire_v1::CalculatedEvidenceCapacity&;
    [[nodiscard]] auto cpus() const noexcept -> std::span<const core::LinuxCpuId>;
    [[nodiscard]] auto workers() const noexcept -> std::span<const core::WorkerId>;
    [[nodiscard]] auto assignments(bool) const noexcept -> std::span<const runtime::ipc::PlacementCpuAssignment>;
    [[nodiscard]] auto memory_requests(bool) const noexcept -> std::span<const ExpectedMemoryCell>;
    [[nodiscard]] auto control_assignment() const noexcept -> const runtime::ServiceThreadAssignment*;
    [[nodiscard]] auto monitoring_preparation() const noexcept -> const ExpectedMonitoringPreparation*;
    [[nodiscard]] auto complete_preparation() const noexcept -> const ExpectedCompletePreparation*;
    [[nodiscard]] auto usage() const noexcept -> ContextUsage;
  private:
    explicit PairFactsLease(std::shared_ptr<const DecodedPairFacts>) noexcept;
    std::variant<ExpectedPairContext,std::shared_ptr<const DecodedPairFacts>> value_;
    friend struct event_profile::detail::PairContextDecoder;
};
} // namespace epycbench::results::block_evidence
