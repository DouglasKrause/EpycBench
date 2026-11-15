#pragma once

#include "epycbench/results/journal_records.hpp"
#include "epycbench/results/pairwise_run_result.hpp"
#include "epycbench/runtime/admitted_placement_request.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"
#include "runtime/finalized_evidence_wire_capacity.hpp"

#include <array>
#include <memory>

namespace epycbench::results::snapshot {

struct Limits {
    journal::Limits outer;
    std::size_t retained_bytes;
};
// Conservative simultaneous canonical-text reservation, not allocator overhead.
[[nodiscard]] auto required_retained_bytes(std::size_t scenario_bytes, std::size_t plan_bytes,
                                           std::size_t evidence_bytes,
                                           std::size_t outer_body_bytes) noexcept
    -> std::optional<std::size_t>;

class Context {
  public:
    struct State;
    [[nodiscard]] static auto prepare(journal::RunId, const scenario::ScenarioDigestedPlan&,
                                      const runtime::AdmittedPairwiseProductionRequest&,
                                      const runtime::wire_v1::CalculatedEvidenceCapacity&, Limits)
        -> std::expected<Context, core::Error>;
    Context(Context&&) noexcept;
    ~Context();
    Context(const Context&) = delete;
    auto operator=(Context&&) -> Context& = delete;
    auto operator=(const Context&) -> Context& = delete;

  private:
    explicit Context(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
    friend class Capture;
    friend class Replay;
};

class Capture {
  public:
    explicit Capture(Context);
    Capture(Capture&&) noexcept;
    ~Capture();
    Capture(const Capture&) = delete;
    auto operator=(Capture&&) -> Capture& = delete;
    auto operator=(const Capture&) -> Capture& = delete;
    [[nodiscard]] auto finish(const runtime::FinalizedPairwiseRunEvidence&,
                              const std::array<journal::Timestamp, 3>&)
        -> std::expected<std::array<std::string, 3>, core::Error>;

  private:
    Context context_;
};

enum class Completion { complete_snapshot, incomplete_snapshot };
struct Outcome {
    Completion completion;
    journal::Summary framing;
    std::array<std::optional<journal::Record>, 3> prefix;
    std::optional<PairwiseRunResult> result;
};
class Replay {
  public:
    struct State;
    [[nodiscard]] static auto create(Context) -> std::expected<Replay, core::Error>;
    Replay(Replay&&) noexcept;
    ~Replay();
    Replay(const Replay&) = delete;
    auto operator=(Replay&&) -> Replay& = delete;
    auto operator=(const Replay&) -> Replay& = delete;
    [[nodiscard]] auto feed(std::string_view) -> std::expected<std::size_t, core::Error>;
    [[nodiscard]] auto finish() -> std::expected<Outcome, core::Error>;

  private:
    explicit Replay(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
};
} // namespace epycbench::results::snapshot
