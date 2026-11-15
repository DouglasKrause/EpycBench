#pragma once

#include "results/expected_pair_evidence_context.hpp"
#include "results/observation_timing_admission.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"
#include "runtime/finalized_evidence_wire_logical_codec.hpp"

namespace epycbench::results::block_evidence {
enum class Domain : std::uint8_t { supplied_observation, synthetic_execution };
enum class EventKind : std::uint8_t { opened, factual, failed, closed };
struct Limits final {
    std::size_t events, canonical_bytes, nodes, strings, depth, workers, array_elements;
    std::size_t frequency_cpu_memberships, typed_bytes, simultaneous_bytes;
};
namespace detail {
// Pure checked decision shared with the defensive closed-copy capacity proof.
constexpr auto closed_copy_fits(std::size_t retained, std::size_t copy, std::size_t limit) noexcept -> bool {
    return retained <= limit && copy <= limit - retained;
}
}
struct ComponentUsage final {
    runtime::wire_v1::detail::ComponentWireShape wire;
    std::size_t typed_payload{}, classifier_scratch{}, projected_dom{}, retained_bytes{};
    std::size_t phase1{}, phase2a{}, phase2b{};
};
// Only internally supplied closed diagnostic paths are normalized into this
// record. There is no JSON key or other candidate text in it.
class Component final {
  public:
    struct State;
    Component() noexcept;
    Component(Component&&) noexcept;
    auto operator=(Component&&) noexcept -> Component&;
    ~Component();
    Component(const Component&) = delete;
    auto operator=(const Component&) -> Component& = delete;
    [[nodiscard]] auto available() const noexcept -> bool { return bool(state_); }
    [[nodiscard]] auto kind() const noexcept -> runtime::wire_v1::EvidenceComponentKind;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto usage() const noexcept -> ComponentUsage;
    [[nodiscard]] auto plan() const noexcept -> const measurement::MeasurementBlockPlan*;
    [[nodiscard]] auto factual() const noexcept -> const runtime::FactualObservationEvidence*;
    [[nodiscard]] auto failed() const noexcept -> const runtime::FailedObservationEvidence*;
  private:
    explicit Component(std::unique_ptr<State>) noexcept;
    static auto admit(const PairFactsLease&, runtime::wire_v1::EvidenceComponentKind,
        const Component*, std::size_t, std::string_view, Limits, std::size_t owned)
        -> std::expected<Component, Error>;
    std::unique_ptr<State> state_;
    friend class Accumulator;
};
struct Event final {
    std::uint64_t sequence{};
    std::optional<journal::Timestamp> timestamp;
    EventKind kind{EventKind::opened};
    Component component;
    std::optional<runtime::BlockAttemptCompletion> completion;
    std::optional<observation_timing::Owner> timing;
};
class Accumulator final {
  public:
    struct State;
    static auto prepare(ExpectedPairContext&&, Domain, Limits) -> std::expected<Accumulator, Error>;
    static auto prepare(PairFactsLease&&, Domain, Limits) -> std::expected<Accumulator, Error>;
    Accumulator(Accumulator&&) noexcept;
    auto operator=(Accumulator&&) noexcept -> Accumulator&;
    ~Accumulator();
    Accumulator(const Accumulator&) = delete;
    auto operator=(const Accumulator&) -> Accumulator& = delete;
    [[nodiscard]] auto admit_plan(std::string_view) -> std::expected<Component, Error>;
    [[nodiscard]] auto admit_factual(std::string_view) -> std::expected<Component, Error>;
    [[nodiscard]] auto admit_failed(std::string_view) -> std::expected<Component, Error>;
    [[nodiscard]] auto append(std::uint64_t, const journal::Timestamp&, Component&&)
        -> std::expected<void, Error>;
    [[nodiscard]] auto close(std::uint64_t, const journal::Timestamp&, runtime::BlockAttemptCompletion)
        -> std::expected<void, Error>;
    [[nodiscard]] auto copy_closed_block() -> std::expected<runtime::PairwiseBlockAttemptEvidence, Error>;
    [[nodiscard]] auto events() const noexcept -> std::span<const Event>;
    [[nodiscard]] auto consumed() const noexcept -> bool;
    [[nodiscard]] auto closed() const noexcept -> bool;
    [[nodiscard]] auto domain() const noexcept -> Domain;
    [[nodiscard]] auto retained_bytes() const noexcept -> std::size_t;
    [[nodiscard]] static auto timing_staging_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto legacy_timing_staging_bytes() noexcept -> std::size_t;
  private:
    explicit Accumulator(std::unique_ptr<State>) noexcept;
    static auto base_stock(const PairFactsLease&) noexcept -> std::optional<std::size_t>;
    void residual_limits(std::size_t retained, std::size_t simultaneous) noexcept;
    friend class PairPrefix;
    auto append_impl(std::uint64_t, const journal::Timestamp&, Component&&,
        observation_timing::Input*, const observation_timing::Usage*, std::uint64_t)
        -> std::expected<void, Error>;
    auto admit(runtime::wire_v1::EvidenceComponentKind, std::string_view) -> std::expected<Component, Error>;
    std::unique_ptr<State> state_;
};
} // namespace epycbench::results::block_evidence
