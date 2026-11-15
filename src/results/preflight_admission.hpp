#pragma once
#include "results/host_evidence_assembly.hpp"
#include "results/authored_source.hpp"
#include "results/build_manifest_owner.hpp"
#include "results/validation_event_owner.hpp"
#include <memory>
#include <span>

namespace epycbench::results::process_subject { class Session; }
namespace epycbench::results::preflight {
struct References {
    std::uint64_t controller{}, normalization{}, system_state{}, base_build{};
    auto operator==(const References&) const -> bool = default;
};
enum class CheckKind {
    capture_model, capture_signature, capture_online, capture_numa, capture_cores,
    capture_caches, capture_memory, capture_kernel, capture_build, capture_compiler,
    capture_frequency, capture_controls, capture_anonymous_thp, capture_process_thp,
    capture_scenario, operating_system, architecture, cpu_vendor, cpu_nodes, memory_nodes,
    smt_permitted, required_feature, preferred_feature, numa_balancing, cpu_exclusivity,
    unintended_smt, hierarchy, monitoring_policy, monitoring_channel, thermal_observation,
    thermal_suitability, frequency_difference, placement_intent, service_core
};
struct CheckRef {
    CheckKind kind;
    std::size_t scope{}, item{};
    auto operator<=>(const CheckRef&) const = default;
};
enum class Evaluation { not_evaluated, evaluated };
using Outcome = validation_event::Outcome;
using Status = validation_event::Status;
enum class Reason { none, not_attempted, source_unavailable, source_failed, unknown_fact,
    conflicting_fact, missing_policy, unsupported_rule, policy_failure, pending_preparation };
struct CheckDeclaration {
    CheckRef check;
    Evaluation evaluation;
    const journal::Timestamp* checked_at{};
};
struct DeclarationRequirement {
    CheckRef check;
    bool required{};
    std::optional<Evaluation> locked_evaluation;
    bool time_locked{};
    std::optional<std::string> checked_at;
};
enum class ThermalState { not_attempted, available, unavailable, failed };
enum class ThermalReason { not_supported, source_missing, access_denied, read_failed,
    malformed_source, unsupported_value };
struct TemperatureFact {
    ThermalState state{ThermalState::not_attempted};
    std::optional<monitoring::TemperatureSnapshot> value;
    const journal::Timestamp* observed_at{};
    std::optional<ThermalReason> reason;
    std::optional<std::uint64_t> failure_event;
};
struct FailureReference { CheckRef check; std::uint64_t event; };
struct OverrideAttempt { CheckRef check; const journal::Timestamp& attempted_at; };
struct Input {
    References references;
    const journal::Timestamp& started_at;
    const journal::Timestamp& ended_at;
    std::span<const CheckDeclaration> declarations;
    std::optional<validation_event::Owner> monitoring;
    std::span<const TemperatureFact> temperatures;
    std::span<const FailureReference> failures;
    std::span<const OverrideAttempt> overrides;
};
struct Quantities {
    std::size_t records{}, scopes{}, checks{}, worker_associations{}, cache_memberships{},
        target_nodes{}, requirement_tokens{}, temperatures{}, timestamps{}, warnings{},
        overrides{}, failures{}, text_bytes{}, canonical_child_bytes{}, retained_bytes{},
        consumer_simultaneous_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits = Quantities;
using Usage = Quantities;
enum class Error { state, reference, capacity, invalid_input, topology, policy };
// A factual requirement inventory, never a successful preflight Owner/status.
struct DeclarationInventory { std::vector<DeclarationRequirement> rows;Usage usage{}; };
struct Check {
    CheckRef ref;
    bool required;
    Evaluation evaluation;
    std::optional<Outcome> outcome;
    Reason reason;
    std::optional<std::string> checked_at;
    // Exact original fact remains in the referenced immutable Session entry.
    std::optional<std::uint64_t> failure_event;
    std::string requirement_token;
};
struct Scope {
    bool matrix;
    std::size_t experiment_index, baseline_index, candidate_index;
    std::string experiment_id, baseline_id, candidate_id;
};
struct Worker {
    std::size_t scope, role, index;
    std::string stable_pu, stable_core;
    core::LinuxCpuId linux_cpu;
    std::uint64_t working_bytes, page_bytes, threshold_numerator, threshold_denominator;
    bool prefault, verify_before, verify_after;
};
struct CacheMembership {
    std::size_t worker;
    core::CacheId cache;
    std::uint32_t level;
    topology::CacheKind kind;
    std::uint64_t bytes, line_bytes;
};
struct TargetNode {
    std::size_t worker;
    std::string stable_id;
    core::LinuxNumaNodeId linux_node;
    std::optional<bool> local;
};
struct Temperature {
    ThermalState state;
    std::optional<monitoring::TemperatureSnapshot> value;
    std::optional<std::string> observed_at;
    std::optional<ThermalReason> reason;
    std::optional<std::uint64_t> failure_event;
};
struct Override { CheckRef check; std::string attempted_at; bool allowed; };
namespace detail {
struct Context {
    std::string_view run;
    const topology_evidence::Owner& topology;
    const host_assembly::Owner& host;
    const authored_source::ResolutionRecord& source;
    const build_manifest::Owner* build;
    bool build_attempted;
};
[[nodiscard]] auto declarations(const Context&,const Input&,const Limits&,
    std::size_t prior,std::size_t session_stock) -> std::expected<DeclarationInventory,Error>;
enum class SlotState { not_attempted, unavailable, failed, retained };
template<class T> struct SlotView {
    SlotState state{SlotState::not_attempted};
    const T* value{};
    std::optional<std::uint64_t> failure;
};
struct HostFacts {
    SlotView<host_controls::Owner> system_controls;
    SlotView<private_kernel_identity::Owner> kernel_identity;
    SlotView<pu_cpu_identity::Owner> pu_identity;
    SlotView<frequency_policy_owner::Owner> frequency;
    SlotView<anonymous_thp_policy_owner::Owner> anonymous_thp;
    SlotView<process_thp_restriction::Owner> process_thp;
};
// Pure retained facts. No successful Scenario/Resolution/Host wrapper is created.
struct FactualContext {
    std::string_view run;
    const topology_evidence::Owner& topology;
    const nlohmann::ordered_json& effective;
    const std::map<std::string, scenario::DispatchedWorkload>& workloads;
    const std::vector<scenario::ExperimentWorkloadBinding>& workload_bindings;
    const std::vector<scenario::ResolvedComparisonExperiment>& comparisons;
    const std::vector<scenario::ExpandedMatrixExperiment>& matrices;
    const std::vector<scenario::MatrixWorkerSelectorTemplate>& worker_templates;
    const std::vector<scenario::ExperimentWorkerReference>& worker_bindings;
    const std::vector<topology::StableCoreId>& reserved_cores;
    HostFacts host;
    const build_manifest::Owner* build;
    bool build_attempted;
};
struct State {
    References references;
    std::string started_at, ended_at;
    std::vector<Scope> scopes;
    std::vector<Worker> workers;
    std::vector<CacheMembership> caches;
    std::vector<TargetNode> nodes;
    std::vector<Check> checks;
    std::vector<Temperature> temperatures;
    std::vector<Override> overrides;
    std::vector<CheckRef> warnings;
    std::vector<FailureReference> failures;
    std::optional<validation_event::Owner> monitoring;
    Usage usage;
    Status status{Status::accepted};
};
[[nodiscard]] auto evaluate(const FactualContext&, const Input&, const Limits&,
    std::size_t prior, std::size_t session_stock)
    -> std::expected<std::unique_ptr<State>, Error>;
[[nodiscard]] auto fits(const Usage&, const Limits&) noexcept -> bool;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
}
class Owner {
  public:
    ~Owner();
    Owner(Owner&&) noexcept;
    auto operator=(Owner&&) noexcept -> Owner&;
    Owner(const Owner&) = delete;
    auto operator=(const Owner&) -> Owner& = delete;
    [[nodiscard]] auto facts() const noexcept -> const detail::State* { return state_.get(); }
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] auto status() const noexcept -> std::optional<Status>;
  private:
    explicit Owner(std::unique_ptr<detail::State>) noexcept;
    static auto prepare(const detail::Context&, const Input&, const Limits&,
        std::size_t prior, std::size_t session_stock) -> std::expected<Owner, Error>;
    void commit(std::optional<validation_event::Owner>&) noexcept;
    std::unique_ptr<detail::State> state_;
    friend class process_subject::Session;
};
} // namespace epycbench::results::preflight
