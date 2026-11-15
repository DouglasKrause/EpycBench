#pragma once

#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "epycbench/runtime/runner_control_monitor.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ1FaultCounters final {
    std::uint64_t minor_faults{};
    std::uint64_t major_faults{};
    auto operator<=>(const MonitoringQ1FaultCounters&) const noexcept = default;
};

enum class MonitoringQ1FaultCounterKind : std::uint8_t { minor_faults, major_faults };

struct MonitoringQ1OracleMismatch final {
    std::uint64_t cycle_index{};
    core::WorkerId worker_id{0};
    MonitoringQ1FaultCounterKind counter_kind{};
    std::uint64_t before{};
    std::uint64_t production{};
    std::uint64_t after{};
    auto operator<=>(const MonitoringQ1OracleMismatch&) const noexcept = default;
};

struct MonitoringQ1PageFaultObservation final {
    std::uint64_t cycle_index{};
    core::WorkerId worker_id{0};
    std::uint64_t collection_start_ns{};
    std::uint64_t collection_end_ns{};
    std::uint64_t elapsed_ns{};
    MonitoringQ1FaultCounters oracle_before{};
    MonitoringQ1FaultCounters production{};
    MonitoringQ1FaultCounters oracle_after{};
    bool minor_equal{};
    bool major_equal{};
    auto operator<=>(const MonitoringQ1PageFaultObservation&) const noexcept = default;
};

struct MonitoringQ1PageFaultSummary final {
    std::optional<core::WorkerId> worker_id;
    std::uint64_t count{};
    std::uint64_t minimum_elapsed_ns{};
    std::uint64_t maximum_elapsed_ns{};
    std::uint64_t lower_median_elapsed_ns{};
    std::uint64_t minor_equality_count{};
    std::uint64_t major_equality_count{};
    auto operator<=>(const MonitoringQ1PageFaultSummary&) const noexcept = default;
};

class MonitoringQ1PageFaultCollection final {
  public:
    MonitoringQ1PageFaultCollection(const MonitoringQ1PageFaultCollection&) = delete;
    MonitoringQ1PageFaultCollection(MonitoringQ1PageFaultCollection&&) noexcept = default;
    auto operator=(const MonitoringQ1PageFaultCollection&)
        -> MonitoringQ1PageFaultCollection& = delete;
    auto operator=(MonitoringQ1PageFaultCollection&&)
        -> MonitoringQ1PageFaultCollection& = delete;

    [[nodiscard]] auto plan() const noexcept -> const MonitoringQ1CollectorPlan& { return plan_; }
    [[nodiscard]] auto observations() const noexcept
        -> std::span<const MonitoringQ1PageFaultObservation> { return observations_; }
    [[nodiscard]] auto worker_summaries() const noexcept
        -> std::span<const MonitoringQ1PageFaultSummary> { return workers_; }
    [[nodiscard]] auto global_summary() const noexcept -> const MonitoringQ1PageFaultSummary& {
        return global_;
    }

  private:
    MonitoringQ1PageFaultCollection(MonitoringQ1CollectorPlan,
                                    std::vector<MonitoringQ1PageFaultObservation>,
                                    std::vector<MonitoringQ1PageFaultSummary>,
                                    MonitoringQ1PageFaultSummary) noexcept;
    MonitoringQ1CollectorPlan plan_;
    std::vector<MonitoringQ1PageFaultObservation> observations_;
    std::vector<MonitoringQ1PageFaultSummary> workers_;
    MonitoringQ1PageFaultSummary global_;
    friend class MonitoringQ1PageFaultCollectorAccess;
};

enum class MonitoringQ1PageFaultCollectionStage : std::uint8_t {
    preparation,
    oracle,
    production_source,
    clock,
    association,
    timing,
    finalization
};

class MonitoringQ1PageFaultCollectionFailure final {
  public:
    [[nodiscard]] auto stage() const noexcept -> MonitoringQ1PageFaultCollectionStage {
        return stage_;
    }
    [[nodiscard]] auto primary() const noexcept -> const core::Error& { return primary_; }
    [[nodiscard]] auto secondary() const noexcept -> std::span<const core::Error> {
        return secondary_;
    }
    [[nodiscard]] auto oracle_mismatch() const noexcept
        -> const std::optional<MonitoringQ1OracleMismatch>& { return oracle_mismatch_; }

  private:
    MonitoringQ1PageFaultCollectionFailure(MonitoringQ1PageFaultCollectionStage, core::Error,
                                           std::vector<core::Error>,
                                           std::optional<MonitoringQ1OracleMismatch> = std::nullopt) noexcept;
    MonitoringQ1PageFaultCollectionStage stage_;
    core::Error primary_;
    std::vector<core::Error> secondary_;
    std::optional<MonitoringQ1OracleMismatch> oracle_mismatch_;
    friend class MonitoringQ1PageFaultCollectorAccess;
};

[[nodiscard]] auto collect_monitoring_q1_page_faults(
    MonitoringQ1CollectorPlan&&, runtime::RunnerCancellation&) noexcept
    -> std::expected<MonitoringQ1PageFaultCollection, MonitoringQ1PageFaultCollectionFailure>;

} // namespace epycbench::monitoring::qualification::detail
