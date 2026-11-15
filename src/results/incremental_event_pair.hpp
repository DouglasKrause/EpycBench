#pragma once
#include "results/incremental_event_profile.hpp"
#include "results/incremental_event_preparation.hpp"
#include "results/incremental_pair_evidence.hpp"
#include "results/execution_request_association.hpp"
#include "results/pair_analysis_admission.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
#include "runtime/finalized_evidence_wire_startup_codec.hpp"

namespace epycbench::results::event_profile {
struct CapacityFactsLimits {
    std::size_t cpus{},domains{},workers{},text{},retained{},simultaneous{};
    std::uint64_t size_max{},byte_container_max{},node_container_max{},parser_depth_max{};
};
struct PairContextLimits {
    std::size_t bytes{},nodes{},depth{},text{},workers{},targets{},retained{},simultaneous{};
    CapacityFactsLimits capacity;
    request_association::Limits association;
};
struct DecodedContext {
    block_evidence::PairFactsLease lease;
    runtime::wire_v1::detail::ReconstructedEvidenceHeader header;
    std::string canonical;
    std::string request_body;
    platform_linux::detail::LinuxMonitoringCapacityProvenance monitoring_provenance;
    std::array<request_association::ServiceLocator,2> services;
    std::size_t service_count{};
    std::string session;
    // Checked conservative stock of this decoded owner, not its input ceiling.
    std::size_t retained_bytes{};
};
struct CompositionLimits {
    PlanLimits planning;
    PreflightLimits preflight;
    PairContextLimits context;
    block_evidence::PairLimits pair;
    observation_timing::Limits timing;
    error_serialization::Limits error;
    pair_analysis::Limits analysis;
    std::size_t pairs{},events{},retained{},simultaneous{};
};
namespace detail {
// Internal handoff of the existing fixed diagnostic; the public rejection
// remains Error::association and never retains candidate text.
struct PairAdmissionFailure final { block_evidence::Error error; };
struct SuppliedExit {
    std::uint64_t child{},value{};
    bool signaled{};
    auto operator==(const SuppliedExit&) const -> bool = default;
};
struct CaptureAssociation { std::uint64_t capture{},observation{}; };
struct ComposedPair {
    std::uint64_t start_record{},request_record{},child_token{},root_record{},analysis_record{};
    std::string session;
    std::shared_ptr<const DecodedContext> context;
    std::optional<block_evidence::PairPrefix> prefix;
    std::optional<PairwiseRunResult> analysis;
    std::string analysis_canonical;
    std::size_t analysis_retained{};
    std::vector<std::uint64_t> events;
    std::vector<std::uint64_t> children;
    std::vector<CaptureAssociation> captures;
    std::optional<SuppliedExit> exit;
    std::uint64_t completion_record{},eof_record{},reap_record{},cleanup_record{};
    bool clean_eof{},cleanup_complete{};
    std::uint64_t cancellation_record{},reap_failure_record{};
    std::size_t retained{};
};
struct CompositionState {
    std::optional<core::Sha256Digest> check_failure_digest;
    std::size_t check_failure_count{},check_failure_total{};
    std::shared_ptr<const PlanFacts> planning;
    std::shared_ptr<const preflight::detail::State> preflight;
    std::uint64_t normalization_record{},planning_record{},preflight_record{},closed_record{};
    std::size_t retained{};
    bool operational_failure{};
    std::vector<std::shared_ptr<const ComposedPair>> pairs;
};
[[nodiscard]] auto compose(std::span<const Record>,const Record&,
    const std::shared_ptr<const CompositionState>&,const CompositionLimits&,Work&)
    -> std::shared_ptr<const CompositionState>;
struct PairContextDecoder {
    [[nodiscard]] static auto observation(block_evidence::PairPrefix&,std::uint64_t,const journal::Timestamp&,
        bool,std::string_view,const nlohmann::json&,const DecodedContext&,const error_serialization::Limits&,Work&)
        -> std::expected<void,block_evidence::Error>;
    [[nodiscard]] static auto graph(block_evidence::PairPrefix&,std::uint64_t,const journal::Timestamp&,
        const nlohmann::json&,const DecodedContext&,const PairContextLimits&,Work&)
        -> std::expected<void,block_evidence::Error>;
    [[nodiscard]] static auto monitoring(block_evidence::PairPrefix&,std::uint64_t,const journal::Timestamp&,
        const nlohmann::json&,const DecodedContext&,const PairContextLimits&,Work&)
        -> std::expected<void,block_evidence::Error>;
    [[nodiscard]] static auto workers(const nlohmann::json&,const PairContextLimits&,Work&)
        -> std::vector<runtime::WorkerReadyEvidence>;
    [[nodiscard]] static auto control(const nlohmann::json&,Work&) -> runtime::ServiceThreadReadyEvidence;
    [[nodiscard]] static auto memory(const nlohmann::json&,const DecodedContext&,bool,
        const PairContextLimits&,Work&) -> std::vector<block_evidence::MemoryPreparationRow>;
    [[nodiscard]] static auto prepare(const DecodedContext&,block_evidence::PairLimits,
        observation_timing::Limits) -> std::expected<block_evidence::PairPrefix,block_evidence::Error>;
    [[nodiscard]] static auto decode(std::string_view,const normalization_serialization::Owner&,
        const PlanFacts&,std::size_t,const PairContextLimits&,Work&,
        std::optional<std::string_view> expected_origin={}) -> DecodedContext;
};
// Supplied synthetic arithmetic consistency. No resource reservation or admission.
[[nodiscard]] auto reconstruct_capacity(const nlohmann::json&,
    const runtime::wire_v1::detail::ParsedCapacityOffer&,
    const runtime::detail::PairwiseHostCapacityRequestTranscript&,
    const topology::TopologySnapshot&,const monitoring::MonitoringExecutionPolicy&,
    const CapacityFactsLimits&,Work&) -> runtime::wire_v1::CalculatedEvidenceCapacity;
}
}
