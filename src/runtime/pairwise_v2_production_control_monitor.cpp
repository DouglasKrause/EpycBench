#include "runtime/pairwise_v2_production_control_monitor.hpp"

#include "runtime/production_control_monitor_access.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace epycbench::runtime::detail {
namespace native = platform_linux::detail;
namespace {
struct FrameBuilder final {
    PairwiseV2ControlFrame frame;
    auto text(std::string_view value) noexcept -> bool {
        if (value.size() > frame.bytes.size() - frame.size) return false;
        for (const auto c : value) frame.bytes[frame.size++] = std::byte{static_cast<unsigned char>(c)};
        return true;
    }
    auto number(std::uint64_t value) noexcept -> bool {
        std::array<char, 20> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return converted.ec == std::errc{} && text({buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
    }
};
constexpr std::array reasons{"user_interrupt", "controller_shutdown", "controller_timeout"};
constexpr std::array phases{"startup", "admission", "service", "execution", "cleanup"};

auto retryable(native::FixedProcessFailure failure, std::size_t progress) noexcept -> bool {
    return failure.kind == native::FixedProcessFailureKind::would_block ||
        (failure.kind == native::FixedProcessFailureKind::interrupted && progress == 0);
}
auto failure(PairwiseV2ControlFailureKind kind) noexcept -> PairwiseV2ControlFailure {
    return {kind, std::nullopt};
}
auto cancel_reason(PairwiseV2CancellationReason reason) noexcept -> ipc::CancelReason {
    switch (reason) {
    case PairwiseV2CancellationReason::user_interrupt: return ipc::CancelReason::user_interrupt;
    case PairwiseV2CancellationReason::controller_shutdown: return ipc::CancelReason::controller_shutdown;
    case PairwiseV2CancellationReason::controller_timeout: return ipc::CancelReason::controller_timeout;
    }
    return ipc::CancelReason::controller_shutdown;
}
} // namespace

auto pairwise_v2_cancel_frame(PairwiseV2PairIdentity pair, std::size_t reason,
                 std::optional<std::size_t> phase) noexcept -> PairwiseV2ControlFrame {
    FrameBuilder frame;
    if (reason >= reasons.size() || (phase && *phase >= phases.size())) {
        frame.frame.size = 0;
        return frame.frame;
    }
    bool ok = frame.text(R"({"payload":{"first_request_id":")") && frame.number(pair.first.value()) && frame.text(R"(",)");
    if (phase) ok = ok && frame.text(R"("phase":")") && frame.text(phases[*phase]) && frame.text(R"(",)");
    ok = ok && frame.text(R"("reason":")") && frame.text(reasons[reason]) &&
        frame.text(R"(","second_request_id":")") && frame.number(pair.second.value()) &&
        frame.text(R"("},"payload_profile":")") && frame.text(phase ? pairwise_cancelled_profile : pairwise_cancel_profile) &&
        frame.text(R"(","protocol":"epycbench.controller-runner-ipc/v2","request_id":")") && frame.number(pair.first.value()) &&
        frame.text(R"(","sequence":"1","type":")") && frame.text(phase ? "pairwise_cancelled" : "cancel_pairwise") && frame.text(R"("})");
    if (!ok) { frame.frame.size = 0; return frame.frame; }
    const auto length = frame.frame.size - 4;
    for (unsigned i = 0; i < 4; ++i)
        frame.frame.bytes[i] = std::byte{static_cast<unsigned char>(length >> (24U - 8U * i))};
    return frame.frame;
}

PairwiseV2FixedControlReader::PairwiseV2FixedControlReader(PairwiseV2PairIdentity pair,
    std::optional<progress::Offer> profile) noexcept : progress_{std::move(profile)} {
    for (std::size_t i = 0; i < expected_.size(); ++i)
        expected_[i] = pairwise_v2_cancel_frame(pair, i, std::nullopt);
}
auto PairwiseV2FixedControlReader::arm_ack(std::uint64_t ordinal,
    const progress::Digest& digest) noexcept -> bool {
    if (!progress_ || ordinal == 0 || expected_ack_ || result_.acknowledgement || result_.failure)
        return false;
    expected_ack_ = ExpectedAck{ordinal, digest};
    return true;
}
auto PairwiseV2FixedControlReader::take_ack() noexcept -> std::optional<progress::Frame> {
    if (result_.failure || result_.cancellation) return std::nullopt;
    return std::exchange(result_.acknowledgement, std::nullopt);
}
auto PairwiseV2FixedControlReader::step(platform_linux::LocalIpcEndpoint& endpoint, int wait_ms,
    PairwiseV2CancellationPhase observation,
    const std::atomic<PairwiseV2CancellationPhase>* current_phase) noexcept -> PairwiseV2ControlSnapshot {
    if (result_.failure) return result_;
    auto failed = [&](PairwiseV2ControlFailureKind kind,
                      std::optional<native::FixedProcessFailure> error = std::nullopt) noexcept {
        result_.failure = PairwiseV2ControlFailure{kind, error};
        return result_;
    };
    if (wait_ms < 0 || !endpoint.active()) return failed(PairwiseV2ControlFailureKind::invalid_state);
    for (;;) {
        const auto ready = native::LocalIpcAccess::poll(endpoint, false, wait_ms);
        wait_ms = 0;
        if (!ready) {
            if (retryable(ready.error(), size_)) return result_;
            return failed(PairwiseV2ControlFailureKind::native, ready.error());
        }
        if (ready->timed_out) return result_;
        const auto read = native::LocalIpcAccess::try_receive(endpoint,
            std::span{input_}.subspan(size_, target_ - size_));
        if (!read) {
            if (retryable(read.error(), size_)) return result_;
            return failed(PairwiseV2ControlFailureKind::native, read.error());
        }
        if (*read == 0) {
            result_.eof = true;
            return failed(PairwiseV2ControlFailureKind::protocol);
        }
        if (result_.cancellation && !progress_) return failed(PairwiseV2ControlFailureKind::protocol);
        if (size_ == 0) frame_ack_eligible_ = expected_ack_.has_value();
        size_ += *read;
        if (size_ == 4 && target_ == 4) {
            std::uint32_t length{};
            for (std::size_t i = 0; i < 4; ++i)
                length = (length << 8U) | std::to_integer<unsigned>(input_[i]);
            if (length == 0 || length > (progress_ ? 1024U : 1020U))
                return failed(PairwiseV2ControlFailureKind::protocol);
            target_ = 4 + static_cast<std::size_t>(length);
        }
        if (size_ != target_) continue;
        for (std::size_t i = 0; i < expected_.size(); ++i) {
            if (!std::ranges::equal(std::span<const std::byte>{input_.data(), size_}, expected_[i].view())) continue;
            if (result_.cancellation) return failed(PairwiseV2ControlFailureKind::protocol);
            result_.cancellation = static_cast<PairwiseV2CancellationReason>(i);
            result_.first_observation = current_phase ? current_phase->load(std::memory_order_acquire) : observation;
            size_ = 0;
            target_ = 4;
            if (progress_) return result_;
            std::array<std::byte, 1> extra{};
            const auto trailing = native::LocalIpcAccess::try_receive(endpoint, extra);
            if (trailing || trailing.error().kind != native::FixedProcessFailureKind::would_block)
                return failed(PairwiseV2ControlFailureKind::protocol);
            return result_;
        }
        if (progress_ && frame_ack_eligible_ && expected_ack_ && !result_.acknowledgement) {
            auto ack = progress::decode(std::span<const std::byte>{input_.data(), size_});
            if (ack && ack->message == progress::Message::ack &&
                ack->offer.identity == progress_->identity &&
                ack->payload.ordinal == expected_ack_->ordinal &&
                ack->payload.sha256 == expected_ack_->digest) {
                result_.acknowledgement = *ack;
                expected_ack_.reset();
                size_ = 0;
                target_ = 4;
                return result_;
            }
        }
        return failed(PairwiseV2ControlFailureKind::protocol);
    }
}

struct PairwiseV2ProductionControlMonitor::Storage final {
    PairwiseV2PairIdentity pair;
    ServicePlacementPlan placement;
    std::shared_ptr<RunnerCancellation> cancellation;
    ServicePlatformOperations platform;
    PairwiseV2FixedControlReader reader;
    platform_linux::LocalIpcEndpoint* endpoint{};
    std::atomic<PairwiseV2CancellationPhase> phase{PairwiseV2CancellationPhase::service};
    std::atomic<bool> stop{};
    mutable std::mutex mutex;
    std::condition_variable condition;
    PairwiseV2ControlSnapshot result;
    std::optional<ServiceThreadReadyEvidence> readiness;
    std::thread thread;
    bool attempted{}, launched{}, startup_done{}, retired{}, progress_enabled{};
    struct EndCommand {
        platform_linux::LocalIpcEndpoint* events;
        std::span<const std::byte> remaining;
        std::uint64_t ordinal;
        progress::Digest digest;
        std::optional<std::expected<std::size_t, PairwiseV2ControlFailure>> result;
    };
    std::optional<EndCommand> end_command;

    Storage(PairwiseV2PairIdentity identity, ServicePlacementPlan plan,
        std::shared_ptr<RunnerCancellation> latch, ServicePlatformOperations operations,
        std::optional<progress::Offer> profile)
        : pair{identity}, placement{std::move(plan)}, cancellation{std::move(latch)},
          platform{operations}, reader{identity, profile}, progress_enabled{profile.has_value()} {}
    void entry() noexcept {
        // Error/placement temporaries are retired before publishing readiness.
        {
            auto ready = ProductionControlMonitorAccess::startup(placement.assignments().front(), platform);
            if (ready) readiness.emplace(std::move(*ready));
            else result.failure = failure(PairwiseV2ControlFailureKind::placement);
        }
        if (readiness) {
            result = reader.step(*endpoint, 0, PairwiseV2CancellationPhase::service, &phase);
            if (result.cancellation)
                static_cast<void>(cancellation->try_request(cancel_reason(*result.cancellation)));
            if (result.failure)
                static_cast<void>(cancellation->try_request(ipc::CancelReason::controller_shutdown));
        }
        const bool usable = readiness.has_value() && !result.failure;
        {
            std::lock_guard lock{mutex};
            startup_done = true;
        }
        condition.notify_one();
        if (!usable) return;
        while (!stop.load(std::memory_order_acquire)) {
            std::unique_lock lock{mutex};
            auto value = reader.step(*endpoint, 0,
                PairwiseV2CancellationPhase::service, &phase);
            if (end_command && !end_command->result) {
                auto& command = *end_command;
                if (value.failure) command.result = std::unexpected{*value.failure};
                else {
                    const auto written = native::LocalIpcAccess::try_send(*command.events, command.remaining);
                    if (!written) command.result = std::unexpected{PairwiseV2ControlFailure{
                        PairwiseV2ControlFailureKind::native, written.error()}};
                    else if (*written == command.remaining.size() &&
                             !reader.arm_ack(command.ordinal, command.digest)) {
                        command.result = std::unexpected{failure(PairwiseV2ControlFailureKind::protocol)};
                        value.failure = failure(PairwiseV2ControlFailureKind::protocol);
                    } else command.result = *written;
                }
            }
            if (value.cancellation)
                static_cast<void>(cancellation->try_request(cancel_reason(*value.cancellation)));
            if (value.failure)
                static_cast<void>(cancellation->try_request(ipc::CancelReason::controller_shutdown));
            result = value;
            condition.notify_all();
            if (value.failure) break;
            // Waiting must release the state lock: the producer must be able to
            // publish an end command or observe cancellation while control is
            // quiet. Repeated blocking polls under this lock can starve it.
            condition.wait_for(lock, std::chrono::milliseconds{10}, [&] {
                return stop.load(std::memory_order_acquire) || (end_command && !end_command->result);
            });
        }
    }
    auto close() noexcept -> PairwiseV2ControlSnapshot {
        if (!retired) {
            stop.store(true, std::memory_order_release);
            condition.notify_all();
            try {
                if (thread.joinable()) thread.join();
            } catch (...) { std::terminate(); }
            // Never detach or release the borrowed endpoint with a live reader.
            endpoint = nullptr;
            std::lock_guard lock{mutex};
            result.local_stop = true;
            result.joined = launched;
            retired = true;
        }
        std::lock_guard lock{mutex};
        return result;
    }
    ~Storage() noexcept { static_cast<void>(close()); }
};

PairwiseV2ProductionControlMonitor::PairwiseV2ProductionControlMonitor(std::unique_ptr<Storage> value) noexcept
    : storage_{std::move(value)} {}
PairwiseV2ProductionControlMonitor::PairwiseV2ProductionControlMonitor(PairwiseV2ProductionControlMonitor&&) noexcept = default;
PairwiseV2ProductionControlMonitor::~PairwiseV2ProductionControlMonitor() noexcept = default;
auto PairwiseV2ProductionControlMonitor::auxiliary_bytes() noexcept -> std::size_t { return sizeof(Storage); }
auto PairwiseV2ProductionControlMonitor::prepare(const AdmittedPlacementRequest& admitted,
    PairwiseV2PairIdentity pair, std::shared_ptr<RunnerCancellation> cancellation,
    ServicePlatformOperations platform, std::size_t limit,
    std::optional<progress::Offer> profile) noexcept
    -> std::expected<PairwiseV2ProductionControlMonitor, PairwiseV2ControlFailure> {
    if (limit < auxiliary_bytes()) return std::unexpected{failure(PairwiseV2ControlFailureKind::capacity)};
    if (!cancellation || !platform.current_thread_id || !platform.set_current_thread_affinity ||
        !platform.get_current_thread_affinity || !platform.current_cpu)
        return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    if (profile) {
        const auto bound = progress::bind_offer(*profile);
        if (!bound || *bound != *profile || profile->identity.first != pair.first.value() ||
            profile->identity.second != pair.second.value())
            return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    }
    try {
        auto placement = ServicePlacementPlan::create(admitted);
        if (!placement || placement->assignments().empty())
            return std::unexpected{failure(PairwiseV2ControlFailureKind::preparation)};
        return PairwiseV2ProductionControlMonitor{std::make_unique<Storage>(
            pair, std::move(*placement), std::move(cancellation), platform, std::move(profile))};
    } catch (...) { return std::unexpected{failure(PairwiseV2ControlFailureKind::preparation)}; }
}
auto PairwiseV2ProductionControlMonitor::start(platform_linux::LocalIpcEndpoint& endpoint,
    PairwiseV2ControlReadPermit permit) noexcept -> std::expected<void, PairwiseV2ControlFailure> {
    if (!storage_ || storage_->attempted || storage_->retired || !permit.valid_ ||
        permit.pair_ != storage_->pair || !endpoint.active())
        return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    permit.valid_ = false;
    auto& self = *storage_;
    self.attempted = true;
    self.endpoint = &endpoint;
    try {
        self.thread = std::thread([state = &self] { state->entry(); });
        self.launched = true;
        std::unique_lock lock{self.mutex};
        self.condition.wait(lock, [&] { return self.startup_done; });
        if (!self.readiness) {
            const auto failed = *self.result.failure;
            lock.unlock();
            static_cast<void>(self.close());
            return std::unexpected{failed};
        }
        return {};
    } catch (...) {
        static_cast<void>(self.close());
        return std::unexpected{failure(PairwiseV2ControlFailureKind::preparation)};
    }
}
auto PairwiseV2ProductionControlMonitor::submit_progress_end(
    platform_linux::LocalIpcEndpoint& events, std::span<const std::byte> remaining,
    std::uint64_t ordinal, const progress::Digest& digest) noexcept
    -> std::expected<void, PairwiseV2ControlFailure> {
    if (!storage_) return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    std::lock_guard lock{storage_->mutex};
    if (!storage_->launched || storage_->retired || !storage_->progress_enabled || storage_->end_command ||
        storage_->result.failure || !events.active() || remaining.empty() ||
        remaining.size() > 1028U || ordinal == 0)
        return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    storage_->end_command.emplace(Storage::EndCommand{&events, remaining, ordinal, digest, std::nullopt});
    storage_->condition.notify_all();
    return {};
}
auto PairwiseV2ProductionControlMonitor::take_progress_end_result() noexcept
    -> std::optional<std::expected<std::size_t, PairwiseV2ControlFailure>> {
    if (!storage_) return std::unexpected{failure(PairwiseV2ControlFailureKind::invalid_state)};
    std::lock_guard lock{storage_->mutex};
    if (!storage_->end_command || !storage_->end_command->result) return std::nullopt;
    const auto result = storage_->end_command->result;
    storage_->end_command.reset();
    return result;
}
auto PairwiseV2ProductionControlMonitor::take_ack() noexcept -> PairwiseV2ControlSnapshot {
    if (!storage_) return snapshot();
    std::lock_guard lock{storage_->mutex};
    auto observed = storage_->result;
    observed.acknowledgement.reset();
    if (!observed.failure && !observed.cancellation) {
        observed.acknowledgement = storage_->reader.take_ack();
        storage_->result.acknowledgement.reset();
    }
    return observed;
}
void PairwiseV2ProductionControlMonitor::phase(PairwiseV2CancellationPhase value) noexcept {
    if (storage_) storage_->phase.store(value, std::memory_order_release);
}
auto PairwiseV2ProductionControlMonitor::wait_progress() noexcept -> PairwiseV2ControlSnapshot {
    if (!storage_) return snapshot();
    std::unique_lock lock{storage_->mutex};
    storage_->condition.wait_for(lock, std::chrono::milliseconds{10}, [&] {
        return storage_->retired || storage_->result.failure || storage_->result.cancellation ||
            storage_->result.acknowledgement ||
            (storage_->end_command && storage_->end_command->result);
    });
    return storage_->result;
}
auto PairwiseV2ProductionControlMonitor::snapshot() const noexcept -> PairwiseV2ControlSnapshot {
    if (!storage_) return {failure(PairwiseV2ControlFailureKind::invalid_state), {}, {}, false, false, false};
    std::lock_guard lock{storage_->mutex};
    return storage_->result;
}
auto PairwiseV2ProductionControlMonitor::readiness() const noexcept -> const ServiceThreadReadyEvidence* {
    return storage_ && storage_->readiness ? &*storage_->readiness : nullptr;
}
auto PairwiseV2ProductionControlMonitor::assignment() const noexcept -> const ServiceThreadAssignment* {
    return storage_ && !storage_->placement.assignments().empty() ? &storage_->placement.assignments().front() : nullptr;
}
auto PairwiseV2ProductionControlMonitor::stop_and_join() noexcept -> PairwiseV2ControlSnapshot {
    if (!storage_) return snapshot();
    return storage_->close();
}

} // namespace epycbench::runtime::detail
