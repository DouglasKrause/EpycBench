#pragma once

#include "monitoring/host_q2_context_switch_source.hpp"
#include "monitoring/host_q2_terminal_containment.hpp"

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ2TrialKind : std::uint8_t { voluntary, preemption };
struct MonitoringQ2ContextSwitchTrial final {
    core::WorkerId worker{0};
    MonitoringQ2TrialKind kind{};
    std::uint64_t ordinal{}, start_ns{}, end_ns{}, drain_ordinal{};
    std::uint64_t voluntary_count{}, involuntary_count{}, scoped_records{};
    bool capture_complete{};
};
struct MonitoringQ2RequiredEventDiagnostic final {
    MonitoringQ2TrialKind kind{};
    core::WorkerId worker{0};
    std::uint64_t ordinal{}, voluntary_count{}, involuntary_count{}, scoped_records{};
};
struct MonitoringQ2DurationSummary final {
    std::uint64_t count{}, minimum_ns{}, lower_median_ns{}, maximum_ns{};
};
struct MonitoringQ2ContextSwitchSummary final {
    std::optional<core::WorkerId> worker;
    std::uint64_t voluntary_trials{}, preemption_trials{}, drains{}, decoded_records{};
    MonitoringQ2DurationSummary prepare_duration, drain_duration;
};
class MonitoringQ2ContextSwitchCollection final {
  public:
    MonitoringQ2ContextSwitchCollection(MonitoringQ2ContextSwitchCollection&&) noexcept = default;
    MonitoringQ2ContextSwitchCollection(const MonitoringQ2ContextSwitchCollection&) = delete;
    auto plan() const noexcept -> const MonitoringQ2CollectorPlan& { return plan_; }
    auto trials() const noexcept -> std::span<const MonitoringQ2ContextSwitchTrial> {
        return trials_;
    }
    auto summaries() const noexcept -> std::span<const MonitoringQ2ContextSwitchSummary> {
        return summaries_;
    }
    auto global_summary() const noexcept -> const MonitoringQ2ContextSwitchSummary& {
        return global_;
    }

  private:
    MonitoringQ2ContextSwitchCollection(MonitoringQ2CollectorPlan plan,
                                        std::vector<MonitoringQ2ContextSwitchTrial> trials,
                                        std::vector<MonitoringQ2ContextSwitchSummary> summaries,
                                        MonitoringQ2ContextSwitchSummary global) noexcept
        : plan_{std::move(plan)}, trials_{std::move(trials)}, summaries_{std::move(summaries)},
          global_{std::move(global)} {}
    MonitoringQ2CollectorPlan plan_;
    std::vector<MonitoringQ2ContextSwitchTrial> trials_;
    std::vector<MonitoringQ2ContextSwitchSummary> summaries_;
    MonitoringQ2ContextSwitchSummary global_;
    friend class MonitoringQ2ContextSwitchCollector;
    friend class MonitoringQ2EvidenceTestAccess;
};
struct MonitoringQ2UnsupportedCollection final {
    MonitoringQ2CollectorPlan plan;
    MonitoringQ2UnsupportedClassification classification;
    platform_linux::detail::Q2PerfPrepareFailure detail;
};
struct MonitoringQ2FailedCollection final {
    static constexpr std::size_t maximum_cleanup = 64;
    MonitoringQ2CollectionStage stage;
    core::Error primary;
    std::array<std::optional<core::Error>, maximum_cleanup> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
    std::optional<MonitoringQ2RequiredEventDiagnostic> required_event{};
};
using MonitoringQ2ContextSwitchCollectionOutcome =
    std::variant<MonitoringQ2ContextSwitchCollection, MonitoringQ2UnsupportedCollection,
                 MonitoringQ2FailedCollection, MonitoringQ2TerminalContainment>;
struct MonitoringQ2CollectorOperations final {
    void* context{};
    std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*) noexcept {};
    platform_linux::detail::Q2PerfContextSwitchOperations source{};
    Q2HelperOperations helper{};
    std::expected<void, core::Error> (*activate_test)(
        void*, MonitoringQ2WorkerSession&, MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<MonitoringQ2SourcePreparationWorkerHandoff, core::Error> (*begin_source_test)(
        void*, MonitoringQ2WorkerSession&, std::size_t,
        const MonitoringQ2WorkerBinding&) noexcept {};
    bool (*cancellation_test)(void*, std::size_t) noexcept {};
};
class MonitoringQ2ContextSwitchCollector final {
  public:
    static auto collect(MonitoringQ2CollectorPlan&&) noexcept
        -> MonitoringQ2ContextSwitchCollectionOutcome;

  private:
    static auto collect_with(MonitoringQ2CollectorPlan&&, MonitoringQ2CollectorOperations,
                             std::optional<MonitoringQ2HelperProvider> = {}) noexcept
        -> MonitoringQ2ContextSwitchCollectionOutcome;
    friend class MonitoringQ2CollectorTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
