#include "runtime/pairwise_v2_native_lifecycle.hpp"
#include "runtime/pairwise_v2_production_control_monitor.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::runtime::detail {
namespace native = platform_linux::detail;
using Stage = NativePairwiseStage;
using Kind = NativePairwiseFailureKind;
using Phase = NativePairwisePhase;

auto NativeRunnerStartupBudget::start(ControllerClock clock,
    std::chrono::milliseconds duration, std::chrono::milliseconds quantum) noexcept
    -> std::expected<NativeRunnerStartupBudget, NativePairwiseFailure> {
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
        ControllerTimePoint::duration::max());
    if (!clock.now || duration.count() < 0 || quantum.count() <= 0)
        return std::unexpected(NativePairwiseFailure{Kind::invalid_input, Stage::preparation,
            Phase::startup, std::nullopt});
    if (duration > maximum || quantum > maximum)
        return std::unexpected(NativePairwiseFailure{Kind::clock_overflow, Stage::preparation,
            Phase::startup, std::nullopt});
    const auto delta = std::chrono::duration_cast<ControllerTimePoint::duration>(duration);
    const auto now = clock.now(clock.context);
    if (now > ControllerTimePoint::max() - delta)
        return std::unexpected(NativePairwiseFailure{Kind::clock_overflow, Stage::preparation,
            Phase::startup, std::nullopt});
    return NativeRunnerStartupBudget{clock, now, now + delta,
        std::chrono::duration_cast<ControllerTimePoint::duration>(quantum)};
}

auto NativeRunnerStartupBudget::point() noexcept -> bool {
    if (failure_) return false;
    const auto now = clock_.now(clock_.context);
    if (now < last_ || now >= deadline_) {
        failure_ = NativePairwiseFailure{now < last_ ? Kind::clock_regression : Kind::timeout,
            Stage::startup_control, Phase::startup, std::nullopt};
        return false;
    }
    last_ = now;
    return true;
}

auto NativeRunnerStartupBudget::poll_timeout() noexcept -> int {
    if (!point()) return -1;
    const auto remaining = std::min(deadline_ - last_, quantum_);
    const auto millis = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<int>(std::min<std::int64_t>(millis, std::numeric_limits<int>::max()));
}

struct PairwiseV2NativeProtocolAccess final {
    static auto controller(PairwiseV2ControllerResources& role) noexcept -> bool {
        const auto& identity = role.identity();
        const PairwiseV2PairIdentity pair{identity.pair.first, identity.pair.second};
        if (pairwise_v2_resource_identity_commitment(identity) != role.authenticated_identity_ ||
            role.phase_a.control.phase_ != PairwiseV2ControlWriterState::Phase::requested_acknowledged ||
            role.phase_a.event.phase_ != PairwiseV2EventReaderState::Phase::accepted ||
            role.phase_a.control.identity_ != pair || role.phase_a.event.identity_ != pair)
            return false;
        role.phase_a.control.phase_ = PairwiseV2ControlWriterState::Phase::invalid;
        return true;
    }
    static auto runner(PairwiseV2RunnerResources& role) noexcept -> bool {
        const auto& identity = role.identity();
        const PairwiseV2PairIdentity pair{identity.pair.first, identity.pair.second};
        if (pairwise_v2_resource_identity_commitment(identity) != role.authenticated_identity_ ||
            role.control.phase_ != PairwiseV2ControlReaderState::Phase::requested ||
            role.events.phase_ != PairwiseV2EventWriterState::Phase::accepted ||
            !role.control.identity_ || *role.control.identity_ != pair || role.events.identity_ != pair ||
            !role.admission.matches_request(pair.first, pair.second, identity.request_body_sha256))
            return false;
        role.control.phase_ = PairwiseV2ControlReaderState::Phase::invalid;
        return true;
    }
    static auto take_runner(PairwiseV2RunnerResources& role) noexcept
        -> std::optional<PairwiseV2ControlReadPermit> {
        const auto identity = role.identity().pair;
        if (!runner(role)) return std::nullopt;
        return PairwiseV2ControlReadPermit{{identity.first, identity.second}};
    }
};

namespace {
using FixedFrame = PairwiseV2ControlFrame;
constexpr std::array reasons{"user_interrupt", "controller_shutdown", "controller_timeout"};
constexpr std::array phases{"startup", "admission", "service", "execution", "cleanup"};
auto make_cancel(PairwiseV2PairIdentity pair, std::size_t reason,
                 std::optional<std::size_t> phase) noexcept -> FixedFrame {
    return pairwise_v2_cancel_frame(pair, reason, phase);
}
struct FramePosition final {
    std::array<std::byte, 4> prefix{};
    std::size_t progress{}, target{4}, count{};
    void consume(std::span<const std::byte> bytes) noexcept {
        for (auto b : bytes) {
            if (progress < 4) prefix[progress] = b;
            ++progress;
            if (progress == 4) {
                std::uint32_t length{};
                for (auto part : prefix) length = (length << 8U) | std::to_integer<unsigned>(part);
                target = 4 + static_cast<std::size_t>(length);
            }
            if (progress == target) { ++count; progress = 0; target = 4; }
        }
    }
};
auto fixed_failure(Kind kind, Stage stage, Phase phase = Phase::startup) noexcept -> NativePairwiseFailure {
    return {kind, stage, phase, std::nullopt};
}
auto retryable(native::FixedProcessFailure failure, std::size_t progress) noexcept -> bool {
    return failure.kind == native::FixedProcessFailureKind::would_block ||
        (failure.kind == native::FixedProcessFailureKind::interrupted && progress == 0);
}
auto prepared_failure(native::FixedProcessFailure failure) noexcept -> PreparedIoFailure {
    if (failure.kind == native::FixedProcessFailureKind::would_block) return PreparedIoFailure::would_block;
    if (failure.kind == native::FixedProcessFailureKind::interrupted) return PreparedIoFailure::interrupted;
    return PreparedIoFailure::io;
}
auto preparation_error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"runtime.ipc.io_failure"},
        .message = "Native pairwise startup I/O failed", .operation = "native_pairwise_startup",
        .os_error = std::nullopt, .context = {{"classification", classification}},
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command};
}
struct StartupIo final : ipc::ByteWriter, ipc::ByteReader {
    platform_linux::LocalIpcEndpoint* endpoint{};
    void* context{};
    bool (*point)(void*) noexcept{};
    int (*timeout)(void*) noexcept{};
    void (*failed)(void*, native::FixedProcessFailure) noexcept{};
    void (*progressed)(void*, const FramePosition&) noexcept{};
    void (*eof)(void*) noexcept{};
    FramePosition position;
    auto transfer(std::span<std::byte> input, std::span<const std::byte> output, bool writing)
        -> std::expected<std::size_t, core::Error> {
        while (point(context)) {
            const auto milliseconds = timeout(context);
            if (milliseconds < 0) break;
            const auto ready = native::LocalIpcAccess::poll(*endpoint, writing, milliseconds);
            if (!point(context)) break;
            if (!ready) {
                if (ready.error().kind == native::FixedProcessFailureKind::interrupted) {
                    if (position.progress == 0) return std::unexpected{preparation_error("interrupted")};
                }
                failed(context, ready.error());
                return std::unexpected{preparation_error("partial_io")};
            }
            if (ready->timed_out) continue;
            auto io = writing ? native::LocalIpcAccess::try_send(*endpoint, output)
                              : native::LocalIpcAccess::try_receive(*endpoint, input);
            if(io && *io==0 && !writing && eof)eof(context);
            if (io && *io != 0) {
                position.consume(writing ? output.first(*io) : std::span<const std::byte>{input.first(*io)});
                if (progressed) progressed(context, position);
            }
            if (!point(context)) break;
            if (!io) {
                if (io.error().kind == native::FixedProcessFailureKind::would_block) continue;
                if (io.error().kind == native::FixedProcessFailureKind::interrupted && position.progress == 0)
                    return std::unexpected{preparation_error("interrupted")};
                failed(context, io.error());
                return std::unexpected{preparation_error("partial_io")};
            }
            return *io;
        }
        return std::unexpected{preparation_error("native_lifecycle_stopped")};
    }
    auto write(std::span<const std::byte> bytes) -> std::expected<std::size_t, core::Error> override {
        return transfer({}, bytes, true);
    }
    auto read(std::span<std::byte> bytes) -> std::expected<std::size_t, core::Error> override {
        return transfer(bytes, {}, false);
    }
};
auto valid_config(const ControllerLifecycleConfig& config) noexcept -> bool {
    return config.startup_timeout.count() >= 0 && config.execution_timeout.count() >= 0 &&
        config.cancellation_grace.count() >= 0 && config.completion_grace.count() >= 0 &&
        config.termination_grace.count() >= 0 && config.observation_quantum.count() > 0 &&
        config.observation_quantum.count() <= std::numeric_limits<int>::max();
}
} // namespace

struct PairwiseV2NativeController::Storage final {
    PairwiseV2ComposedStartupController startup;
    PairwiseV2PairIdentity pair;
    ControllerLifecycleConfig config;
    NativePairwiseDependencies dependencies;
    NativePairwiseOutcome outcome;
    std::optional<platform_linux::LocalIpcChannels> channels;
    std::optional<platform_linux::ChildProcess> child;
    std::optional<PairwiseV2ActiveEvidenceController> evidence;
    std::optional<progress::Offer> progress_offer;
    std::optional<progress::Receiver> progress_receiver;
    std::array<FixedFrame, 3> cancels;
    std::array<FixedFrame, 15> cancelled;
    std::vector<std::byte> first_frame;
    std::size_t first_size{}, first_cursor{}, first_target{4};
    FramePosition stream_position;
    std::optional<native::FixedProcessFailure> last_io_failure;
    std::optional<ControllerTimePoint> last_now;
    ControllerTimePoint deadline{};
    Phase phase{Phase::startup};
    Stage stage{Stage::preparation};
    enum class State : unsigned char { prepared, started, retired, consumed } state{State::prepared};
    bool have_deadline{}, stop{}, sending_cancel{}, cancel_attempted{}, stream_resolved{}, writing_progress_ack{}, root_transferred{}, observer_failed{};

    Storage(PairwiseV2ComposedStartupController value, PairwiseV2PairIdentity identity,
            ControllerLifecycleConfig policy, NativePairwiseDependencies operations)
        : startup{std::move(value)}, pair{identity}, config{policy}, dependencies{operations},
          first_frame(static_cast<std::size_t>(ipc::maximum_frame_body_size) + 4) {
        for (std::size_t reason = 0; reason < reasons.size(); ++reason) {
            cancels[reason] = make_cancel(pair, reason, std::nullopt);
            for (std::size_t at = 0; at < phases.size(); ++at)
                cancelled[reason * phases.size() + at] = make_cancel(pair, reason, at);
        }
    }
    bool lifecycle_eof{};
    auto observe(NativeLifecycleKind kind, Stage at, bool succeeded = true,
        std::optional<NativePairwiseFailure> failure = std::nullopt,
        std::optional<NativeLifecycleEndpoint> endpoint = std::nullopt,
        bool secondary_failure = false, bool unselected_failure = false) noexcept -> bool {
        if (observer_failed) return false;
        if(kind==NativeLifecycleKind::eof){if(lifecycle_eof)return true;lifecycle_eof=true;}
        if (!dependencies.lifecycle.capture) return true;
        if (dependencies.lifecycle.capture(dependencies.lifecycle.context,
                {kind, at, phase, failure, outcome.exit, outcome.cancellation,
                 outcome.runner_observation, succeeded, endpoint, secondary_failure, unselected_failure,
                 outcome.cancel_sent, outcome.cancelled_received})) return true;
        observer_failed = true;
        stop = true;
        const auto error = fixed_failure(Kind::results, Stage::results, phase);
        if (!outcome.primary) outcome.primary = error;
        else if (outcome.cleanup_count < outcome.cleanup.size())
            outcome.cleanup[outcome.cleanup_count++] = error;
        return false;
    }
    void record(Kind kind, Stage at, std::optional<native::FixedProcessFailure> error = std::nullopt,
                bool cleanup = false) noexcept {
        const NativePairwiseFailure failure{kind, at, phase, error};
        const bool secondary = outcome.primary && cleanup && outcome.cleanup_count < outcome.cleanup.size();
        const bool unselected = outcome.primary && !secondary;
        if (kind == Kind::timeout) outcome.timeout_phases |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(phase));
        if (!outcome.primary) outcome.primary = failure;
        else if (cleanup && outcome.cleanup_count < outcome.cleanup.size())
            outcome.cleanup[outcome.cleanup_count++] = failure;
        static_cast<void>(observe(NativeLifecycleKind::failure, at, false, failure, {}, secondary, unselected));
    }
    auto now() noexcept -> std::optional<ControllerTimePoint> {
        const auto value = dependencies.clock.now(dependencies.clock.context);
        if (last_now && value < *last_now) {
            record(Kind::clock_regression, stage); stop = true; return std::nullopt;
        }
        last_now = value; return value;
    }
    auto deadline_fits(std::chrono::milliseconds duration, ControllerTimePoint current) noexcept -> bool {
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(ControllerTimePoint::duration::max());
        if (duration.count() < 0 || duration > maximum) {
            record(Kind::clock_overflow, stage); stop = true; return false;
        }
        const auto delta = std::chrono::duration_cast<ControllerTimePoint::duration>(duration);
        if (current > ControllerTimePoint::max() - delta) {
            record(Kind::clock_overflow, stage); stop = true; return false;
        }
        return true;
    }
    auto enter(Phase next, std::chrono::milliseconds duration, ControllerTimePoint current) noexcept -> bool {
        if (!deadline_fits(duration, current)) return false;
        deadline = current + std::chrono::duration_cast<ControllerTimePoint::duration>(duration);
        phase = next; have_deadline = true; return true;
    }
    void request_cancel(PairwiseV2CancellationReason reason, Kind kind, ControllerTimePoint current) noexcept {
        if (outcome.cancellation) return;
        outcome.cancellation = reason;
        record(kind, stage);
        static_cast<void>(observe(NativeLifecycleKind::cancelled, stage));
        if (!outcome.request_complete || outcome.completion_received) { stop = true; return; }
        static_cast<void>(enter(Phase::cancellation, config.cancellation_grace, current));
    }
    auto point() noexcept -> bool {
        auto current = now();
        if (!current) return false;
        if (phase != Phase::termination && !outcome.cancellation && dependencies.cancellation.observe) {
            const auto reason = dependencies.cancellation.observe(dependencies.cancellation.context);
            if (reason != ControllerCancellation::none)
                request_cancel(reason == ControllerCancellation::user_interrupt ? PairwiseV2CancellationReason::user_interrupt
                    : PairwiseV2CancellationReason::controller_shutdown, Kind::cancelled, *current);
        }
        if (!stop && have_deadline && *current >= deadline) {
            if (phase == Phase::startup || phase == Phase::execution)
                request_cancel(PairwiseV2CancellationReason::controller_timeout, Kind::timeout, *current);
            else { record(Kind::timeout, stage); stop = true; }
        }
        if (!stop && outcome.cancellation && outcome.request_complete && !sending_cancel && !cancel_attempted &&
            !writing_progress_ack)
            send_cancel();
        return !stop;
    }
    auto timeout() noexcept -> int {
        auto current = now();
        if (!current || !have_deadline || *current >= deadline) return -1;
        const auto remain = deadline - *current;
        const auto quantum = std::chrono::duration_cast<ControllerTimePoint::duration>(config.observation_quantum);
        const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(std::min(remain, quantum)).count();
        return static_cast<int>(std::min<std::int64_t>(milliseconds, std::numeric_limits<int>::max()));
    }
    auto poll(platform_linux::LocalIpcEndpoint& endpoint, bool writing, std::size_t progress = 0) noexcept -> bool {
        if (!point()) return false;
        const auto duration = timeout();
        if (duration < 0) { static_cast<void>(point()); return false; }
        const auto result = native::LocalIpcAccess::poll(endpoint, writing, duration);
        if (!point()) return false;
        if (!result) {
            if (result.error().kind == native::FixedProcessFailureKind::interrupted && progress == 0) return false;
            record(Kind::native, stage, result.error()); stop = true; return false;
        }
        return !result->timed_out;
    }
    void send_cancel() noexcept {
        cancel_attempted = true; sending_cancel = true;
        const auto old_stage = stage; stage = Stage::cancellation;
        const auto bytes = cancels[static_cast<std::size_t>(*outcome.cancellation)].view();
        std::size_t progress{};
        while (!stop && progress < bytes.size()) {
            if (!poll(channels->controller.control_writer, true, progress)) continue;
            const auto written = native::LocalIpcAccess::try_send(channels->controller.control_writer, bytes.subspan(progress));
            if (written) progress += *written;
            if (!point()) break;
            if (!written && !retryable(written.error(), progress)) {
                record(Kind::native, stage, written.error()); stop = true;
            }
        }
        outcome.cancel_sent = progress == bytes.size();
        if (outcome.cancel_sent) static_cast<void>(observe(NativeLifecycleKind::cancelled, Stage::cancellation));
        sending_cancel = false; stage = old_stage;
    }
    // The producer is waiting on this complete component. Only this callback may
    // enter the ordinary allocating Results/filesystem window after acceptance.
    auto consume_progress(const progress::Frame& frame) noexcept -> bool {
        const auto accepted = progress_receiver->accept(frame);
        if (!accepted) { record(Kind::protocol, Stage::evidence); stop = true; return false; }
        if (!*accepted) return true;
        const auto transition = progress_receiver->transition();
        if (!transition) { record(Kind::protocol, Stage::evidence); stop = true; return false; }
        ++outcome.progress_received;
        const auto old_stage = stage;
        stage = Stage::results;
        const auto receipt = dependencies.progress->consume_progress(dependencies.progress->context,
            ValidatedProgressView{*progress_offer, *transition});
        stage = old_stage;
        if (receipt.state == ProgressConsumeResult::State::failed) {
            record(Kind::results, Stage::results); stop = true; return false;
        }
        if (receipt.state == ProgressConsumeResult::State::stop) {
            const auto current = now();
            if (current) request_cancel(PairwiseV2CancellationReason::controller_shutdown, Kind::cancelled, *current);
            progress_receiver->stop();
            return point();
        }
        const auto ack = progress_receiver->acknowledgement(receipt.receipt);
        if (!ack) { record(Kind::protocol, Stage::results); stop = true; return false; }
        // Complete append is a retained fact even if its syscall returned after
        // expiry. It never bypasses the original absolute deadline for continue.
        outcome.last_journal_receipt = receipt.receipt;
        if (!point() || outcome.cancellation) { progress_receiver->stop(); return !stop; }
        writing_progress_ack = true;
        std::size_t offset{};
        while (!stop && !outcome.cancellation && offset < ack->size) {
            if (!poll(channels->controller.control_writer, true, offset)) continue;
            if (outcome.cancellation) break;
            const auto written = native::LocalIpcAccess::try_send(channels->controller.control_writer,
                ack->bytes().subspan(offset));
            if (written) offset += *written;
            else if (!retryable(written.error(), offset)) { record(Kind::native, Stage::evidence, written.error()); stop = true; }
            static_cast<void>(point());
        }
        writing_progress_ack = false;
        if (offset != ack->size) {
            // Never interleave cancel/terminal into a partially written ack.
            if (offset != 0U) stop = true;
            progress_receiver->stop();
            if (!stop) static_cast<void>(point());
            return !stop;
        }
        if (!progress_receiver->acknowledged()) { record(Kind::protocol, Stage::evidence); stop = true; return false; }
        ++outcome.progress_acknowledged;
        return point();
    }
    auto startup_io(platform_linux::LocalIpcEndpoint& endpoint, bool writing) noexcept -> StartupIo {
        StartupIo io;
        io.endpoint = &endpoint; io.context = this;
        io.point = [](void* raw) noexcept { return static_cast<Storage*>(raw)->point(); };
        io.timeout = [](void* raw) noexcept { return static_cast<Storage*>(raw)->timeout(); };
        io.failed = [](void* raw, native::FixedProcessFailure failure) noexcept {
            auto& self = *static_cast<Storage*>(raw); self.record(Kind::native, self.stage, failure); self.stop = true;
        };
        if (writing) io.progressed = [](void* raw, const FramePosition& position) noexcept {
            auto& self = *static_cast<Storage*>(raw);
            if (position.count >= 4 && !self.outcome.request_complete) {
                self.outcome.request_complete = true;
                static_cast<void>(self.observe(NativeLifecycleKind::request_sent, Stage::startup_control));
            }
        };
        else io.eof=[](void* raw) noexcept {
            static_cast<void>(static_cast<Storage*>(raw)->observe(NativeLifecycleKind::eof,Stage::eof));
        };
        return io;
    }
    void close(platform_linux::LocalIpcEndpoint& endpoint, Stage at, NativeLifecycleEndpoint slot) noexcept {
        if (!endpoint.active()) return;
        const auto result = native::LocalIpcAccess::close_fixed(endpoint);
        if (!result) { outcome.closed_with_error = true; record(Kind::native, at, result.error(), true); }
        static_cast<void>(observe(NativeLifecycleKind::endpoint_closed, at, result.has_value(), std::nullopt, slot));
    }
    void observe_child(bool blocking = false) noexcept {
        if (!child || !child->active()) return;
        const auto result = native::ProcessAccess::wait_fixed(*child, blocking);
        if (!result) {
            if (result.error().kind == native::FixedProcessFailureKind::interrupted) return;
            record(Kind::native, Stage::reap, result.error(), true); return;
        }
        if (*result) {
            outcome.exit = **result; outcome.exact_reap = true;
            static_cast<void>(observe(NativeLifecycleKind::reaped, Stage::reap));
        }
    }
    void classify_exit() noexcept {
        if (!outcome.exit || !stream_resolved) return;
        if (const auto* exited = std::get_if<platform_linux::ProcessExited>(&*outcome.exit)) {
            if (exited->code != 0) record(Kind::process_nonzero, Stage::process);
        } else record(Kind::process_signaled, Stage::process);
    }
    void retire() noexcept {
        if (state == State::retired || state == State::consumed) return;
        if (channels) {
            close(channels->runner.control_reader, Stage::parent_close, NativeLifecycleEndpoint::parent_runner_control);
            close(channels->runner.event_writer, Stage::parent_close, NativeLifecycleEndpoint::parent_runner_event);
            close(channels->controller.control_writer, Stage::endpoint_close, NativeLifecycleEndpoint::controller_control);
            close(channels->controller.event_reader, Stage::endpoint_close, NativeLifecycleEndpoint::controller_event);
        }
        if (child && child->active()) observe_child();
        if (child && child->active()) {
            outcome.terminate_attempted = true;
            const auto sent = native::ProcessAccess::signal_fixed(*child, platform_linux::ProcessSignal::terminate);
            if (!sent) record(Kind::native, Stage::terminate, sent.error(), true);
            static_cast<void>(observe(NativeLifecycleKind::termination_sent, Stage::terminate, sent.has_value()));
            stage = Stage::terminate;
            auto current = now();
            bool grace = current && enter(Phase::termination, config.termination_grace, *current);
            // No endpoint remains open. Nonblocking exact-child observations consume this absolute grace.
            while (grace && child->active()) {
                observe_child();
                if (!child->active()) break;
                const auto duration = timeout();
                if (duration < 0) break;
                const auto waited = native::LocalIpcAccess::pause(channels->controller.event_reader, duration);
                if (!waited && waited.error().kind != native::FixedProcessFailureKind::interrupted) {
                    record(Kind::native, Stage::terminate, waited.error(), true); break;
                }
                current = now();
                grace = current && *current < deadline;
            }
            if (child->active()) {
                if (current && *current >= deadline && phase == Phase::termination)
                    outcome.timeout_phases |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(Phase::termination));
                outcome.kill_attempted = true;
                const auto killed = native::ProcessAccess::signal_fixed(*child, platform_linux::ProcessSignal::kill);
                if (!killed) record(Kind::native, Stage::kill, killed.error(), true);
                static_cast<void>(observe(NativeLifecycleKind::kill_sent, Stage::kill, killed.has_value()));
                // Exact reap owns safe EINTR retry, not a new grace window.
                do {
                    const auto reaped = native::ProcessAccess::wait_fixed(*child, true);
                    if (!reaped) {
                        if (reaped.error().kind == native::FixedProcessFailureKind::interrupted) continue;
                        record(Kind::native, Stage::reap, reaped.error(), true); break;
                    }
                    if (*reaped) {
                        outcome.exit = **reaped; outcome.exact_reap = true;
                        static_cast<void>(observe(NativeLifecycleKind::reaped, Stage::reap));
                    }
                    else { record(Kind::native, Stage::reap, std::nullopt, true); break; }
                } while (child->active());
            }
            if (child->active()) {
                record(Kind::unreapable, Stage::reap, std::nullopt, true); outcome.unreapable = true;
                if (dependencies.fatal.unreapable) dependencies.fatal.unreapable(dependencies.fatal.context);
                std::terminate();
            }
        }
        classify_exit();
        static_cast<void>(observe(NativeLifecycleKind::retired, Stage::endpoint_close,
            !outcome.closed_with_error && !outcome.unreapable));
        state = State::retired;
    }
};

PairwiseV2NativeController::PairwiseV2NativeController(std::unique_ptr<Storage> value) noexcept
    : storage_{std::move(value)} {}
PairwiseV2NativeController::PairwiseV2NativeController(PairwiseV2NativeController&&) noexcept = default;
PairwiseV2NativeController::~PairwiseV2NativeController() noexcept {
    if (storage_) storage_->retire();
}
auto PairwiseV2NativeController::auxiliary_bytes() noexcept -> std::size_t {
    return sizeof(Storage) + static_cast<std::size_t>(ipc::maximum_frame_body_size) + 4;
}
auto PairwiseV2NativeController::prepare(wire_v1::detail::StartupSessionId session,
    const PairwiseHostCapacityRequestTranscript& request, const RunnerContext& context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower,
    ControllerLifecycleConfig config, NativePairwiseDependencies dependencies,
    NativePairwiseLimits limits) noexcept
    -> std::expected<PairwiseV2NativeController, NativePairwiseFailure> {
    if (!valid_config(config) || !dependencies.channels.create || !dependencies.image ||
        !dependencies.clock.now || dependencies.arguments.empty() ||
        (dependencies.runner_mode != NativePairwiseRunnerMode::cooperative &&
         dependencies.runner_mode != NativePairwiseRunnerMode::prepared_production))
        return std::unexpected{fixed_failure(Kind::invalid_input, Stage::preparation)};
    if (limits.auxiliary_bytes < auxiliary_bytes())
        return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
    if (dependencies.progress && (dependencies.runner_mode != NativePairwiseRunnerMode::prepared_production ||
                                  !dependencies.progress->consume_progress))
        return std::unexpected{fixed_failure(Kind::invalid_input, Stage::preparation)};
    try {
        auto startup = PairwiseV2ComposedStartupController::prepare(session, request, context, lower, nullptr,
            dependencies.progress ? std::optional{dependencies.progress->configuration} : std::nullopt,
            dependencies.policy, dependencies.authority_source);
        if (!startup) {
            auto failure=fixed_failure(Kind::preparation, Stage::preparation);
            failure.startup=startup.error();
            return std::unexpected{failure};
        }
        const auto offered = startup->progress_before_ack()
            ? std::optional{*startup->progress_before_ack()} : std::nullopt;
        auto storage = std::make_unique<Storage>(std::move(*startup),
            PairwiseV2PairIdentity{request.first_request_id(), request.second_request_id()}, config, dependencies);
        if (offered) {
            auto receiver = progress::Receiver::prepare(*offered);
            if (!receiver) return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            storage->progress_offer = *offered;
            storage->progress_receiver.emplace(std::move(*receiver));
        }
        return PairwiseV2NativeController{std::move(storage)};
    } catch (...) { return std::unexpected{fixed_failure(Kind::preparation, Stage::preparation)}; }
}
auto PairwiseV2NativeController::prepared_capacity_offer() const noexcept
    -> const wire_v1::detail::AuthoredCapacityOffer* {
    return storage_&&storage_->state==Storage::State::prepared?storage_->startup.prepared_capacity_offer():nullptr;
}
auto PairwiseV2NativeController::progress_before_ack() const noexcept -> const progress::Offer* {
    return storage_&&storage_->state==Storage::State::prepared?storage_->startup.progress_before_ack():nullptr;
}
auto PairwiseV2NativeController::bind_progress_consumer(NativeProgressConsumer consumer) noexcept
    -> std::expected<void,NativePairwiseFailure> {
    if(!storage_||storage_->state!=Storage::State::prepared||storage_->dependencies.progress||
       storage_->dependencies.runner_mode!=NativePairwiseRunnerMode::prepared_production||
       !storage_->progress_offer||!consumer.consume_progress)
        return std::unexpected{fixed_failure(Kind::invalid_state,Stage::preparation)};
    const auto& offer=*storage_->progress_offer;
    const progress::Configuration expected{offer.limits,offer.durability,offer.platform,offer.utc};
    if(consumer.configuration!=expected)
        return std::unexpected{fixed_failure(Kind::invalid_input,Stage::preparation)};
    storage_->dependencies.progress=consumer;
    return {};
}
auto PairwiseV2NativeController::start() noexcept -> std::expected<void, NativePairwiseFailure> {
    if (!storage_ || storage_->state != Storage::State::prepared)
        return std::unexpected{fixed_failure(Kind::invalid_state, Stage::preparation)};
    if(storage_->progress_offer&&!storage_->dependencies.progress)
        return std::unexpected{fixed_failure(Kind::invalid_input,Stage::preparation)};
    auto& self = *storage_;
    self.state = Storage::State::started;
    auto fail = [&]() -> std::expected<void, NativePairwiseFailure> {
        if (!self.outcome.primary) self.record(Kind::protocol, self.stage);
        self.stream_resolved = true; self.retire();
        return std::unexpected{*self.outcome.primary};
    };
    try {
        self.stage = Stage::channels;
        auto created = self.dependencies.channels.create(self.dependencies.channels.context);
        if (!created) { self.record(Kind::preparation, self.stage); return fail(); }
        self.channels.emplace(std::move(*created));
        self.stage = Stage::spawn;
        const auto before = self.now();
        if (!before || !self.deadline_fits(self.config.startup_timeout, *before) || !self.point()) return fail();
        auto spawned = self.dependencies.image->spawn(self.dependencies.arguments, self.channels->runner);
        if (!spawned) {
            self.record(Kind::native, self.stage);
            static_cast<void>(self.observe(NativeLifecycleKind::spawn_failed, Stage::spawn, false));
            return fail();
        }
        self.child.emplace(std::move(*spawned));
        // Take the origin before observer/journal work; a clock fault must not
        // erase the already observed successful spawn or reorder it after failure.
        const auto spawned_at = self.dependencies.clock.now(self.dependencies.clock.context);
        if (!self.observe(NativeLifecycleKind::spawned, Stage::spawn)) return fail();
        if (self.last_now && spawned_at < *self.last_now) {
            self.record(Kind::clock_regression, Stage::spawn); return fail();
        }
        self.last_now = spawned_at;
        if (!self.enter(Phase::startup, self.config.startup_timeout, spawned_at)) return fail();
        self.close(self.channels->runner.control_reader, Stage::parent_close, NativeLifecycleEndpoint::parent_runner_control);
        self.close(self.channels->runner.event_writer, Stage::parent_close, NativeLifecycleEndpoint::parent_runner_event);
        if (self.outcome.primary || !self.point()) return fail();
        if (self.dependencies.lifecycle.before_startup &&
            !self.dependencies.lifecycle.before_startup(self.dependencies.lifecycle.context)) {
            self.record(Kind::results, Stage::results);
            return fail();
        }
        if (!self.point()) return fail();
        self.stage = Stage::startup_control;
        auto writer = self.startup_io(self.channels->controller.control_writer, true);
        if (!self.startup.write_control(writer)) return fail();
        self.outcome.request_complete = true;
        self.stage = Stage::startup_events;
        auto reader = self.startup_io(self.channels->controller.event_reader, false);
        auto acknowledged = self.startup.read_acknowledgements(reader);
        if (!acknowledged || !std::holds_alternative<PairwiseV2ControllerResources>(*acknowledged)) return fail();
        auto resources = std::move(std::get<PairwiseV2ControllerResources>(*acknowledged));
        if (resources.progress_offer() != self.progress_offer) return fail();
        self.outcome.accepted = true;
        if (!self.observe(NativeLifecycleKind::accepted, Stage::startup_events)) return fail();
        // This local role activates only after actual acknowledgement decoding.
        self.outcome.activated = true;
        if (!PairwiseV2NativeProtocolAccess::controller(resources)) return fail();
        auto bound = PairwiseV2ActiveEvidenceController::bind(std::move(resources));
        if (!bound) return fail();
        self.evidence.emplace(std::move(*bound));
        if (!self.evidence->start() || !self.point()) return fail();
        if (!self.outcome.cancellation) {
            const auto accepted_at = self.now();
            if (!accepted_at || !self.enter(Phase::execution, self.config.execution_timeout, *accepted_at)) return fail();
        }
        return {};
    } catch (...) { self.record(Kind::preparation, self.stage); return fail(); }
}
auto PairwiseV2NativeController::finish() noexcept -> NativePairwiseOutcome {
    if (!storage_ || storage_->state == Storage::State::consumed) {
        NativePairwiseOutcome invalid; invalid.primary = fixed_failure(Kind::invalid_state, Stage::evidence); return invalid;
    }
    auto& self = *storage_;
    if (self.state == Storage::State::prepared) self.record(Kind::invalid_state, Stage::evidence);
    if (self.state == Storage::State::started && self.evidence) {
        self.stage = Stage::evidence;
        // Progress and terminal share this one reader. Once a base terminal
        // frame is selected, the existing evidence reader owns all later bytes.
        for (;;) {
            while (self.first_size < self.first_target && self.point()) {
                self.observe_child();
                if (!self.poll(self.channels->controller.event_reader, false, self.first_size)) continue;
                auto bytes = std::span{self.first_frame}.subspan(self.first_size, self.first_target - self.first_size);
                const auto read = native::LocalIpcAccess::try_receive(self.channels->controller.event_reader, bytes);
                if (read) {
                    self.first_size += *read;
                    if (*read == 0) {
                        static_cast<void>(self.observe(NativeLifecycleKind::eof,Stage::eof));
                        self.record(Kind::transport, Stage::eof); self.stop = true;
                    }
                    if (self.first_size == 4 && self.first_target == 4) {
                        std::uint32_t length{};
                        for (std::size_t i = 0; i < 4; ++i) length = (length << 8U) | std::to_integer<unsigned>(self.first_frame[i]);
                        if (length == 0 || length > ipc::maximum_frame_body_size) {
                            self.record(Kind::protocol, self.stage); self.stop = true;
                        } else self.first_target = 4 + static_cast<std::size_t>(length);
                    }
                } else if (!retryable(read.error(), self.first_size)) {
                    self.record(Kind::native, self.stage, read.error()); self.stop = true;
                }
                static_cast<void>(self.point());
            }
            if (!self.progress_receiver || self.stop || self.first_size != self.first_target) break;
            const auto frame = progress::decode(std::span<const std::byte>{self.first_frame.data(),self.first_size});
            if (!frame) {
                if (!self.outcome.cancellation && !self.progress_receiver->finish()) {
                    self.record(Kind::protocol, Stage::evidence); self.stop = true;
                }
                break;
            }
            if (!self.consume_progress(*frame)) break;
            self.first_size = 0U; self.first_cursor = 0U; self.first_target = 4U;
        }
        bool cancelled_frame = false;
        if (!self.stop && self.first_size == self.first_target) {
            const auto bytes = std::span<const std::byte>{self.first_frame.data(), self.first_size};
            for (std::size_t i = 0; i < self.cancelled.size(); ++i) {
                if (!std::ranges::equal(bytes, self.cancelled[i].view())) continue;
                cancelled_frame = true;
                const auto reason = static_cast<PairwiseV2CancellationReason>(i / phases.size());
                const auto observed = i % phases.size();
                const bool legal_phase = self.dependencies.runner_mode == NativePairwiseRunnerMode::cooperative
                    ? (observed == 1 || observed == 3) : (observed == 2 || observed == 3 || observed == 4);
                if (!self.outcome.cancel_sent || self.outcome.cancellation != reason || !legal_phase) {
                    self.record(Kind::protocol, Stage::cancellation); self.stop = true;
                } else {
                    self.outcome.cancelled_received = true;
                    self.outcome.runner_observation = static_cast<PairwiseV2CancellationPhase>(observed);
                    static_cast<void>(self.observe(NativeLifecycleKind::cancelled, Stage::cancellation));
                }
                break;
            }
        }
        if (!self.stop && !cancelled_frame) {
            self.outcome.success_started = true;
            const PreparedByteReader reader{&self, [](void* raw, std::span<std::byte> bytes) noexcept
                -> std::expected<std::size_t, PreparedIoFailure> {
                auto& state = *static_cast<Storage*>(raw);
                if (state.first_cursor < state.first_size) {
                    const auto count = std::min(bytes.size(), state.first_size - state.first_cursor);
                    std::copy_n(state.first_frame.data() + state.first_cursor, count, bytes.data());
                    state.first_cursor += count;
                    state.stream_position.consume(bytes.first(count)); return count;
                }
                const auto result = native::LocalIpcAccess::try_receive(state.channels->controller.event_reader, bytes);
                if (!result) { state.last_io_failure = result.error(); return std::unexpected{prepared_failure(result.error())}; }
                state.last_io_failure.reset();
                if (*result == 0) {
                    state.outcome.clean_eof = true;
                    static_cast<void>(state.observe(NativeLifecycleKind::eof, Stage::eof));
                }
                else state.stream_position.consume(bytes.first(*result));
                return *result;
            }};
            bool complete = false;
            while (!complete && self.point()) {
                self.observe_child();
                if (!self.point()) break;
                if (self.first_cursor == self.first_size && !self.poll(self.channels->controller.event_reader, false, self.stream_position.progress)) continue;
                const auto progress = self.evidence->read_step(reader, PreparedTransportSignal::none);
                if (!progress) { self.record(Kind::transport, Stage::evidence, self.last_io_failure); self.stop = true; }
                else complete = *progress == PreparedTransportProgress::complete;
                // Expiry under the old execution/grace deadline must precede any phase reset.
                static_cast<void>(self.point());
                if (!self.stop && self.evidence->completion_received() && !self.outcome.completion_received) {
                    self.outcome.completion_received = true;
                    static_cast<void>(self.observe(NativeLifecycleKind::completion, Stage::evidence));
                    if (!self.outcome.cancellation) {
                        const auto completion_at = self.now();
                        if (completion_at) static_cast<void>(self.enter(Phase::completion, self.config.completion_grace, *completion_at));
                    }
                }
                static_cast<void>(self.point());
            }
            if (complete && !self.stop && !self.outcome.cancellation) {
                self.stage = Stage::reconstruction;
                if (!self.evidence->reconstruct()) self.record(Kind::reconstruction, self.stage);
                else if (self.point()) {
                    self.stage = Stage::results;
                    auto result = self.evidence->project();
                    if (!result) self.record(Kind::results, self.stage);
                    else self.outcome.result.emplace(std::move(*result));
                    static_cast<void>(self.point());
                }
            }
        }
        if (cancelled_frame && !self.stop) {
            std::array<std::byte, 1> extra{};
            while (self.point()) {
                if (!self.poll(self.channels->controller.event_reader, false)) continue;
                const auto read = native::LocalIpcAccess::try_receive(self.channels->controller.event_reader, extra);
                if (read) {
                    if (*read == 0) {
                        self.outcome.clean_eof = true;
                        static_cast<void>(self.observe(NativeLifecycleKind::eof, Stage::eof));
                    }
                    else self.record(Kind::protocol, Stage::eof);
                    break;
                }
                if (!retryable(read.error(), 0)) { self.record(Kind::native, Stage::eof, read.error()); break; }
            }
        }
        self.stream_resolved = true;
        self.classify_exit();
        // An already stopped child can be reaped before endpoint cleanup.
        if (self.outcome.clean_eof) {
            self.close(self.channels->controller.control_writer, Stage::endpoint_close, NativeLifecycleEndpoint::controller_control);
            self.close(self.channels->controller.event_reader, Stage::endpoint_close, NativeLifecycleEndpoint::controller_event);
        }
        while (self.outcome.clean_eof && self.child && self.child->active() && self.point() && !self.stop) {
            self.observe_child();
            if (!self.point()) break;
            if (self.child->active()) {
                const auto duration = self.timeout();
                if (duration < 0) continue;
                const auto waited = native::LocalIpcAccess::pause(self.channels->controller.event_reader, duration);
                if (!waited && waited.error().kind != native::FixedProcessFailureKind::interrupted) {
                    self.record(Kind::native, Stage::reap, waited.error()); break;
                }
                static_cast<void>(self.point());
            }
        }
    }
    self.retire();
    // Final exact reap or endpoint cleanup can cross the operational deadline or
    // observe cancellation even when no active child remains for another loop.
    if (!self.outcome.primary) static_cast<void>(self.point());
    self.state = Storage::State::consumed;
    return std::move(self.outcome);
}

auto PairwiseV2NativeController::take_validated_bytes() noexcept
    -> std::expected<std::span<const std::byte>, NativePairwiseFailure> {
    if (!storage_ || storage_->state != Storage::State::consumed || storage_->root_transferred ||
        !storage_->outcome.completion_received || !storage_->outcome.clean_eof || !storage_->evidence)
        return std::unexpected{fixed_failure(Kind::invalid_state, Stage::evidence)};
    const auto bytes = storage_->evidence->validated_bytes();
    if (bytes.empty()) return std::unexpected{fixed_failure(Kind::invalid_state, Stage::evidence)};
    storage_->root_transferred = true;
    return bytes;
}

struct PairwiseV2NativeRunner::Storage final {
    platform_linux::RunnerIpcEndpoints endpoints;
    PairwiseV2ComposedStartupRunner startup;
    std::optional<NativeRunnerStartupBudget> startup_budget;
    NativeCooperativeBackend backend;
    std::optional<PreparedPairwiseRunnerExecution> production;
    std::optional<PairwiseV2ProductionControlMonitor> monitor;
    std::optional<PairwiseV2ActiveEvidenceRunner> evidence;
    std::optional<progress::Offer> progress_offer;
    std::optional<progress::Sender> progress_sender;
    std::optional<prepared_codec_v1::PreparedCanonicalWriter> progress_arena;
    // Borrowed by the placed monitor only until command completion or join.
    progress::EncodedFrame progress_frame;
    std::uint64_t progress_ordinal{1};
    NativePairwiseOutcome outcome;
    PairwiseV2FixedControlReader control_reader;
    std::array<FixedFrame, 15> cancelled;
    FramePosition stream_position;
    std::optional<native::FixedProcessFailure> last_io_failure;
    bool consumed{}, closed{}, backend_started{}, backend_retired{}, production_mode{};

    Storage(platform_linux::RunnerIpcEndpoints channels, PairwiseV2ComposedStartupRunner prepared,
            NativeCooperativeBackend implementation, PairwiseV2PairIdentity pair)
        : endpoints{std::move(channels)}, startup{std::move(prepared)}, backend{std::move(implementation)}, control_reader{pair} {
        for (std::size_t reason = 0; reason < reasons.size(); ++reason) {
            for (std::size_t at = 0; at < phases.size(); ++at)
                cancelled[reason * phases.size() + at] = make_cancel(pair, reason, at);
        }
    }
    ~Storage() noexcept { close(); }
    void fail(Kind kind, Stage stage, std::optional<native::FixedProcessFailure> error = std::nullopt) noexcept {
        if (!outcome.primary) outcome.primary = NativePairwiseFailure{kind, stage, Phase::execution, error};
    }
    void close() noexcept {
        if (closed) return;
        if (production && production->stage() == PreparedPairwiseExecutionStage::acquired) {
            if (monitor) monitor->phase(PairwiseV2CancellationPhase::cleanup);
            const auto finalization = production->finalize_without_execution();
            if (!finalization) fail(Kind::backend, Stage::backend);
        }
        if (monitor) {
            const auto final_control = monitor->stop_and_join();
            outcome.cancellation = final_control.cancellation;
            outcome.runner_observation = final_control.first_observation;
            if (final_control.failure) {
                const NativePairwiseFailure failure{
                    final_control.failure->kind == PairwiseV2ControlFailureKind::native ? Kind::native : Kind::protocol,
                    Stage::cancellation, Phase::completion, final_control.failure->native};
                if (!outcome.primary) outcome.primary = failure;
                else if (outcome.primary->stage != Stage::cancellation && outcome.cleanup_count < outcome.cleanup.size())
                    outcome.cleanup[outcome.cleanup_count++] = failure;
            } else if (outcome.cancellation && outcome.success_started && !outcome.primary) {
                outcome.primary = fixed_failure(Kind::cancelled, Stage::evidence, Phase::completion);
            }
        }
        if (!backend_retired) {
            backend_retired = true;
            if (backend.retire && !backend.retire(backend.context)) {
                const auto failure = fixed_failure(Kind::backend, Stage::backend, Phase::completion);
                if (!outcome.primary) outcome.primary = failure;
                else if (outcome.cleanup_count < outcome.cleanup.size()) outcome.cleanup[outcome.cleanup_count++] = failure;
            }
            backend.owner.reset();
        }
        for (auto* endpoint : {&endpoints.control_reader, &endpoints.event_writer}) {
            if (!endpoint->active()) continue;
            const auto result = native::LocalIpcAccess::close_fixed(*endpoint);
            if (!result) {
                const NativePairwiseFailure failure{Kind::native, Stage::endpoint_close, Phase::completion, result.error()};
                outcome.closed_with_error = true;
                if (!outcome.primary) outcome.primary = failure;
                else if (outcome.cleanup_count < outcome.cleanup.size()) outcome.cleanup[outcome.cleanup_count++] = failure;
            }
        }
        closed = true;
    }
    // The only control reader. A completed cancellation retains its first local observation.
    auto observe(int wait_ms = 0) noexcept -> bool {
        if (outcome.primary || outcome.cancellation) return !outcome.primary;
        const auto observed = control_reader.step(endpoints.control_reader, wait_ms,
            backend_started ? PairwiseV2CancellationPhase::execution : PairwiseV2CancellationPhase::admission);
        outcome.cancellation = observed.cancellation;
        outcome.runner_observation = observed.first_observation;
        if (observed.failure) {
            fail(observed.failure->kind == PairwiseV2ControlFailureKind::native ? Kind::native : Kind::protocol,
                 Stage::cancellation, observed.failure->native);
            return false;
        }
        return true;
    }
    auto observe_production(const PairwiseV2ControlSnapshot& observed) noexcept -> bool {
        outcome.cancellation = observed.cancellation;
        outcome.runner_observation = observed.first_observation;
        if (observed.failure) {
            fail(observed.failure->kind == PairwiseV2ControlFailureKind::native ? Kind::native : Kind::protocol,
                 Stage::cancellation, observed.failure->native);
            return false;
        }
        return !outcome.primary;
    }
    auto observe_production() noexcept -> bool {
        if (!monitor) { fail(Kind::invalid_state, Stage::cancellation); return false; }
        return observe_production(monitor->snapshot());
    }
    auto poll_write(std::size_t progress) noexcept -> bool {
        const auto ready = native::LocalIpcAccess::poll(endpoints.event_writer, true, 10);
        if (!ready) {
            if (retryable(ready.error(), progress)) return false;
            fail(Kind::native, Stage::evidence, ready.error()); return false;
        }
        return !ready->timed_out;
    }
    auto progress_pause() noexcept -> bool {
        static_cast<void>(monitor->wait_progress());
        return observe_production();
    }
    auto publish_progress(const progress::AvailableFact& fact) noexcept
        -> std::expected<bool, progress::Error> {
        if (!progress_sender || !progress_arena || !monitor)
            return std::unexpected{progress::Error::state};
        if (!observe_production()) return std::unexpected{progress::Error::state};
        if (outcome.cancellation) { progress_sender->cancel(); return false; }
        if (fact.kind == progress::Kind::preparation && !monitor->readiness())
            return std::unexpected{progress::Error::state};
        const auto component = fact.kind == progress::Kind::preparation
            ? progress::encode_preparation(*progress_arena, fact, *monitor->readiness())
            : progress::encode_component(*progress_arena, fact);
        if (!component) return std::unexpected{component.error()};
        const auto digest = core::sha256(std::string_view{component->data(), component->size()}).bytes();
        const progress::Transition transition{fact.kind, progress_ordinal, fact.operation,
            fact.availability, fact.receipt.epoch, digest, *component};
        const auto begun = progress_sender->begin(transition);
        if (!begun) return std::unexpected{begun.error()};
        while (!progress_sender->waiting()) {
            if (!observe_production()) return std::unexpected{progress::Error::state};
            if (outcome.cancellation) { progress_sender->cancel(); return false; }
            const auto next = progress_sender->next();
            if (!next) return std::unexpected{next.error()};
            progress_frame = *next;
            const auto decoded = progress::decode(progress_frame.bytes());
            if (!decoded) return std::unexpected{decoded.error()};
            const bool end = decoded->message == progress::Message::end;
            std::size_t offset{};
            while (offset < progress_frame.size) {
                if (!observe_production()) return std::unexpected{progress::Error::state};
                if (outcome.cancellation && offset == 0) { progress_sender->cancel(); return false; }
                if (end) {
                    const auto queued = monitor->submit_progress_end(endpoints.event_writer,
                        progress_frame.bytes().subspan(offset), progress_ordinal, digest);
                    if (!queued) {
                        fail(Kind::protocol, Stage::evidence, queued.error().native);
                        return std::unexpected{progress::Error::state};
                    }
                    // The placed thread owns both the pre-write control check and
                    // final write/arming operation. Backing survives until join,
                    // including cancellation while a command is outstanding.
                    for (;;) {
                        const auto result = monitor->take_progress_end_result();
                        if (result) {
                            if (*result) offset += **result;
                            else if (!result->error().native || !retryable(*result->error().native, offset)) {
                                fail(Kind::transport, Stage::evidence, result->error().native);
                                return std::unexpected{progress::Error::state};
                            }
                            break;
                        }
                        if (!progress_pause()) return std::unexpected{progress::Error::state};
                        if (outcome.cancellation) {
                            // The attempt may have transferred bytes. Never append
                            // a fallback terminal after an unresolved command.
                            fail(Kind::cancelled, Stage::evidence);
                            progress_sender->cancel();
                            return false;
                        }
                    }
                } else {
                    if (!poll_write(offset)) {
                        if (outcome.primary) return std::unexpected{progress::Error::state};
                        continue;
                    }
                    const auto written = native::LocalIpcAccess::try_send(endpoints.event_writer,
                        progress_frame.bytes().subspan(offset));
                    if (written) offset += *written;
                    else if (!retryable(written.error(), offset)) {
                        fail(Kind::native, Stage::evidence, written.error());
                        return std::unexpected{progress::Error::state};
                    }
                }
            }
            const auto transferred = progress_sender->transferred();
            if (!transferred) return std::unexpected{transferred.error()};
        }
        ++outcome.progress_received; // Runner fact: the complete component was written.
        for (;;) {
            const auto observed = monitor->take_ack();
            if (!observe_production(observed)) return std::unexpected{progress::Error::state};
            if (outcome.cancellation) { progress_sender->cancel(); return false; }
            if (const auto& ack = observed.acknowledgement) {
                const auto admitted = progress_sender->acknowledge(*ack);
                if (!admitted) return std::unexpected{admitted.error()};
                outcome.last_journal_receipt = progress::Acknowledgement{ack->payload.ordinal,
                    ack->payload.first_record, ack->payload.last_record, ack->payload.sha256,
                    ack->payload.journal_digest, ack->payload.receipt};
                ++outcome.progress_acknowledged;
                ++progress_ordinal; // Sender already rejected the overflowing ordinal.
                return true;
            }
            if (!progress_pause()) return std::unexpected{progress::Error::state};
        }
    }
    void send_cancelled() noexcept {
        const auto reason = static_cast<std::size_t>(*outcome.cancellation);
        const auto observed = static_cast<std::size_t>(*outcome.runner_observation);
        const auto bytes = cancelled[reason * phases.size() + observed].view();
        std::size_t progress{};
        while (!outcome.primary && progress < bytes.size()) {
            if (production_mode && !observe_production()) break;
            if (!poll_write(progress)) continue;
            const auto written = native::LocalIpcAccess::try_send(endpoints.event_writer, bytes.subspan(progress));
            if (written) progress += *written;
            else if (!retryable(written.error(), progress)) fail(Kind::native, Stage::cancellation, written.error());
        }
        outcome.cancelled_received = progress == bytes.size();
    }
};

PairwiseV2NativeRunner::PairwiseV2NativeRunner(std::unique_ptr<Storage> value) noexcept : storage_{std::move(value)} {}
PairwiseV2NativeRunner::PairwiseV2NativeRunner(PairwiseV2NativeRunner&&) noexcept = default;
PairwiseV2NativeRunner::~PairwiseV2NativeRunner() noexcept { if (storage_) storage_->close(); }
auto PairwiseV2NativeRunner::auxiliary_bytes() noexcept -> std::size_t { return sizeof(Storage); }
auto PairwiseV2NativeRunner::prepare(platform_linux::RunnerIpcEndpoints channels,
    wire_v1::detail::StartupSessionId session, ipc::RequestId first_request, RunnerContext context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower,
    NativeBackendPreparation backend, NativePairwiseLimits limits) noexcept
    -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure> {
    if (!channels.control_reader.active() || !channels.event_writer.active() || !backend.prepare)
        return std::unexpected{fixed_failure(Kind::invalid_input, Stage::preparation)};
    if (limits.auxiliary_bytes < auxiliary_bytes())
        return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
    try {
        StartupIo reader;
        reader.endpoint = &channels.control_reader;
        reader.point = [](void*) noexcept { return true; };
        reader.timeout = [](void*) noexcept { return 10; };
        reader.failed = [](void*, native::FixedProcessFailure) noexcept {};
        auto startup = PairwiseV2ComposedStartupRunner::admit(reader, session, first_request, std::move(context), lower);
        if (!startup || !startup->admitted_before_ack())
            return std::unexpected{fixed_failure(Kind::protocol, Stage::startup_control)};
        const auto& admitted = *startup->admitted_before_ack();
        const auto& request = admitted.request();
        const PairwiseV2PairIdentity pair{request.first_request_id(), request.second_request_id()};
        auto storage = std::make_unique<Storage>(std::move(channels), std::move(*startup), NativeCooperativeBackend{}, pair);
        auto prepared = backend.prepare(backend.context, *storage->startup.admitted_before_ack());
        if (!prepared) return std::unexpected{fixed_failure(Kind::backend, Stage::preparation)};
        storage->backend = std::move(*prepared);
        if (!storage->backend.step || !storage->backend.retire || !storage->backend.evidence.produce)
            return std::unexpected{fixed_failure(Kind::backend, Stage::preparation)};
        return PairwiseV2NativeRunner{std::move(storage)};
    } catch (...) { return std::unexpected{fixed_failure(Kind::preparation, Stage::preparation)}; }
}
auto PairwiseV2NativeRunner::run() noexcept -> NativePairwiseOutcome {
    if (!storage_ || storage_->consumed || storage_->production_mode) {
        NativePairwiseOutcome invalid; invalid.primary = fixed_failure(Kind::invalid_state, Stage::backend); return invalid;
    }
    auto& self = *storage_;
    self.consumed = true;
    auto finish = [&]() noexcept -> NativePairwiseOutcome { self.close(); return std::move(self.outcome); };
    StartupIo writer;
    writer.endpoint = &self.endpoints.event_writer;
    writer.point = [](void*) noexcept { return true; };
    writer.timeout = [](void*) noexcept { return 10; };
    writer.failed = [](void*, native::FixedProcessFailure) noexcept {};
    auto acknowledged = self.startup.write_acknowledgements(writer);
    if (!acknowledged || !std::holds_alternative<PairwiseV2RunnerResources>(*acknowledged)) {
        self.fail(Kind::protocol, Stage::startup_events); return finish();
    }
    auto resources = std::move(std::get<PairwiseV2RunnerResources>(*acknowledged));
    self.outcome.accepted = true;
    self.outcome.activated = true; // Every acknowledgement byte was actually written.
    if (!PairwiseV2NativeProtocolAccess::runner(resources)) {
        self.fail(Kind::protocol, Stage::startup_events); return finish();
    }
    auto active = PairwiseV2ActiveEvidenceRunner::bind(std::move(resources));
    if (!active) { self.fail(Kind::protocol, Stage::startup_events); return finish(); }
    self.evidence.emplace(std::move(*active));
    if (!self.observe()) return finish();
    if (self.outcome.cancellation) { self.send_cancelled(); return finish(); }
    for (;;) {
        self.backend_started = true;
        const auto step = self.backend.step(self.backend.context, self.outcome.cancellation);
        if (step == NativeBackendStep::failed) { self.fail(Kind::backend, Stage::backend); return finish(); }
        if (!self.observe(step == NativeBackendStep::pending ? 10 : 0)) return finish();
        if (self.outcome.cancellation) {
            // The cooperative callback observes the retained reason, without any other reader.
            static_cast<void>(self.backend.step(self.backend.context, self.outcome.cancellation));
            self.send_cancelled(); return finish();
        }
        if (step == NativeBackendStep::cancelled) { self.fail(Kind::backend, Stage::backend); return finish(); }
        if (step == NativeBackendStep::ready) break;
    }
    if (!self.evidence->produce(self.backend.evidence)) { self.fail(Kind::backend, Stage::backend); return finish(); }
    if (!self.observe()) return finish();
    if (self.outcome.cancellation) { self.send_cancelled(); return finish(); }
    if (!self.evidence->encode()) { self.fail(Kind::encoding, Stage::encoding); return finish(); }
    if (!self.observe()) return finish();
    if (self.outcome.cancellation) { self.send_cancelled(); return finish(); }
    self.outcome.success_started = true; // Terminal selection precedes the first evidence write.
    const PreparedByteWriter output{&self, [](void* raw, std::span<const std::byte> bytes) noexcept
        -> std::expected<std::size_t, PreparedIoFailure> {
        auto& state = *static_cast<Storage*>(raw);
        const auto sent = native::LocalIpcAccess::try_send(state.endpoints.event_writer, bytes);
        if (!sent) { state.last_io_failure = sent.error(); return std::unexpected{prepared_failure(sent.error())}; }
        state.last_io_failure.reset();
        state.stream_position.consume(bytes.first(*sent));
        return *sent;
    }};
    bool complete = false;
    while (!complete && !self.outcome.primary) {
        if (!self.observe()) break;
        if (self.outcome.cancellation) {
            static_cast<void>(self.evidence->write_step(output, PreparedTransportSignal::cancelled));
            self.fail(Kind::cancelled, Stage::evidence); break;
        }
        // The shared prepared writer owns frame progress and interruption classification.
        const auto ready = native::LocalIpcAccess::poll(self.endpoints.event_writer, true, 10);
        if (!ready) {
            if (ready.error().kind == native::FixedProcessFailureKind::interrupted && self.stream_position.progress == 0) continue;
            self.fail(Kind::native, Stage::evidence, ready.error()); break;
        }
        if (ready->timed_out) continue;
        const auto progress = self.evidence->write_step(output, PreparedTransportSignal::none);
        if (!progress) { self.fail(Kind::transport, Stage::evidence, self.last_io_failure); break; }
        complete = *progress == PreparedTransportProgress::complete;
    }
    self.outcome.completion_received = complete;
    return finish();
}

auto PairwiseV2NativeRunner::production_auxiliary_bytes(bool progress) noexcept -> std::size_t {
    const auto execution = PreparedPairwiseRunnerExecution::auxiliary_bytes(progress);
    const auto control = PairwiseV2ProductionControlMonitor::auxiliary_bytes();
    if (execution > std::numeric_limits<std::size_t>::max() - sizeof(Storage) ||
        control > std::numeric_limits<std::size_t>::max() - sizeof(Storage) - execution)
        return std::numeric_limits<std::size_t>::max();
    return sizeof(Storage) + execution + control;
}
auto PairwiseV2NativeRunner::prepare_production(platform_linux::RunnerIpcEndpoints channels,
    wire_v1::detail::StartupSessionId session, ipc::RequestId first_request, RunnerContext context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower,
    NativePreparedProductionDependencies dependencies, NativePairwiseLimits limits) noexcept
    -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure> {
    return prepare_production_impl(std::move(channels), std::move(session), first_request,
        std::move(context), lower, std::move(dependencies), limits, std::nullopt);
}

auto PairwiseV2NativeRunner::prepare_production_from_channel(
    platform_linux::RunnerIpcEndpoints channels, RunnerContext context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower,
    NativePreparedProductionDependencies dependencies, NativePairwiseLimits limits,
    NativeRunnerStartupBudget budget) noexcept
    -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure> {
    dependencies.typed.policy = stage_b_policy::Selection::stage_b_v1;
    return prepare_production_impl(std::move(channels), std::nullopt, std::nullopt,
        std::move(context), lower, std::move(dependencies), limits, budget);
}

auto PairwiseV2NativeRunner::prepare_production_impl(platform_linux::RunnerIpcEndpoints channels,
    std::optional<wire_v1::detail::StartupSessionId> session, std::optional<ipc::RequestId> first_request,
    RunnerContext context, PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower,
    NativePreparedProductionDependencies dependencies, NativePairwiseLimits limits,
    std::optional<NativeRunnerStartupBudget> budget) noexcept
    -> std::expected<PairwiseV2NativeRunner, NativePairwiseFailure> {
    if (!channels.control_reader.active() || !channels.event_writer.active())
        return std::unexpected{fixed_failure(Kind::invalid_input, Stage::preparation)};
    if (limits.auxiliary_bytes < production_auxiliary_bytes(dependencies.progress.has_value() || budget.has_value()))
        return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
    if (!budget && dependencies.progress && !dependencies.graph_limits)
        return std::unexpected{fixed_failure(Kind::invalid_input, Stage::preparation)};
    if (budget && !budget->point()) return std::unexpected(*budget->failure());
    try {
        // Each native session owns a fresh latch. Lower prepared APIs retain
        // injection for their actual composition, never native cross-run aliasing.
        dependencies.typed.shared_cancellation = std::make_shared<RunnerCancellation>();
        StartupIo reader;
        reader.endpoint = &channels.control_reader;
        reader.point = [](void*) noexcept { return true; };
        reader.timeout = [](void*) noexcept { return 10; };
        reader.failed = [](void*, native::FixedProcessFailure) noexcept {};
        if (budget) {
            reader.context = &*budget;
            reader.point = [](void* raw) noexcept { return static_cast<NativeRunnerStartupBudget*>(raw)->point(); };
            reader.timeout = [](void* raw) noexcept { return static_cast<NativeRunnerStartupBudget*>(raw)->poll_timeout(); };
        }
        auto startup = session && first_request
            ? PairwiseV2ComposedStartupRunner::admit(reader, std::move(*session), *first_request,
                std::move(context), lower, dependencies.progress, dependencies.typed.policy, dependencies.authority_source)
            : PairwiseV2ComposedStartupRunner::admit_from_channel(reader, std::move(context), lower,
                dependencies.progress, dependencies.authority_source);
        if (budget && !budget->point()) return std::unexpected(*budget->failure());
        if (!startup || !startup->admitted_before_ack())
            return std::unexpected{fixed_failure(Kind::protocol, Stage::startup_control)};
        if (budget) {
            const auto* graph = startup->production_graph_limits();
            if (!graph || !startup->progress_before_ack())
                return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            dependencies.graph_limits = *graph;
        }
        const auto& admitted = *startup->admitted_before_ack();
        const auto& request = admitted.request();
        const PairwiseV2PairIdentity pair{request.first_request_id(), request.second_request_id()};
        const auto offered = startup->progress_before_ack()
            ? std::optional{*startup->progress_before_ack()} : std::nullopt;
        if (offered) {
            std::array<std::uint64_t, progress::kind_count> ceilings{};
            for (std::size_t i = 0; i < ceilings.size(); ++i)
                ceilings[i] = offered->limits.components[i].bytes;
            const auto required = progress::limits(admitted.inputs().pairwise_request().first()
                .execution_inputs().measurement().repetition_maximum_blocks(), ceilings);
            if (!required) return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            for (std::size_t i = 0; i < ceilings.size(); ++i)
                if (offered->limits.components[i].count < required->components[i].count)
                    return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
        }
        auto monitor = PairwiseV2ProductionControlMonitor::prepare(admitted.inputs().pairwise_request().first(),
            pair, dependencies.typed.shared_cancellation, dependencies.monitor,
            PairwiseV2ProductionControlMonitor::auxiliary_bytes(), offered);
        if (budget && !budget->point()) return std::unexpected(*budget->failure());
        if (!monitor) return std::unexpected{fixed_failure(Kind::preparation, Stage::preparation)};
        auto execution = PreparedPairwiseRunnerExecution::prepare(admitted.inputs(), std::move(dependencies.typed),
            PreparedPairwiseRunnerExecution::auxiliary_bytes(offered.has_value()), offered.has_value(),
            dependencies.graph_limits);
        if (budget && !budget->point()) return std::unexpected(*budget->failure());
        if (!execution) return std::unexpected{fixed_failure(Kind::backend, Stage::preparation)};
        if (offered) {
            const auto* graph = execution->preparation_graph();
            if (!graph) return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            const auto bound = progress::preparation_ceiling(*graph, *dependencies.graph_limits);
            const auto* assignment = monitor->assignment();
            if (!bound || !assignment) return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            // Closed control row, singleton affinity and two exact hex stable IDs.
            std::uint64_t required = *bound;
            for (const auto length : {assignment->stable_pu.value().size(), assignment->stable_core.value().size()}) {
                if (length > (UINT64_MAX-required)/2U)
                    return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
                required += 2U*length;
            }
            if (required > UINT64_MAX-512U) return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            required += 512U;
            if (offered->limits.components[static_cast<std::size_t>(progress::Kind::preparation)].bytes < required ||
                offered->limits.components[static_cast<std::size_t>(progress::Kind::block_closed)].bytes < progress::block_closed_ceiling())
                return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
        }
        auto storage = std::make_unique<Storage>(std::move(channels), std::move(*startup), NativeCooperativeBackend{}, pair);
        storage->startup_budget = budget;
        storage->production_mode = true;
        storage->production.emplace(std::move(*execution));
        storage->monitor.emplace(std::move(*monitor));
        if (offered) {
            if (offered->limits.arena_bytes > std::numeric_limits<std::size_t>::max())
                return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            auto arena = prepared_codec_v1::PreparedCanonicalWriter::prepare_progress(
                static_cast<std::size_t>(offered->limits.arena_bytes));
            if (!arena || !arena->activate())
                return std::unexpected{fixed_failure(Kind::capacity, Stage::preparation)};
            storage->progress_offer = *offered;
            storage->progress_sender.emplace(*offered);
            storage->progress_arena.emplace(std::move(*arena));
        }
        if (storage->startup_budget && !storage->startup_budget->point())
            return std::unexpected(*storage->startup_budget->failure());
        return PairwiseV2NativeRunner{std::move(storage)};
    } catch (...) { return std::unexpected{fixed_failure(Kind::preparation, Stage::preparation)}; }
}
auto PairwiseV2NativeRunner::run_production() noexcept -> NativePreparedPairwiseOutcome {
    if (!storage_ || storage_->consumed || !storage_->production_mode) {
        NativePreparedPairwiseOutcome invalid;
        invalid.lifecycle.primary = fixed_failure(Kind::invalid_state, Stage::backend);
        return invalid;
    }
    auto& self = *storage_;
    self.consumed = true;
    auto finish = [&]() noexcept -> NativePreparedPairwiseOutcome {
        self.close();
        return {std::move(self.outcome), std::move(self.production), std::move(self.evidence), std::move(self.monitor)};
    };
    StartupIo writer;
    writer.endpoint = &self.endpoints.event_writer;
    writer.point = [](void*) noexcept { return true; };
    writer.timeout = [](void*) noexcept { return 10; };
    writer.failed = [](void*, native::FixedProcessFailure) noexcept {};
    if (self.startup_budget) {
        writer.context = &*self.startup_budget;
        writer.point = [](void* raw) noexcept { return static_cast<NativeRunnerStartupBudget*>(raw)->point(); };
        writer.timeout = [](void* raw) noexcept { return static_cast<NativeRunnerStartupBudget*>(raw)->poll_timeout(); };
    }
    auto acknowledged = self.startup.write_acknowledgements(writer);
    if (self.startup_budget && !self.startup_budget->point()) {
        self.outcome.primary = *self.startup_budget->failure();
        return finish();
    }
    if (!acknowledged || !std::holds_alternative<PairwiseV2RunnerResources>(*acknowledged)) {
        self.fail(Kind::protocol, Stage::startup_events); return finish();
    }
    auto resources = std::move(std::get<PairwiseV2RunnerResources>(*acknowledged));
    self.outcome.accepted = true;
    auto permit = PairwiseV2NativeProtocolAccess::take_runner(resources);
    if (resources.progress_offer() != self.progress_offer) {
        self.fail(Kind::protocol, Stage::startup_events); return finish();
    }
    if (!permit) { self.fail(Kind::protocol, Stage::startup_events); return finish(); }
    auto active = PairwiseV2ActiveEvidenceRunner::bind(std::move(resources));
    if (!active) { self.fail(Kind::protocol, Stage::startup_events); return finish(); }
    self.evidence.emplace(std::move(*active));
    const auto started = self.monitor->start(self.endpoints.control_reader, std::move(*permit));
    if (!started) { self.fail(Kind::preparation, Stage::cancellation); return finish(); }
    self.outcome.activated = true; // Production-only ordinary monitor initialization has ended.
    if (!self.observe_production()) return finish();
    if (self.outcome.cancellation) { self.send_cancelled(); return finish(); }
    const auto acquired = self.production->acquire();
    // A failed acquisition may own an actual pre-run operational ledger.
    if (!self.observe_production()) return finish();
    if (!acquired && !self.production->finalized_evidence()) {
        self.fail(Kind::backend, Stage::backend); return finish();
    }
    if (acquired && self.outcome.cancellation) {
        self.monitor->phase(PairwiseV2CancellationPhase::cleanup);
        const auto finalized = self.production->finalize_without_execution();
        if (!finalized) { self.fail(Kind::backend, Stage::backend); return finish(); }
        self.send_cancelled(); return finish();
    }
    if (acquired) {
        self.backend_started = true;
        self.monitor->phase(PairwiseV2CancellationPhase::execution);
        const auto before_finalization = [](void* raw) noexcept {
            static_cast<PairwiseV2ProductionControlMonitor*>(raw)->phase(PairwiseV2CancellationPhase::cleanup);
        };
        std::expected<bool, PairwiseRuntimeFailure> executed{true};
        if (self.progress_offer) {
            executed = self.production->execute_progress({&self,
                [](void* raw, const progress::AvailableFact& fact) noexcept {
                    return static_cast<Storage*>(raw)->publish_progress(fact);
                }}, &*self.monitor, before_finalization);
        } else {
            const auto legacy = self.production->execute(&*self.monitor, before_finalization);
            if (!legacy) executed = std::unexpected{legacy.error()};
        }
        self.monitor->phase(PairwiseV2CancellationPhase::cleanup);
        if (!executed) { self.fail(Kind::backend, Stage::backend); return finish(); }
        if (!*executed) {
            if (self.production->progress_failure()) self.fail(Kind::transport, Stage::evidence);
            if (self.observe_production() && self.outcome.cancellation) self.send_cancelled();
            else if (!self.outcome.primary) self.fail(Kind::transport, Stage::evidence);
            return finish();
        }
    }
    const auto* root = self.production->finalized_evidence();
    const bool operational = root && std::holds_alternative<OperationalRunTerminalEvidence>(root->terminal());
    if (!self.observe_production()) return finish();
    if (self.outcome.cancellation) {
        if (operational) self.fail(Kind::backend, Stage::backend);
        else self.send_cancelled();
        return finish();
    }
    if (!self.evidence->produce(*self.production)) { self.fail(Kind::backend, Stage::backend); return finish(); }
    if (!self.observe_production()) return finish();
    if (self.outcome.cancellation) {
        if (operational) self.fail(Kind::backend, Stage::backend); else self.send_cancelled();
        return finish();
    }
    if (!self.evidence->encode()) { self.fail(Kind::encoding, Stage::encoding); return finish(); }
    if (!self.observe_production()) return finish();
    if (self.outcome.cancellation) {
        if (operational) self.fail(Kind::backend, Stage::backend); else self.send_cancelled();
        return finish();
    }
    const PreparedByteWriter output{&self, [](void* raw, std::span<const std::byte> bytes) noexcept
        -> std::expected<std::size_t, PreparedIoFailure> {
        auto& state = *static_cast<Storage*>(raw);
        // This checkpoint shares the placed parser snapshot; it never reads control.
        if (!state.observe_production() || state.outcome.cancellation)
            return std::unexpected{PreparedIoFailure::io};
        const auto sent = native::LocalIpcAccess::try_send(state.endpoints.event_writer, bytes);
        if (!sent) { state.last_io_failure = sent.error(); return std::unexpected{prepared_failure(sent.error())}; }
        state.outcome.success_started = true;
        state.last_io_failure.reset();
        state.stream_position.consume(bytes.first(*sent));
        return *sent;
    }};
    bool complete = false;
    while (!complete && !self.outcome.primary) {
        if (!self.observe_production()) break;
        if (self.outcome.cancellation) {
            static_cast<void>(self.evidence->write_step(output, PreparedTransportSignal::cancelled));
            self.fail(operational ? Kind::backend : Kind::cancelled, Stage::evidence); break;
        }
        const auto ready = native::LocalIpcAccess::poll(self.endpoints.event_writer, true, 10);
        if (!ready) {
            if (ready.error().kind == native::FixedProcessFailureKind::interrupted && self.stream_position.progress == 0) continue;
            self.fail(Kind::native, Stage::evidence, ready.error()); break;
        }
        if (!self.observe_production()) break;
        if (ready->timed_out) continue;
        const auto progress = self.evidence->write_step(output, PreparedTransportSignal::none);
        if (!progress) {
            self.fail(self.outcome.cancellation ? (operational ? Kind::backend : Kind::cancelled) : Kind::transport,
                      Stage::evidence, self.last_io_failure);
            break;
        }
        if (!self.observe_production()) break;
        complete = *progress == PreparedTransportProgress::complete;
    }
    self.outcome.completion_received = complete;
    return finish();
}

} // namespace epycbench::runtime::detail
