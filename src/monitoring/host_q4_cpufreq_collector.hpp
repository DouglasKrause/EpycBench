#pragma once

#include "monitoring/host_q4_cpufreq_cycle_authority.hpp"
#include "epycbench/core/error.hpp"
#include <optional>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ4CpuFreqInterval final {
    std::uint64_t start_ns{}, end_ns{};
    auto operator==(const MonitoringQ4CpuFreqInterval&) const noexcept -> bool = default;
};
template<class Fact> struct MonitoringQ4CpuFreqTimedFact final {
    std::uint64_t cycle_ordinal{};
    MonitoringQ4CpuFreqInterval interval;
    Fact fact;
};
enum class MonitoringQ4CpuFreqCollectionFailureKind : unsigned char {
    operations, allocation, capacity, clock, deadline, semantic_drift, cycle, contained
};
struct MonitoringQ4CpuFreqCollectionFailure final {
    MonitoringQ4CpuFreqCollectionFailureKind kind{};
    std::uint64_t cycle_ordinal{};
    std::optional<core::Error> clock;
    std::optional<MonitoringQ4CpuFreqCycleFailure> cycle;
    std::optional<MonitoringQ4CpuFreqCycleFailure> cleanup;
    std::optional<MonitoringQ4CpuFreqCleanupLedger> ledger;
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
};
class MonitoringQ4CpuFreqCollection final {
  public:
    MonitoringQ4CpuFreqCollection(const MonitoringQ4CpuFreqCollection&) = delete;
    MonitoringQ4CpuFreqCollection(MonitoringQ4CpuFreqCollection&&) noexcept = default;
    [[nodiscard]] auto unsupported() const noexcept { return unsupported_; }
    [[nodiscard]] auto cycles() const noexcept { return std::span{cycles_}; }
    [[nodiscard]] auto discoveries() const noexcept { return std::span{discoveries_}; }
    [[nodiscard]] auto preparations() const noexcept { return std::span{preparations_}; }
    [[nodiscard]] auto raw_reads() const noexcept { return std::span{raw_}; }
    [[nodiscard]] auto typed_reads() const noexcept { return std::span{typed_}; }
    [[nodiscard]] auto cleanup() const noexcept -> const MonitoringQ4CpuFreqCleanupLedger& {
        return ledger_;
    }
  private:
    MonitoringQ4CpuFreqCollection(std::shared_ptr<const void> binding, bool unsupported,
        std::vector<MonitoringQ4CpuFreqCycleResult> cycles,
        std::vector<MonitoringQ4CpuFreqInterval> discoveries,
        std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCyclePreparationFact>> preparations,
        std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCycleReadFact>> raw,
        std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCycleReadFact>> typed,
        MonitoringQ4CpuFreqCleanupLedger ledger) noexcept
        : binding_{std::move(binding)}, unsupported_{unsupported}, cycles_{std::move(cycles)}, discoveries_{std::move(discoveries)},
          preparations_{std::move(preparations)}, raw_{std::move(raw)}, typed_{std::move(typed)},
          ledger_{std::move(ledger)} {}
    std::shared_ptr<const void> binding_;
    bool projected_{};
    bool unsupported_{};
    std::vector<MonitoringQ4CpuFreqCycleResult> cycles_;
    std::vector<MonitoringQ4CpuFreqInterval> discoveries_;
    std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCyclePreparationFact>> preparations_;
    std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCycleReadFact>> raw_, typed_;
    MonitoringQ4CpuFreqCleanupLedger ledger_;
    friend class MonitoringQ4CpuFreqCollector;
    friend class MonitoringQ4CpuFreqEvidenceBuilder;
};
class MonitoringQ4CpuFreqCollectorTestAccess;
class MonitoringQ4CpuFreqCollector final {
  public:
    [[nodiscard]] static auto collect(const MonitoringQ4SourceReady&) noexcept
        -> std::expected<MonitoringQ4CpuFreqCollection, MonitoringQ4CpuFreqCollectionFailure>;
  private:
    struct Operations final {
        void* clock_context{};
        std::expected<std::uint64_t, core::Error> (*now)(void*) noexcept{};
        MonitoringQ4CpuFreqInventoryAuthority::Operations source;
        void* fault_context{};
        void (*fault)(void*, unsigned){};
        bool force_capacity_failure{};
    };
    static auto collect_with(const MonitoringQ4SourceReady&, Operations) noexcept
        -> std::expected<MonitoringQ4CpuFreqCollection, MonitoringQ4CpuFreqCollectionFailure>;
    static auto classify_cleanup_ledger_failure(MonitoringQ4CpuFreqCycleFailureKind) noexcept
        -> MonitoringQ4CpuFreqCollectionFailureKind;
    friend class MonitoringQ4CpuFreqCollectorTestAccess;
};
}
