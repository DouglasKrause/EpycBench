#include "monitoring/host_q2_context_switch_source.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto failure(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Q2 context-switch source rejected",
            .operation = "monitoring_q2_context_switch_source",
            .os_error = std::nullopt,
            .context = {},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
} // namespace
MonitoringQ2ContextSwitchSourceOwner::MonitoringQ2ContextSwitchSourceOwner(
    platform_linux::detail::Q2PerfContextSwitchSource source, MonitoringQ2WorkerBinding binding,
    platform_linux::LinuxThreadId tid, std::uint64_t cap, std::uint64_t per_drain,
    core::Error cursor_error, core::Error coverage_error, core::Error retention_error,
    core::Error terminal_state_error) noexcept
    : source_{std::move(source)}, record_cap_{cap}, records_per_drain_{per_drain},
      binding_{std::move(binding)}, tid_{tid}, cursor_error_{std::move(cursor_error)},
      coverage_error_{std::move(coverage_error)}, retention_error_{std::move(retention_error)},
      terminal_state_error_{std::move(terminal_state_error)} {}
MonitoringQ2ContextSwitchSourceOwner::MonitoringQ2ContextSwitchSourceOwner(
    MonitoringQ2ContextSwitchSourceOwner&& other) noexcept
    : source_{std::move(other.source_)}, record_cap_{other.record_cap_},
      records_per_drain_{other.records_per_drain_}, retained_{other.retained_},
      drains_{other.drains_}, binding_{std::move(other.binding_)}, tid_{other.tid_},
      cursor_error_{std::move(other.cursor_error_)},
      coverage_error_{std::move(other.coverage_error_)},
      operational_failure_{std::move(other.operational_failure_)},
      retention_error_{std::move(other.retention_error_)},
      terminal_state_error_{std::move(other.terminal_state_error_)},
      coverage_start_{other.coverage_start_}, coverage_end_{other.coverage_end_},
      initial_empty_{other.initial_empty_}, coverage_started_{other.coverage_started_},
      final_committed_{other.final_committed_}, terminalized_{other.terminalized_},
      drain_decode_failed_{other.drain_decode_failed_} {
    other.source_.reset();
    other.terminalized_ = true;
}
MonitoringQ2ContextSwitchSourceOwner::~MonitoringQ2ContextSwitchSourceOwner() noexcept {
    if (source_ && !terminalized_)
        std::terminate();
}
void MonitoringQ2ContextSwitchSourceOwner::retain_operational_failure(
    const core::Error& value) noexcept {
    if (operational_failure_)
        return;
    try {
        operational_failure_.emplace(value);
    } catch (...) {
        operational_failure_.emplace(std::move(retention_error_));
    }
}
auto MonitoringQ2ContextSwitchSourceOwner::prepare_production(
    const MonitoringQ2WorkerBinding& binding, platform_linux::LinuxThreadId tid,
    const MonitoringQ2PlanValue& plan, std::uint64_t first_page,
    MonitoringQ2PageSizeRecheck&& token) noexcept
    -> std::expected<MonitoringQ2ContextSwitchSourceOwner,
                     platform_linux::detail::Q2PerfPrepareFailure> {
    return prepare(binding, tid, plan, first_page, std::move(token),
                   platform_linux::detail::q2_perf_production_provider().operations());
}
auto MonitoringQ2ContextSwitchSourceOwner::prepare_worker(
    MonitoringQ2SourcePreparationWorkerHandoff&& handoff, const MonitoringQ2PlanValue& plan,
    std::uint64_t first_page,
    platform_linux::detail::Q2PerfContextSwitchOperations operations) noexcept
    -> MonitoringQ2SourcePreparationOutcome {
    return prepare_worker(std::move(handoff), plan, first_page, operations, {});
}
auto MonitoringQ2ContextSwitchSourceOwner::prepare_worker(
    MonitoringQ2SourcePreparationWorkerHandoff&& handoff, const MonitoringQ2PlanValue& plan,
    std::uint64_t first_page, platform_linux::detail::Q2PerfContextSwitchOperations operations,
    PreparationTestHook hook) noexcept -> MonitoringQ2SourcePreparationOutcome {
    auto token = handoff.take_page_recheck();
    std::optional<Q2TargetSnapshot> target;
    std::optional<MonitoringQ2WorkerBinding> binding;
    try {
        target.emplace(handoff.target_);
        binding.emplace(MonitoringQ2WorkerBinding{target->worker, target->cpu, target->core,
                                                  target->pu, target->stable_core,
                                                  target->stable_pu});
    } catch (...) {
        static_cast<void>(token.consume());
        handoff.fail();
        return MonitoringQ2SourcePreparationOutcome{MonitoringQ2FailedSourceWorker{
            platform_linux::detail::Q2PerfPrepareFailure{
                platform_linux::detail::Q2PerfPrepareFailure::Stage::construction,
                core::Error{},
                {},
                {}},
            std::move(handoff.target_)}};
    }
    auto prepared = prepare(*binding, target->tid, plan, first_page, std::move(token), operations);
    if (!prepared) {
        auto detail = std::move(prepared.error());
        const auto code = detail.primary.code.value();
        const bool clean_open =
            detail.stage == platform_linux::detail::Q2PerfPrepareFailure::Stage::open &&
            !detail.cleanup && !detail.cleanup2;
        handoff.fail();
        if (clean_open && code == "monitoring.q2.context.permission_denied")
            return MonitoringQ2SourcePreparationOutcome{MonitoringQ2UnsupportedSourceWorker{
                MonitoringQ2UnsupportedClassification::permission_denied, std::move(detail),
                std::move(*target)}};
        if (clean_open && code == "monitoring.q2.context.feature_unavailable")
            return MonitoringQ2SourcePreparationOutcome{MonitoringQ2UnsupportedSourceWorker{
                MonitoringQ2UnsupportedClassification::feature_unavailable, std::move(detail),
                std::move(*target)}};
        return MonitoringQ2SourcePreparationOutcome{
            MonitoringQ2FailedSourceWorker{std::move(detail), std::move(*target)}};
    }
    auto bridge = MonitoringQ2ControlledEventRuntimeBridge::adopt_existing_borrow(
        std::move(handoff), hook.context, hook.before_bridge_adoption);
    if (!bridge) {
        auto primary = std::move(bridge.error());
        auto terminal = prepared->terminalize();
        std::optional<core::Error> cleanup;
        std::optional<core::Error> cleanup2;
        if (auto* failure = std::get_if<MonitoringQ2ContextSourceFailure>(&terminal)) {
            cleanup.emplace(std::move(failure->primary_));
            if (failure->cleanup_count_ != 0U)
                cleanup2.emplace(std::move(*failure->cleanup_[0]));
            failure->consumed_ = true;
        } else {
            auto& final = std::get<MonitoringQ2ContextSourceFinalization>(terminal);
            final.consumed_ = true;
        }
        handoff.fail();
        return MonitoringQ2SourcePreparationOutcome{MonitoringQ2FailedSourceWorker{
            platform_linux::detail::Q2PerfPrepareFailure{
                platform_linux::detail::Q2PerfPrepareFailure::Stage::construction,
                std::move(primary), std::move(cleanup), std::move(cleanup2)},
            std::move(*target)}};
    }
    return MonitoringQ2SourcePreparationOutcome{MonitoringQ2PreparedSourceWorker{
        std::move(*prepared), std::move(*bridge), std::move(*target)}};
}
auto MonitoringQ2ContextSwitchSourceOwner::prepare(
    const MonitoringQ2WorkerBinding& binding, platform_linux::LinuxThreadId tid,
    const MonitoringQ2PlanValue& plan, std::uint64_t first_page,
    MonitoringQ2PageSizeRecheck&& token,
    platform_linux::detail::Q2PerfContextSwitchOperations operations) noexcept
    -> std::expected<MonitoringQ2ContextSwitchSourceOwner,
                     platform_linux::detail::Q2PerfPrepareFailure> {
    auto page = token.consume();
    if (!page)
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            platform_linux::detail::Q2PerfPrepareFailure::Stage::validation,
            std::move(page.error()),
            {},
            {}}};
    std::optional<MonitoringQ2WorkerBinding> retained_binding;
    std::optional<core::Error> allocation_error;
    std::optional<core::Error> cursor_error, coverage_error, retention_error, terminal_state_error;
    try {
        retained_binding.emplace(binding);
        allocation_error.emplace(failure("monitoring.q2.context.allocation"));
        cursor_error.emplace(failure("monitoring.q2.context.cursor"));
        coverage_error.emplace(failure("monitoring.q2.context.coverage"));
        retention_error.emplace(failure("monitoring.q2.context.failure_retention"));
        terminal_state_error.emplace(failure("monitoring.q2.context.not_finalized"));
    } catch (...) {
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            platform_linux::detail::Q2PerfPrepareFailure::Stage::construction,
            core::Error{},
            {},
            {}}};
    }
    if (plan.ring_data_pages == 0 || (plan.ring_data_pages & (plan.ring_data_pages - 1U)) != 0 ||
        plan.ring_data_pages > 65536 || plan.total_decoded_records == 0 ||
        plan.records_per_drain == 0 || plan.raw_in_memory_bytes == 0 || first_page == 0 ||
        first_page > 65536U || (first_page & (first_page - 1U)) != 0U ||
        plan.ring_data_pages + 1U > std::numeric_limits<std::uint64_t>::max() / first_page ||
        plan.total_decoded_records > std::numeric_limits<std::uint64_t>::max() /
                                         sizeof(platform_linux::PerfContextSwitchRecord) ||
        plan.raw_in_memory_bytes <
            plan.total_decoded_records * sizeof(platform_linux::PerfContextSwitchRecord) ||
        (plan.ring_data_pages + 1U) * first_page >
            plan.raw_in_memory_bytes -
                plan.total_decoded_records * sizeof(platform_linux::PerfContextSwitchRecord))
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            platform_linux::detail::Q2PerfPrepareFailure::Stage::validation,
            failure("monitoring.q2.context.invalid_limits"),
            {},
            {}}};
    if (*page != first_page || *page == 0 || *page > 65536U || (*page & (*page - 1U)) != 0U ||
        *page > std::numeric_limits<std::size_t>::max() ||
        plan.ring_data_pages > std::numeric_limits<std::size_t>::max() ||
        plan.ring_data_pages + 1U > std::numeric_limits<std::uint64_t>::max() / *page ||
        (plan.ring_data_pages + 1U) * *page > plan.raw_in_memory_bytes)
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            platform_linux::detail::Q2PerfPrepareFailure::Stage::validation,
            failure("monitoring.q2.context.capacity"),
            {},
            {}}};
    auto source = platform_linux::detail::Q2PerfContextSwitchSource::prepare(
        tid, static_cast<std::size_t>(plan.ring_data_pages), static_cast<std::size_t>(*page),
        operations);
    if (!source) {
        auto prepared_failure = std::move(source.error());
        auto error = std::move(prepared_failure.primary);
        if (prepared_failure.stage == platform_linux::detail::Q2PerfPrepareFailure::Stage::open &&
            error.os_error) {
            const auto value = error.os_error->value;
            if (value == EPERM || value == EACCES) {
                auto classified = failure("monitoring.q2.context.permission_denied");
                classified.os_error = std::move(error.os_error);
                return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
                    prepared_failure.stage, std::move(classified),
                    std::move(prepared_failure.cleanup), std::move(prepared_failure.cleanup2)}};
            }
            if (value == ENOSYS || value == EOPNOTSUPP || value == ENODEV) {
                auto classified = failure("monitoring.q2.context.feature_unavailable");
                classified.os_error = std::move(error.os_error);
                return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
                    prepared_failure.stage, std::move(classified),
                    std::move(prepared_failure.cleanup), std::move(prepared_failure.cleanup2)}};
            }
        }
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            prepared_failure.stage, std::move(error), std::move(prepared_failure.cleanup),
            std::move(prepared_failure.cleanup2)}};
    }
    try {
        return MonitoringQ2ContextSwitchSourceOwner{std::move(*source),
                                                    std::move(*retained_binding),
                                                    tid,
                                                    plan.total_decoded_records,
                                                    plan.records_per_drain,
                                                    std::move(*cursor_error),
                                                    std::move(*coverage_error),
                                                    std::move(*retention_error),
                                                    std::move(*terminal_state_error)};
    } catch (...) {
        auto cleanup = source->retire();
        return std::unexpected{platform_linux::detail::Q2PerfPrepareFailure{
            platform_linux::detail::Q2PerfPrepareFailure::Stage::construction,
            std::move(*allocation_error), std::move(cleanup.primary), std::move(cleanup.cleanup)}};
    }
}
auto MonitoringQ2ContextSwitchSourceOwner::drain(
    std::span<platform_linux::PerfContextSwitchRecord> storage,
    MonitoringQ2ContextDrainCursor& cursor) noexcept
    -> std::expected<MonitoringQ2ContextDrainResult, core::Error> {
    if (!source_ || terminalized_ || final_committed_)
        std::terminate();
    if (storage.size() < cursor.committed_records) {
        retain_operational_failure(*cursor_error_);
        return std::unexpected{std::move(*cursor_error_)};
    }
    const auto remaining = record_cap_ - retained_;
    auto available = storage.subspan(
        cursor.committed_records,
        std::min({storage.size() - cursor.committed_records, static_cast<std::size_t>(remaining),
                  static_cast<std::size_t>(records_per_drain_)}));
    auto decoded = source_->drain_bounded(available);
    if (!decoded) {
        drain_decode_failed_ = true;
        retain_operational_failure(decoded.error());
        return std::unexpected{std::move(decoded.error())};
    }
    if (decoded->completeness != platform_linux::PerfCaptureCompleteness::complete) {
        auto value = failure("monitoring.q2.context.incomplete");
        retain_operational_failure(value);
        return std::unexpected{std::move(value)};
    }
    const auto start = cursor.committed_records;
    cursor.committed_records += decoded->record_count;
    ++cursor.ordinal;
    ++drains_;
    retained_ += decoded->record_count;
    if (!coverage_started_ && decoded->record_count == 0)
        initial_empty_ = true;
    return MonitoringQ2ContextDrainResult{storage.subspan(start, decoded->record_count),
                                          cursor.ordinal};
}
auto MonitoringQ2ContextSwitchSourceOwner::preview_pending(
    std::span<platform_linux::PerfContextSwitchRecord> storage,
    const MonitoringQ2ContextDrainCursor& cursor) noexcept
    -> std::expected<std::span<const platform_linux::PerfContextSwitchRecord>, core::Error> {
    if (!source_ || terminalized_ || final_committed_ || !coverage_started_)
        std::terminate();
    if (storage.size() < cursor.committed_records || retained_ > record_cap_) {
        retain_operational_failure(*cursor_error_);
        return std::unexpected{std::move(*cursor_error_)};
    }
    const auto remaining = record_cap_ - retained_;
    auto available = storage.subspan(
        cursor.committed_records,
        std::min({storage.size() - cursor.committed_records, static_cast<std::size_t>(remaining),
                  static_cast<std::size_t>(records_per_drain_)}));
    auto decoded = source_->preview_bounded(available);
    if (!decoded) {
        drain_decode_failed_ = true;
        retain_operational_failure(decoded.error());
        return std::unexpected{std::move(decoded.error())};
    }
    return std::span<const platform_linux::PerfContextSwitchRecord>{available}.first(
        decoded->record_count);
}
auto MonitoringQ2ContextSwitchSourceOwner::start_coverage_after_empty(
    MonitoringQ2RawClockOperations clock) noexcept -> std::expected<void, core::Error> {
    if (!initial_empty_ || coverage_started_ || !clock.now) {
        retain_operational_failure(*coverage_error_);
        return std::unexpected{std::move(*coverage_error_)};
    }
    std::optional<std::expected<platform_linux::RawMonotonicTimestamp, core::Error>> now;
    try {
        now.emplace(clock.now(clock.context));
    } catch (...) {
        retain_operational_failure(*coverage_error_);
        return std::unexpected{std::move(*coverage_error_)};
    }
    if (!now->has_value()) {
        retain_operational_failure(now->error());
        return std::unexpected{std::move(now->error())};
    }
    coverage_start_.emplace(**now);
    coverage_started_ = true;
    return {};
}
auto MonitoringQ2ContextSwitchSourceOwner::final_drain(
    std::span<platform_linux::PerfContextSwitchRecord> storage,
    MonitoringQ2ContextDrainCursor& cursor, MonitoringQ2RawClockOperations clock) noexcept
    -> std::expected<MonitoringQ2ContextDrainResult, core::Error> {
    if (!coverage_started_ || final_committed_ || !clock.now) {
        retain_operational_failure(*coverage_error_);
        return std::unexpected{std::move(*coverage_error_)};
    }
    std::optional<std::expected<platform_linux::RawMonotonicTimestamp, core::Error>> boundary;
    try {
        boundary.emplace(clock.now(clock.context));
    } catch (...) {
        retain_operational_failure(*coverage_error_);
        return std::unexpected{std::move(*coverage_error_)};
    }
    if (!boundary->has_value()) {
        retain_operational_failure(boundary->error());
        return std::unexpected{std::move(boundary->error())};
    }
    if (drain_decode_failed_) {
        const auto remaining = record_cap_ - retained_;
        auto available = storage.subspan(
            cursor.committed_records,
            std::min({storage.size() - cursor.committed_records,
                      static_cast<std::size_t>(remaining),
                      static_cast<std::size_t>(records_per_drain_)}));
        auto terminal = source_->terminal_drain_after_failure(available);
        return std::unexpected{std::move(terminal.error())};
    }
    auto committed = drain(storage, cursor);
    if (!committed)
        return committed;
    coverage_end_.emplace(**boundary);
    final_committed_ = true;
    return committed;
}
auto MonitoringQ2ContextSwitchSourceOwner::terminalize() noexcept -> Terminalization {
    if (terminalized_ || !source_)
        std::terminate();
    auto result = source_->retire();
    source_.reset();
    terminalized_ = true;
    if (!operational_failure_ && !result.primary && !result.cleanup && final_committed_ &&
        coverage_start_ && coverage_end_)
        return MonitoringQ2ContextSourceFinalization{coverage_start_->nanoseconds(),
                                                     coverage_end_->nanoseconds(),
                                                     retained_,
                                                     drains_,
                                                     binding_,
                                                     tid_,
                                                     std::move(result.deferred_state)};
    std::array<std::optional<core::Error>, 2> cleanup{};
    std::size_t count{};
    core::Error primary = operational_failure_ ? std::move(*operational_failure_)
                          : result.primary     ? std::move(*result.primary)
                                               : std::move(terminal_state_error_);
    if (operational_failure_ && result.primary)
        cleanup[count++] = std::move(*result.primary);
    if (result.cleanup)
        cleanup[count++] = std::move(*result.cleanup);
    return MonitoringQ2ContextSourceFailure{std::move(primary),
                                            std::move(cleanup),
                                            count,
                                            binding_,
                                            tid_,
                                            std::move(result.deferred_state)};
}
} // namespace epycbench::monitoring::qualification::detail
