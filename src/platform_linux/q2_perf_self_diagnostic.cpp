#include "platform_linux/q2_perf_self_diagnostic.hpp"

#include <cerrno>

namespace epycbench::platform_linux::detail {
namespace {
class OpenDescriptor final {
  public:
    OpenDescriptor(int descriptor, BoundedPerfContextSwitchOperations operations) noexcept
        : descriptor_{descriptor}, operations_{operations} {}
    OpenDescriptor(const OpenDescriptor&) = delete;
    ~OpenDescriptor() noexcept {
        if (descriptor_ >= 0) (void)operations_.close(operations_.context, descriptor_);
    }
    auto close_once() noexcept -> BoundedPerfContextSwitchOperations::StatusResult {
        const auto descriptor = descriptor_;
        descriptor_ = -1;
        // Linux close consumes the descriptor even on EINTR. Never retry it.
        return operations_.close(operations_.context, descriptor);
    }
  private:
    int descriptor_;
    BoundedPerfContextSwitchOperations operations_;
};
} // namespace

auto diagnose_q2_perf_self(LinuxThreadId thread, BoundedPerfContextSwitchOperations operations)
    noexcept -> Q2PerfSelfDiagnosticOutcome {
    if (operations.open == nullptr || operations.close == nullptr)
        return {Q2PerfSelfDiagnosticStatus::invalid_operations, 0, 0};
    const auto opened = operations.open(operations.context, thread);
    if (opened.value < 0)
        return {Q2PerfSelfDiagnosticStatus::open_failed,
                opened.error_number > 0 ? opened.error_number : EIO, 0};
    OpenDescriptor descriptor{opened.value, operations};
    const auto closed = descriptor.close_once();
    if (closed.value != 0)
        return {Q2PerfSelfDiagnosticStatus::close_failed, 0,
                closed.error_number > 0 ? closed.error_number : EIO};
    return {Q2PerfSelfDiagnosticStatus::open_closed, 0, 0};
}
} // namespace epycbench::platform_linux::detail

#include <array>
#include <atomic>
#include <sys/mman.h>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
class CaptureProbeOwner final {
  public:
    CaptureProbeOwner(BoundedPerfContextSwitchOperations operations,
                      PerfEpochProbeOutcome& outcome) noexcept : ops_{operations}, result_{outcome} {}
    CaptureProbeOwner(const CaptureProbeOwner&) = delete;
    auto operator=(const CaptureProbeOwner&) -> CaptureProbeOwner& = delete;
    ~CaptureProbeOwner() noexcept { retire_once(); }
    void retire_once() noexcept {
        if (retired_) return;
        retired_ = true;
        const auto failed = [](auto value) {
            return value.value != 0 || value.error_number != 0;
        };
        // Cleanup never overwrites primary errno. Each owned operation is consumed
        // before its attempt, including uncertain disable and Linux close/EINTR.
        if (enabled && !disable_attempted) {
            disable_attempted = true;
            if (failed(ops_.disable(ops_.context, descriptor))) result_.disable_failed = true;
        }
        if (mapping != nullptr) {
            auto* released = mapping;
            mapping = nullptr;
            if (failed(ops_.unmap(ops_.context, released, 36864))) result_.unmap_failed = true;
        }
        if (descriptor >= 0) {
            const auto released = descriptor;
            descriptor = -1;
            if (failed(ops_.close(ops_.context, released))) result_.close_failed = true;
        }
        if (result_.disable_failed || result_.unmap_failed || result_.close_failed)
            result_.status = PerfEpochProbeStatus::failed;
    }
    int descriptor{-1};
    void* mapping{};
    bool enabled{}, disable_attempted{};
  private:
    BoundedPerfContextSwitchOperations ops_;
    PerfEpochProbeOutcome& result_;
    bool retired_{};
};
} // namespace

auto diagnose_perf_capture_epoch(LinuxThreadId thread, std::size_t page_size,
    BoundedPerfContextSwitchOperations ops, PerfEpochProbeTiming timing) noexcept
    -> PerfEpochProbeOutcome {
    PerfEpochProbeOutcome result;
    constexpr std::size_t ring_bytes = 32768, map_bytes = ring_bytes + 4096;
    constexpr std::uint64_t budget_ns = 5'000'000'000;
    if (page_size != 4096 || !ops.open_capture || !ops.map || !ops.unmap ||
        !ops.close || !ops.enable || !ops.disable || !ops.read_loss ||
        !timing.now || !timing.sleep_once) return result;
    auto scratch = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array{((void)I, PerfContextSwitchRecord::create(
            RawMonotonicTimestamp::from_nanoseconds(0), PerfContextSwitchKind::switch_in))...};
    }(std::make_index_sequence<2048>{});
    CaptureProbeOwner owner{ops, result};
    auto& descriptor = owner.descriptor;
    auto& mapping = owner.mapping;
    auto& enabled = owner.enabled;
    auto& disable_attempted = owner.disable_attempted;
    std::uint64_t start{}, previous{};
    const auto control_ok = [&](auto value) {
        if (value.value == 0 && value.error_number == 0) return true;
        result.native_error = value.error_number;
        return false;
    };
    const auto within_budget = [&]() {
        std::uint64_t now{};
        if (!timing.now(timing.context, now) || now < previous) {
            result.phase = "clock";
            return false;
        }
        previous = now;
        result.elapsed_ns = now - start;
        if (result.elapsed_ns >= budget_ns) {
            result.status = PerfEpochProbeStatus::inconclusive;
            result.phase = "deadline";
            return false;
        }
        return true;
    };
    const auto read = [&](std::uint64_t& lost) {
        const auto value = ops.read_loss(ops.context, descriptor);
        if (value.bytes != 16 || value.error_number != 0) {
            result.native_error = value.error_number;
            return false;
        }
        lost = value.lost;
        return true;
    };
    const auto enable = [&]() {
        enabled = true; // An unsuccessful enable still requires one disable.
        disable_attempted = false;
        return control_ok(ops.enable(ops.context, descriptor));
    };
    const auto disable = [&]() {
        disable_attempted = true;
        if (!control_ok(ops.disable(ops.context, descriptor))) {
            result.disable_failed = true;
            return false;
        }
        enabled = false;
        return true;
    };
    const auto body = [&]() {
        result.phase = "clock";
        if (!timing.now(timing.context, start)) return;
        previous = start;
        result.phase = "open";
        const auto opened = ops.open_capture(ops.context, thread);
        if (opened.value >= 0) descriptor = opened.value;
        if (opened.value < 0 || opened.error_number != 0) {
            result.native_error = opened.error_number;
            return;
        }
        result.phase = "map";
        const auto mapped = ops.map(ops.context, descriptor, map_bytes);
        if (mapped.value != nullptr && mapped.value != MAP_FAILED) mapping = mapped.value;
        if (!mapping || mapped.error_number != 0) {
            result.native_error = mapped.error_number;
            return;
        }
        result.phase = "metadata";
        const auto layout = validate_bounded_perf_metadata(mapping, map_bytes, ring_bytes, scratch.size());
        if (!layout || layout->ring_size != ring_bytes || layout->ring_offset != 4096) return;
        auto* meta = static_cast<perf_event_mmap_page*>(mapping);
        const auto head = [&]() {
            const auto value = meta->data_head;
            std::atomic_thread_fence(std::memory_order_acquire);
            return value;
        };
        result.phase = "disabled_baseline";
        std::uint64_t lost{};
        if (!read(lost) || lost != 0 || head() != 0 || meta->data_tail != 0) return;
        for (std::size_t i = 0; i < 32; ++i) {
            if (!within_budget()) return;
            result.phase = "disabled_sleep";
            if (!timing.sleep_once(timing.context, 100'000)) return;
            if (!within_budget()) return;
        }
        result.phase = "disabled_idle";
        if (!read(lost) || lost != 0 || head() != 0 || meta->data_tail != 0) return;
        result.disabled_idle_verified = true;
        if (!within_budget()) return;
        result.phase = "clean_enable";
        if (!enable()) return;
        result.phase = "clean_disable";
        if (!disable()) return;
        result.phase = "clean_loss";
        if (!read(lost) || lost != 0) return;
        result.phase = "clean_drain";
        const auto clean_head = head();
        const auto clean_tail = meta->data_tail;
        const auto clean = drain_bounded_perf_records(mapping, *layout, scratch, nullptr, nullptr);
        if (!clean || clean->lost) return;
        result.clean_records = clean->record_count;
        result.no_event_observed = clean_head == clean_tail;
        result.clean_epoch_verified = true;
        result.phase = "positive_baseline";
        if (!read(lost) || lost != 0) return;
        if (!within_budget()) return;
        result.phase = "positive_enable";
        if (!enable()) return;
        for (std::size_t i = 0; i < 16384; ++i) {
            if (!within_budget()) return;
            result.phase = "positive_sleep";
            ++result.sleep_calls;
            if (!timing.sleep_once(timing.context, 100'000)) return;
            if (!within_budget()) return;
        }
        result.phase = "positive_disable";
        if (!disable()) return;
        result.phase = "positive_loss";
        if (!read(result.lost_before_drain)) return;
        result.positive_head = head();
        result.positive_tail = meta->data_tail;
        if (result.lost_before_drain == 0) {
            result.status = PerfEpochProbeStatus::inconclusive;
            return;
        }
        result.phase = "positive_drain";
        const auto drained = drain_bounded_perf_records(mapping, *layout, scratch, nullptr, nullptr,
            false, true, &result.first_loss);
        if (!drained && drained.error() != BoundedPerfFailureKind::lost) return;
        result.parser_reported_lost = !drained;
        if (drained) result.positive_records = drained->record_count;
        result.phase = "post_drain_loss";
        if (!read(result.lost_after_drain) || result.lost_after_drain != result.lost_before_drain) return;
        if (!within_budget()) return;
        result.phase = "complete";
        result.status = PerfEpochProbeStatus::passed;
    };
    body();
    // Explicit retirement precedes the return copy; the destructor is a no-op
    // afterward and remains the fallback for every scope-exit path.
    owner.retire_once();
    return result;
}
} // namespace epycbench::platform_linux::detail
