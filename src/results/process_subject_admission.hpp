#pragma once
#include "results/host_evidence_assembly.hpp"
#include "results/execution_plan_admission.hpp"
#include "results/private_error_owner.hpp"
#include "results/authored_source.hpp"
#include "results/build_manifest_owner.hpp"
#include "results/pair_lifecycle_admission.hpp"
#include "results/pair_analysis_admission.hpp"
#include "results/preflight_admission.hpp"
#include "results/run_creation_admission.hpp"
#include "results/host_projection.hpp"
#include "results/build_projection.hpp"
#include "results/private_error_serialization.hpp"
#include "results/private_source_serialization.hpp"
#include "results/private_source_outcome_serialization.hpp"
#include "results/private_normalization_serialization.hpp"
#include "results/private_base_build_serialization.hpp"
#include "results/private_topology_serialization.hpp"
#include "results/private_kernel_identity_serialization.hpp"
#include "results/private_process_thp_restriction_serialization.hpp"
#include "results/private_host_serialization.hpp"
#include "results/private_run_creation_serialization.hpp"
#include "results/private_anonymous_thp_policy_serialization.hpp"
#include "results/private_frequency_policy_serialization.hpp"
#include "results/private_pu_cpu_identity_serialization.hpp"
#include "results/private_system_controls_serialization.hpp"
#include <compare>
#include <span>
#include <vector>
namespace epycbench::results::process_subject {
struct Normalization { authored_source::ResolutionRecord source; std::uint64_t topology_event_sequence; };
struct SourceOutcome { authored_source::ResolutionRecord source; };
struct Input {
    Domain domain;
    std::uint64_t sequence;
    const journal::Timestamp& timestamp;
    Producer producer;
    Creation creation;
    std::optional<host_assembly::Owner> host;
    std::optional<FailureFact> failure{};
    std::optional<Normalization> normalization{};
    std::optional<SourceOutcome> source_outcome{};
    std::optional<BaseBuild> base_build{};
};
struct Limits { std::size_t entries, runner_instances, timestamp_bytes, canonical_child_bytes, retained_bytes;
    std::size_t failure_facts{}, targets{}, context_nodes{}, private_string_bytes{}, private_binary_bytes{};
    std::size_t normalizations{}, source_nodes{}, source_canonical_bytes{}, source_outcomes{}; std::size_t base_builds{};
    pair_lifecycle::Limits lifecycle{}; pair_analysis::Limits analysis{}; preflight::Limits preflight{}; run_creation::Limits run_creation{}; planning::Limits planning{}; request_association::Limits request_association{}; };
struct Usage {
    std::size_t entries{}, runner_instances{}, timestamp_bytes{}, canonical_child_bytes{}, retained_bytes{};
    std::size_t failure_facts{}, targets{}, context_nodes{}, private_string_bytes{}, private_binary_bytes{};
    std::size_t normalizations{}, source_nodes{}, source_canonical_bytes{}, source_outcomes{}; std::size_t base_builds{};
    pair_lifecycle::Usage lifecycle{};
    pair_analysis::Usage analysis{};
    preflight::Usage preflight{};
    run_creation::Usage run_creation{};
    planning::Usage planning{};
    request_association::Usage request_association{};
    auto operator==(const Usage&) const -> bool = default;
};
struct ResolvedSubject { process_thp_restriction::Role role; std::uint64_t creation_sequence; };
struct Entry {
    std::uint64_t sequence;
    std::string timestamp;
    Producer producer;
    Creation creation;
    std::optional<host_assembly::Owner> host;
    std::optional<FailureFact> failure;
    std::vector<std::size_t> target_index;
    std::optional<Normalization> normalization;
    std::optional<SourceOutcome> source_outcome;
    // Absence means no process subject.
    std::optional<ResolvedSubject> resolved_subject;
    std::optional<BaseBuild> base_build{};
    std::optional<pair_lifecycle::Owner> lifecycle{};
    std::optional<pair_analysis::Owner> analysis{};
    std::optional<preflight::Owner> preflight{};
    std::optional<run_creation::Owner> run_creation{};
    std::optional<planning::Record> planning{};
    std::optional<planning::Membership> planning_membership{};
};
namespace detail {
[[nodiscard]] auto creation_session_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto creation_append_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto creation_lookup_fixed_stock() noexcept -> std::size_t;
// Logical fixed stock of the ordinary Session append call chain, including
// non-elided caller/callee result cells and the unchanged incoming owner.
struct AnalysisSessionStaging final {
    const Entry* parent;
    const journal::Timestamp* timestamp;
    std::string_view supplied;
    std::uint64_t sequence, lifecycle_sequence;
    pair_analysis::References references;
    pair_analysis::Limits remaining;
    Usage before, base, next;
    pair_analysis::Usage child, child_return;
    std::expected<pair_analysis::Usage, pair_analysis::Error> combined;
    std::optional<std::expected<pair_analysis::Owner, pair_analysis::Error>> admitted;
    std::optional<Entry> prepared;
    std::optional<core::Error> failure;
    std::expected<void, core::Error> returned;
    std::size_t old_capacity, new_capacity, entry_stock, owner_allowance;
};
struct LifecycleSessionStaging final {
    runtime::detail::SyntheticLifecycleView* incoming;
    block_evidence::PairPrefix* prefix;
    std::optional<runtime::detail::SyntheticLifecycleView> consumed;
    std::optional<decltype(journal::RunId::parse(std::string_view{}))> parsed_run;
    std::optional<std::expected<pair_lifecycle::Owner, block_evidence::Error>> admitted;
    std::optional<Entry> prepared;
    Usage before, base, next;
    pair_lifecycle::Limits remaining;
    pair_lifecycle::References references;
    // The child local and the non-elided Owner::usage return may coexist.
    pair_lifecycle::Usage child, child_return;
    const Entry *controller, *runner, *normalization;
    const scenario::ScenarioDigestedPlan* plan;
    std::size_t old_capacity, new_capacity, entry_stock, owner_allowance;
    std::uint64_t sequence, controller_sequence;
    std::optional<std::uint64_t> runner_sequence;
    const journal::Timestamp* timestamp;
    std::optional<core::Error> failure;
    std::expected<void, core::Error> returned;
    std::optional<std::expected<planning::Membership, planning::Error>> planning_result;
    std::optional<std::expected<request_association::Evidence, request_association::Error>> request_result;
    planning::Usage planning_candidate;
    request_association::Usage request_candidate, request_return;
    const runtime::detail::PairwiseV2AdmittedStartup* actual;
    std::size_t request_phase_peak;

};
}
struct Summary { std::size_t entries; std::optional<std::uint64_t> last_sequence; };
class Session {
  public:
    [[nodiscard]] static auto create(const journal::RunId&, Domain, Limits) -> std::expected<Session, core::Error>;
    Session(const Session&) = delete;
    auto operator=(const Session&) -> Session& = delete;
    Session(Session&&) noexcept;
    auto operator=(Session&&) noexcept -> Session&;
    [[nodiscard]] auto append_creation(std::uint64_t, const journal::Timestamp&, run_creation::Input&)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto append_planning(std::uint64_t, const journal::Timestamp&, execution_plan::Owner&)
        -> std::expected<void, planning::Error>;
    [[nodiscard]] auto append(Input&) -> std::expected<void, core::Error>;
    [[nodiscard]] auto append_preflight(std::uint64_t, const journal::Timestamp&, preflight::Input&)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto append_lifecycle(std::uint64_t sequence, const journal::Timestamp&,
        std::uint64_t controller_sequence, std::optional<std::uint64_t> runner_sequence,
        runtime::detail::SyntheticLifecycleView&, block_evidence::PairPrefix* = nullptr)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto append_analysis(std::uint64_t sequence, const journal::Timestamp&,
        std::uint64_t lifecycle_sequence, std::string_view canonical_analysis)
        -> std::expected<void, core::Error>;
    // A summary describes an open prefix, never a completed or recovered run.
    [[nodiscard]] auto finish() -> std::expected<Summary, core::Error>;
    [[nodiscard]] auto entries() const noexcept -> std::span<const Entry> { return present_ ? std::span<const Entry>{entries_} : std::span<const Entry>{}; }
    [[nodiscard]] auto run_id() const noexcept -> std::string_view { return present_ ? std::string_view{run_} : std::string_view{}; }
    [[nodiscard]] auto domain() const noexcept -> std::optional<Domain> { return present_ ? std::optional{domain_} : std::nullopt; }
    [[nodiscard]] auto usage() const noexcept -> Usage { return usage_; }
    [[nodiscard]] auto appendable() const noexcept -> bool { return live_; }
    [[nodiscard]] auto project_host(std::uint64_t host_sequence, host_projection::Limits) const
        -> std::expected<host_projection::Owner, host_projection::Error>;
    [[nodiscard]] auto project_base_build(std::uint64_t sequence, build_projection::Limits) const
        -> std::expected<build_projection::Owner, build_projection::Error>;
    [[nodiscard]] auto serialize_run_creation(std::uint64_t, run_creation_serialization::Limits) const
        -> std::expected<run_creation_serialization::Owner, run_creation_serialization::Error>;
    [[nodiscard]] auto serialize_host(std::uint64_t, host_serialization::Limits) const
        -> std::expected<host_serialization::Owner, host_serialization::Error>;
    [[nodiscard]] auto serialize_process_thp_restriction(std::uint64_t, process_thp_restriction_serialization::Limits) const
        -> std::expected<process_thp_restriction_serialization::Owner, process_thp_restriction_serialization::Error>;
    [[nodiscard]] auto serialize_anonymous_thp_policy(std::uint64_t, anonymous_thp_policy_serialization::Limits) const
        -> std::expected<anonymous_thp_policy_serialization::Owner, anonymous_thp_policy_serialization::Error>;
    [[nodiscard]] auto serialize_frequency_policy(std::uint64_t, frequency_policy_serialization::Limits) const
        -> std::expected<frequency_policy_serialization::Owner, frequency_policy_serialization::Error>;
    [[nodiscard]] auto serialize_pu_identity(std::uint64_t, pu_identity_serialization::Limits) const
        -> std::expected<pu_identity_serialization::Owner, pu_identity_serialization::Error>;
    [[nodiscard]] auto serialize_kernel_identity(std::uint64_t, kernel_identity_serialization::Limits) const
        -> std::expected<kernel_identity_serialization::Owner, kernel_identity_serialization::Error>;
    [[nodiscard]] auto serialize_system_controls(std::uint64_t, system_controls_serialization::Limits) const
        -> std::expected<system_controls_serialization::Owner, system_controls_serialization::Error>;
    [[nodiscard]] auto serialize_topology(std::uint64_t, topology_serialization::Limits) const
        -> std::expected<topology_serialization::Owner, topology_serialization::Error>;
    [[nodiscard]] auto serialize_base_build(std::uint64_t, base_build_serialization::Limits) const
        -> std::expected<base_build_serialization::Owner, base_build_serialization::Error>;
    [[nodiscard]] auto serialize_normalization(std::uint64_t, normalization_serialization::Limits) const
        -> std::expected<normalization_serialization::Owner, normalization_serialization::Error>;
    [[nodiscard]] auto serialize_source_outcome(std::uint64_t, source_outcome_serialization::Limits) const
        -> std::expected<source_outcome_serialization::Owner, source_outcome_serialization::Error>;
    [[nodiscard]] auto serialize_source(source_serialization::Address, source_serialization::Limits) const
        -> std::expected<source_serialization::Owner, source_serialization::Error>;
    [[nodiscard]] auto serialize_error(error_serialization::Address, error_serialization::Limits) const
        -> std::expected<error_serialization::Owner, error_serialization::Error>;
  private:
    Session() = default;
    bool present_{true}, live_{true};
    std::string run_;
    Domain domain_{};
    Limits limits_{};
    Usage usage_{};
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> pairs_;
    std::optional<std::uint64_t> normalization_sequence_, source_outcome_sequence_, base_build_sequence_;
    std::optional<std::uint64_t> preflight_sequence_;
    std::optional<std::uint64_t> planning_sequence_;
    bool preflight_blocked_{};
};
namespace detail {
[[nodiscard]] auto add_stock(Usage&, std::size_t timestamp, std::size_t canonical,
    std::size_t host_stock, bool has_host, bool runner, std::size_t index_size) noexcept -> bool;
}
} // namespace epycbench::results::process_subject
