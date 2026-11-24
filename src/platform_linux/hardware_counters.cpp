#include "platform_linux/hardware_counters_internal.hpp"

#include <array>
#include <cerrno>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace epycbench::platform_linux {
namespace {
auto failure(std::string_view reason, int native = 0) -> std::unexpected<core::Error> {
    std::optional<core::OsError> os;
    if (native) os = core::OsError::capture({native, std::generic_category()});
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{"linux.hardware_counters." + std::string{reason}},
        .message = "Cannot acquire worker hardware counter evidence",
        .operation = "hardware_counters", .os_error = std::move(os), .context = {}}};
}

auto open_event(LinuxThreadId thread, std::uint64_t event, int leader)
    -> std::expected<FileDescriptor, core::Error> {
    perf_event_attr attributes{};
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.size = sizeof(attributes);
    attributes.config = event;
    attributes.disabled = leader < 0;
    attributes.exclude_kernel = 1;
    attributes.exclude_hv = 1;
    attributes.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
        PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    const auto descriptor = static_cast<int>(
        ::syscall(SYS_perf_event_open, &attributes, static_cast<pid_t>(thread.value()), -1,
                  leader, PERF_FLAG_FD_CLOEXEC));
    if (descriptor >= 0) return FileDescriptor::adopt(descriptor);
    const auto error = errno;
    switch (error) {
    case EACCES:
    case EPERM: return failure("access_denied", error);
    case EINVAL:
    case ENOSYS:
    case ENODEV:
    case ENOENT:
    case EOPNOTSUPP: return failure("unsupported_event", error);
    case ESRCH: return failure("worker_retired", error);
    default: return failure("attach_failed", error);
    }
}

auto event_id(const FileDescriptor& descriptor) -> std::expected<std::uint64_t, core::Error> {
    std::uint64_t id{};
    int result;
    do {
        result = ::ioctl(descriptor.native_handle(), PERF_EVENT_IOC_ID, &id);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return failure("event_identity", errno);
    return id;
}

auto transition(const FileDescriptor& descriptor, unsigned long command) noexcept -> int {
    int result;
    do {
        result = ::ioctl(descriptor.native_handle(), command, static_cast<unsigned long>(PERF_IOC_FLAG_GROUP));
    } while (result != 0 && errno == EINTR);
    return result == 0 ? 0 : errno;
}
} // namespace

auto hardware_counter_detail::decode(std::span<const std::uint64_t> words, std::uint64_t cycles_id,
                                     std::uint64_t instructions_id)
    -> std::expected<HardwareCounterValues, core::Error> {
    constexpr std::size_t header = 3U, group_members = 2U, words_per_member = 2U;
    if (words.size() != header + group_members * words_per_member ||
        words[0] != group_members || words[2] > words[1] || cycles_id == instructions_id)
        return failure("malformed_group_read");
    const auto enabled = core::Duration::from_base_units(words[1]);
    const auto running = core::Duration::from_base_units(words[2]);
    if (words[4] == cycles_id && words[6] == instructions_id)
        return HardwareCounterValues{words[3], words[5], enabled, running};
    if (words[4] == instructions_id && words[6] == cycles_id)
        return HardwareCounterValues{words[5], words[3], enabled, running};
    return failure("mismatched_group_identity");
}

auto hardware_counter_detail::subtract(const HardwareCounterValues& initial, const HardwareCounterValues& final)
    -> std::expected<HardwareCounterValues, core::Error> {
    if (final.cpu_cycles < initial.cpu_cycles || final.retired_instructions < initial.retired_instructions ||
        final.enabled < initial.enabled || final.running < initial.running)
        return failure("nonmonotonic_snapshot");
    const auto enabled = final.enabled.value() - initial.enabled.value();
    const auto running = final.running.value() - initial.running.value();
    if (running > enabled) return failure("invalid_coverage");
    return HardwareCounterValues{
        final.cpu_cycles - initial.cpu_cycles, final.retired_instructions - initial.retired_instructions,
        core::Duration::from_base_units(enabled), core::Duration::from_base_units(running)};
}

HardwareCounterGroup::HardwareCounterGroup(FileDescriptor cycles, FileDescriptor instructions,
                                           std::uint64_t cycles_id, std::uint64_t instructions_id) noexcept
    : cycles_{std::move(cycles)}, instructions_{std::move(instructions)}, cycles_id_{cycles_id},
      instructions_id_{instructions_id} {}

auto HardwareCounterGroup::attach(LinuxThreadId thread) -> std::expected<HardwareCounterGroup, core::Error> {
    auto cycles = open_event(thread, PERF_COUNT_HW_CPU_CYCLES, -1);
    if (!cycles) return std::unexpected{std::move(cycles.error())};
    auto instructions = open_event(thread, PERF_COUNT_HW_INSTRUCTIONS, cycles->native_handle());
    if (!instructions) return std::unexpected{std::move(instructions.error())};
    auto cycles_id = event_id(*cycles);
    if (!cycles_id) return std::unexpected{std::move(cycles_id.error())};
    auto instructions_id = event_id(*instructions);
    if (!instructions_id) return std::unexpected{std::move(instructions_id.error())};
    if (*cycles_id == *instructions_id) return failure("mismatched_group_identity");
    return HardwareCounterGroup{std::move(*cycles), std::move(*instructions), *cycles_id, *instructions_id};
}

auto HardwareCounterGroup::read() const -> std::expected<HardwareCounterValues, core::Error> {
    std::array<std::uint64_t, 7> words{};
    ssize_t count;
    do {
        count = ::read(cycles_.native_handle(), words.data(), sizeof(words));
    } while (count < 0 && errno == EINTR);
    if (count < 0) return failure("read_failed", errno);
    if (static_cast<std::size_t>(count) != sizeof(words)) return failure("malformed_group_read");
    return hardware_counter_detail::decode(words, cycles_id_, instructions_id_);
}

void HardwareCounterGroup::retire_uncertain_group() noexcept {
    // Moving both descriptors into a temporary retires the whole group in
    // member-before-leader order even when a transition failed ambiguously.
    auto retired = std::move(*this);
}

auto HardwareCounterGroup::begin() -> std::expected<void, core::Error> {
    if (baseline_ || !cycles_.active() || !instructions_.active()) return failure("invalid_state");
    auto initial = read();
    if (!initial) return std::unexpected{std::move(initial.error())};
    auto before = raw_monotonic_now();
    if (!before) return std::unexpected{std::move(before.error())};
    if (const auto error = transition(cycles_, PERF_EVENT_IOC_ENABLE); error) {
        if (transition(cycles_, PERF_EVENT_IOC_DISABLE) != 0) retire_uncertain_group();
        return failure("enable_failed", error);
    }
    auto after = raw_monotonic_now();
    if (!after || *after < *before) {
        if (transition(cycles_, PERF_EVENT_IOC_DISABLE) != 0) retire_uncertain_group();
        if (!after) return std::unexpected{std::move(after.error())};
        return failure("invalid_clock_bracket");
    }
    baseline_.emplace(*initial, CounterTransition{*before, *after});
    return {};
}

auto HardwareCounterGroup::finish() -> std::expected<HardwareCounterWindow, core::Error> {
    if (!baseline_ || !cycles_.active() || !instructions_.active()) return failure("invalid_state");
    const auto initial = *baseline_;
    baseline_.reset();
    auto before = raw_monotonic_now();
    if (const auto error = transition(cycles_, PERF_EVENT_IOC_DISABLE); error) {
        retire_uncertain_group();
        return failure("disable_failed", error);
    }
    if (!before) return std::unexpected{std::move(before.error())};
    auto after = raw_monotonic_now();
    if (!after) return std::unexpected{std::move(after.error())};
    if (*before < initial.enable.after || *after < *before) return failure("invalid_clock_bracket");
    auto final = read();
    if (!final) return std::unexpected{std::move(final.error())};
    auto delta = hardware_counter_detail::subtract(initial.values, *final);
    if (!delta) return std::unexpected{std::move(delta.error())};
    return HardwareCounterWindow{initial.enable, {*before, *after}, *delta};
}

} // namespace epycbench::platform_linux
