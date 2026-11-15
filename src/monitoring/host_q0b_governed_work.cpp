#include "monitoring/host_q0b_governed_work.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "core/json_ownership.hpp"

#include "monitoring/host_baseline_work_admission.hpp"

#include <array>
#include <cerrno>
#include <limits>
#include <new>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace work = platform_linux::host_work;
namespace file = platform_linux::host_baseline_file;
namespace native = platform_linux::detail;
namespace {
constexpr std::array names{"invalid_input", "association", "admission", "native_limits",
    "projection", "allocation", "state", "retirement", "contained", "source_close",
    "projection_close", "calibration", "clock", "deadline", "cancelled"};
auto error(HostQ0bWorkReason reason) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{std::string{"monitoring.q0b.governed_work."} + names[static_cast<std::size_t>(reason)]},
        .message = "Governed Q0b work failed", .operation = "monitoring.q0b.governed_work",
        .os_error = std::nullopt, .context = nlohmann::json::object(),
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command};
}
auto succeeded(native::RawStatusResult value) noexcept -> bool {
    return value.value == 0 && value.error_number == 0;
}

// Read-only verification, never native limit mutation. The sealed entry remains
// responsible for establishing pre-ELF delivery, which getrlimit cannot prove.
auto verify_limits(int expected_parent, work::LimitDemand demand,
    const work::LimitOperations& operations) noexcept -> std::expected<void, work::Reason> {
    auto rejected = [](work::Reason reason) { return std::unexpected{reason}; };
    std::uint64_t first{}, latest{};
    const auto sample = [&]() -> std::expected<void, work::Reason> {
        if (operations.guard.cancelled(operations.guard.context)) return rejected(work::Reason::cancelled);
        std::uint64_t now{};
        if (!succeeded(operations.guard.now(operations.guard.context, &now))) return rejected(work::Reason::clock);
        if (operations.guard.cancelled(operations.guard.context)) return rejected(work::Reason::cancelled);
        if (now < latest) return rejected(work::Reason::clock);
        latest = now;
        if (now - first >= UINT64_C(5000000000)) return rejected(work::Reason::deadline);
        return {};
    };
    if (operations.guard.cancelled(operations.guard.context)) return rejected(work::Reason::cancelled);
    if (!succeeded(operations.guard.now(operations.guard.context, &first))) return rejected(work::Reason::clock);
    if (operations.guard.cancelled(operations.guard.context)) return rejected(work::Reason::cancelled);
    latest = first;
    int original_pid{}, original_parent{}; std::uint64_t original_uid{};
    auto ready = sample(); if (!ready) return ready;
    if (!succeeded(operations.identity(operations.context, &original_pid, &original_parent, &original_uid)))
        return rejected(work::Reason::authentication);
    ready = sample(); if (!ready) return ready;
    if (original_pid <= 0 || original_parent != expected_parent || original_pid == original_parent || original_uid == 0)
        return rejected(work::Reason::authentication);
    constexpr std::array<std::uint64_t, 3> maximum{UINT64_C(21474836480), UINT64_C(8388608), 1024};
    const std::array wanted{demand.address_space.value(), demand.stack.value(),
        static_cast<std::uint64_t>(demand.descriptors)};
    for (std::size_t i = 0; i < maximum.size(); ++i) {
        ready = sample(); if (!ready) return ready;
        work::LimitPair actual{};
        if (!succeeded(operations.get(operations.context, static_cast<work::NativeLimit>(i), &actual)))
            return rejected(work::Reason::limit_get);
        ready = sample(); if (!ready) return ready;
        if (actual.soft > actual.hard || actual.hard > maximum[i] || actual.hard > operations.maximum_finite)
            return rejected(work::Reason::authentication);
        if (wanted[i] > actual.soft) return rejected(work::Reason::capacity);
    }
    ready = sample(); if (!ready) return ready;
    int pid{}, parent{}; std::uint64_t uid{};
    if (!succeeded(operations.identity(operations.context, &pid, &parent, &uid)))
        return rejected(work::Reason::authentication);
    ready = sample(); if (!ready) return ready;
    if (pid != original_pid || parent != original_parent || uid != original_uid)
        return rejected(work::Reason::authentication);
    return {};
}
} // namespace

struct HostQ0bGovernedState final {
    HostQ0bGovernedState(int parent, work::ResourceLedger ledger_value,
        HostQ0bBackendDependencies dependencies_value, HostQ0bGovernedLeaves leaves_value,
        const HostFrozenSourceLaunchBinding* source_value, std::optional<HostQ0bInvocationGuard> guard_value)
        : expected_parent{parent}, ledger{std::move(ledger_value)}, lower{dependencies_value},
          leaves{leaves_value}, source{source_value}, invocation{guard_value},
          projection_clock{leaves_value.projection.clock},
          last_ns{guard_value ? guard_value->last_observed_ns : 0} {
        struct Partial final {
            HostQ0bGovernedState* state;
            ~Partial() noexcept { if (state) state->retire_errors(); }
        } partial{this};
        pending.reserve(5);
        for (std::size_t i = 0; i < failures.size(); ++i)
            failures[i].emplace(error(static_cast<HostQ0bWorkReason>(i)));
        for (auto& item : source_closes) item.emplace(error(HostQ0bWorkReason::source_close));
        for (auto& item : initial_source_closes)
            item.emplace(project_host_q0b_failure_stage({error(HostQ0bWorkReason::source_close), {}},
                "composition").primary);
        projection_close.emplace(project_host_q0b_failure_stage(
            {error(HostQ0bWorkReason::projection_close), {}}, "composition").primary);
        partial.state = nullptr;
    }
    ~HostQ0bGovernedState() noexcept { retire_errors(); }
    void retire_errors() noexcept {
        for (auto& item : failures) if (item) core::detail::retire_json(item->context);
        for (auto& item : source_closes) if (item) core::detail::retire_json(item->context);
        for (auto& item : initial_source_closes) if (item) core::detail::retire_json(item->context);
        if (projection_close) core::detail::retire_json(projection_close->context);
        for (auto& item : pending) core::detail::retire_json(item.context);
    }
    auto take_error(HostQ0bWorkReason reason) -> core::Error {
        auto& stored = failures[static_cast<std::size_t>(reason)];
        if (!stored) return error(reason); // Ordinary repeated misuse, never delegation.
        auto value = std::move(*stored); stored.reset(); return value;
    }
    void fail(HostQ0bWorkReason reason) noexcept { if (!receipt.failure) receipt.failure = reason; }
    auto guard() noexcept -> bool {
        if (!invocation) return !source || source->guard().has_value();
        if (receipt.failure) return false;
        const auto refuse = [&](work::Reason reason, std::optional<native::RawStatusResult> raw = std::nullopt) {
            fail(reason == work::Reason::cancelled ? HostQ0bWorkReason::cancelled :
                reason == work::Reason::deadline ? HostQ0bWorkReason::deadline : HostQ0bWorkReason::clock);
            receipt.native_failure = reason;
            receipt.guard_native = raw;
            return false;
        };
        const auto& operations = invocation->operations;
        if (operations.cancelled(operations.context)) return refuse(work::Reason::cancelled);
        std::uint64_t now{};
        const auto checked = operations.now(operations.context, &now);
        const auto actual = operations.actual_raw_now ? *operations.actual_raw_now : checked;
        if (!succeeded(checked)) return refuse(work::Reason::clock, actual);
        if (operations.cancelled(operations.context)) return refuse(work::Reason::cancelled);
        if (now < invocation->origin_ns || now < last_ns) return refuse(work::Reason::clock, actual);
        if (now >= invocation->end_ns) return refuse(work::Reason::deadline, actual);
        last_ns = now;
        return true;
    }
    int expected_parent;
    work::ResourceLedger ledger;
    HostQ0bBackendDependencies lower;
    HostQ0bGovernedLeaves leaves;
    const HostFrozenSourceLaunchBinding* source;
    std::optional<HostQ0bInvocationGuard> invocation;
    platform_linux::detail::ClockOperations projection_clock;
    std::uint64_t last_ns{};
    std::weak_ptr<HostQ0bGovernedState> self;
    std::optional<platform_linux::FileDescriptor> projection;
    std::optional<HostBaselinePreparation> original;
    std::optional<HostQ0bBackendOperations> lower_operations;
    // These are opaque association values. Never dereference retained pointers.
    const void* session{}; const void* plan{}; const void* inputs{};
    const runtime::CalibrationOnlyProductionAdmission* bandwidth{};
    const runtime::CalibrationOnlyProductionAdmission* latency{};
    std::array<std::optional<core::Error>, names.size()> failures;
    std::array<std::optional<core::Error>, 4> source_closes;
    std::array<std::optional<core::Error>, 4> initial_source_closes;
    std::optional<core::Error> projection_close;
    std::vector<core::Error> pending;
    HostQ0bWorkSnapshot receipt{.phase = HostQ0bWorkPhase::prepared};
    bool invoked{}, active{};
};

namespace {
auto reject(HostQ0bGovernedState& state, HostQ0bWorkReason reason) -> core::Error {
    state.fail(reason); return state.take_error(reason);
}
auto fresh(HostQ0bGovernedState& state, const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    std::size_t descriptors) -> std::expected<HostBaselinePreparation, core::Error> {
    if (!state.guard())
        return std::unexpected{reject(state, state.receipt.failure.value_or(HostQ0bWorkReason::association))};
    auto admitted = admit_host_baseline_work(session, plan, inputs,
        core::ByteCount::from_base_units(8388608), descriptors, state.leaves.headroom);
    if (!admitted) {
        if (admitted.error().admission) {
            const auto& failure = *admitted.error().admission;
            for (std::size_t i = 0; i < failure.cleanup_count; ++i) {
                auto& item = descriptors == 7 ? state.initial_source_closes[i] : state.source_closes[i];
                state.pending.push_back(std::move(*item)); item.reset();
            }
        }
        return std::unexpected{reject(state, state.receipt.failure.value_or(HostQ0bWorkReason::admission))};
    }
    auto limits = verify_limits(state.expected_parent, *admitted->limit_demand(), state.leaves.limits);
    if (!limits) {
        if (!state.receipt.native_failure) state.receipt.native_failure = limits.error();
        return std::unexpected{reject(state, state.receipt.failure.value_or(HostQ0bWorkReason::native_limits))};
    }
    if (!state.guard())
        return std::unexpected{reject(state, state.receipt.failure.value_or(HostQ0bWorkReason::association))};
    auto prepared = admitted->take_preparation();
    if (!prepared) return std::unexpected{reject(state, HostQ0bWorkReason::state)};
    return std::move(*prepared);
}
auto bind(void* p, const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs)
    -> std::expected<void, core::Error> {
    auto& state = *static_cast<HostQ0bGovernedState*>(p);
    if (!state.active || state.receipt.failure || state.receipt.phase != HostQ0bWorkPhase::prepared)
        return std::unexpected{reject(state, HostQ0bWorkReason::state)};
    auto prepared = fresh(state, session, plan, inputs, 7);
    if (!prepared) return std::unexpected{std::move(prepared.error())};
    state.original.emplace(std::move(*prepared)); state.receipt.original_retained = true;
    state.session = &session; state.plan = &plan; state.inputs = &inputs;
    state.bandwidth = &inputs.bandwidth(); state.latency = &inputs.latency();
    const auto body = state.original->physical_projection();
    auto exported = file::export_bytes_with(std::move(*state.projection),
        std::as_bytes(std::span{body.data(), body.size()}), {}, state.leaves.projection);
    state.projection.reset();
    if (!exported) {
        if (exported.error().secondary) {
            state.pending.push_back(std::move(*state.projection_close)); state.projection_close.reset();
        }
        return std::unexpected{reject(state, state.receipt.failure.value_or(HostQ0bWorkReason::projection))};
    }
    state.receipt.projection_exported = true; state.receipt.phase = HostQ0bWorkPhase::exported;
    return {};
}
auto admit(void* p, const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    HostQ0bFamily family) -> std::expected<void, Q0bC0DriverFailure> {
    auto& state = *static_cast<HostQ0bGovernedState*>(p);
    const auto failed = [&](HostQ0bWorkReason reason) {
        return std::unexpected{Q0bC0DriverFailure{reject(state, reason), {}}};
    };
    if (family != HostQ0bFamily::bandwidth && family != HostQ0bFamily::latency)
        return failed(HostQ0bWorkReason::association);
    const auto expected = family == HostQ0bFamily::bandwidth
        ? HostQ0bWorkPhase::exported : HostQ0bWorkPhase::bandwidth_retired;
    if (!state.active || state.receipt.failure || state.receipt.phase != expected)
        return failed(HostQ0bWorkReason::state);
    if (state.session != &session || state.plan != &plan || state.inputs != &inputs ||
        state.bandwidth != &inputs.bandwidth() || state.latency != &inputs.latency() ||
        !inputs.bandwidth().has_value() || !inputs.latency().has_value())
        return failed(HostQ0bWorkReason::association);
    auto prepared = fresh(state, session, plan, inputs, 6);
    if (!prepared) {
        auto secondary = std::move(state.pending);
        return std::unexpected{Q0bC0DriverFailure{std::move(prepared.error()), std::move(secondary)}};
    }
    if (prepared->resources() != state.original->resources() ||
        prepared->physical_sha256() != state.original->physical_sha256() ||
        prepared->physical_projection() != state.original->physical_projection())
        return failed(HostQ0bWorkReason::association);
    state.receipt.phase = family == HostQ0bFamily::bandwidth
        ? HostQ0bWorkPhase::bandwidth_ready : HostQ0bWorkPhase::latency_ready;
    return {};
}

class GovernedRun final : public runtime::CalibrationOnlyProductionRun::Implementation {
  public:
    GovernedRun(std::shared_ptr<HostQ0bGovernedState> state, bool bandwidth) noexcept
        : state_{std::move(state)}, bandwidth_{bandwidth} {}
    ~GovernedRun() noexcept override {
        if (lower_) state_->fail(HostQ0bWorkReason::retirement);
        lower_.reset(); lease_.reset(); // Abandonment poisons and retains charge.
    }
    void install(runtime::CalibrationOnlyProductionRun run, work::ResourceLease lease) noexcept {
        lower_.emplace(std::move(run)); lease_.emplace(std::move(lease));
    }
    auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> runtime::CalibrationOnlyOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> override {
        if (!can_calibrate(true)) return std::unexpected{invalid()};
        calibrated_ = true;
        auto result = lower_->calibrate_bandwidth(policy);
        if (!result) state_->fail(HostQ0bWorkReason::calibration);
        else if (state_->invocation && !state_->guard()) return std::unexpected{invalid()};
        return result;
    }
    auto calibrate_latency(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> runtime::CalibrationOnlyOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> override {
        if (!can_calibrate(false)) return std::unexpected{invalid()};
        calibrated_ = true;
        auto result = lower_->calibrate_latency(policy);
        if (!result) state_->fail(HostQ0bWorkReason::calibration);
        else if (state_->invocation && !state_->guard()) return std::unexpected{invalid()};
        return result;
    }
    auto finalize() noexcept -> runtime::CalibrationOnlyOperationResult<void> override {
        if (!active_ || !state_->active || !lower_) return std::unexpected{invalid()};
        active_ = false; // Lower rvalue finalize only forwards: latch before entry.
        ++state_->receipt.finalizations;
        auto result = std::move(*lower_).finalize();
        lower_.reset(); // Destroy actual retained lower ownership before release.
        if (!result) {
            state_->fail(HostQ0bWorkReason::retirement); lease_.reset();
            return std::unexpected{std::move(result.error())};
        }
        auto retired = lease_->retire(); lease_.reset();
        if (!retired) {
            state_->fail(HostQ0bWorkReason::retirement);
            return std::unexpected{runtime::CalibrationOnlyOperationFailure{
                state_->take_error(HostQ0bWorkReason::retirement), {}}};
        }
        ++state_->receipt.retirements;
        state_->receipt.phase = bandwidth_ ? HostQ0bWorkPhase::bandwidth_retired : HostQ0bWorkPhase::completed;
        return {};
    }
  private:
    auto can_calibrate(bool bandwidth) noexcept -> bool {
        return active_ && state_->active && !state_->receipt.failure && lower_ &&
            !calibrated_ && bandwidth_ == bandwidth && state_->guard();
    }
    auto invalid() -> runtime::CalibrationOnlyOperationFailure {
        state_->fail(HostQ0bWorkReason::state); calibrated_ = true;
        return {state_->take_error(state_->invocation && state_->receipt.native_failure
            ? state_->receipt.failure.value_or(HostQ0bWorkReason::state) : HostQ0bWorkReason::state), {}};
    }
    std::shared_ptr<HostQ0bGovernedState> state_;
    bool bandwidth_, active_{true}, calibrated_{};
    std::optional<runtime::CalibrationOnlyProductionRun> lower_;
    std::optional<work::ResourceLease> lease_;
};

auto prepare_run(void* p, const runtime::CalibrationOnlyProductionAdmission& inputs) noexcept
    -> runtime::CalibrationOnlyOperationResult<runtime::CalibrationOnlyProductionRun> {
    auto& state = *static_cast<HostQ0bGovernedState*>(p);
    const auto failed = [&](HostQ0bWorkReason reason) {
        return std::unexpected{runtime::CalibrationOnlyOperationFailure{reject(state, reason), {}}};
    };
    const bool bandwidth = state.receipt.phase == HostQ0bWorkPhase::bandwidth_ready;
    if (!state.active || state.receipt.failure || !state.lower_operations ||
        (!bandwidth && state.receipt.phase != HostQ0bWorkPhase::latency_ready))
        return failed(HostQ0bWorkReason::state);
    if (&inputs != (bandwidth ? state.bandwidth : state.latency) || !inputs.has_value())
        return failed(HostQ0bWorkReason::association);
    if (state.invocation && !state.guard()) return failed(state.receipt.failure.value_or(HostQ0bWorkReason::association));
    try {
        auto wrapper = std::make_unique<GovernedRun>(state.self.lock(), bandwidth);
        const auto summary = *state.original->resources();
        const auto workers = bandwidth ? summary.bandwidth_measurement_workers : summary.latency_measurement_workers;
        auto lease = state.ledger.reserve({0, workers, workers, 0, 0});
        if (!lease) return failed(HostQ0bWorkReason::admission);
        state.receipt.phase = bandwidth ? HostQ0bWorkPhase::bandwidth_running : HostQ0bWorkPhase::latency_running;
        ++state.receipt.preparations;
        auto lower = state.lower_operations->operations.prepare_run(inputs);
        if (!lower) {
            state.fail(HostQ0bWorkReason::retirement); // No cleanup receipt: abandon lease.
            return std::unexpected{std::move(lower.error())};
        }
        wrapper->install(std::move(*lower), std::move(*lease));
        if (state.invocation) static_cast<void>(state.guard());
        return runtime::CalibrationOnlyProductionRun{std::move(wrapper)};
    } catch (const std::bad_alloc&) { return failed(HostQ0bWorkReason::allocation); }
      catch (...) { return failed(HostQ0bWorkReason::contained); }
}
auto operations(void* p) -> std::expected<HostQ0bBackendOperations, core::Error> {
    auto& state = *static_cast<HostQ0bGovernedState*>(p);
    if (!state.active || state.receipt.failure || state.receipt.phase != HostQ0bWorkPhase::exported || state.lower_operations)
        return std::unexpected{reject(state, HostQ0bWorkReason::state)};
    auto lower = state.lower.make_operations(state.lower.context);
    if (!lower) return std::unexpected{std::move(lower.error())};
    state.lower_operations.emplace(std::move(*lower));
    auto owner = state.self.lock();
    auto decorated = runtime::CalibrationOnlyProductionOperations::create(owner, prepare_run);
    if (!decorated) return std::unexpected{std::move(decorated.error())};
    return HostQ0bBackendOperations{std::move(owner), std::move(*decorated)};
}
} // namespace

HostQ0bGovernedWork::HostQ0bGovernedWork(std::shared_ptr<HostQ0bGovernedState> state) noexcept
    : state_{std::move(state)} {}
HostQ0bGovernedWork::HostQ0bGovernedWork(HostQ0bGovernedWork&&) noexcept = default;
HostQ0bGovernedWork::~HostQ0bGovernedWork() noexcept { end(); }
auto HostQ0bGovernedWork::prepare(int parent, platform_linux::FileDescriptor& projection,
    HostQ0bBackendDependencies lower, HostQ0bGovernedLeaves leaves) noexcept
    -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason> {
    return prepare_impl(parent, projection, lower, leaves, nullptr);
}
auto HostQ0bGovernedWork::prepare_governed(int parent, platform_linux::FileDescriptor& projection,
    HostQ0bBackendDependencies lower, HostQ0bGovernedLeaves leaves, HostQ0bInvocationGuard guard) noexcept
    -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason> {
    if (!guard.operations.now || !guard.operations.cancelled || !leaves.projection.clock.clock_gettime_raw || guard.origin_ns == 0 ||
        guard.end_ns <= guard.origin_ns || guard.end_ns - guard.origin_ns > UINT64_C(300000000000) ||
        guard.last_observed_ns < guard.origin_ns || guard.last_observed_ns >= guard.end_ns)
        return std::unexpected{HostQ0bWorkReason::invalid_input};
    return prepare_impl(parent, projection, lower, leaves, nullptr, guard);
}
auto HostQ0bGovernedWork::prepare_source(const HostFrozenSourceChildQuota& quota,
    platform_linux::FileDescriptor& projection, HostQ0bBackendDependencies lower,
    HostQ0bGovernedLeaves leaves) noexcept
    -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason> {
    return prepare_impl(quota.parent_pid(), projection, lower, leaves, &quota);
}
auto HostQ0bGovernedWork::prepare_impl(int parent, platform_linux::FileDescriptor& projection,
    HostQ0bBackendDependencies lower, HostQ0bGovernedLeaves leaves,
    const HostFrozenSourceChildQuota* quota, std::optional<HostQ0bInvocationGuard> invocation) noexcept
    -> std::expected<HostQ0bGovernedWork, HostQ0bWorkReason> {
    if (parent <= 0 || !projection.active() || projection.native_handle() < 3 || projection.native_handle() >= 1024 ||
        !lower.load || !lower.acquire || !lower.make_operations || !lower.clock ||
        lower.prepare_baseline || lower.stage_admission.admit ||
        !leaves.limits.identity || !leaves.limits.get || !leaves.limits.maximum_finite ||
        !leaves.limits.guard.now || !leaves.limits.guard.cancelled)
        return std::unexpected{HostQ0bWorkReason::invalid_input};
    if (quota && (!quota->ready() || !quota->guard()))
        return std::unexpected{HostQ0bWorkReason::association};
    auto ledger = quota ? work::ResourceLedger::prepare(quota->resources()) : work::ResourceLedger::prepare();
    if (!ledger) return std::unexpected{HostQ0bWorkReason::allocation};
    try {
        auto state = std::make_shared<HostQ0bGovernedState>(parent, std::move(*ledger), lower, leaves,
            quota ? quota->binding_ : nullptr, invocation);
        state->self = state;
        if (quota) {
            // Every lower read/limit leaf uses this exact authenticated binding
            // and owning cancellation token. Its absolute phase/PERMIT fence
            // remains in force even where a legacy algorithm has a local span.
            const work::GuardOperations original{
                const_cast<HostFrozenSourceLaunchBinding*>(quota->binding_),
                [](void* raw, std::uint64_t* output) noexcept -> native::RawStatusResult {
                    auto& binding = *static_cast<HostFrozenSourceLaunchBinding*>(raw);
                    auto checked = binding.guard();
                    if (!checked) return {-1, checked.error() == HostFrozenSourceLaunchReason::deadline
                        ? ETIMEDOUT : checked.error() == HostFrozenSourceLaunchReason::cancelled ? ECANCELED : EINVAL};
                    *output = binding.last_ns_;
                    return {0, 0};
                },
                [](void* raw) noexcept {
                    return static_cast<HostFrozenSourceLaunchBinding*>(raw)->owning_cancellation().requested();
                }};
            state->leaves.headroom.guard = original;
            state->leaves.limits.guard = original;
            if (!quota->ready() || !quota->guard()) return std::unexpected{HostQ0bWorkReason::association};
            quota->binding_->quota_claimed_ = true;
        }
        if (invocation) {
            // Relative five-second leaf spans use this same RAW clock. They
            // never replace the original invocation end or cancellation token.
            const work::GuardOperations original{state.get(),
                [](void* raw, std::uint64_t* output) noexcept -> native::RawStatusResult {
                    auto& owner = *static_cast<HostQ0bGovernedState*>(raw);
                    if (!owner.guard()) return {-1, 0}; // Semantic refusal, no invented errno.
                    *output = owner.last_ns;
                    return {0, 0};
                },
                [](void* raw) noexcept {
                    auto& owner = *static_cast<HostQ0bGovernedState*>(raw);
                    const bool requested = owner.invocation->operations.cancelled(owner.invocation->operations.context);
                    if (requested && !owner.receipt.failure) {
                        owner.fail(HostQ0bWorkReason::cancelled);
                        owner.receipt.native_failure = work::Reason::cancelled;
                    }
                    return requested;
                }};
            state->leaves.headroom.guard = original;
            state->leaves.limits.guard = original;
            state->leaves.projection.clock = {state.get(),
                [](void* raw, clockid_t clock) noexcept -> native::RawClockResult {
                    auto& owner = *static_cast<HostQ0bGovernedState*>(raw);
                    if (!owner.guard()) return {-1, 0, {}};
                    // Keep the writer's local MONOTONIC span separate from RAW.
                    return owner.projection_clock.clock_gettime_raw(owner.projection_clock.context, clock);
                }};
            if (!state->guard()) return std::unexpected{*state->receipt.failure};
            // Verify the already delivered process/role envelope before the
            // first provider call. Original mapping demand is derived later.
            const auto envelope = verify_limits(parent,
                {core::ByteCount::from_base_units(UINT64_C(4294967296)),
                    core::ByteCount::from_base_units(UINT64_C(8388608)), 7}, state->leaves.limits);
            if (!envelope) return std::unexpected{state->receipt.failure.value_or(HostQ0bWorkReason::native_limits)};
            if (!state->guard()) return std::unexpected{*state->receipt.failure};
        }
        state->projection.emplace(std::move(projection));
        return HostQ0bGovernedWork{std::move(state)};
    } catch (...) { return std::unexpected{HostQ0bWorkReason::allocation}; }
}
auto HostQ0bGovernedWork::begin() noexcept -> bool {
    if (!state_ || state_->invoked) return false;
    state_->invoked = true; state_->active = true; return true;
}
auto HostQ0bGovernedWork::dependencies() const noexcept -> HostQ0bBackendDependencies {
    return {state_.get(),
        [](void* p, const std::filesystem::path& root) {
            auto& s = *static_cast<HostQ0bGovernedState*>(p); return s.lower.load(s.lower.context, root);
        },
        [](void* p, MonitoringHostInputTemplateAuthority input) {
            auto& s = *static_cast<HostQ0bGovernedState*>(p); return s.lower.acquire(s.lower.context, std::move(input));
        }, operations,
        [](void* p) { auto& s = *static_cast<HostQ0bGovernedState*>(p); return s.lower.clock(s.lower.context); },
        [](void* p) -> std::expected<void, core::Error> {
            auto& s = *static_cast<HostQ0bGovernedState*>(p);
            if (s.invocation && !s.guard()) return std::unexpected{reject(s, s.receipt.failure.value_or(HostQ0bWorkReason::association))};
            auto result = s.lower.after_composition ? s.lower.after_composition(s.lower.context) : std::expected<void, core::Error>{};
            if (result && s.invocation && !s.guard()) return std::unexpected{reject(s, s.receipt.failure.value_or(HostQ0bWorkReason::association))};
            return result;
        },
        [](void* p, std::string_view stage) -> std::expected<void, core::Error> {
            auto& s = *static_cast<HostQ0bGovernedState*>(p);
            if (s.invocation && !s.guard()) return std::unexpected{reject(s, s.receipt.failure.value_or(HostQ0bWorkReason::association))};
            auto result = s.lower.before_stage ? s.lower.before_stage(s.lower.context, stage) : std::expected<void, core::Error>{};
            if (result && s.invocation && !s.guard()) return std::unexpected{reject(s, s.receipt.failure.value_or(HostQ0bWorkReason::association))};
            return result;
        }, bind, {state_.get(), admit}};
}
void HostQ0bGovernedWork::end() noexcept {
    if (!state_) return;
    state_->active = false;
    state_->session = nullptr; state_->plan = nullptr; state_->inputs = nullptr;
    state_->bandwidth = nullptr; state_->latency = nullptr;
    if (state_->projection && state_->projection->active()) {
        const auto closed = native::DescriptorAccess::close_raw(*state_->projection);
        if (!succeeded(closed)) {
            state_->fail(HostQ0bWorkReason::projection_close);
            state_->pending.push_back(std::move(*state_->projection_close)); state_->projection_close.reset();
        }
    }
    state_->projection.reset(); state_->receipt.phase = HostQ0bWorkPhase::terminal;
}
auto HostQ0bGovernedWork::run_once(const std::filesystem::path& root, std::string_view qualification,
    runtime::ControllerProcessProductionAuthority& authority) noexcept
    -> std::expected<MonitoringHostQ0bBackendResult, Q0bBackendFailure> {
    if (!begin()) return std::unexpected{Q0bBackendFailure{error(HostQ0bWorkReason::state), {}}};
    auto result = run_host_q0b_backend_driver_with_dependencies(root, qualification, authority, dependencies());
    if (result && state_->invocation && !state_->guard())
        result = std::unexpected{Q0bBackendFailure{reject(*state_, state_->receipt.failure.value_or(HostQ0bWorkReason::association)), {}}};
    end();
    // Pending failures only arise before lower operations: the driver has no
    // secondary stock there. Later stage admission carries its own secondaries.
    if (!state_->pending.empty()) {
        if (!result) {
            if (!result.error().secondary.empty()) std::terminate();
            result.error().secondary = std::move(state_->pending);
        } else {
            auto primary = std::move(state_->pending.front());
            state_->pending.clear();
            return std::unexpected{Q0bBackendFailure{std::move(primary), {}}};
        }
    }
    state_->receipt.successful = result.has_value();
    return result;
}
auto HostQ0bGovernedWork::snapshot() const noexcept -> HostQ0bWorkSnapshot {
    if (!state_) return {};
    auto value = state_->receipt;
    value.last_accepted_raw_ns = state_->last_ns;
    value.outstanding = state_->ledger.outstanding(); value.poisoned = state_->ledger.poisoned();
    return value;
}
auto HostQ0bGovernedWork::original_projection() const noexcept -> std::string_view {
    return state_ && state_->original ? state_->original->physical_projection() : std::string_view{};
}
auto production_host_q0b_governed_leaves() noexcept -> HostQ0bGovernedLeaves {
    return {work::production_read_operations(), work::production_limit_operations(), file::production_operations()};
}

} // namespace epycbench::monitoring::qualification::detail
