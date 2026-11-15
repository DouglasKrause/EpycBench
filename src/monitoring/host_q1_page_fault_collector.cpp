#include "monitoring/host_q1_page_fault_collector_test_access.hpp"

#include "monitoring/page_fault_source_access.hpp"
#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {

using Stage = MonitoringQ1PageFaultCollectionStage;
using Collection = MonitoringQ1PageFaultCollection;
using Failure = MonitoringQ1PageFaultCollectionFailure;
using Observation = MonitoringQ1PageFaultObservation;
using Counters = MonitoringQ1FaultCounters;

auto collector_error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Monitoring Q1 page-fault collection failed",
            .operation = "collect_monitoring_q1_page_faults",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto production_source_read(void*, platform_linux::LinuxThreadId tid) noexcept
    -> std::expected<platform_linux::LinuxTaskPageFaultCounters, core::Error> {
    return platform_linux::read_task_page_fault_counters(tid);
}

auto production_dependencies() noexcept -> MonitoringQ1PageFaultCollectorDependencies {
    return {.workers = runtime::detail::production_worker_team_dependencies(),
            .clock = platform_linux::detail::production_clock_operations(),
            .oracle = platform_linux::detail::production_thread_rusage_operations(),
            .source_context = nullptr,
            .source_read = production_source_read,
            .session_context = nullptr,
            .session_ready = nullptr,
            .callback_fault = MonitoringQ1CollectorCallbackFault::none,
            .cancellation_to_request = nullptr};
}

struct Exchange final {
    const MonitoringQ1PageFaultCollectorDependencies* dependencies{};
    core::WorkerId expected_worker{0};
    std::size_t expected_index{};
    std::optional<platform_linux::LinuxThreadId>* retained_tid{};
    std::optional<platform_linux::LinuxThreadId> tid;
    platform_linux::detail::ThreadRusagePageFaultResult before;
    platform_linux::detail::ThreadRusagePageFaultResult after;
    Stage error_stage{Stage::oracle};
    bool association_failed{};
    std::string_view association_classification;
    bool first_sample{};
    std::atomic<bool> before_published{};
    std::atomic<bool> controller_release{};
};

auto oracle_callback(void* opaque, const runtime::WorkerExecutionContext& worker,
                     const runtime::detail::TargetedWorkerExecutionControl& control) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult {
    auto& exchange = *static_cast<Exchange*>(opaque);
    if (worker.worker_index != exchange.expected_index ||
        worker.worker_id != exchange.expected_worker) {
        exchange.error_stage = Stage::association;
        exchange.association_failed = true;
        exchange.association_classification = "target_worker_mismatch";
    } else if (*exchange.retained_tid &&
               **exchange.retained_tid != worker.readiness.tid) {
        exchange.error_stage = Stage::association;
        exchange.association_failed = true;
        exchange.association_classification = "worker_tid_changed";
    } else {
        if (!*exchange.retained_tid)
            exchange.retained_tid->emplace(worker.readiness.tid);
        exchange.tid.emplace(worker.readiness.tid);
        // Exercise the same control objects before this worker's first sample.
        // Both remain false, so wait(true) is already satisfied and cannot wait
        // for the controller. This performs no oracle or production acquisition.
        if (exchange.first_sample) {
            if (exchange.dependencies->control_event)
                exchange.dependencies->control_event(
                    exchange.dependencies->control_event_context, exchange.expected_worker,
                    MonitoringQ1CollectorControlEvent::preparation);
            exchange.before_published.notify_one();
            exchange.before_published.wait(true, std::memory_order_acquire);
            exchange.controller_release.notify_one();
            exchange.controller_release.wait(true, std::memory_order_acquire);
        }
        exchange.before = platform_linux::detail::ThreadRusageAccess::read_page_fault_counters_pod(
            exchange.dependencies->oracle);
    }

    exchange.before_published.store(true, std::memory_order_release);
    if (exchange.dependencies->control_event)
        exchange.dependencies->control_event(
            exchange.dependencies->control_event_context, exchange.expected_worker,
            MonitoringQ1CollectorControlEvent::sandwich);
    exchange.before_published.notify_one();
    exchange.controller_release.wait(false, std::memory_order_acquire);

    if (exchange.before.disposition ==
            platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded &&
        !exchange.association_failed) {
        exchange.after = platform_linux::detail::ThreadRusageAccess::read_page_fault_counters_pod(
            exchange.dependencies->oracle);
    }
    if (exchange.dependencies->callback_fault ==
        MonitoringQ1CollectorCallbackFault::missing_signal)
        return {.disposition = runtime::detail::TargetedWorkerDisposition::succeeded,
                .opaque_token = 0};
    control.signal_timed_completion();
    if (exchange.dependencies->callback_fault ==
        MonitoringQ1CollectorCallbackFault::duplicate_signal)
        control.signal_timed_completion();
    if (exchange.dependencies->callback_fault ==
        MonitoringQ1CollectorCallbackFault::caller_failure)
        return {.disposition = runtime::detail::TargetedWorkerDisposition::caller_failure,
                .opaque_token = 1};
    if (exchange.dependencies->callback_fault ==
        MonitoringQ1CollectorCallbackFault::success_bad_token)
        return {.disposition = runtime::detail::TargetedWorkerDisposition::succeeded,
                .opaque_token = 1};
    if (exchange.dependencies->callback_fault ==
        MonitoringQ1CollectorCallbackFault::caller_failure_bad_token)
        return {.disposition = runtime::detail::TargetedWorkerDisposition::caller_failure,
                .opaque_token = 0};
    if (exchange.dependencies->callback_fault ==
            MonitoringQ1CollectorCallbackFault::request_cancellation &&
        exchange.dependencies->cancellation_to_request)
        (void)exchange.dependencies->cancellation_to_request->try_request(
            runtime::ipc::CancelReason::controller_timeout);
    return {.disposition = (exchange.before.disposition !=
                                 platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                             exchange.after.disposition !=
                                 platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                             exchange.association_failed)
                               ? runtime::detail::TargetedWorkerDisposition::caller_failure
                               : runtime::detail::TargetedWorkerDisposition::succeeded,
            .opaque_token = (exchange.before.disposition !=
                                  platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                              exchange.after.disposition !=
                                  platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                              exchange.association_failed) ? 1U : 0U};
}

struct PendingFailure final {
    Stage stage;
    core::Error error;
};

class GenerationGuard final {
  public:
    enum class Phase : std::uint8_t { arming, ready, released, terminal };
    GenerationGuard(runtime::detail::TargetedWorkerGeneration generation,
                    runtime::detail::MonitoringQ1WorkerSession& session,
                    Exchange& exchange) noexcept
        : generation_{std::move(generation)}, session_{&session}, exchange_{&exchange} {}
    GenerationGuard(const GenerationGuard&) = delete;
    GenerationGuard(GenerationGuard&&) = delete;
    auto operator=(const GenerationGuard&) -> GenerationGuard& = delete;
    auto operator=(GenerationGuard&&) -> GenerationGuard& = delete;
    ~GenerationGuard() noexcept {
        if (!generation_) return;
        try {
            if (phase_ == Phase::arming) {
                auto ready = generation_->await_ready();
                if (ready) phase_ = Phase::ready;
                else phase_ = Phase::terminal;
            }
            if (phase_ == Phase::ready) {
                generation_->cancel_before_release();
                phase_ = Phase::terminal;
            } else if (phase_ == Phase::released) {
                release_exchange();
                auto timed = generation_->await_timed_completion();
                if (timed) (void)generation_->await_quiescence();
                phase_ = Phase::terminal;
                (void)generation_->collect_result();
            }
            generation_.reset();
            (void)runtime::detail::reset_monitoring_q1_targeted_control(*session_);
        } catch (...) {
            std::terminate();
        }
    }
    [[nodiscard]] auto get() noexcept -> runtime::detail::TargetedWorkerGeneration& {
        return *generation_;
    }
    void ready() noexcept { phase_ = Phase::ready; }
    void released() noexcept { phase_ = Phase::released; }
    void terminal() noexcept { phase_ = Phase::terminal; }
    void release_exchange() noexcept {
        exchange_->controller_release.store(true, std::memory_order_release);
        exchange_->controller_release.notify_one();
    }
    void retire() noexcept {
        generation_.reset();
        phase_ = Phase::terminal;
    }
  private:
    std::optional<runtime::detail::TargetedWorkerGeneration> generation_;
    runtime::detail::MonitoringQ1WorkerSession* session_;
    Exchange* exchange_;
    Phase phase_{Phase::arming};
};

auto summarize(std::span<const Observation> observations,
               std::optional<core::WorkerId> worker,
               std::vector<std::uint64_t>& scratch) -> MonitoringQ1PageFaultSummary {
    scratch.clear();
    for (const auto& observation : observations)
        if (!worker || observation.worker_id == *worker)
            scratch.push_back(observation.elapsed_ns);
    std::ranges::sort(scratch);
    return {.worker_id = worker,
            .count = static_cast<std::uint64_t>(scratch.size()),
            .minimum_elapsed_ns = scratch.front(),
            .maximum_elapsed_ns = scratch.back(),
            .lower_median_elapsed_ns = scratch[(scratch.size() - 1U) / 2U],
            .minor_equality_count = static_cast<std::uint64_t>(scratch.size()),
            .major_equality_count = static_cast<std::uint64_t>(scratch.size())};
}

} // namespace

MonitoringQ1PageFaultCollection::MonitoringQ1PageFaultCollection(
    MonitoringQ1CollectorPlan plan, std::vector<Observation> observations,
    std::vector<MonitoringQ1PageFaultSummary> workers,
    MonitoringQ1PageFaultSummary global) noexcept
    : plan_{std::move(plan)}, observations_{std::move(observations)},
      workers_{std::move(workers)}, global_{std::move(global)} {}

MonitoringQ1PageFaultCollectionFailure::MonitoringQ1PageFaultCollectionFailure(
    Stage stage, core::Error primary, std::vector<core::Error> secondary,
    std::optional<MonitoringQ1OracleMismatch> oracle_mismatch) noexcept
    : stage_{stage}, primary_{std::move(primary)}, secondary_{std::move(secondary)},
      oracle_mismatch_{oracle_mismatch} {}

auto MonitoringQ1PageFaultCollectorAccess::collect(
    MonitoringQ1CollectorPlan&& plan, runtime::RunnerCancellation& cancellation,
    MonitoringQ1PageFaultCollectorDependencies dependencies) noexcept
    -> std::expected<Collection, Failure> {
    std::optional<PendingFailure> primary;
    Stage active_stage{Stage::preparation};
    std::vector<Observation> observations;
    std::vector<MonitoringQ1PageFaultSummary> worker_summaries;
    std::vector<std::uint64_t> scratch;
    std::vector<std::optional<platform_linux::LinuxThreadId>> retained_tids;
    std::vector<core::Error> secondary;
    Exchange exchange;
    exchange.dependencies = &dependencies;
    std::optional<runtime::detail::MonitoringQ1WorkerSession> session;
    std::optional<core::Error> contained_bad_alloc;
    std::optional<core::Error> contained_standard;
    std::optional<core::Error> contained_unknown;
    std::optional<core::Error> mismatch_minor;
    std::optional<core::Error> mismatch_major;
    std::optional<MonitoringQ1OracleMismatch> oracle_mismatch;
    const auto fail = [&](Stage stage, core::Error error) {
        if (!primary)
            primary.emplace(PendingFailure{stage, std::move(error)});
    };
    const auto lifecycle_failure = [&](Stage stage, core::Error error) noexcept {
        if (!primary) {
            fail(stage, std::move(error));
        } else if (secondary.size() < secondary.capacity()) {
            secondary.push_back(std::move(error));
        } else {
            std::terminate();
        }
    };
    const auto finalize = [&]() noexcept {
        if (!session) return;
        auto finalized = runtime::detail::finalize_monitoring_q1_worker_session(
            std::move(*session));
        session.reset();
        auto final_primary = std::move(finalized).take_primary();
        auto final_secondary = std::move(finalized).take_secondary();
        if (final_primary) {
            if (!primary)
                fail(Stage::finalization, std::move(*final_primary));
            else
                lifecycle_failure(Stage::finalization, std::move(*final_primary));
        }
        for (auto& error : final_secondary)
            lifecycle_failure(Stage::finalization, std::move(error));
    };

    try {
        const auto workers = plan.worker_ids();
        if (workers.empty() || plan.observations_per_worker() == 0 ||
            plan.total_observations() == 0 || dependencies.source_read == nullptr ||
            dependencies.clock.clock_gettime_raw == nullptr ||
            dependencies.oracle.get_thread_rusage == nullptr) {
            fail(Stage::preparation,
                 collector_error("monitoring.q1.collector.invalid_input", "invalid_dependency"));
        } else if (plan.total_observations() > std::numeric_limits<std::size_t>::max() ||
                   plan.observations_per_worker() > std::numeric_limits<std::size_t>::max() ||
                   workers.size() > (std::numeric_limits<std::size_t>::max() - 8U) / 2U ||
                   workers.size() > std::numeric_limits<std::size_t>::max() /
                                        static_cast<std::size_t>(plan.observations_per_worker()) ||
                   workers.size() * static_cast<std::size_t>(plan.observations_per_worker()) !=
                       static_cast<std::size_t>(plan.total_observations())) {
            fail(Stage::preparation,
                 collector_error("monitoring.q1.collector.invalid_input", "observation_count"));
        } else {
            const auto total = static_cast<std::size_t>(plan.total_observations());
            observations.reserve(total);
            worker_summaries.reserve(workers.size());
            scratch.reserve(total);
            retained_tids.resize(workers.size());
            // Worker-session finalization owns at most 2W+3 secondaries and one
            // primary. One stopped collection can additionally retain at most
            // timed, quiescence, result, and control-reset failures.
            secondary.reserve(workers.size() * 2U + 8U);
            contained_bad_alloc.emplace(collector_error(
                "monitoring.q1.collector.contained_failure", "contained_bad_alloc"));
            contained_standard.emplace(collector_error(
                "monitoring.q1.collector.contained_failure",
                "contained_standard_exception"));
            contained_unknown.emplace(collector_error(
                "monitoring.q1.collector.contained_failure",
                "contained_unknown_exception"));
            mismatch_minor.emplace(collector_error(
                "monitoring.q1.collector.oracle_mismatch", "minor_faults"));
            mismatch_major.emplace(collector_error(
                "monitoring.q1.collector.oracle_mismatch", "major_faults"));
        }

        if (!primary) {
            auto snapshot = std::move(plan).take_worker_snapshot();
            if (!snapshot) {
                fail(Stage::preparation,
                     collector_error("monitoring.q1.collector.invalid_input",
                                     "missing_worker_binding"));
            } else {
                auto created = runtime::detail::make_monitoring_q1_worker_session(
                    std::move(*snapshot), cancellation, std::move(dependencies.workers));
                if (!created)
                    fail(Stage::preparation, std::move(created.error()));
                else
                    session.emplace(std::move(*created));
            }
        }
        if (session && dependencies.session_ready)
            dependencies.session_ready(dependencies.session_context, *session);

        if (session) {
            for (std::uint64_t cycle = 0; cycle < plan.observations_per_worker() && !primary;
                 ++cycle) {
                for (std::size_t worker_index = 0;
                     worker_index < plan.worker_ids().size() && !primary; ++worker_index) {
                    exchange.expected_worker = core::WorkerId{0};
                    exchange.expected_index = 0;
                    exchange.retained_tid = nullptr;
                    exchange.tid.reset();
                    exchange.before = {};
                    exchange.after = {};
                    exchange.error_stage = Stage::oracle;
                    exchange.association_failed = false;
                    exchange.association_classification = {};
                    exchange.first_sample = cycle == 0;
                    exchange.before_published.store(false, std::memory_order_relaxed);
                    exchange.controller_release.store(false, std::memory_order_relaxed);
                    exchange.expected_worker = plan.worker_ids()[worker_index];
                    exchange.expected_index = worker_index;
                    exchange.retained_tid = &retained_tids[worker_index];

                    active_stage = Stage::association;
                    auto generation = runtime::detail::begin_monitoring_q1_targeted_generation(
                        *session, worker_index,
                        {.context = &exchange, .run = oracle_callback});
                    if (!generation) {
                        fail(Stage::association, std::move(generation.error()));
                        break;
                    }
                    GenerationGuard handle{std::move(*generation), *session, exchange};
                    auto ready = handle.get().await_ready();
                    if (!ready) {
                        handle.terminal();
                        (void)handle.get().collect_result();
                        handle.retire();
                        (void)runtime::detail::reset_monitoring_q1_targeted_control(*session);
                        fail(Stage::association, std::move(ready.error()));
                        break;
                    }
                    handle.ready();
                    handle.get().release();
                    handle.released();
                    exchange.before_published.wait(false, std::memory_order_acquire);

                    std::optional<platform_linux::RawMonotonicTimestamp> start;
                    std::optional<platform_linux::RawMonotonicTimestamp> end;
                    std::optional<PageFaultSnapshot> production;
                    if (exchange.association_failed) {
                        fail(Stage::association,
                             collector_error("monitoring.q1.collector.association_failed",
                                             std::string{exchange.association_classification}));
                    } else {
                        active_stage = Stage::oracle;
                        auto before = platform_linux::detail::ThreadRusageAccess::project_page_fault_counters(
                            exchange.before);
                        if (!before) {
                            fail(Stage::oracle, std::move(before.error()));
                        }
                    }
                    if (!primary) {
                        active_stage = Stage::production_source;
                        auto source = monitoring::detail::PageFaultSourceAccess::create(
                            exchange.expected_worker, *exchange.tid,
                            dependencies.source_context, dependencies.source_read);
                        auto operations = source.operations();
                        if (!operations || operations->support() != MonitoringSourceSupport::supported) {
                            fail(Stage::production_source,
                                 operations ? collector_error(
                                                  "monitoring.q1.collector.source_failed",
                                                  "source_unavailable")
                                            : std::move(operations.error()));
                        } else {
                            active_stage = Stage::clock;
                            auto first = platform_linux::detail::RawMonotonicClockAccess::now(
                                dependencies.clock);
                            if (!first) {
                                fail(Stage::clock, std::move(first.error()));
                            } else {
                                start.emplace(*first);
                                active_stage = Stage::production_source;
                                auto read = operations->read();
                                if (!read) {
                                    fail(Stage::production_source, std::move(read.error()));
                                } else if (read->status() != MonitoringAcquisitionStatus::available ||
                                           !read->payload()) {
                                    fail(Stage::production_source,
                                         collector_error("monitoring.q1.collector.source_failed",
                                                         "source_unavailable"));
                                } else {
                                    production.emplace(*read->payload());
                                    active_stage = Stage::clock;
                                    auto last =
                                        platform_linux::detail::RawMonotonicClockAccess::now(
                                            dependencies.clock);
                                    if (!last)
                                        fail(Stage::clock, std::move(last.error()));
                                    else
                                        end.emplace(*last);
                                }
                            }
                        }
                    }

                    active_stage = Stage::association;
                    handle.release_exchange();

                    auto timed = handle.get().await_timed_completion();
                    if (!timed)
                        lifecycle_failure(Stage::association, std::move(timed.error()));
                    std::optional<std::expected<void, core::Error>> quiet;
                    if (timed) {
                        quiet.emplace(handle.get().await_quiescence());
                        if (!*quiet)
                            lifecycle_failure(Stage::association, std::move(quiet->error()));
                    }
                    handle.terminal();
                    auto result = handle.get().collect_result();
                    if (!result) {
                        if (timed && (!quiet || *quiet))
                            lifecycle_failure(Stage::association, std::move(result.error()));
                    } else {
                        const bool callback_failed =
                            exchange.before.disposition != platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                            exchange.after.disposition != platform_linux::detail::ThreadRusagePageFaultDisposition::succeeded ||
                            exchange.association_failed;
                        const auto expected_disposition = callback_failed
                            ? runtime::detail::TargetedWorkerDisposition::caller_failure
                            : runtime::detail::TargetedWorkerDisposition::succeeded;
                        const std::uint32_t expected_token = callback_failed ? 1U : 0U;
                        if (result->worker_id != exchange.expected_worker ||
                            result->disposition != expected_disposition ||
                            result->opaque_token != expected_token ||
                            result->cancellation_observed) {
                            lifecycle_failure(Stage::association, collector_error(
                                "monitoring.q1.collector.association_failed",
                                "target_result_mismatch"));
                        }
                    }
                    handle.retire();
                    auto reset = runtime::detail::reset_monitoring_q1_targeted_control(*session);
                    if (!reset)
                        lifecycle_failure(Stage::association, std::move(reset.error()));

                    if (!primary) {
                        active_stage = Stage::oracle;
                        auto after = platform_linux::detail::ThreadRusageAccess::project_page_fault_counters(
                            exchange.after);
                        if (!after) fail(Stage::oracle, std::move(after.error()));
                    }

                    if (!primary) {
                        if (*end < *start) {
                            fail(Stage::timing,
                                 collector_error("monitoring.q1.collector.timing_failed",
                                                 "reversed_interval"));
                        } else if (production->worker_id() != exchange.expected_worker) {
                            fail(Stage::association,
                                 collector_error("monitoring.q1.collector.association_failed",
                                                 "production_worker_mismatch"));
                        } else {
                            const Counters before{exchange.before.counters.minor_faults,
                                                  exchange.before.counters.major_faults};
                            const Counters value{production->minor_faults().value(),
                                                 production->major_faults().value()};
                            const Counters after{exchange.after.counters.minor_faults,
                                                 exchange.after.counters.major_faults};
                            if (before != value || value != after) {
                                const bool minor = before.minor_faults != value.minor_faults ||
                                    value.minor_faults != after.minor_faults;
                                oracle_mismatch.emplace(MonitoringQ1OracleMismatch{
                                    cycle, exchange.expected_worker,
                                    minor ? MonitoringQ1FaultCounterKind::minor_faults
                                          : MonitoringQ1FaultCounterKind::major_faults,
                                    minor ? before.minor_faults : before.major_faults,
                                    minor ? value.minor_faults : value.major_faults,
                                    minor ? after.minor_faults : after.major_faults});
                                fail(Stage::oracle,
                                     std::move(minor ? *mismatch_minor : *mismatch_major));
                            } else {
                                observations.push_back({
                                    .cycle_index = cycle,
                                    .worker_id = exchange.expected_worker,
                                    .collection_start_ns = start->nanoseconds(),
                                    .collection_end_ns = end->nanoseconds(),
                                    .elapsed_ns = end->nanoseconds() - start->nanoseconds(),
                                    .oracle_before = before,
                                    .production = value,
                                    .oracle_after = after,
                                    .minor_equal = true,
                                    .major_equal = true});
                            }
                        }
                    }
                }
            }

            finalize();
        }

        if (primary)
            return std::unexpected{Failure{primary->stage, std::move(primary->error),
                                           std::move(secondary), oracle_mismatch}};

        for (const auto worker : plan.worker_ids())
            worker_summaries.push_back(summarize(observations, worker, scratch));
        auto global = summarize(observations, std::nullopt, scratch);
        return Collection{std::move(plan), std::move(observations),
                          std::move(worker_summaries), std::move(global)};
    } catch (const std::bad_alloc&) {
        if (!primary)
            fail(active_stage, contained_bad_alloc
                                   ? std::move(*contained_bad_alloc)
                                   : collector_error("monitoring.q1.collector.contained_failure",
                                                     "contained_bad_alloc"));
        finalize();
        return std::unexpected{Failure{primary->stage, std::move(primary->error),
                                       std::move(secondary), oracle_mismatch}};
    } catch (const std::exception&) {
        if (!primary)
            fail(active_stage, contained_standard
                                   ? std::move(*contained_standard)
                                   : collector_error("monitoring.q1.collector.contained_failure",
                                                     "contained_standard_exception"));
        finalize();
        return std::unexpected{Failure{primary->stage, std::move(primary->error),
                                       std::move(secondary), oracle_mismatch}};
    } catch (...) {
        if (!primary)
            fail(active_stage, contained_unknown
                                   ? std::move(*contained_unknown)
                                   : collector_error("monitoring.q1.collector.contained_failure",
                                                     "contained_unknown_exception"));
        finalize();
        return std::unexpected{Failure{primary->stage, std::move(primary->error),
                                       std::move(secondary), oracle_mismatch}};
    }
}

auto collect_monitoring_q1_page_faults(
    MonitoringQ1CollectorPlan&& plan, runtime::RunnerCancellation& cancellation) noexcept
    -> std::expected<Collection, Failure> {
    return MonitoringQ1PageFaultCollectorAccess::collect(
        std::move(plan), cancellation, production_dependencies());
}

} // namespace epycbench::monitoring::qualification::detail
