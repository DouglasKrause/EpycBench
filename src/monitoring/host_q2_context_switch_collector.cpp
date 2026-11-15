#include "monitoring/host_q2_context_switch_collector.hpp"
#include "epycbench/measurement/empty_harness.hpp"
#include "monitoring/host_q2_helper_provider.hpp"
#include <algorithm>
#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
struct MonotonicCollectorClock final {
    MonitoringQ2RawClockOperations underlying;
    std::optional<core::Error>* reversal{};
    std::optional<std::uint64_t> last;
    bool failed{};
};
auto monotonic_now(void* raw) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    auto& state = *static_cast<MonotonicCollectorClock*>(raw);
    auto value = state.underlying.now(state.underlying.context);
    if (!value)
        return value;
    if (!state.failed && state.last && value->nanoseconds() < *state.last) {
        state.failed = true;
        return std::unexpected{std::move(**state.reversal)};
    }
    state.last = value->nanoseconds();
    return value;
}
auto error(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Q2 collection failed",
            .operation = "monitoring_q2_context_switch_collector",
            .os_error = std::nullopt,
            .context = {},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto deadline(std::uint64_t now, std::uint64_t ms)
    -> std::optional<MonitoringQ2DeadlineCancellationContext> {
    if (ms > std::numeric_limits<std::uint64_t>::max() / 1000000ULL)
        return {};
    const auto delta = ms * 1000000ULL;
    if (now > std::numeric_limits<std::uint64_t>::max() - delta)
        return {};
    return MonitoringQ2DeadlineCancellationContext{{now + delta}, {}};
}
auto ok(Q2EventStatus value) noexcept { return value == Q2EventStatus::ok; }
auto summarize(std::span<const std::uint64_t> values, std::span<std::uint64_t> scratch) noexcept
    -> MonitoringQ2DurationSummary {
    if (values.empty())
        return {};
    if (scratch.size() < values.size())
        std::terminate();
    auto ordered = scratch.first(values.size());
    std::copy(values.begin(), values.end(), ordered.begin());
    std::sort(ordered.begin(), ordered.end());
    return {static_cast<std::uint64_t>(ordered.size()), ordered.front(),
            ordered[(ordered.size() - 1U) / 2U], ordered.back()};
}
} // namespace
auto MonitoringQ2ContextSwitchCollector::collect(MonitoringQ2CollectorPlan&& input) noexcept
    -> MonitoringQ2ContextSwitchCollectionOutcome {
    auto helper = MonitoringQ2HelperProvider::create();
    if (!helper)
        return MonitoringQ2FailedCollection{
            MonitoringQ2CollectionStage::preparation, std::move(helper.error()), {}, 0, false};
    const auto clock = measurement::detail::production_raw_monotonic_clock_operations();
    auto operations = helper->operations();
    return collect_with(std::move(input),
                        {clock.context,
                         clock.now,
                         platform_linux::detail::q2_perf_production_provider().operations(),
                         operations,
                         {},
                         {}},
                        std::optional<MonitoringQ2HelperProvider>{std::move(*helper)});
}
auto MonitoringQ2ContextSwitchCollector::collect_with(
    MonitoringQ2CollectorPlan&& input, MonitoringQ2CollectorOperations ops,
    std::optional<MonitoringQ2HelperProvider> helper_provider) noexcept
    -> MonitoringQ2ContextSwitchCollectionOutcome {
    MonitoringQ2WorkerSession* active_session{};
    MonitoringQ2ContextSwitchSourceOwner* active_source{};
    MonitoringQ2ControlledEventRuntimeBridge* active_bridge{};
    MonitoringQ2ControlledEventSession* active_controlled{};
    MonitoringQ2QuiescedWorkerBorrow* active_borrow{};
    Q2ControlledFailure* active_retirement_failure{};
    MonitoringQ2TerminalContainmentStock* active_stock{};
    MonitoringQ2ContextDrainCursor* active_cursor{};
    std::span<platform_linux::PerfContextSwitchRecord> active_records{};
    MonitoringQ2RawClockOperations active_clock{};
    bool active_coverage{};
    MonitoringQ2DeadlineCancellationContext fallback_deadline{
        {std::numeric_limits<std::uint64_t>::max()}, {}};
    MonitoringQ2DeadlineCancellationContext* active_deadline{&fallback_deadline};
    std::uint64_t active_generation{};
    static constexpr std::array active_error_codes{
        "monitoring.q2.collector.generation_abort",
        "monitoring.q2.collector.clock",
        "monitoring.q2.collector.clock_reversal",
        "monitoring.q2.collector.controlled",
        "monitoring.q2.collector.deadline",
        "monitoring.q2.collector.helper",
        "monitoring.q2.collector.preemption",
        "monitoring.q2.collector.preemption_begin",
        "monitoring.q2.collector.preemption_compete",
        "monitoring.q2.collector.preemption_ready",
        "monitoring.q2.collector.preemption_release_helper",
        "monitoring.q2.collector.preemption_release_target",
        "monitoring.q2.collector.preemption_stop",
        "monitoring.q2.collector.preemption_target_ready",
        "monitoring.q2.collector.preemption_target_running",
        "monitoring.q2.collector.preemption_target_terminal",
        "monitoring.q2.collector.preemption_terminal",
        "monitoring.q2.collector.required_event",
        "monitoring.q2.collector.retirement",
        "monitoring.q2.collector.source",
        "monitoring.q2.collector.voluntary_begin",
        "monitoring.q2.collector.voluntary_finish",
        "monitoring.q2.collector.voluntary_ready",
        "monitoring.q2.collector.voluntary_release",
        "monitoring.q2.collector.voluntary_resumed",
        "monitoring.q2.collector.voluntary_wake",
        "monitoring.q2.collector.voluntary_event_deadline",
        "monitoring.q2.collector.voluntary_event_cancelled",
        "monitoring.q2.collector.contained"};
    std::array<std::optional<core::Error>, active_error_codes.size()> active_errors;
    try {
        for (std::size_t i{}; i < active_errors.size(); ++i)
            active_errors[i].emplace(error(active_error_codes[i]));
    } catch (...) {
        if (!input.worker_session_)
            std::terminate();
        auto primary = std::move(input.worker_session_->activation_error_);
        auto finalized = input.worker_session_->finalize(false, fallback_deadline);
        input.worker_session_.reset();
        MonitoringQ2FailedCollection failure{
            MonitoringQ2CollectionStage::preparation, std::move(primary), {}, 0U, false};
        const auto append = [&](core::Error diagnostic) {
            if (failure.cleanup_count < failure.cleanup.size())
                failure.cleanup[failure.cleanup_count++].emplace(std::move(diagnostic));
            else
                failure.cleanup_overflowed = true;
        };
        if (finalized.primary)
            append(std::move(*finalized.primary));
        for (auto& diagnostic : finalized.cleanup)
            append(std::move(diagnostic));
        return failure;
    }
    const auto active_error = [&](std::string_view code) -> core::Error {
        for (std::size_t i{}; i < active_error_codes.size(); ++i)
            if (active_error_codes[i] == code && active_errors[i])
                return std::move(*std::exchange(active_errors[i], std::nullopt));
        std::terminate();
    };
    MonotonicCollectorClock observed_clock{
        {ops.context, ops.now}, &active_errors[2], std::nullopt, false};
    const MonitoringQ2RawClockOperations collection_clock{&observed_clock, monotonic_now};
    auto fail = [&](MonitoringQ2CollectionStage stage, core::Error primary,
                    std::optional<core::Error> initial_cleanup = {},
                    std::optional<core::Error> initial_cleanup2 = {}) {
        MonitoringQ2FailedCollection failure{stage, std::move(primary), {}, 0, false};
        const auto append = [&](core::Error value) {
            if (failure.cleanup_count < failure.cleanup.size())
                failure.cleanup[failure.cleanup_count++].emplace(std::move(value));
            else
                failure.cleanup_overflowed = true;
        };
        const auto duplicate_primary = [&](const core::Error& value) {
            return value.category == failure.primary.category &&
                   value.code.value() == failure.primary.code.value() &&
                   value.operation == failure.primary.operation &&
                   (!value.os_error == !failure.primary.os_error) &&
                   (!value.os_error || value.os_error->value == failure.primary.os_error->value);
        };
        if (initial_cleanup)
            append(std::move(*initial_cleanup));
        if (initial_cleanup2)
            append(std::move(*initial_cleanup2));
        if (active_controlled && active_bridge && active_source && active_session &&
            active_deadline) {
            if (active_generation != 0U) {
                auto aborted = active_controlled->abort(
                    active_generation, active_error("monitoring.q2.collector.generation_abort"),
                    *active_deadline);
                if (!aborted) {
                    auto diagnostics = std::move(aborted.error());
                    if (diagnostics.has_primary)
                        append(std::move(diagnostics.primary));
                    for (std::size_t i{}; i < diagnostics.cleanup_count; ++i)
                        append(std::move(*diagnostics.cleanup[i]));
                }
                active_generation = 0U;
            }
            auto retired = active_bridge->retire(std::move(*active_controlled), *active_deadline);
            active_controlled = nullptr;
            if (auto* recovery = std::get_if<MonitoringQ2BridgeRetirementRecovery>(&retired)) {
                if (!active_stock)
                    std::terminate();
                return MonitoringQ2ContextSwitchCollectionOutcome{
                    MonitoringQ2TerminalContainment::contain(
                        std::move(*active_stock), std::move(*recovery), std::move(*active_source),
                        std::move(input), stage, std::move(failure.primary),
                        std::move(failure.cleanup), failure.cleanup_count,
                        failure.cleanup_overflowed, std::move(helper_provider))};
            }
            std::optional<MonitoringQ2QuiescedWorkerBorrow> borrow;
            if (auto* quiesced = std::get_if<MonitoringQ2QuiescedWorkerBorrow>(&retired))
                borrow.emplace(std::move(*quiesced));
            else {
                auto recovered = std::get<MonitoringQ2RecoveredWorkerFailure>(std::move(retired));
                if (recovered.failure.has_primary)
                    append(std::move(recovered.failure.primary));
                for (std::size_t i{}; i < recovered.failure.cleanup_count; ++i)
                    append(std::move(*recovered.failure.cleanup[i]));
                borrow.emplace(std::move(recovered.borrow));
            }
            if (active_coverage && active_cursor) {
                auto drained =
                    active_source->final_drain(active_records, *active_cursor, active_clock);
                if (!drained)
                    append(std::move(drained.error()));
                active_coverage = false;
            }
            auto terminal = active_source->terminalize();
            active_source = nullptr;
            active_bridge = nullptr;
            if (auto* source_failure = std::get_if<MonitoringQ2ContextSourceFailure>(&terminal))
                active_session->complete_worker_failure(
                    std::move(*borrow), std::move(*source_failure), *active_deadline);
            else {
                auto complete = active_session->complete_worker(
                    std::move(*borrow),
                    std::get<MonitoringQ2ContextSourceFinalization>(std::move(terminal)),
                    *active_deadline);
                if (!complete)
                    append(std::move(complete.error()));
            }
            auto finalized = active_session->finalize(false, *active_deadline);
            if (finalized.primary && !duplicate_primary(*finalized.primary))
                append(std::move(*finalized.primary));
            for (auto& item : finalized.cleanup)
                append(std::move(item));
            if (finalized.retirement_retryable) {
                active_session = nullptr;
                return MonitoringQ2ContextSwitchCollectionOutcome{
                    MonitoringQ2TerminalContainment::contain_session(
                        std::move(*active_stock), std::move(input), stage,
                        std::move(failure.primary), std::move(failure.cleanup),
                        failure.cleanup_count, failure.cleanup_overflowed, true,
                        std::move(helper_provider))};
            }
            active_session = nullptr;
        } else if (active_borrow && active_source && active_session && active_deadline) {
            if (active_coverage && active_cursor) {
                auto drained =
                    active_source->final_drain(active_records, *active_cursor, active_clock);
                if (!drained)
                    append(std::move(drained.error()));
                active_coverage = false;
            }
            if (active_retirement_failure) {
                auto terminal = active_source->terminalize();
                auto recovered = MonitoringQ2RecoveredWorkerFailure{
                    std::move(*active_borrow), std::move(*active_retirement_failure)};
                if (auto* source_failure =
                        std::get_if<MonitoringQ2ContextSourceFailure>(&terminal)) {
                    active_session->complete_worker_combined_failure(
                        std::move(recovered), std::move(*source_failure), *active_deadline);
                } else
                    active_session->complete_worker_combined_failure(
                        std::move(recovered),
                        std::get<MonitoringQ2ContextSourceFinalization>(std::move(terminal)),
                        *active_deadline);
                active_retirement_failure = nullptr;
            } else {
                auto terminal = active_source->terminalize();
                if (auto* source_failure = std::get_if<MonitoringQ2ContextSourceFailure>(&terminal))
                    active_session->complete_worker_failure(
                        std::move(*active_borrow), std::move(*source_failure), *active_deadline);
                else {
                    auto complete = active_session->complete_worker(
                        std::move(*active_borrow),
                        std::get<MonitoringQ2ContextSourceFinalization>(std::move(terminal)),
                        *active_deadline);
                    if (!complete)
                        append(std::move(complete.error()));
                }
            }
            active_borrow = nullptr;
            active_source = nullptr;
            auto finalized = active_session->finalize(false, *active_deadline);
            if (finalized.primary && !duplicate_primary(*finalized.primary))
                append(std::move(*finalized.primary));
            for (auto& item : finalized.cleanup)
                append(std::move(item));
            if (finalized.retirement_retryable) {
                active_session = nullptr;
                return MonitoringQ2ContextSwitchCollectionOutcome{
                    MonitoringQ2TerminalContainment::contain_session(
                        std::move(*active_stock), std::move(input), stage,
                        std::move(failure.primary), std::move(failure.cleanup),
                        failure.cleanup_count, failure.cleanup_overflowed, true,
                        std::move(helper_provider))};
            }
            active_session = nullptr;
        } else if (active_session && active_deadline) {
            auto finalized = active_session->finalize(false, *active_deadline);
            if (finalized.primary && !duplicate_primary(*finalized.primary))
                append(std::move(*finalized.primary));
            for (auto& item : finalized.cleanup)
                append(std::move(item));
            if (finalized.retirement_retryable) {
                active_session = nullptr;
                return MonitoringQ2ContextSwitchCollectionOutcome{
                    MonitoringQ2TerminalContainment::contain_session(
                        std::move(*active_stock), std::move(input), stage,
                        std::move(failure.primary), std::move(failure.cleanup),
                        failure.cleanup_count, failure.cleanup_overflowed, true,
                        std::move(helper_provider))};
            }
            active_session = nullptr;
        }
        return MonitoringQ2ContextSwitchCollectionOutcome{std::move(failure)};
    };
    try {
        if (!input.worker_session_ || !ops.now)
            return fail(MonitoringQ2CollectionStage::preparation,
                        error("monitoring.q2.collector.operations"));
        auto& session = *input.worker_session_;
        active_session = &session;
        const auto& value = input.plan_;
        const auto w = input.workers_.size();
        constexpr auto size_max = std::numeric_limits<std::size_t>::max();
        constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();
        if (w == 0 || w > value.maximum_worker_count || w == size_max ||
            value.voluntary_trials_per_worker > u64_max - value.preemption_trials_per_worker)
            return fail(MonitoringQ2CollectionStage::preparation,
                        error("monitoring.q2.collector.capacity"));
        const auto per = value.voluntary_trials_per_worker + value.preemption_trials_per_worker;
        if (per == 0 || per > size_max || w > size_max / static_cast<std::size_t>(per))
            return fail(MonitoringQ2CollectionStage::preparation,
                        error("monitoring.q2.collector.capacity"));
        const auto total = w * static_cast<std::size_t>(per);
        if (total > value.maximum_total_trials || value.drains > size_max ||
            value.total_decoded_records > size_max ||
            value.execution_deadline_seconds > u64_max / 1000ULL)
            return fail(MonitoringQ2CollectionStage::preparation,
                        error("monitoring.q2.collector.capacity"));
        std::vector<MonitoringQ2ContextSwitchTrial> trials;
        trials.reserve(total);
        std::vector<MonitoringQ2ContextSwitchSummary> summaries;
        summaries.reserve(w + 1U);
        std::vector<std::uint64_t> prepare_durations;
        prepare_durations.reserve(w);
        std::vector<std::uint64_t> drain_durations;
        drain_durations.reserve(static_cast<std::size_t>(value.drains));
        std::vector<std::uint64_t> duration_scratch(
            std::max(w, static_cast<std::size_t>(value.drains)));
        const auto empty_record = platform_linux::PerfContextSwitchRecord::create(
            platform_linux::RawMonotonicTimestamp::from_nanoseconds(0),
            platform_linux::PerfContextSwitchKind::switch_in);
        std::vector<platform_linux::PerfContextSwitchRecord> records(
            static_cast<std::size_t>(value.total_decoded_records), empty_record);
        auto containment_stock = MonitoringQ2TerminalContainment::reserve(
            error("monitoring.q2.collector.containment_reserve"));
        if (!containment_stock)
            return fail(MonitoringQ2CollectionStage::preparation,
                        std::move(containment_stock.error()));
        active_stock = &*containment_stock;
        auto initial = collection_clock.now(collection_clock.context);
        if (!initial)
            return fail(MonitoringQ2CollectionStage::clock, std::move(initial.error()));
        auto global = deadline(initial->nanoseconds(), value.execution_deadline_seconds * 1000ULL);
        if (!global)
            return fail(MonitoringQ2CollectionStage::preparation,
                        active_error("monitoring.q2.collector.deadline"));
        active_deadline = &*global;
        auto activated = ops.activate_test ? ops.activate_test(ops.context, session, *global)
                                           : session.activate(*global);
        if (!activated) {
            return fail(MonitoringQ2CollectionStage::session, std::move(activated.error()));
        }
        std::uint64_t drain_ordinal{};
        std::size_t record_cursor{};
        std::uint64_t generation{};
        for (std::size_t wi{}; wi < w; ++wi) {
            const auto worker_record_start = record_cursor;
            const auto worker_drain_start = drain_ordinal;
            const auto worker_drain_duration_start = drain_durations.size();
            auto handoff = ops.begin_source_test
                               ? ops.begin_source_test(ops.context, session, wi, input.workers_[wi])
                               : session.begin_source_preparation(wi, input.workers_[wi]);
            if (!handoff) {
                return fail(MonitoringQ2CollectionStage::association, std::move(handoff.error()));
            }
            auto prepare_start = collection_clock.now(collection_clock.context);
            if (!prepare_start) {
                handoff->cancel_before_prepare();
                return fail(MonitoringQ2CollectionStage::clock, std::move(prepare_start.error()));
            }
            auto prep = MonitoringQ2ContextSwitchSourceOwner::prepare_worker(
                std::move(*handoff), value, input.page_size_, ops.source);
            if (auto* unsupported =
                    std::get_if<MonitoringQ2UnsupportedSourceWorker>(&prep.value())) {
                if (wi != 0U)
                    return fail(MonitoringQ2CollectionStage::source_preparation,
                                std::move(unsupported->detail.primary));
                auto finalized = session.finalize(false, *global);
                active_session = nullptr;
                if (finalized.retirement_retryable) {
                    auto primary = finalized.primary
                                       ? std::move(*finalized.primary)
                                       : active_error("monitoring.q2.collector.retirement");
                    std::array<std::optional<core::Error>, 64> cleanup{};
                    const auto count = std::min(finalized.cleanup.size(), cleanup.size());
                    for (std::size_t index{}; index < count; ++index)
                        cleanup[index].emplace(std::move(finalized.cleanup[index]));
                    return MonitoringQ2ContextSwitchCollectionOutcome{
                        MonitoringQ2TerminalContainment::contain_session(
                            std::move(*containment_stock), std::move(input),
                            MonitoringQ2CollectionStage::finalization, std::move(primary),
                            std::move(cleanup), count, finalized.cleanup.size() > cleanup.size(),
                            false, std::move(helper_provider))};
                }
                if (finalized.primary || !finalized.cleanup.empty()) {
                    auto primary = finalized.primary ? std::move(*finalized.primary)
                                                     : std::move(finalized.cleanup.front());
                    MonitoringQ2FailedCollection failure{MonitoringQ2CollectionStage::finalization,
                                                         std::move(primary),
                                                         {},
                                                         0,
                                                         false};
                    const std::size_t first = finalized.primary ? 0U : 1U;
                    for (std::size_t i = first; i < finalized.cleanup.size() &&
                                                failure.cleanup_count < failure.cleanup.size();
                         ++i)
                        failure.cleanup[failure.cleanup_count++].emplace(
                            std::move(finalized.cleanup[i]));
                    return MonitoringQ2ContextSwitchCollectionOutcome{std::move(failure)};
                }
                input.worker_session_.reset();
                return MonitoringQ2UnsupportedCollection{
                    std::move(input), unsupported->classification, std::move(unsupported->detail)};
            }
            if (auto* failed = std::get_if<MonitoringQ2FailedSourceWorker>(&prep.value())) {
                return fail(MonitoringQ2CollectionStage::source_preparation,
                            std::move(failed->detail.primary), std::move(failed->detail.cleanup),
                            std::move(failed->detail.cleanup2));
            }
            auto prepared = std::get<MonitoringQ2PreparedSourceWorker>(std::move(prep.value()));
            auto fail_before_controlled = [&](MonitoringQ2CollectionStage stage,
                                              core::Error primary) {
                std::array<std::optional<core::Error>, 64> cleanup{};
                std::size_t cleanup_count{};
                bool cleanup_overflowed{};
                const auto append_cleanup = [&](core::Error diagnostic) {
                    if (cleanup_count < cleanup.size())
                        cleanup[cleanup_count++].emplace(std::move(diagnostic));
                    else
                        cleanup_overflowed = true;
                };
                auto borrow = prepared.bridge.retire_before_controlled_session();
                auto source = prepared.source.terminalize();
                if (auto* source_failure = std::get_if<MonitoringQ2ContextSourceFailure>(&source))
                    session.complete_worker_failure(std::move(borrow), std::move(*source_failure),
                                                    *global);
                else {
                    auto completed = session.complete_worker(
                        std::move(borrow),
                        std::get<MonitoringQ2ContextSourceFinalization>(std::move(source)),
                        *global);
                    if (!completed)
                        append_cleanup(std::move(completed.error()));
                }
                auto finalized = session.finalize(false, *global);
                if (finalized.primary)
                    append_cleanup(std::move(*finalized.primary));
                for (auto& item : finalized.cleanup)
                    append_cleanup(std::move(item));
                active_session = nullptr;
                if (finalized.retirement_retryable) {
                    return MonitoringQ2ContextSwitchCollectionOutcome{
                        MonitoringQ2TerminalContainment::contain_session(
                            std::move(*containment_stock), std::move(input), stage,
                            std::move(primary), std::move(cleanup), cleanup_count,
                            cleanup_overflowed, true, std::move(helper_provider))};
                }
                auto outcome = fail(stage, std::move(primary));
                auto& failed = std::get<MonitoringQ2FailedCollection>(outcome);
                for (std::size_t index{}; index < cleanup_count; ++index) {
                    if (failed.cleanup_count < failed.cleanup.size())
                        failed.cleanup[failed.cleanup_count++].emplace(std::move(*cleanup[index]));
                    else
                        failed.cleanup_overflowed = true;
                }
                failed.cleanup_overflowed = failed.cleanup_overflowed || cleanup_overflowed;
                return outcome;
            };
            auto prepare_end = collection_clock.now(collection_clock.context);
            if (!prepare_end) {
                return fail_before_controlled(MonitoringQ2CollectionStage::clock,
                                              std::move(prepare_end.error()));
            }
            if (prepare_end->nanoseconds() < prepare_start->nanoseconds()) {
                return fail_before_controlled(
                    MonitoringQ2CollectionStage::clock,
                    active_error("monitoring.q2.collector.clock_reversal"));
            }
            prepare_durations.push_back(prepare_end->nanoseconds() - prepare_start->nanoseconds());
            auto controlled = MonitoringQ2ControlledEventSession::create(
                input.workers_, wi, prepared.bridge.target_operations(), ops.helper);
            if (!controlled) {
                return fail_before_controlled(MonitoringQ2CollectionStage::session,
                                              active_error("monitoring.q2.collector.controlled"));
            }
            active_source = &prepared.source;
            active_bridge = &prepared.bridge;
            active_controlled = &*controlled;
            MonitoringQ2ContextDrainCursor cursor{record_cursor, drain_ordinal};
            active_cursor = &cursor;
            active_records = records;
            active_clock = collection_clock;
            auto timed_drain = [&](bool final) -> std::expected<void, core::Error> {
                auto before = collection_clock.now(collection_clock.context);
                if (!before)
                    return std::unexpected{std::move(before.error())};
                auto drained =
                    final ? prepared.source.final_drain(records, cursor, collection_clock)
                          : prepared.source.drain(records, cursor);
                if (!drained)
                    return std::unexpected{std::move(drained.error())};
                auto after = collection_clock.now(collection_clock.context);
                if (!after)
                    return std::unexpected{std::move(after.error())};
                if (after->nanoseconds() < before->nanoseconds())
                    return std::unexpected{active_error("monitoring.q2.collector.clock_reversal")};
                drain_durations.push_back(after->nanoseconds() - before->nanoseconds());
                drain_ordinal = cursor.ordinal;
                record_cursor = cursor.committed_records;
                return {};
            };
            auto drained = timed_drain(false);
            if (!drained) {
                return fail(MonitoringQ2CollectionStage::drain, std::move(drained.error()));
            }
            auto started = prepared.source.start_coverage_after_empty(collection_clock);
            if (!started) {
                return fail(MonitoringQ2CollectionStage::clock, std::move(started.error()));
            }
            active_coverage = true;
            std::size_t phase_start = trials.size();
            std::uint64_t group{};
            for (std::uint64_t ordinal{}; ordinal < value.voluntary_trials_per_worker; ++ordinal) {
                ++generation;
                auto n = collection_clock.now(collection_clock.context);
                if (!n)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(n.error()));
                auto trial = deadline(n->nanoseconds(), value.per_trial_deadline_ms);
                if (!trial)
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.deadline"));
                trial->deadline.nanoseconds =
                    std::min(trial->deadline.nanoseconds, global->deadline.nanoseconds);
                if (n->nanoseconds() >= global->deadline.nanoseconds)
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.deadline"));
                active_generation = generation;
                if (!ok(controlled->begin_voluntary(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_begin"));
                if (!ok(controlled->voluntary_ready(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_ready"));
                auto start = collection_clock.now(collection_clock.context);
                if (!start)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(start.error()));
                if (!ok(controlled->release_wait(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_release"));
                // A parked publication precedes the worker's native wait. Keep
                // its command unchanged until this source observes a real
                // voluntary switch-out in this trial; an early wake could
                // otherwise skip the wait entirely. Preview uses existing
                // uncommitted record storage and leaves scheduled drains intact.
                for (;;) {
                    auto observed = collection_clock.now(collection_clock.context);
                    if (!observed)
                        return fail(MonitoringQ2CollectionStage::clock, std::move(observed.error()));
                    if (observed->nanoseconds() >= trial->deadline.nanoseconds)
                        return fail(MonitoringQ2CollectionStage::voluntary_event,
                                    active_error("monitoring.q2.collector.voluntary_event_deadline"));
                    auto pending = prepared.source.preview_pending(records, cursor);
                    if (!pending)
                        return fail(MonitoringQ2CollectionStage::drain, std::move(pending.error()));
                    const bool cancelled = ops.cancellation_test
                        ? ops.cancellation_test(ops.context, wi)
                        : !session.native_runtime_ ||
                              session.native_runtime_->cancellation_requested(wi);
                    if (cancelled ||
                        (trial->cancellation.requested &&
                         trial->cancellation.requested(trial->cancellation.context)))
                        return fail(MonitoringQ2CollectionStage::voluntary_event,
                                    active_error("monitoring.q2.collector.voluntary_event_cancelled"));
                    if (std::ranges::any_of(*pending, [&](const auto& record) {
                            return record.kind() ==
                                       platform_linux::PerfContextSwitchKind::voluntary_switch_out &&
                                   record.timestamp().nanoseconds() >= start->nanoseconds() &&
                                   record.timestamp().nanoseconds() <= observed->nanoseconds();
                        }))
                        break;
                }
                if (!ok(controlled->wake(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_wake"));
                if (!ok(controlled->resumed(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_resumed"));
                auto end = collection_clock.now(collection_clock.context);
                if (!end)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(end.error()));
                if (end->nanoseconds() < start->nanoseconds() ||
                    (trials.size() > phase_start && start->nanoseconds() < trials.back().end_ns))
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.clock_reversal"));
                if (!ok(controlled->finish_voluntary(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::voluntary_event,
                                active_error("monitoring.q2.collector.voluntary_finish"));
                active_generation = 0U;
                trials.push_back({input.workers_[wi].worker, MonitoringQ2TrialKind::voluntary,
                                  ordinal, start->nanoseconds(), end->nanoseconds(), 0, 0, 0, 0,
                                  false});
                ++group;
                if (group == value.periodic_drain_after_trials) {
                    auto d = timed_drain(false);
                    if (!d)
                        return fail(MonitoringQ2CollectionStage::drain, std::move(d.error()));
                    for (auto i = trials.size() - static_cast<std::size_t>(group);
                         i < trials.size(); ++i)
                        trials[i].drain_ordinal = drain_ordinal;
                    group = 0;
                }
            }
            if (group) {
                auto d = timed_drain(false);
                if (!d)
                    return fail(MonitoringQ2CollectionStage::drain, std::move(d.error()));
                for (auto i = trials.size() - static_cast<std::size_t>(group); i < trials.size();
                     ++i)
                    trials[i].drain_ordinal = drain_ordinal;
            }
            auto helper = controlled->finish_voluntary_and_prepare_helper(*global);
            if (!helper)
                return fail(MonitoringQ2CollectionStage::preemption_event,
                            active_error("monitoring.q2.collector.helper"));
            group = 0;
            for (std::uint64_t ordinal{}; ordinal < value.preemption_trials_per_worker; ++ordinal) {
                ++generation;
                auto n = collection_clock.now(collection_clock.context);
                if (!n)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(n.error()));
                auto trial = deadline(n->nanoseconds(), value.per_trial_deadline_ms);
                if (!trial)
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.deadline"));
                trial->deadline.nanoseconds =
                    std::min(trial->deadline.nanoseconds, global->deadline.nanoseconds);
                if (n->nanoseconds() >= global->deadline.nanoseconds)
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.deadline"));
                active_generation = generation;
                if (!ok(controlled->begin(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_begin"));
                if (!ok(controlled->ready(generation, false, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_target_ready"));
                if (!ok(controlled->ready(generation, true, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_ready"));
                auto start = collection_clock.now(collection_clock.context);
                if (!start)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(start.error()));
                if (!ok(controlled->release_target(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_release_target"));
                if (!ok(controlled->target_running(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_target_running"));
                if (!ok(controlled->release_helper(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_release_helper"));
                if (!ok(controlled->compete(generation, Q2Winner::helper_first, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_compete"));
                if (!ok(controlled->stop(generation, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_stop"));
                if (!ok(controlled->terminal(generation, false, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_target_terminal"));
                if (!ok(controlled->terminal(generation, true, *trial)))
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption_terminal"));
                auto end = collection_clock.now(collection_clock.context);
                if (end &&
                    (end->nanoseconds() < start->nanoseconds() ||
                     (trials.size() > phase_start && start->nanoseconds() < trials.back().end_ns)))
                    return fail(MonitoringQ2CollectionStage::clock,
                                active_error("monitoring.q2.collector.clock_reversal"));
                Q2GenerationResult result{};
                const auto finished = controlled->finish(generation, &result, *trial);
                if (finished == Q2EventStatus::ok || finished == Q2EventStatus::ceiling)
                    active_generation = 0U;
                if (!end)
                    return fail(MonitoringQ2CollectionStage::clock, std::move(end.error()));
                if (!ok(finished) || result.winner != Q2Winner::helper_first)
                    return fail(MonitoringQ2CollectionStage::preemption_event,
                                active_error("monitoring.q2.collector.preemption"));
                trials.push_back({input.workers_[wi].worker, MonitoringQ2TrialKind::preemption,
                                  ordinal, start->nanoseconds(), end->nanoseconds(), 0, 0, 0, 0,
                                  false});
                ++group;
                if (group == value.periodic_drain_after_trials) {
                    auto d = timed_drain(false);
                    if (!d)
                        return fail(MonitoringQ2CollectionStage::drain, std::move(d.error()));
                    for (auto i = trials.size() - static_cast<std::size_t>(group);
                         i < trials.size(); ++i)
                        trials[i].drain_ordinal = drain_ordinal;
                    group = 0;
                }
            }
            if (group) {
                auto d = timed_drain(false);
                if (!d)
                    return fail(MonitoringQ2CollectionStage::drain, std::move(d.error()));
                for (auto i = trials.size() - static_cast<std::size_t>(group); i < trials.size();
                     ++i)
                    trials[i].drain_ordinal = drain_ordinal;
            }
            auto retired = prepared.bridge.retire(std::move(*controlled), *global);
            active_controlled = nullptr;
            if (auto* recovery = std::get_if<MonitoringQ2BridgeRetirementRecovery>(&retired)) {
                return MonitoringQ2TerminalContainment::contain(
                    std::move(*containment_stock), std::move(*recovery), std::move(prepared.source),
                    std::move(input), MonitoringQ2CollectionStage::finalization,
                    active_error("monitoring.q2.collector.retirement"), {}, 0U, false,
                    std::move(helper_provider));
            }
            if (auto* recovered = std::get_if<MonitoringQ2RecoveredWorkerFailure>(&retired)) {
                active_borrow = &recovered->borrow;
                active_retirement_failure = &recovered->failure;
                return fail(MonitoringQ2CollectionStage::finalization,
                            active_error("monitoring.q2.collector.retirement"));
            }
            active_borrow = &std::get<MonitoringQ2QuiescedWorkerBorrow>(retired);
            auto final = timed_drain(true);
            if (!final)
                return fail(MonitoringQ2CollectionStage::drain, std::move(final.error()));
            active_coverage = false;
            for (std::size_t i = phase_start; i < trials.size(); ++i) {
                for (std::size_t r = worker_record_start; r < record_cursor; ++r) {
                    const auto stamp = records[r].timestamp().nanoseconds();
                    if (stamp >= trials[i].start_ns && stamp <= trials[i].end_ns) {
                        ++trials[i].scoped_records;
                        if (records[r].kind() ==
                            platform_linux::PerfContextSwitchKind::voluntary_switch_out)
                            ++trials[i].voluntary_count;
                        if (records[r].kind() ==
                            platform_linux::PerfContextSwitchKind::involuntary_switch_out)
                            ++trials[i].involuntary_count;
                    }
                }
                trials[i].capture_complete = true;
                if ((trials[i].kind == MonitoringQ2TrialKind::voluntary &&
                     trials[i].voluntary_count == 0) ||
                    (trials[i].kind == MonitoringQ2TrialKind::preemption &&
                     trials[i].involuntary_count == 0)) {
                    const MonitoringQ2RequiredEventDiagnostic diagnostic{
                        trials[i].kind, trials[i].worker, trials[i].ordinal,
                        trials[i].voluntary_count, trials[i].involuntary_count,
                        trials[i].scoped_records};
                    auto failed = fail(MonitoringQ2CollectionStage::completeness,
                                       active_error("monitoring.q2.collector.required_event"));
                    if (auto* ordinary = std::get_if<MonitoringQ2FailedCollection>(&failed))
                        ordinary->required_event = diagnostic;
                    return failed;
                }
            }
            auto terminal = prepared.source.terminalize();
            active_source = nullptr;
            active_bridge = nullptr;
            if (auto* source_failure = std::get_if<MonitoringQ2ContextSourceFailure>(&terminal)) {
                session.complete_worker_failure(
                    std::get<MonitoringQ2QuiescedWorkerBorrow>(std::move(retired)),
                    std::move(*source_failure), *global);
                active_borrow = nullptr;
                return fail(MonitoringQ2CollectionStage::finalization,
                            active_error("monitoring.q2.collector.source"));
            }
            auto complete = session.complete_worker(
                std::get<MonitoringQ2QuiescedWorkerBorrow>(std::move(retired)),
                std::get<MonitoringQ2ContextSourceFinalization>(std::move(terminal)), *global);
            active_borrow = nullptr;
            active_cursor = nullptr;
            if (!complete)
                return fail(MonitoringQ2CollectionStage::finalization, std::move(complete.error()));
            summaries.push_back(
                {input.workers_[wi].worker, value.voluntary_trials_per_worker,
                 value.preemption_trials_per_worker, drain_ordinal - worker_drain_start,
                 static_cast<std::uint64_t>(record_cursor - worker_record_start),
                 summarize(std::span{prepare_durations}.last(1), duration_scratch),
                 summarize(std::span{drain_durations}.subspan(worker_drain_duration_start),
                           duration_scratch)});
        }
        auto finalized = session.finalize(true, *global);
        active_session = nullptr;
        if (finalized.retirement_retryable) {
            auto primary = finalized.primary ? std::move(*finalized.primary)
                                             : active_error("monitoring.q2.collector.retirement");
            std::array<std::optional<core::Error>, 64> cleanup{};
            const auto count = std::min(finalized.cleanup.size(), cleanup.size());
            for (std::size_t index{}; index < count; ++index)
                cleanup[index].emplace(std::move(finalized.cleanup[index]));
            return MonitoringQ2TerminalContainment::contain_session(
                std::move(*containment_stock), std::move(input),
                MonitoringQ2CollectionStage::finalization, std::move(primary), std::move(cleanup),
                count, finalized.cleanup.size() > cleanup.size(), false,
                std::move(helper_provider));
        }
        if (finalized.primary)
            return fail(MonitoringQ2CollectionStage::finalization, std::move(*finalized.primary));
        if (!finalized.cleanup.empty()) {
            auto outcome = fail(MonitoringQ2CollectionStage::finalization,
                                std::move(finalized.cleanup.front()));
            auto& failed = std::get<MonitoringQ2FailedCollection>(outcome);
            for (std::size_t index{1}; index < finalized.cleanup.size(); ++index) {
                if (failed.cleanup_count < failed.cleanup.size())
                    failed.cleanup[failed.cleanup_count++].emplace(
                        std::move(finalized.cleanup[index]));
                else
                    failed.cleanup_overflowed = true;
            }
            return outcome;
        }
        summaries.push_back({{},
                             value.voluntary_trials_per_worker * static_cast<std::uint64_t>(w),
                             value.preemption_trials_per_worker * static_cast<std::uint64_t>(w),
                             drain_ordinal,
                             static_cast<std::uint64_t>(record_cursor),
                             summarize(prepare_durations, duration_scratch),
                             summarize(drain_durations, duration_scratch)});
        auto global_summary = summaries.back();
        summaries.pop_back();
        input.worker_session_.reset();
        return MonitoringQ2ContextSwitchCollection{std::move(input), std::move(trials),
                                                   std::move(summaries), std::move(global_summary)};
    } catch (...) {
        return fail(MonitoringQ2CollectionStage::preparation,
                    active_error("monitoring.q2.collector.contained"));
    }
}
} // namespace epycbench::monitoring::qualification::detail
