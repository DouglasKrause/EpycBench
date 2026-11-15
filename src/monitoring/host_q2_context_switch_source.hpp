#pragma once
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_controlled_event_runtime_bridge.hpp"
#include "platform_linux/perf_context_switch_access.hpp"
#include <array>
#include <variant>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2SourcePreparationOutcome;
struct MonitoringQ2ContextDrainCursor final {
    std::size_t committed_records{};
    std::uint64_t ordinal{};
};
struct MonitoringQ2ContextDrainResult final {
    std::span<const platform_linux::PerfContextSwitchRecord> records;
    std::uint64_t ordinal{};
};
struct MonitoringQ2RawClockOperations final {
    void* context{};
    std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*){};
};
class MonitoringQ2ContextSourceFinalization final {
  public:
    MonitoringQ2ContextSourceFinalization(MonitoringQ2ContextSourceFinalization&& other) noexcept
        : coverage_start_ns_{other.coverage_start_ns_}, coverage_end_ns_{other.coverage_end_ns_},
          decoded_records_{other.decoded_records_}, drain_count_{other.drain_count_},
          binding_{std::move(other.binding_)}, tid_{other.tid_},
          deferred_state_{std::move(other.deferred_state_)},
          consumed_{std::exchange(other.consumed_, true)} {}
    MonitoringQ2ContextSourceFinalization(const MonitoringQ2ContextSourceFinalization&) = delete;
    ~MonitoringQ2ContextSourceFinalization() noexcept {
        if (!consumed_)
            std::terminate();
    }
    auto coverage_start_ns() const noexcept { return coverage_start_ns_; }
    auto coverage_end_ns() const noexcept { return coverage_end_ns_; }
    auto decoded_records() const noexcept { return decoded_records_; }
    auto drain_count() const noexcept { return drain_count_; }

  private:
    MonitoringQ2ContextSourceFinalization(
        std::uint64_t start, std::uint64_t end, std::uint64_t records, std::uint64_t drains,
        MonitoringQ2WorkerBinding binding, platform_linux::LinuxThreadId tid,
        platform_linux::detail::Q2PerfRetiredState deferred_state = {}) noexcept
        : coverage_start_ns_{start}, coverage_end_ns_{end}, decoded_records_{records},
          drain_count_{drains}, binding_{std::move(binding)}, tid_{tid},
          deferred_state_{std::move(deferred_state)} {}
    std::uint64_t coverage_start_ns_{};
    std::uint64_t coverage_end_ns_{};
    std::uint64_t decoded_records_{};
    std::uint64_t drain_count_{};
    MonitoringQ2WorkerBinding binding_;
    platform_linux::LinuxThreadId tid_;
    platform_linux::detail::Q2PerfRetiredState deferred_state_;
    bool consumed_{};
    friend class MonitoringQ2ContextSwitchSourceOwner;
    friend class MonitoringQ2WorkerSession;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2ContextSwitchCollector;
};
class MonitoringQ2ContextSourceFailure final {
  public:
    MonitoringQ2ContextSourceFailure(MonitoringQ2ContextSourceFailure&& other) noexcept
        : primary_{std::move(other.primary_)}, cleanup_{std::move(other.cleanup_)},
          cleanup_count_{other.cleanup_count_}, binding_{std::move(other.binding_)},
          tid_{other.tid_}, deferred_state_{std::move(other.deferred_state_)},
          consumed_{std::exchange(other.consumed_, true)} {}
    MonitoringQ2ContextSourceFailure(const MonitoringQ2ContextSourceFailure&) = delete;
    ~MonitoringQ2ContextSourceFailure() noexcept {
        if (!consumed_)
            std::terminate();
    }

  private:
    MonitoringQ2ContextSourceFailure(
        core::Error primary, std::array<std::optional<core::Error>, 2> cleanup,
        std::size_t cleanup_count, MonitoringQ2WorkerBinding binding,
        platform_linux::LinuxThreadId tid,
        platform_linux::detail::Q2PerfRetiredState deferred_state = {}) noexcept
        : primary_{std::move(primary)}, cleanup_{std::move(cleanup)}, cleanup_count_{cleanup_count},
          binding_{std::move(binding)}, tid_{tid}, deferred_state_{std::move(deferred_state)} {}
    core::Error primary_;
    std::array<std::optional<core::Error>, 2> cleanup_{};
    std::size_t cleanup_count_{};
    MonitoringQ2WorkerBinding binding_;
    platform_linux::LinuxThreadId tid_;
    platform_linux::detail::Q2PerfRetiredState deferred_state_;
    bool consumed_{};
    friend class MonitoringQ2ContextSwitchSourceOwner;
    friend class MonitoringQ2WorkerSession;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2ContextSwitchCollector;
};
class MonitoringQ2ContextSwitchSourceOwner final {
  public:
    struct PreparationTestHook final {
        void* context{};
        void (*before_bridge_adoption)(void*){};
    };
    static auto prepare_worker(MonitoringQ2SourcePreparationWorkerHandoff&&,
                               const MonitoringQ2PlanValue&, std::uint64_t,
                               platform_linux::detail::Q2PerfContextSwitchOperations) noexcept
        -> MonitoringQ2SourcePreparationOutcome;
    static auto prepare_worker(MonitoringQ2SourcePreparationWorkerHandoff&&,
                               const MonitoringQ2PlanValue&, std::uint64_t,
                               platform_linux::detail::Q2PerfContextSwitchOperations,
                               PreparationTestHook) noexcept
        -> MonitoringQ2SourcePreparationOutcome;
    static auto prepare(const MonitoringQ2WorkerBinding&, platform_linux::LinuxThreadId,
                        const MonitoringQ2PlanValue&, std::uint64_t first_page_size,
                        MonitoringQ2PageSizeRecheck&&,
                        platform_linux::detail::Q2PerfContextSwitchOperations) noexcept
        -> std::expected<MonitoringQ2ContextSwitchSourceOwner,
                         platform_linux::detail::Q2PerfPrepareFailure>;
    static auto prepare_production(const MonitoringQ2WorkerBinding&, platform_linux::LinuxThreadId,
                                   const MonitoringQ2PlanValue&, std::uint64_t,
                                   MonitoringQ2PageSizeRecheck&&) noexcept
        -> std::expected<MonitoringQ2ContextSwitchSourceOwner,
                         platform_linux::detail::Q2PerfPrepareFailure>;
    MonitoringQ2ContextSwitchSourceOwner(MonitoringQ2ContextSwitchSourceOwner&&) noexcept;
    MonitoringQ2ContextSwitchSourceOwner(const MonitoringQ2ContextSwitchSourceOwner&) = delete;
    ~MonitoringQ2ContextSwitchSourceOwner() noexcept;
    auto drain(std::span<platform_linux::PerfContextSwitchRecord>,
               MonitoringQ2ContextDrainCursor&) noexcept
        -> std::expected<MonitoringQ2ContextDrainResult, core::Error>;
    // The uncommitted storage suffix is scratch; every previewed record remains
    // pending for its scheduled drain. No published cursor/count is changed.
    auto preview_pending(std::span<platform_linux::PerfContextSwitchRecord>,
                         const MonitoringQ2ContextDrainCursor&) noexcept
        -> std::expected<std::span<const platform_linux::PerfContextSwitchRecord>, core::Error>;
    auto start_coverage_after_empty(MonitoringQ2RawClockOperations) noexcept
        -> std::expected<void, core::Error>;
    auto final_drain(std::span<platform_linux::PerfContextSwitchRecord>,
                     MonitoringQ2ContextDrainCursor&, MonitoringQ2RawClockOperations) noexcept
        -> std::expected<MonitoringQ2ContextDrainResult, core::Error>;
    using Terminalization =
        std::variant<MonitoringQ2ContextSourceFinalization, MonitoringQ2ContextSourceFailure>;
    auto terminalize() noexcept -> Terminalization;

  private:
    explicit MonitoringQ2ContextSwitchSourceOwner(
        platform_linux::detail::Q2PerfContextSwitchSource source, MonitoringQ2WorkerBinding,
        platform_linux::LinuxThreadId, std::uint64_t record_cap, std::uint64_t records_per_drain,
        core::Error cursor_error, core::Error coverage_error, core::Error retention_error,
        core::Error terminal_state_error) noexcept;
    std::optional<platform_linux::detail::Q2PerfContextSwitchSource> source_;
    std::uint64_t record_cap_{};
    std::uint64_t records_per_drain_{};
    std::uint64_t retained_{};
    std::uint64_t drains_{};
    MonitoringQ2WorkerBinding binding_;
    platform_linux::LinuxThreadId tid_;
    void retain_operational_failure(const core::Error&) noexcept;
    std::optional<core::Error> cursor_error_, coverage_error_, operational_failure_;
    core::Error retention_error_, terminal_state_error_;
    std::optional<platform_linux::RawMonotonicTimestamp> coverage_start_, coverage_end_;
    bool initial_empty_{}, coverage_started_{}, final_committed_{}, terminalized_{},
        drain_decode_failed_{};
};
enum class MonitoringQ2UnsupportedClassification : std::uint8_t {
    permission_denied,
    feature_unavailable,
};
struct MonitoringQ2PreparedSourceWorker final {
    MonitoringQ2ContextSwitchSourceOwner source;
    MonitoringQ2ControlledEventRuntimeBridge bridge;
    Q2TargetSnapshot target;
};
struct MonitoringQ2UnsupportedSourceWorker final {
    MonitoringQ2UnsupportedClassification classification;
    platform_linux::detail::Q2PerfPrepareFailure detail;
    Q2TargetSnapshot target;
};
struct MonitoringQ2FailedSourceWorker final {
    platform_linux::detail::Q2PerfPrepareFailure detail;
    Q2TargetSnapshot target;
};
class MonitoringQ2SourcePreparationOutcome final {
  public:
    using Value = std::variant<MonitoringQ2PreparedSourceWorker,
                               MonitoringQ2UnsupportedSourceWorker, MonitoringQ2FailedSourceWorker>;
    explicit MonitoringQ2SourcePreparationOutcome(Value value) noexcept
        : value_{std::move(value)} {}
    MonitoringQ2SourcePreparationOutcome(MonitoringQ2SourcePreparationOutcome&&) noexcept = default;
    MonitoringQ2SourcePreparationOutcome(const MonitoringQ2SourcePreparationOutcome&) = delete;
    auto value() noexcept -> Value& { return value_; }

  private:
    Value value_;
};
} // namespace epycbench::monitoring::qualification::detail
