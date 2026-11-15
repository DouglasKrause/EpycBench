#pragma once

#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "runtime/finalized_evidence_prepared_resources.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::runtime::prepared_codec_v1 {

class PreparedTypedBuilderPlan;

enum class ActivePipelineFailureKind : std::uint8_t {
    capacity_invariant,
    malformed,
    noncanonical,
    registry_shape,
    registry_domain,
    registry_cardinality,
    typed_factory,
    ledger_validation,
    reencode_mismatch,
    results_projection,
    results_recalculation,
    transport_state,
    io,
    timeout,
    cancelled,
    peer_exit,
    cleanup,
};
enum class ActivePipelineStage : std::uint8_t { prepared, active, encoded };

struct ActiveCodecFailure final {
    ActivePipelineFailureKind kind;
    ActivePipelineStage stage;
    std::uint64_t sequence_index{};
    std::uint64_t limit{};
    std::uint64_t observed{};
    std::optional<std::int32_t> normalized_os_error;
    friend constexpr auto operator==(const ActiveCodecFailure&, const ActiveCodecFailure&) noexcept
        -> bool = default;
};

// Fixed-capacity canonical-byte owner. All storage is constructed and touched
// before activation. Active operations only write live vector elements.
class PreparedCanonicalWriter final {
  public:
    PreparedCanonicalWriter(const PreparedCanonicalWriter&) = delete;
    PreparedCanonicalWriter(PreparedCanonicalWriter&&) noexcept;
    auto operator=(const PreparedCanonicalWriter&) -> PreparedCanonicalWriter& = delete;
    auto operator=(PreparedCanonicalWriter&&) -> PreparedCanonicalWriter& = delete;

    [[nodiscard]] static auto prepare(prepared_resources_v1::PreparedResourceOwner owner) noexcept
        -> std::expected<PreparedCanonicalWriter, prepared_resources_v1::PreparedResourceError>;
    // Separate cumulative progress stock, never a finalized-resource capability.
    [[nodiscard]] static auto prepare_progress(std::size_t bytes) noexcept
        -> std::expected<PreparedCanonicalWriter, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto progress_slice(std::size_t begin) const noexcept
        -> std::expected<std::span<const char>, ActiveCodecFailure>;
    [[nodiscard]] auto activate() noexcept -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto append_literal(std::string_view) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto append_unsigned(std::uint64_t) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto append_signed(std::int64_t) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto append_binary64(std::uint64_t bits) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto append_json_string(std::string_view utf8) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<std::span<const char>, ActiveCodecFailure>;
    [[nodiscard]] auto reject_domain() noexcept -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto size() const noexcept -> std::size_t { return cursor_; }
    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return storage_.size(); }

  private:
    PreparedCanonicalWriter(std::optional<prepared_resources_v1::PreparedResourceOwner> owner,
                            std::vector<char> storage) noexcept
        : owner_{std::move(owner)}, storage_{std::move(storage)} {}
    [[nodiscard]] auto append_char(char) noexcept -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto fail(ActiveCodecFailure) noexcept -> std::expected<void, ActiveCodecFailure>;
    std::optional<prepared_resources_v1::PreparedResourceOwner> owner_;
    std::vector<char> storage_;
    std::size_t cursor_{};
    bool active_{};
    bool finished_{};
    bool failed_{};
    ActivePipelineStage stage_{ActivePipelineStage::prepared};
};

// Allocation-free, nonrecursive validator for a complete canonical evidence
// body. The explicit stack and every definition cardinality are allocated and
// fixed before activation from the accepted prepared-resource value.
class PreparedCanonicalCursor final {
  public:
    PreparedCanonicalCursor(const PreparedCanonicalCursor&) = delete;
    PreparedCanonicalCursor(PreparedCanonicalCursor&&) noexcept;
    auto operator=(const PreparedCanonicalCursor&) -> PreparedCanonicalCursor& = delete;
    auto operator=(PreparedCanonicalCursor&&) -> PreparedCanonicalCursor& = delete;

    [[nodiscard]] static auto prepare(prepared_resources_v1::PreparedResourceOwner owner) noexcept
        -> std::expected<PreparedCanonicalCursor, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] static auto prepare(prepared_resources_v1::PreparedResourceOwner owner,
        const wire_v1::CalculatedEvidenceCapacity&) noexcept
        -> std::expected<PreparedCanonicalCursor, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto activate() noexcept -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto validate(std::span<const char>) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto decode(std::span<const char>, PreparedTypedBuilderPlan&) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto complete(const PreparedTypedBuilderPlan&) noexcept
        -> std::expected<void, ActiveCodecFailure>;

  private:
    struct Frame final {
        std::uint32_t node{};
        std::uint32_t index{};
        std::uint64_t count{};
        std::size_t event{};
        std::size_t lexical_begin{};
        std::uint8_t phase{};
        std::uint8_t monitoring_presence{};
        std::uint8_t side{};
        std::uint64_t owner_pages{}, owner_targets{};
    };
    struct Context final {
        std::array<std::vector<std::uint64_t>, 2> pages, targets;
        std::uint64_t nodes{}, present_cpus{}, maximum_domains{};
    };
    PreparedCanonicalCursor(prepared_resources_v1::PreparedResourceOwner owner,
                            std::vector<Frame> stack) noexcept
        : owner_{std::move(owner)}, stack_{std::move(stack)} {}
    [[nodiscard]] auto walk(std::span<const char>, PreparedTypedBuilderPlan*) noexcept
        -> std::expected<void, ActiveCodecFailure>;
    prepared_resources_v1::PreparedResourceOwner owner_;
    std::vector<Frame> stack_;
    std::optional<Context> context_;
    bool active_{};
    bool consumed_{};
    bool decoded_{};
};

// Fixed-capacity, schema-indexed handoff produced by the canonical cursor.
// It deliberately retains offsets into the caller-owned canonical body rather
// than creating strings. Later typed builders consume these records while the
// body remains alive.
enum class PreparedParsedNodeKind : std::uint8_t {
    scalar,
    object,
    array,
    option,
    sum,
};

struct PreparedParsedNode final {
    std::uint32_t definition{};
    std::uint64_t occurrence{};
    PreparedParsedNodeKind kind{};
    std::uint32_t selected_child{};
    std::uint64_t element_count{};
    std::size_t lexical_begin{};
    std::size_t lexical_size{};
    bool present{true};
};

class PreparedTypedBuilderPlan final {
  public:
    PreparedTypedBuilderPlan(const PreparedTypedBuilderPlan&) = delete;
    PreparedTypedBuilderPlan(PreparedTypedBuilderPlan&&) noexcept;
    auto operator=(const PreparedTypedBuilderPlan&) -> PreparedTypedBuilderPlan& = delete;
    auto operator=(PreparedTypedBuilderPlan&&) -> PreparedTypedBuilderPlan& = delete;

    [[nodiscard]] static auto prepare(const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedTypedBuilderPlan, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto activate() noexcept -> std::expected<void, ActiveCodecFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<std::span<const PreparedParsedNode>, ActiveCodecFailure>;

  private:
    friend class PreparedCanonicalCursor;
    PreparedTypedBuilderPlan(std::vector<PreparedParsedNode> slots,
                             std::vector<std::uint64_t> occurrences,
                             std::vector<std::uint64_t> limits,
                             std::vector<std::uint8_t> initialized_bits) noexcept
        : slots_{std::move(slots)}, occurrences_{std::move(occurrences)},
          limits_{std::move(limits)}, initialized_bits_{std::move(initialized_bits)} {}
    [[nodiscard]] auto begin_node(std::uint32_t definition, PreparedParsedNodeKind kind,
                                  std::size_t lexical_begin) noexcept
        -> std::expected<std::size_t, ActiveCodecFailure>;
    [[nodiscard]] auto fail(ActiveCodecFailure) noexcept -> std::expected<void, ActiveCodecFailure>;
    std::vector<PreparedParsedNode> slots_;
    std::vector<std::uint64_t> occurrences_;
    std::vector<std::uint64_t> limits_;
    std::vector<std::uint8_t> initialized_bits_;
    std::size_t initialized_{};
    bool active_{};
    bool finished_{};
    bool failed_{};
};

// First typed-reconstruction owner slice. Retained provenance strings are
// individually reserved from their exact per-definition U(d) stocks before
// activation. build() consumes only validated lexical views and constructs
// the header/policy graph without an allocator or an owning parse tree.
class PreparedEvidenceHeaderBuilder final {
  public:
    PreparedEvidenceHeaderBuilder(const PreparedEvidenceHeaderBuilder&) = delete;
    PreparedEvidenceHeaderBuilder(PreparedEvidenceHeaderBuilder&&) noexcept = default;
    auto operator=(const PreparedEvidenceHeaderBuilder&) -> PreparedEvidenceHeaderBuilder& = delete;
    auto operator=(PreparedEvidenceHeaderBuilder&&) -> PreparedEvidenceHeaderBuilder& = delete;

    [[nodiscard]] static auto prepare(const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedEvidenceHeaderBuilder,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes) noexcept
        -> std::expected<wire_v1::detail::ReconstructedEvidenceHeader, ActiveCodecFailure>;

  private:
    explicit PreparedEvidenceHeaderBuilder(std::array<std::string, 6> strings,
                                           std::array<std::size_t, 6> limits) noexcept
        : strings_{std::move(strings)}, limits_{limits} {}
    std::array<std::string, 6> strings_;
    std::array<std::size_t, 6> limits_;
    bool consumed_{};
};

// Second typed-reconstruction owner slice.  Every vector and the warm-up
// median scratch are allocated and touched before active decode.  build()
// consumes only builder-plan events and moves those prepared allocations into
// the production Measurement outcomes.
class PreparedCalibrationWarmupBuilder final {
  public:
    PreparedCalibrationWarmupBuilder(const PreparedCalibrationWarmupBuilder&) = delete;
    PreparedCalibrationWarmupBuilder(PreparedCalibrationWarmupBuilder&&) noexcept = default;
    auto operator=(const PreparedCalibrationWarmupBuilder&)
        -> PreparedCalibrationWarmupBuilder& = delete;
    auto operator=(PreparedCalibrationWarmupBuilder&&)
        -> PreparedCalibrationWarmupBuilder& = delete;

    [[nodiscard]] static auto prepare(const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedCalibrationWarmupBuilder,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes) noexcept
        -> std::expected<wire_v1::detail::ReconstructedCalibrationWarmup, ActiveCodecFailure>;

  private:
    PreparedCalibrationWarmupBuilder(
        std::vector<measurement::EmptyHarnessTrial> empty,
        std::vector<measurement::SequentialReadBandwidthCalibrationTrial> bandwidth,
        std::vector<measurement::DependentLoadLatencyCalibrationTrial> latency,
        std::vector<measurement::WarmupTrial> warmup, std::vector<std::uint64_t> scratch) noexcept
        : empty_{std::move(empty)}, bandwidth_{std::move(bandwidth)}, latency_{std::move(latency)},
          warmup_{std::move(warmup)}, scratch_{std::move(scratch)} {}
    std::vector<measurement::EmptyHarnessTrial> empty_;
    std::vector<measurement::SequentialReadBandwidthCalibrationTrial> bandwidth_;
    std::vector<measurement::DependentLoadLatencyCalibrationTrial> latency_;
    std::vector<measurement::WarmupTrial> warmup_;
    std::vector<std::uint64_t> scratch_;
    bool consumed_{};
};

struct ReconstructedMeasurementValidity final {
    measurement::MeasurementObservation observation;
    measurement::ObservationValidityEvidence validity;
};

// Dedicated inactive owner for one Monitoring result.  Its storage is derived
// from the authenticated Monitoring O/A/U definition stocks; no producer-side
// defaults or host discovery state are reused by this wire reconstruction.
class PreparedMonitoringBuilder final {
  public:
    PreparedMonitoringBuilder(const PreparedMonitoringBuilder&) = delete;
    PreparedMonitoringBuilder(PreparedMonitoringBuilder&&) noexcept = default;
    auto operator=(const PreparedMonitoringBuilder&) -> PreparedMonitoringBuilder& = delete;
    auto operator=(PreparedMonitoringBuilder&&) -> PreparedMonitoringBuilder& = delete;
    ~PreparedMonitoringBuilder() noexcept;

    [[nodiscard]] static auto prepare(const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedMonitoringBuilder, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes,
                             const PreparedParsedNode& factual_observation,
                             std::span<const core::WorkerId> authenticated_workers,
                             std::span<const core::LinuxCpuId> authenticated_cpus,
                             platform_linux::RawMonotonicTimestamp observation_start,
                             platform_linux::RawMonotonicTimestamp observation_end,
                             std::span<const core::LinuxCpuId> selected_worker_cpus = {}) noexcept
        -> std::expected<std::optional<monitoring::MonitoringCaptureResult>, ActiveCodecFailure>;

  private:
    friend class PreparedFactualObservationPool;
    [[nodiscard]] static auto prepare_for_factual_owner(
        const prepared_resources_v1::PreparedResourceValue&, std::uint64_t owners,
        std::optional<std::size_t> domain_cpu_bound = std::nullopt) noexcept
        -> std::expected<PreparedMonitoringBuilder, prepared_resources_v1::PreparedResourceError>;
    struct Storage;
    explicit PreparedMonitoringBuilder(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
    bool consumed_{};
};

// Prepared owner for the allocation-bearing worker arrays used by one factual
// measurement. Validity itself is fixed-size. The builder deliberately stops
// before Monitoring and placement/migration association.
class PreparedMeasurementValidityBuilder final {
  public:
    PreparedMeasurementValidityBuilder(const PreparedMeasurementValidityBuilder&) = delete;
    PreparedMeasurementValidityBuilder(PreparedMeasurementValidityBuilder&&) noexcept = default;
    auto operator=(const PreparedMeasurementValidityBuilder&)
        -> PreparedMeasurementValidityBuilder& = delete;
    auto operator=(PreparedMeasurementValidityBuilder&&)
        -> PreparedMeasurementValidityBuilder& = delete;

    [[nodiscard]] static auto prepare(const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedMeasurementValidityBuilder,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes,
                             const PreparedParsedNode& factual_observation) noexcept
        -> std::expected<ReconstructedMeasurementValidity, ActiveCodecFailure>;

  private:
    PreparedMeasurementValidityBuilder(
        std::vector<measurement::SequentialReadBandwidthWorkerObservation> bandwidth,
        std::vector<measurement::DependentLoadLatencyWorkerObservation> latency) noexcept
        : bandwidth_{std::move(bandwidth)}, latency_{std::move(latency)} {}
    std::vector<measurement::SequentialReadBandwidthWorkerObservation> bandwidth_;
    std::vector<measurement::DependentLoadLatencyWorkerObservation> latency_;
    bool consumed_{};
};

// Fixed-size prepared owner for one durable error.  The wire value itself has
// no allocation-bearing members, but the one-use owner authenticates that the
// finalized-error definition is admitted by the prepared-resource profile.
class PreparedFinalizedErrorBuilder final {
  public:
    PreparedFinalizedErrorBuilder(const PreparedFinalizedErrorBuilder&) = delete;
    PreparedFinalizedErrorBuilder(PreparedFinalizedErrorBuilder&&) noexcept = default;
    auto operator=(const PreparedFinalizedErrorBuilder&)
        -> PreparedFinalizedErrorBuilder& = delete;
    auto operator=(PreparedFinalizedErrorBuilder&&)
        -> PreparedFinalizedErrorBuilder& = delete;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedFinalizedErrorBuilder,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes,
                             const PreparedParsedNode& finalized_error) noexcept
        -> std::expected<FinalizedEvidenceError, ActiveCodecFailure>;

  private:
    explicit PreparedFinalizedErrorBuilder(std::uint64_t admitted) noexcept
        : admitted_{admitted} {}
    std::uint64_t admitted_{};
    bool consumed_{};
};

// Ordinal-bound owners for every authenticated durable-error occurrence.
class PreparedFinalizedErrorPool final {
  public:
    PreparedFinalizedErrorPool(const PreparedFinalizedErrorPool&) = delete;
    PreparedFinalizedErrorPool(PreparedFinalizedErrorPool&&) noexcept = default;
    auto operator=(const PreparedFinalizedErrorPool&) -> PreparedFinalizedErrorPool& = delete;
    auto operator=(PreparedFinalizedErrorPool&&) -> PreparedFinalizedErrorPool& = delete;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedFinalizedErrorPool,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::size_t ordinal, std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes,
                             const PreparedParsedNode& finalized_error) noexcept
        -> std::expected<FinalizedEvidenceError, ActiveCodecFailure>;

  private:
    explicit PreparedFinalizedErrorPool(
        std::vector<PreparedFinalizedErrorBuilder> builders,
        std::vector<std::uint8_t> consumed) noexcept
        : builders_{std::move(builders)}, consumed_{std::move(consumed)} {}
    std::vector<PreparedFinalizedErrorBuilder> builders_;
    std::vector<std::uint8_t> consumed_;
};

// Prepared owner for one Memory region-finalization diagnostic.  All target,
// placement-count, and decoded stable-id storage is reserved from the
// authenticated definition-local stocks before activation.
class PreparedMemoryDiagnosticBuilder final {
  public:
    PreparedMemoryDiagnosticBuilder(const PreparedMemoryDiagnosticBuilder&) = delete;
    PreparedMemoryDiagnosticBuilder(PreparedMemoryDiagnosticBuilder&&) noexcept = default;
    auto operator=(const PreparedMemoryDiagnosticBuilder&)
        -> PreparedMemoryDiagnosticBuilder& = delete;
    auto operator=(PreparedMemoryDiagnosticBuilder&&)
        -> PreparedMemoryDiagnosticBuilder& = delete;
    ~PreparedMemoryDiagnosticBuilder() noexcept;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedMemoryDiagnosticBuilder,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(std::span<const char> canonical_body,
                             std::span<const PreparedParsedNode> nodes,
                             const PreparedParsedNode& region,
                             core::WorkerId authenticated_worker) noexcept
        -> std::expected<memory::RegionFinalizationDiagnostic, ActiveCodecFailure>;

  private:
    struct Storage;
    explicit PreparedMemoryDiagnosticBuilder(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
    bool consumed_{};
};

// Prepared owner for every admitted factual-observation occurrence.  Each
// occurrence owns its measurement, Monitoring, placement, and migration
// destinations before activation; build() binds the wire ordinal to the
// authenticated scheduled slot supplied by the enclosing block builder.
class PreparedFactualObservationPool final {
  public:
    PreparedFactualObservationPool(const PreparedFactualObservationPool&) = delete;
    PreparedFactualObservationPool(PreparedFactualObservationPool&&) noexcept;
    auto operator=(const PreparedFactualObservationPool&)
        -> PreparedFactualObservationPool& = delete;
    auto operator=(PreparedFactualObservationPool&&)
        -> PreparedFactualObservationPool& = delete;
    ~PreparedFactualObservationPool() noexcept;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedFactualObservationPool,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&, const wire_v1::CalculatedEvidenceCapacity&,
        const detail::PairwiseHostCapacityRequestTranscript&) noexcept
        -> std::expected<PreparedFactualObservationPool, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(
        std::size_t ordinal, std::span<const char> canonical_body,
        std::span<const PreparedParsedNode> nodes,
        const PreparedParsedNode& factual_observation,
        const measurement::ScheduledObservationSlot& authenticated_slot,
        std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
        -> std::expected<FactualObservationEvidence, ActiveCodecFailure>;

  private:
    [[nodiscard]] static auto prepare_impl(const prepared_resources_v1::PreparedResourceValue&,
        const wire_v1::CalculatedEvidenceCapacity*, const detail::PairwiseHostCapacityRequestTranscript*) noexcept
        -> std::expected<PreparedFactualObservationPool, prepared_resources_v1::PreparedResourceError>;
    struct Storage;
    explicit PreparedFactualObservationPool(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
};

// Prepared owner for the complete block ledger.  Block, factual-observation,
// failed-observation, and durable-error ordinals are bound while replaying the
// authenticated four-slot schedule; all vectors and nested builders are
// prepared before build().
class PreparedBlockPool final {
  public:
    PreparedBlockPool(const PreparedBlockPool&) = delete;
    PreparedBlockPool(PreparedBlockPool&&) noexcept = default;
    auto operator=(const PreparedBlockPool&) -> PreparedBlockPool& = delete;
    auto operator=(PreparedBlockPool&&) -> PreparedBlockPool& = delete;
    ~PreparedBlockPool() noexcept;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&, const wire_v1::CalculatedEvidenceCapacity&,
        const detail::PairwiseHostCapacityRequestTranscript&) noexcept
        -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(
        std::size_t ordinal, std::span<const char> canonical_body,
        std::span<const PreparedParsedNode> nodes, const PreparedParsedNode& block,
        std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
        -> std::expected<PairwiseBlockAttemptEvidence, ActiveCodecFailure>;

  private:
    [[nodiscard]] static auto prepare_impl(const prepared_resources_v1::PreparedResourceValue&,
        const wire_v1::CalculatedEvidenceCapacity*, const detail::PairwiseHostCapacityRequestTranscript*) noexcept
        -> std::expected<PreparedBlockPool, prepared_resources_v1::PreparedResourceError>;
    struct Storage;
    explicit PreparedBlockPool(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
};

// Prepared terminal slice.  It owns every allocation-bearing stopping,
// operational-error, and finalization-worker destination before activation.
// build() replays the block/stopping/terminal/finalization invariants against
// the already reconstructed header and block ledger, without constructing a
// DOM or growing a container.
class PreparedTerminalReconstruction final {
  public:
    PreparedTerminalReconstruction(const PreparedTerminalReconstruction&) = delete;
    PreparedTerminalReconstruction(PreparedTerminalReconstruction&&) noexcept = default;
    auto operator=(const PreparedTerminalReconstruction&)
        -> PreparedTerminalReconstruction& = delete;
    auto operator=(PreparedTerminalReconstruction&&)
        -> PreparedTerminalReconstruction& = delete;
    ~PreparedTerminalReconstruction() noexcept;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedTerminalReconstruction,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(
        std::span<const char> canonical_body,
        std::span<const PreparedParsedNode> nodes,
        const wire_v1::detail::ReconstructedEvidenceHeader& header,
        std::span<const PairwiseBlockAttemptEvidence> blocks) noexcept
        -> std::expected<std::pair<std::vector<RunnerStoppingEvaluationEvidence>,
                                   wire_v1::detail::ReconstructedTerminalAndFinalization>,
                         ActiveCodecFailure>;
    [[nodiscard]] auto build(
        std::span<const char> canonical_body,
        std::span<const PreparedParsedNode> nodes,
        const StatisticsProductionPolicy& statistics_policy,
        const measurement::RepetitionPolicy& repetition_policy,
        core::ExperimentId experiment_id,
        std::span<const PairwiseBlockAttemptEvidence> blocks) noexcept
        -> std::expected<std::pair<std::vector<RunnerStoppingEvaluationEvidence>,
                                   wire_v1::detail::ReconstructedTerminalAndFinalization>,
                         ActiveCodecFailure>;

  private:
    struct Storage;
    explicit PreparedTerminalReconstruction(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
};

// Complete inactive controller-side typed reconstruction. Preparation owns
// every cursor, typed-builder, ledger, and re-encoding destination before
// activation. build() is one-use: success returns the finalized production
// ledger only after exact byte-equal typed re-encoding; every failure consumes
// the capability and leaves destruction to ordinary RAII.
class PreparedFinalizedEvidenceReconstruction final {
  public:
    PreparedFinalizedEvidenceReconstruction(
        const PreparedFinalizedEvidenceReconstruction&) = delete;
    PreparedFinalizedEvidenceReconstruction(
        PreparedFinalizedEvidenceReconstruction&&) noexcept;
    auto operator=(const PreparedFinalizedEvidenceReconstruction&)
        -> PreparedFinalizedEvidenceReconstruction& = delete;
    auto operator=(PreparedFinalizedEvidenceReconstruction&&)
        -> PreparedFinalizedEvidenceReconstruction& = delete;
    ~PreparedFinalizedEvidenceReconstruction() noexcept;

    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&) noexcept
        -> std::expected<PreparedFinalizedEvidenceReconstruction,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] static auto prepare(
        const prepared_resources_v1::PreparedResourceValue&,
        const wire_v1::CalculatedEvidenceCapacity&,
        const detail::PairwiseHostCapacityRequestTranscript&) noexcept
        -> std::expected<PreparedFinalizedEvidenceReconstruction,
                         prepared_resources_v1::PreparedResourceError>;
    [[nodiscard]] auto build(
        std::span<const char> canonical_body,
        std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
        -> std::expected<FinalizedPairwiseRunEvidence, ActiveCodecFailure>;

  private:
    [[nodiscard]] static auto prepare_impl(
        const prepared_resources_v1::PreparedResourceValue&,
        const wire_v1::CalculatedEvidenceCapacity*,
        const detail::PairwiseHostCapacityRequestTranscript*) noexcept
        -> std::expected<PreparedFinalizedEvidenceReconstruction,
                         prepared_resources_v1::PreparedResourceError>;
    struct Storage;
    explicit PreparedFinalizedEvidenceReconstruction(
        std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_;
};

[[nodiscard]] auto write_finalized_evidence(PreparedCanonicalWriter&,
                                            const FinalizedPairwiseRunEvidence&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_policy_fields(PreparedCanonicalWriter&,
    const StatisticsProductionPolicy&, const monitoring::MonitoringExecutionPolicy&,
    const measurement::RepetitionPolicy&, const measurement::ComparisonOrderPolicy&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_terminal(PreparedCanonicalWriter&,
    const NormalRunTerminalEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_terminal(PreparedCanonicalWriter&,
    const OperationalRunTerminalEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_calibration(PreparedCanonicalWriter&,
    const PairwiseCalibrationEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_warmup(PreparedCanonicalWriter&,
    const PairwiseWarmupEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_block_plan(PreparedCanonicalWriter&,
    const measurement::MeasurementBlockPlan&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_failed_observation(PreparedCanonicalWriter&,
    const FailedObservationEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_progress_finalization(PreparedCanonicalWriter&,
    const PairwiseFinalizationEvidence&) noexcept -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto
write_calibration_warmup(PreparedCanonicalWriter&,
                         const wire_v1::detail::ReconstructedCalibrationWarmup&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_measurement_validity(PreparedCanonicalWriter&,
                                              const ReconstructedMeasurementValidity&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto
write_monitoring(PreparedCanonicalWriter&,
                 const std::optional<monitoring::MonitoringCaptureResult>&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_finalized_error_value(PreparedCanonicalWriter&,
                                                const FinalizedEvidenceError&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_memory_diagnostic(
    PreparedCanonicalWriter&, const memory::RegionFinalizationDiagnostic&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_factual_observation(
    PreparedCanonicalWriter&, const FactualObservationEvidence&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_block_attempt(
    PreparedCanonicalWriter&, const PairwiseBlockAttemptEvidence&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_stopping_evaluation(
    PreparedCanonicalWriter&, const RunnerStoppingEvaluationEvidence&) noexcept
    -> std::expected<void, ActiveCodecFailure>;
[[nodiscard]] auto write_terminal_and_finalization(
    PreparedCanonicalWriter&,
    const wire_v1::detail::ReconstructedTerminalAndFinalization&) noexcept
    -> std::expected<void, ActiveCodecFailure>;

} // namespace epycbench::runtime::prepared_codec_v1
