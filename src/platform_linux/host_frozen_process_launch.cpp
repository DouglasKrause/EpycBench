#include "platform_linux/host_frozen_process_launch.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace epycbench::platform_linux::host_work {
namespace {
using Raw = detail::RawStatusResult;
constexpr std::uint64_t cleanup_span = UINT64_C(30000000000);
// At most108,000 enclosing 100 ms turns over the unchanged10,800 s
// invocation: receive and wait each have two surrounding guard samples. The
// request bank covers228 serial publication Git calls, six phase children,
// and the bounded per-cell control/producer/publication handoffs.
constexpr std::size_t native_call_capacity = 6U * 108000U + 32U * 4096U;
// The complete campaign equation is established by the enclosing source loop.
// This separate finite tail permits its 100 ms retirement turns for the
// original thirty-second restoration tail, two signals per role, and every FD.
constexpr std::size_t retirement_call_capacity =
    4U * 302U * 3U + 4U * 2U * 3U + 4U * 48U;
auto index(SourceLaunchRole role) noexcept -> std::size_t {
    return static_cast<std::size_t>(role);
}
auto success(Raw result) noexcept -> bool {
    return result.value == 0 && result.error_number == 0;
}
auto native_pipe(void*, int output[2]) noexcept -> Raw {
    const auto result = ::pipe2(output, O_CLOEXEC | O_NONBLOCK);
    return {result, result < 0 ? errno : 0};
}
auto native_null(void*) noexcept -> Raw {
    const auto result = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    return {result, result < 0 ? errno : 0};
}
auto native_socket_pair(void*, int output[2]) noexcept -> Raw {
    const auto result = ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0, output);
    return {result, result < 0 ? errno : 0};
}
auto native_fork(void*) noexcept -> Raw {
    const auto result = ::fork();
    return {result, result < 0 ? errno : 0};
}
auto native_group(void*, int pid) noexcept -> Raw {
    const auto result = ::setpgid(pid, pid);
    return {result, result < 0 ? errno : 0};
}
auto native_gate(void*, int descriptor) noexcept -> Raw {
    const char ready = 'R';
    const auto result = ::send(descriptor, &ready, 1, MSG_NOSIGNAL);
    return {result, result < 0 ? errno : 0};
}
auto native_close(void*, int descriptor) noexcept -> Raw {
    const auto result = ::close(descriptor);
    return {result, result < 0 ? errno : 0};
}
auto native_credentials(void*, int descriptor) noexcept -> Raw {
    const int enabled = 1;
    const auto result = ::setsockopt(descriptor, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled));
    return {result, result < 0 ? errno : 0};
}
auto native_receive(void*, int descriptor, std::span<std::byte> bytes,
    SourceLaunchAncillary* received) noexcept -> Raw {
    if (!received || bytes.empty()) return {-1, EINVAL};
    *received = {};
    received->descriptors.fill(-1);
    // Even a credential-free malformed message can fit at most sixteen FDs.
    // Valid records have at most three rights plus the kernel credential row.
    alignas(struct cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * 16)> control{};
    struct iovec vector{bytes.data(), bytes.size()};
    struct msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const auto count = ::recvmsg(descriptor, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0) return {count, errno};
    received->truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
        const auto* position = reinterpret_cast<const std::byte*>(item);
        const auto* end = control.data() + std::min(message.msg_controllen, control.size());
        if (position < control.data() || position > end ||
            static_cast<std::size_t>(end - position) < sizeof(struct cmsghdr) ||
            item->cmsg_len < CMSG_LEN(0) ||
            item->cmsg_len > static_cast<std::size_t>(end - position)) {
            received->malformed = true;
            break;
        }
        const auto payload = item->cmsg_len - CMSG_LEN(0);
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS) {
            if (payload == 0 || payload % sizeof(int) != 0) received->malformed = true;
            const auto rights = payload / sizeof(int);
            for (std::size_t i = 0; i < rights; ++i) {
                int fd{};
                std::memcpy(&fd, CMSG_DATA(item) + i * sizeof(int), sizeof(fd));
                if (received->count < received->descriptors.size())
                    received->descriptors[received->count++] = fd;
                else received->malformed = true; // Kernel buffer bound forbids this.
            }
        } else if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS) {
            if (received->credentials || payload != sizeof(struct ucred)) {
                received->malformed = true;
                continue;
            }
            struct ucred credentials{};
            std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
            received->peer = {credentials.pid, credentials.uid, credentials.gid};
            received->credentials = true;
        } else received->malformed = true;
    }
    return {count, 0};
}
auto native_send(void*, int descriptor, std::span<const std::byte> bytes,
    std::span<const int> rights) noexcept -> Raw {
    if (bytes.empty() || rights.size() > 3) return {-1, EINVAL};
    alignas(struct cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * 3)> control{};
    struct iovec vector{const_cast<std::byte*>(bytes.data()), bytes.size()};
    struct msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (!rights.empty()) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * rights.size());
        auto* item = CMSG_FIRSTHDR(&message);
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_len = CMSG_LEN(sizeof(int) * rights.size());
        std::memcpy(CMSG_DATA(item), rights.data(), sizeof(int) * rights.size());
    }
    const auto count = ::sendmsg(descriptor, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    return {count, count < 0 ? errno : 0};
}
auto child_raw_now(void* opaque, std::uint64_t* output) noexcept -> Raw {
    SourceLaunchChildFailure local;
    auto& sample = opaque ? *static_cast<SourceLaunchChildFailure*>(opaque) : local;
    return source_launch_raw_now(sample, output);
}
[[noreturn]] void native_child(void*, const SourceLaunchSpec& spec, int gate, int setup) noexcept {
    // No C++ allocation, registry, stream or exception projection after fork.
    const auto failed_record = [&](const SourceLaunchChildFailure& raw) noexcept {
        // Setup uses the owned seqpacket endpoint, so a failed diagnostic send
        // cannot deliver SIGPIPE before the factual child exit. Missing output
        // is never promoted to a fabricated diagnostic by the parent.
        const auto bytes = encode_source_launch_child_failure(raw);
        static_cast<void>(::send(setup, bytes.data(), bytes.size(), MSG_NOSIGNAL));
        ::_exit(126);
    };
    const auto failed = [&](SourceLaunchReason reason, int number,
        SourceLaunchChildOperation operation = SourceLaunchChildOperation::limits,
        long native_result = -1, int argument = -1) noexcept {
        SourceLaunchChildFailure failure{};
        failure.reason = reason; failure.operation = operation;
        failure.native_result = native_result; failure.error_number = number;
        failure.native_argument = argument;
        failure.raw_present = operation != SourceLaunchChildOperation::limits;
        failed_record(failure);
    };
    SourceLaunchChildFailure clock_failure{};
    std::uint64_t latest = spec.origin_ns;
    const auto guard = [&]() noexcept {
        std::uint64_t now{};
        const auto sampled = child_raw_now(&clock_failure, &now);
        if (!success(sampled))
            failed_record(clock_failure);
        if (now < latest) failed(SourceLaunchReason::clock, 0, SourceLaunchChildOperation::clock, sampled.value);
        if (now >= spec.entry_end_ns) failed(SourceLaunchReason::deadline, 0, SourceLaunchChildOperation::clock, sampled.value);
        latest = now;
    };
    guard();
    const auto parent_death = ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (parent_death != 0)
        failed(SourceLaunchReason::child_setup, errno, SourceLaunchChildOperation::parent_death, parent_death);
    guard();
    const auto parent = ::getppid();
    if (parent != spec.expected_parent)
        failed(SourceLaunchReason::association, 0, SourceLaunchChildOperation::parent, parent);
    const auto grouped = ::setpgid(0, 0);
    if (grouped != 0) failed(SourceLaunchReason::group, errno, SourceLaunchChildOperation::group, grouped);
    guard();
    char permission{};
    std::size_t attempts{};
    for (;;) {
        guard();
        if (++attempts > native_call_capacity) failed(SourceLaunchReason::capacity, 0);
        const auto count = ::read(gate, &permission, 1);
        if (count == 1) break;
        if (count == 0) failed(SourceLaunchReason::gate, 0, SourceLaunchChildOperation::gate, count, gate);
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            failed(SourceLaunchReason::gate, errno, SourceLaunchChildOperation::gate, count, gate);
        struct pollfd item{gate, POLLIN, 0};
        const auto waited = ::poll(&item, 1, 10);
        if (waited < 0 && errno != EINTR)
            failed(SourceLaunchReason::gate, errno, SourceLaunchChildOperation::gate_wait, waited, gate);
    }
    if (permission != 'R') failed(SourceLaunchReason::gate, 0, SourceLaunchChildOperation::gate, 1, gate);
    guard();
    auto limit_operations = production_limit_operations();
    limit_operations.guard = {
        &clock_failure, child_raw_now,
        [](void*) noexcept { return false; }};
    auto limited = apply_source_launch_child_limits(spec, limit_operations, &clock_failure, &latest);
    if (!limited) failed_record(limited.error());
    guard();
    if (spec.working_directory >= 0) {
        auto changed = apply_source_launch_working_directory(spec, nullptr,
            [](void*, int descriptor) noexcept -> detail::RawStatusResult {
                const auto result = ::fchdir(descriptor);
                return {result, result != 0 ? errno : 0};
            }, limit_operations.guard, &clock_failure, &latest);
        if (!changed) failed_record(changed.error());
        guard();
    }
    for (const auto mapping : spec.mappings) {
        if (mapping.target < 3) {
            // Every inherited stdio copy is a real child owner. Consume it
            // explicitly so dup2 cannot hide its close status. Every source
            // was proved >=3 before fork, so this cannot consume a source.
            const auto closed = ::close(mapping.target);
            if (closed != 0)
                failed(SourceLaunchReason::close, errno, SourceLaunchChildOperation::standard_close, closed, mapping.target);
            guard();
        } else {
            // Custom targets were proved disjoint from every prepared owner
            // and source before fork. Refuse an unlisted live copy as well;
            // dup2 must never silently consume an unknown descriptor owner.
            const auto flags = ::fcntl(mapping.target, F_GETFD);
            if (flags >= 0 || errno != EBADF)
                failed(SourceLaunchReason::association, flags >= 0 ? 0 : errno,
                       SourceLaunchChildOperation::descriptor_flags, flags, mapping.target);
        }
        const auto duplicated = ::dup2(mapping.source, mapping.target);
        if (duplicated < 0)
            failed(SourceLaunchReason::child_setup, errno, SourceLaunchChildOperation::duplicate, duplicated, mapping.target);
        const auto marked = ::fcntl(mapping.target, F_SETFD, 0);
        if (marked < 0)
            failed(SourceLaunchReason::child_setup, errno, SourceLaunchChildOperation::descriptor_flags, marked, mapping.target);
        if (mapping.target == 1 || mapping.target == 2) {
            const auto blocking = ::fcntl(mapping.target, F_SETFL, 0);
            if (blocking < 0)
                failed(SourceLaunchReason::child_setup, errno, SourceLaunchChildOperation::descriptor_flags, blocking, mapping.target);
        }
        guard();
    }
    for (const auto descriptor : spec.inherited_descriptors) {
        bool retain = descriptor == spec.image || descriptor == setup;
        for (const auto mapping : spec.mappings)
            retain = retain || descriptor == mapping.target;
        if (!retain) {
            const auto closed = ::close(descriptor);
            if (closed != 0)
                failed(SourceLaunchReason::close, errno, SourceLaunchChildOperation::descriptor_close, closed, descriptor);
        }
        guard();
    }
    guard();
    const auto executed = ::syscall(SYS_execveat, spec.image, "", spec.arguments,
                                   spec.environment, AT_EMPTY_PATH);
    failed(SourceLaunchReason::child_setup, errno, SourceLaunchChildOperation::execute, executed, spec.image);
    ::_exit(127);
}
} // namespace

auto decode_source_launch_raw_clock(Raw native, std::int64_t seconds,
    std::int64_t nanoseconds, std::uint64_t* output, SourceLaunchChildFailure& current,
    Raw* actual_native) noexcept -> Raw {
    if (actual_native) *actual_native = native;
    current = {};
    const auto retain = [&]() noexcept {
        current.reason = SourceLaunchReason::clock;
        current.operation = SourceLaunchChildOperation::clock;
        current.raw_present = true;
        current.native_result = native.value;
        current.error_number = native.error_number;
    };
    if (!success(native)) { retain(); return native; }
    if (!output || seconds < 0 || nanoseconds < 0 || nanoseconds >= INT64_C(1000000000)) {
        retain(); return {-1, EINVAL};
    }
    const auto whole = static_cast<std::uint64_t>(seconds);
    const auto fraction = static_cast<std::uint64_t>(nanoseconds);
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / UINT64_C(1000000000)) {
        retain(); return {-1, EOVERFLOW};
    }
    *output = whole * UINT64_C(1000000000) + fraction;
    return native;
}
auto source_launch_raw_now(SourceLaunchChildFailure& current, std::uint64_t* output,
    Raw* actual_native) noexcept -> Raw {
    struct timespec value{};
    const auto result = ::clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    const auto number = result != 0 ? errno : 0;
    return decode_source_launch_raw_clock({result, number},
        static_cast<std::int64_t>(value.tv_sec), static_cast<std::int64_t>(value.tv_nsec),
        output, current, actual_native);
}

auto encode_source_launch_child_failure(const SourceLaunchChildFailure& failure) noexcept
    -> SourceLaunchChildFailureBytes {
    SourceLaunchChildFailureBytes bytes{};
    const auto put = [&](std::size_t offset, std::size_t count, std::uint64_t value) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            bytes[offset + count - 1U - i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
    };
    put(0, 4, failure.magic); put(4, 2, failure.version);
    put(6, 1, static_cast<unsigned>(failure.reason));
    put(7, 1, (failure.lower_present ? 1U : 0U) | (failure.raw_present ? 2U : 0U));
    put(8, 2, failure.lower_present ? static_cast<unsigned>(failure.lower) : 0U);
    put(10, 2, static_cast<unsigned>(failure.operation));
    put(12, 8, failure.raw_present ? std::bit_cast<std::uint64_t>(failure.native_result) : 0U);
    put(20, 4, failure.raw_present ? std::bit_cast<std::uint32_t>(failure.error_number) : 0U);
    put(24, 4, failure.raw_present ? std::bit_cast<std::uint32_t>(failure.native_argument) : 0U);
    return bytes;
}
auto decode_source_launch_child_failure(std::span<const std::byte> bytes) noexcept
    -> std::expected<SourceLaunchChildFailure, SourceLaunchReason> {
    const auto get = [&](std::size_t offset, std::size_t count) noexcept {
        std::uint64_t value{};
        for (std::size_t i = 0; i < count; ++i)
            value = (value << 8U) | std::to_integer<unsigned>(bytes[offset + i]);
        return value;
    };
    if (bytes.size() != 32U || get(0, 4) != UINT32_C(0x45534243) || get(4, 2) != 1U ||
        get(6, 1) > static_cast<unsigned>(SourceLaunchReason::input) || get(7, 1) > 3U ||
        get(8, 2) > static_cast<unsigned>(Reason::allocation) ||
        get(10, 2) > static_cast<unsigned>(SourceLaunchChildOperation::working_directory) ||
        !std::all_of(bytes.begin() + 28, bytes.end(), [](std::byte value) { return value == std::byte{}; }))
        return std::unexpected{SourceLaunchReason::invalid_input};
    const auto flags = get(7, 1);
    if (((flags & 1U) == 0U && get(8, 2) != 0U) ||
        ((flags & 2U) == 0U && (get(12, 8) != 0U || get(20, 4) != 0U || get(24, 4) != 0U)))
        return std::unexpected{SourceLaunchReason::invalid_input};
    SourceLaunchChildFailure failure{};
    failure.reason = static_cast<SourceLaunchReason>(get(6, 1));
    failure.lower_present = (flags & 1U) != 0U; failure.raw_present = (flags & 2U) != 0U;
    failure.lower = static_cast<Reason>(get(8, 2));
    failure.operation = static_cast<SourceLaunchChildOperation>(get(10, 2));
    failure.native_result = std::bit_cast<std::int64_t>(get(12, 8));
    failure.error_number = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(get(20, 4)));
    failure.native_argument = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(get(24, 4)));
    return failure;
}

auto apply_source_launch_child_limits(const SourceLaunchSpec& spec, const LimitOperations& lower,
    const SourceLaunchChildFailure* original_clock, std::uint64_t* last_observed) noexcept
    -> std::expected<void, SourceLaunchChildFailure> {
    auto local_last = spec.origin_ns;
    struct Context final {
        const SourceLaunchSpec* spec;
        const LimitOperations* lower;
        const SourceLaunchChildFailure* original_clock;
        SourceLaunchChildFailure failure;
        std::uint64_t* last;
        bool guard_failure{};
        void capture(SourceLaunchChildOperation operation, Raw raw, int argument = -1) noexcept {
            if (failure.raw_present || success(raw)) return;
            failure.raw_present = true; failure.operation = operation;
            failure.native_result = raw.value; failure.error_number = raw.error_number;
            failure.native_argument = argument;
        }
    } context{&spec, &lower, original_clock, {}, last_observed ? last_observed : &local_last, false};
    auto admission = ChildLimitAdmission::prepare(spec.expected_parent, spec.limits);
    if (!admission) {
        context.failure.reason = SourceLaunchReason::child_setup;
        context.failure.lower_present = true; context.failure.lower = admission.error();
        context.failure.operation = SourceLaunchChildOperation::limits;
        return std::unexpected{context.failure};
    }
    if (!lower.identity || !lower.get || !lower.set || !lower.guard.now || !lower.guard.cancelled) {
        context.failure.reason = SourceLaunchReason::invalid_input;
        context.failure.operation = SourceLaunchChildOperation::limits;
        return std::unexpected{context.failure};
    }
    LimitOperations wrapped{&context,
        [](void* raw, int* pid, int* parent, std::uint64_t* uid) noexcept -> Raw {
            auto& state = *static_cast<Context*>(raw);
            const auto result = state.lower->identity(state.lower->context, pid, parent, uid);
            state.capture(SourceLaunchChildOperation::limit_identity, result);
            return result;
        },
        [](void* raw, NativeLimit limit, LimitPair* output) noexcept -> Raw {
            auto& state = *static_cast<Context*>(raw);
            const auto result = state.lower->get(state.lower->context, limit, output);
            state.capture(SourceLaunchChildOperation::limit_get, result, static_cast<int>(limit));
            return result;
        },
        [](void* raw, NativeLimit limit, LimitPair value) noexcept -> Raw {
            auto& state = *static_cast<Context*>(raw);
            const auto result = state.lower->set(state.lower->context, limit, value);
            state.capture(SourceLaunchChildOperation::limit_set, result, static_cast<int>(limit));
            return result;
        }, lower.maximum_finite,
        {&context,
         [](void* raw, std::uint64_t* output) noexcept -> Raw {
             auto& state = *static_cast<Context*>(raw);
             const auto sampled = state.lower->guard.now(state.lower->guard.context, output);
             if (!success(sampled)) {
                 if (!state.failure.raw_present && state.original_clock && state.original_clock->raw_present)
                     state.failure = *state.original_clock;
                 else state.capture(SourceLaunchChildOperation::clock, sampled);
                 state.failure.reason = SourceLaunchReason::clock;
                 state.guard_failure = true;
                 return sampled;
             }
             if (*output < *state.last || *output >= state.spec->entry_end_ns) {
                 state.failure.reason = *output < *state.last ? SourceLaunchReason::clock : SourceLaunchReason::deadline;
                 state.failure.operation = SourceLaunchChildOperation::clock;
                 if (!state.failure.raw_present) {
                     state.failure.raw_present = true;
                     state.failure.native_result = sampled.value;
                     state.failure.error_number = sampled.error_number;
                 }
                 state.guard_failure = true;
                 return {-1, *output < *state.last ? EINVAL : ETIMEDOUT};
             }
             *state.last = *output;
             return sampled;
         },
         [](void* raw) noexcept {
             auto& state = *static_cast<Context*>(raw);
             return state.lower->guard.cancelled(state.lower->guard.context);
         }}};
    auto applied = admission->apply(wrapped);
    if (applied) return {};
    if (!context.guard_failure) context.failure.reason = SourceLaunchReason::child_setup;
    context.failure.lower_present = true; context.failure.lower = applied.error();
    if (!context.failure.raw_present && !context.guard_failure)
        context.failure.operation = SourceLaunchChildOperation::limits;
    return std::unexpected{context.failure};
}

auto apply_source_launch_working_directory(const SourceLaunchSpec& spec, void* context,
    detail::RawStatusResult (*change)(void*, int) noexcept, GuardOperations guard,
    const SourceLaunchChildFailure* original_clock, std::uint64_t* last_observed) noexcept
    -> std::expected<void, SourceLaunchChildFailure> {
    if (spec.working_directory == -1) return {};
    SourceLaunchChildFailure failure{};
    failure.reason = SourceLaunchReason::invalid_input;
    failure.operation = SourceLaunchChildOperation::working_directory;
    if (spec.working_directory < 3 || !change || !guard.now || !guard.cancelled ||
        spec.entry_end_ns <= spec.origin_ns || spec.entry_end_ns - spec.origin_ns > UINT64_C(5000000000))
        return std::unexpected{failure};
    auto local_last = spec.origin_ns;
    auto& last = last_observed ? *last_observed : local_last;
    const auto check = [&]() noexcept -> bool {
        failure = {};
        failure.operation = SourceLaunchChildOperation::clock;
        if (guard.cancelled(guard.context)) {
            failure.reason = SourceLaunchReason::cancelled;
            return false;
        }
        std::uint64_t now{};
        const auto sampled = guard.now(guard.context, &now);
        failure.raw_present = true;
        failure.native_result = sampled.value;
        failure.error_number = sampled.error_number;
        if (!success(sampled)) {
            if (original_clock && original_clock->raw_present) failure = *original_clock;
            failure.reason = SourceLaunchReason::clock;
            return false;
        }
        if (now < last || now >= spec.entry_end_ns) {
            failure.reason = now < last ? SourceLaunchReason::clock : SourceLaunchReason::deadline;
            return false;
        }
        last = now;
        if (guard.cancelled(guard.context)) {
            failure.reason = SourceLaunchReason::cancelled;
            return false;
        }
        return true;
    };
    if (!check()) return std::unexpected{failure};
    const auto changed = change(context, spec.working_directory);
    if (!success(changed)) {
        failure.reason = SourceLaunchReason::child_setup;
        failure.operation = SourceLaunchChildOperation::working_directory;
        failure.raw_present = true;
        failure.native_result = changed.value;
        failure.error_number = changed.error_number;
        failure.native_argument = spec.working_directory;
        return std::unexpected{failure};
    }
    if (!check()) return std::unexpected{failure};
    return {};
}

SourceLaunchInvocation::SourceLaunchInvocation(ResourceLedger& ledger, GuardOperations guard,
    SourceLaunchOperations operations, std::uint64_t origin, std::uint64_t end,
    const SourceLaunchChildFailure* current_clock, SourceLaunchPhase initial_phase,
    std::optional<SourceLaunchDeadline> inherited_launch) noexcept
    : ledger_{&ledger}, guard_{guard}, operations_{operations}, current_clock_{current_clock}, invocation_origin_ns_{origin},
      campaign_end_ns_{end}, phase_origin_ns_{origin},
      phase_end_ns_{end > origin ? origin + std::min(end - origin, initial_phase == SourceLaunchPhase::q0b
          ? UINT64_C(300000000000) : UINT64_C(600000000000)) : end},
      last_ns_{origin}, phase_{initial_phase} {
    received_.fill(-1);
    receive_borrowed_.fill(-1);
    valid_ = end > origin && end - origin <= UINT64_C(10800000000000) &&
        end <= std::numeric_limits<std::uint64_t>::max() - cleanup_span &&
        guard.now && guard.cancelled && operations.pipe && operations.gate_pair && operations.open_null &&
        operations.fork && operations.group && operations.gate && operations.close &&
        operations.child_entry && operations.process.wait_pid && operations.process.send_signal;
    if (initial_phase == SourceLaunchPhase::q0b) {
        valid_ = valid_ && end - origin <= UINT64_C(300000000000) && inherited_launch &&
            inherited_launch->origin_ns >= origin &&
            inherited_launch->end_ns > inherited_launch->origin_ns &&
            inherited_launch->end_ns <= phase_end_ns_ &&
            inherited_launch->end_ns - inherited_launch->origin_ns <= UINT64_C(5000000000);
        stage_ = SourceLaunchStage::bootstrap;
        launch_role_ = SourceLaunchRole::collector;
        launch_deadline_ = inherited_launch;
    } else {
        valid_ = valid_ && initial_phase == SourceLaunchPhase::q5;
        if (inherited_launch) {
            valid_ = valid_ && inherited_launch->origin_ns == origin &&
                inherited_launch->end_ns > origin && inherited_launch->end_ns <= phase_end_ns_ &&
                inherited_launch->end_ns - origin <= UINT64_C(5000000000);
            launch_role_ = SourceLaunchRole::controller;
            launch_deadline_ = inherited_launch;
            inherited_controller_launch_ = true;
        }
    }
    if (valid_) cleanup_end_ns_ = phase_end_ns_ + cleanup_span;
}
SourceLaunchInvocation::~SourceLaunchInvocation() noexcept {
    // A live source invocation must remain in its retention loop. This detects
    // a programming error; it is never represented as an ownership remedy.
    if (has_owned_resources()) std::terminate();
}
void SourceLaunchInvocation::retain_fact(SourceLaunchFact fact) noexcept {
    if (!primary_) primary_ = fact;
    else if (secondary_count_ < secondary_.size()) secondary_[secondary_count_++] = fact;
    else uncertain_ = true;
}
auto SourceLaunchInvocation::refuse(SourceLaunchReason reason, SourceLaunchOperation operation,
    SourceLaunchRole role, Raw result, int descriptor) noexcept -> std::unexpected<SourceLaunchReason> {
    retain_fact({reason, operation, role, index(role) < slots_.size()
        ? slots_[index(role)].view.generation : 0, result.value, result.error_number, descriptor});
    return std::unexpected{reason};
}
auto SourceLaunchInvocation::check_guard(bool retirement) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (!valid_) return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::guard,
                               SourceLaunchRole::controller);
    // Keep fixed stock available for every consuming close even after a work
    // budget refusal. Cleanup never retries a descriptor or drops its fact.
    const auto call_end = native_call_capacity + (retirement ? retirement_call_capacity : 0U);
    if (native_calls_ >= call_end - 3U ||
        secondary_count_ >= secondary_.size() - role_count * descriptor_capacity) {
        uncertain_ = has_owned_resources();
        return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::guard,
                      SourceLaunchRole::controller);
    }
    if (!retirement && guard_.cancelled(guard_.context))
        return refuse(SourceLaunchReason::cancelled, SourceLaunchOperation::guard,
                      SourceLaunchRole::controller);
    std::uint64_t now{};
    ++native_calls_;
    const auto sampled = guard_.now(guard_.context, &now);
    if (!success(sampled) || now < last_ns_) {
        auto original = sampled;
        if (current_clock_ && current_clock_->raw_present &&
            current_clock_->operation == SourceLaunchChildOperation::clock)
            original = {static_cast<long>(current_clock_->native_result), current_clock_->error_number};
        return refuse(SourceLaunchReason::clock, SourceLaunchOperation::guard,
                      SourceLaunchRole::controller, original);
    }
    last_ns_ = now;
    const auto end = retirement ? cleanup_end_ns_ :
        (launch_deadline_ ? std::min(phase_end_ns_, launch_deadline_->end_ns) : phase_end_ns_);
    if (now >= end) {
        uncertain_ = has_owned_resources();
        return refuse(SourceLaunchReason::deadline, SourceLaunchOperation::guard,
                      SourceLaunchRole::controller);
    }
    if (!retirement && guard_.cancelled(guard_.context))
        return refuse(SourceLaunchReason::cancelled, SourceLaunchOperation::guard,
                      SourceLaunchRole::controller);
    return {};
}
auto SourceLaunchInvocation::allowed(SourceLaunchRole role) const noexcept -> bool {
    switch (stage_) {
    case SourceLaunchStage::startup: return role == SourceLaunchRole::controller;
    case SourceLaunchStage::bootstrap:
        return role == SourceLaunchRole::collector || role == SourceLaunchRole::git;
    case SourceLaunchStage::collection:
        return role == SourceLaunchRole::validator;
    case SourceLaunchStage::publication: return role == SourceLaunchRole::git;
    case SourceLaunchStage::terminal: return false;
    }
    return false;
}

SourceInputFileOwner::SourceInputFileOwner(ResourceLedger& ledger, GuardOperations guard,
    detail::TopologySourceOperations operations, int root, SourceInputRootIdentity identity,
    std::span<char> scratch, std::uint64_t origin, std::uint64_t end, std::uint64_t& last,
    const SourceLaunchChildFailure* clock, std::span<const int> borrowed) noexcept
    : guard_{guard}, operations_{operations}, root_{root}, identity_{identity}, scratch_{scratch},
      borrowed_{borrowed}, origin_{origin}, end_{end}, last_{&last}, clock_{clock} {
    if (root < 3 || scratch.size() != input_capacity || end <= origin || last < origin ||
        last >= end || !guard.now || !guard.cancelled || !operations.open_at ||
        !operations.stat_at || !operations.stat_fd || !operations.read || !operations.close) {
        static_cast<void>(fail(SourceLaunchReason::invalid_input, SourceLaunchOperation::reserve));
        return;
    }
    auto reserved = ledger.reserve(Resources{.descriptors = owned_.size()});
    if (!reserved) {
        static_cast<void>(fail(SourceLaunchReason::capacity, SourceLaunchOperation::reserve));
        return;
    }
    lease_.emplace(std::move(*reserved));
}
SourceInputFileOwner::~SourceInputFileOwner() noexcept {
    // The actual source invocation retains this owner after uncertainty. A
    // destructor cannot substitute for consuming retirement or release a lease.
    if (lease_ || uncertain_ || std::ranges::any_of(owned_, [](long fd) { return fd >= 0; }))
        std::terminate();
}
auto SourceInputFileOwner::fail(SourceLaunchReason reason, SourceLaunchOperation operation,
    Raw raw, int descriptor) noexcept -> bool {
    const SourceLaunchFact fact{reason, operation, SourceLaunchRole::collector, 0,
                               raw.value, raw.error_number, descriptor};
    if (!primary_) primary_ = fact;
    else if (cleanup_count_ < cleanup_.size()) cleanup_[cleanup_count_++] = fact;
    else uncertain_ = true;
    return false;
}
auto SourceInputFileOwner::guard() noexcept -> bool {
    // Three fixed files, at most17 4KiB reads each, two three-component
    // traversals, fixed metadata checks and their before/after samples. This
    // local finite work stock is cumulative and never renewed per file.
    if (guard_.cancelled(guard_.context))
        return fail(SourceLaunchReason::cancelled, SourceLaunchOperation::guard);
    std::uint64_t now{};
    if (!charge_native()) return false;
    const auto sampled = guard_.now(guard_.context, &now);
    if (!success(sampled) || now < *last_ || now < origin_) {
        auto original = sampled;
        if (clock_ && clock_->raw_present && clock_->operation == SourceLaunchChildOperation::clock)
            original = {static_cast<long>(clock_->native_result), clock_->error_number};
        else if (guard_.actual_raw_now)
            original = *guard_.actual_raw_now;
        return fail(SourceLaunchReason::clock, SourceLaunchOperation::guard, original);
    }
    *last_ = now;
    if (now >= end_)
        return fail(SourceLaunchReason::deadline, SourceLaunchOperation::guard);
    if (guard_.cancelled(guard_.context))
        return fail(SourceLaunchReason::cancelled, SourceLaunchOperation::guard);
    return true;
}
auto SourceInputFileOwner::charge_native() noexcept -> bool {
    if (native_calls_ >= 512)
        return fail(SourceLaunchReason::capacity, SourceLaunchOperation::guard);
    ++native_calls_;
    return true;
}
auto SourceInputFileOwner::close_cell(std::size_t slot) noexcept -> bool {
    auto& cell = owned_[slot];
    if (cell < 0) return true;
    if (cell > std::numeric_limits<int>::max()) {
        uncertain_ = true;
        return false; // Preserve the positive acquisition in its actual cell.
    }
    // Two traversals of at most three directories close at most six walking
    // FDs, plus one file: 7/file, 21 for the three allowed inputs. The separate
    // fixed24 consuming tail also covers a last partial traversal. No guard or
    // clock failure prevents draining known siblings; no close is retried.
    if (close_calls_ >= 24) {
        uncertain_ = true;
        return fail(SourceLaunchReason::capacity, SourceLaunchOperation::close);
    }
    ++close_calls_;
    const auto descriptor = static_cast<int>(std::exchange(cell, -1));
    const auto closed = operations_.close(operations_.context, descriptor);
    if (!success(closed)) {
        uncertain_ = true;
        return fail(SourceLaunchReason::close, SourceLaunchOperation::close, closed, descriptor);
    }
    return true;
}
auto SourceInputFileOwner::close_owned() noexcept -> bool {
    bool confirmed = !uncertain_;
    for (std::size_t slot{}; slot < owned_.size(); ++slot)
        if (!close_cell(slot)) confirmed = false;
    return confirmed;
}
auto SourceInputFileOwner::root_matches() noexcept -> bool {
    if (!guard()) return false;
    struct stat held{};
    if (!charge_native()) return false;
    const auto result = operations_.stat_fd(operations_.context, root_, &held);
    if (!success(result))
        return fail(SourceLaunchReason::input, SourceLaunchOperation::input_stat, result, root_);
    if (!S_ISDIR(held.st_mode) || static_cast<std::uint64_t>(held.st_dev) != identity_.device ||
        static_cast<std::uint64_t>(held.st_ino) != identity_.inode)
        return fail(SourceLaunchReason::association, SourceLaunchOperation::input_stat, result, root_);
    return guard();
}
auto SourceInputFileOwner::read(SourceInputPath selected) noexcept
    -> std::expected<std::string_view, SourceLaunchReason> {
    if (!lease_ || retired_ || primary_ || uncertain_ || reads_ >= 3 ||
        static_cast<unsigned>(selected) > static_cast<unsigned>(SourceInputPath::current_source_campaign)) {
        if (!primary_) static_cast<void>(fail(SourceLaunchReason::state, SourceLaunchOperation::input_open));
        return std::unexpected{primary_ ? primary_->reason : SourceLaunchReason::state};
    }
    ++reads_;
    // The input manifest and template have one extra directory. Rolling two
    // walk roles leave the third role for the held file. The original parent
    // is closed before the second traversal; its identity is reobserved from
    // the authentic root while the actual file remains held.
    constexpr std::array<const char*, 3> directories{"config", "monitoring-qualification", "q0b"};
    constexpr std::array<const char*, 8> leaves{
        "host-v2-inputs.json", "host-v2-policy.json", "host-v2-input-template.json",
        "host-v2-campaign-20261005-01.json", "host-v2-campaign-20261018-01.json", "host-v2-campaign-20261020-01.json", "host-v2-campaign-20261024-01.json", "host-v2-campaign-20261102-01.json"};
    const auto count = selected == SourceInputPath::policy || selected == SourceInputPath::campaign ||
        selected == SourceInputPath::post_reboot_campaign ||
        selected == SourceInputPath::governed_reboot_campaign ||
        selected == SourceInputPath::environment_recovery_campaign ||
        selected == SourceInputPath::current_source_campaign ? 2U : 3U;
    const auto* leaf = leaves[static_cast<std::size_t>(selected)];
    std::array<struct stat, 3> directory_identity{};
    auto matches = [](const struct stat& a, const struct stat& b) noexcept {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode &&
               a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink &&
               a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
               a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
               a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    };
    auto native_stat = [&](int parent, const char* name, struct stat& output) noexcept {
        if (!guard()) return false;
        if (!charge_native()) return false;
        const auto result = name ? operations_.stat_at(operations_.context, parent, name, &output)
                                 : operations_.stat_fd(operations_.context, parent, &output);
        if (!success(result))
            return fail(SourceLaunchReason::input, SourceLaunchOperation::input_stat, result, parent);
        return guard();
    };
    auto close_slot = [&](std::size_t slot) noexcept { return close_cell(slot); };
    auto open_owned = [&](int parent, const char* name, int flags, std::size_t slot) noexcept {
        if (!guard()) return false;
        if (!charge_native()) return false;
        const auto opened = operations_.open_at(operations_.context, parent, name, flags);
        if (opened.value >= 0) {
            const bool alias = opened.value == root_ || std::ranges::find(owned_, opened.value) != owned_.end() ||
                (opened.value <= std::numeric_limits<int>::max() &&
                 std::ranges::find(borrowed_, static_cast<int>(opened.value)) != borrowed_.end());
            if (alias)
                return fail(SourceLaunchReason::association, SourceLaunchOperation::input_open, opened);
            owned_[slot] = opened.value; // Nonallocating adoption precedes every check.
            if (opened.value > std::numeric_limits<int>::max()) {
                uncertain_ = true;
                return fail(SourceLaunchReason::uncertainty, SourceLaunchOperation::input_open, opened);
            }
        }
        if (opened.value < 0 || opened.error_number)
            return fail(SourceLaunchReason::input, SourceLaunchOperation::input_open, opened, parent);
        return guard();
    };
    auto traverse = [&](bool reobserve) noexcept -> int {
        int parent = root_;
        for (unsigned component = 0; component < count; ++component) {
            struct stat named{}, held{};
            if (!native_stat(parent, directories[component], named) || !S_ISDIR(named.st_mode)) {
                if (!primary_) static_cast<void>(fail(SourceLaunchReason::association, SourceLaunchOperation::input_stat));
                return -1;
            }
            const auto slot = component % 2U;
            if (!open_owned(parent, directories[component], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, slot) ||
                !native_stat(static_cast<int>(owned_[slot]), nullptr, held) || !matches(named, held) ||
                (reobserve && !matches(directory_identity[component], held))) {
                if (!primary_) static_cast<void>(fail(SourceLaunchReason::association, SourceLaunchOperation::input_stat));
                return -1;
            }
            if (!reobserve) directory_identity[component] = held;
            if (component && !close_slot((component - 1U) % 2U)) return -1;
            parent = static_cast<int>(owned_[slot]);
        }
        return parent;
    };
    std::size_t bytes{};
    bool completed = root_matches();
    struct stat original{}, opened{}, final_stat{};
    const int parent = completed ? traverse(false) : -1;
    completed = parent >= 0 && native_stat(parent, leaf, original) && S_ISREG(original.st_mode) &&
        original.st_size >= 0 && static_cast<std::uint64_t>(original.st_size) <= input_capacity;
    if (completed)
        completed = open_owned(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 2) &&
            native_stat(static_cast<int>(owned_[2]), nullptr, opened) && matches(original, opened);
    if (completed) completed = close_slot((count - 1U) % 2U);
    for (unsigned refill = 0; completed && refill < 17; ++refill) {
        if (!guard()) { completed = false; break; }
        std::array<char, 4096> chunk{};
        if (!charge_native()) { completed = false; break; }
        const auto result = operations_.read(operations_.context, static_cast<int>(owned_[2]), chunk);
        if (result.value < 0 || result.error_number || static_cast<unsigned long>(result.value) > chunk.size()) {
            static_cast<void>(fail(SourceLaunchReason::input, SourceLaunchOperation::input_read,
                                   result, static_cast<int>(owned_[2])));
            completed = false; break;
        }
        if (!guard()) { completed = false; break; }
        if (result.value == 0) break;
        const auto size = static_cast<std::size_t>(result.value);
        if (size > scratch_.size() - bytes || refill == 16) {
            static_cast<void>(fail(SourceLaunchReason::capacity, SourceLaunchOperation::input_read, result));
            completed = false; break;
        }
        std::copy_n(chunk.begin(), size, scratch_.begin() + static_cast<std::ptrdiff_t>(bytes));
        bytes += size;
    }
    if (completed)
        completed = bytes == static_cast<std::uint64_t>(opened.st_size) &&
            native_stat(static_cast<int>(owned_[2]), nullptr, final_stat) && matches(opened, final_stat);
    const int observed_parent = completed ? traverse(true) : -1;
    if (completed)
        completed = observed_parent >= 0 && native_stat(observed_parent, leaf, final_stat) &&
                    matches(opened, final_stat) && root_matches();
    if (!completed && !primary_)
        static_cast<void>(fail(SourceLaunchReason::association, SourceLaunchOperation::input_stat));
    const bool closed = close_owned(); // Every known sibling even after the first failure.
    if (!completed || !closed || !guard())
        return std::unexpected{primary_ ? primary_->reason : SourceLaunchReason::uncertainty};
    return std::string_view{scratch_.data(), bytes};
}
auto SourceInputFileOwner::retire() noexcept -> std::expected<void, SourceLaunchReason> {
    if (retired_) return std::unexpected{SourceLaunchReason::state};
    const bool closed = close_owned();
    if (!closed || uncertain_)
        return std::unexpected{SourceLaunchReason::uncertainty};
    if (lease_) {
        if (!lease_->retire())
            return std::unexpected{SourceLaunchReason::state};
        lease_.reset();
    }
    retired_ = true;
    return {};
}
auto SourceLaunchInvocation::set_stage(SourceLaunchStage stage) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (uncertain_ || primary_ || static_cast<unsigned>(stage) != static_cast<unsigned>(stage_) + 1U)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve,
                      SourceLaunchRole::controller);
    if (stage == SourceLaunchStage::collection &&
        (slots_[index(SourceLaunchRole::git)].view.occupied ||
         slots_[index(SourceLaunchRole::collector)].bootstrap_lease))
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::git);
    if (stage == SourceLaunchStage::publication &&
        (slots_[index(SourceLaunchRole::collector)].view.occupied ||
         slots_[index(SourceLaunchRole::validator)].view.occupied))
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve,
                      SourceLaunchRole::collector);
    stage_ = stage;
    return {};
}
auto SourceLaunchInvocation::phase_deadline() const noexcept -> SourceLaunchPhaseDeadline {
    return {phase_, phase_generation_, phase_origin_ns_, phase_end_ns_,
            invocation_origin_ns_, campaign_end_ns_};
}
auto SourceLaunchInvocation::advance_phase(SourceLaunchPhase next, std::uint64_t previous_generation) noexcept
    -> std::expected<SourceLaunchPhaseDeadline, SourceLaunchReason> {
    if (primary_ || uncertain_ || launch_deadline_ || stage_ != SourceLaunchStage::publication ||
        previous_generation != phase_generation_ ||
        static_cast<unsigned>(next) != static_cast<unsigned>(phase_) + 1U ||
        (next != SourceLaunchPhase::q6 && next != SourceLaunchPhase::q7))
        return refuse(SourceLaunchReason::sequence, SourceLaunchOperation::reserve,
                      SourceLaunchRole::controller);
    for (const auto role : {SourceLaunchRole::collector, SourceLaunchRole::validator, SourceLaunchRole::git})
        if (slots_[index(role)].view.occupied)
            return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, role);
    auto checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    const auto span = next == SourceLaunchPhase::q6 ? UINT64_C(6000000000000) : UINT64_C(120000000000);
    phase_origin_ns_ = last_ns_;
    phase_end_ns_ = last_ns_ + std::min(campaign_end_ns_ - last_ns_, span);
    cleanup_end_ns_ = phase_end_ns_ + cleanup_span;
    phase_ = next;
    ++phase_generation_;
    stage_ = SourceLaunchStage::bootstrap;
    return phase_deadline();
}
auto SourceLaunchInvocation::begin_launch(SourceLaunchRole role) noexcept
    -> std::expected<SourceLaunchDeadline, SourceLaunchReason> {
    if (index(role) >= slots_.size() || !allowed(role) || primary_ || uncertain_ ||
        (launch_deadline_ && !inherited_controller_launch_) || slots_[index(role)].view.occupied ||
        (inherited_controller_launch_ && role != SourceLaunchRole::controller))
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::guard, role);
    auto checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    if (inherited_controller_launch_) {
        // The finite invoker's preparation/exec and this startup consume one
        // original pair. Returning it once cannot create a new five seconds.
        inherited_controller_launch_ = false;
        return *launch_deadline_;
    }
    if (last_ns_ > std::numeric_limits<std::uint64_t>::max() - UINT64_C(5000000000))
        return refuse(SourceLaunchReason::clock, SourceLaunchOperation::guard, role);
    launch_deadline_.emplace(SourceLaunchDeadline{
        last_ns_, std::min(phase_end_ns_, last_ns_ + UINT64_C(5000000000))});
    launch_role_ = role;
    return *launch_deadline_;
}
auto SourceLaunchInvocation::prepare_receive_bank(std::span<const int> borrowed,
    const SourceLaunchChannelOperations& operations) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (receive_lease_ || received_count_ || borrowed.size() > receive_borrowed_.size() ||
        !operations.receive || !operations.close || !operations.send)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    for (std::size_t i = 0; i < borrowed.size(); ++i) {
        if (borrowed[i] < 0 || std::find(borrowed.begin(), borrowed.begin() +
            static_cast<std::ptrdiff_t>(i), borrowed[i]) != borrowed.begin() + static_cast<std::ptrdiff_t>(i))
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    }
    auto checked = check_guard(false);
    if (!checked) return checked;
    Resources copies{};
    copies.descriptors = 17U;
    auto lease = ledger_->reserve(copies);
    if (!lease) return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    receive_lease_.emplace(std::move(*lease));
    receive_operations_ = operations;
    std::copy(borrowed.begin(), borrowed.end(), receive_borrowed_.begin());
    receive_borrowed_count_ = borrowed.size();
    return {};
}
auto SourceLaunchInvocation::received_descriptors() const noexcept -> std::span<const int> {
    return std::span{received_}.first(received_count_);
}
auto SourceLaunchInvocation::receive_channel(int channel, std::span<std::byte> bytes,
    SourceLaunchAncillary& ancillary) noexcept
    -> std::expected<Raw, SourceLaunchReason> {
    if (!receive_lease_ || !receive_lease_->active() || received_count_ || receive_uncertain_ ||
        channel < 3 || bytes.size() != 1024U ||
        std::find(receive_borrowed_.begin(), receive_borrowed_.begin() +
            static_cast<std::ptrdiff_t>(receive_borrowed_count_), channel) ==
            receive_borrowed_.begin() + static_cast<std::ptrdiff_t>(receive_borrowed_count_))
        return refuse(SourceLaunchReason::association, SourceLaunchOperation::receive_rights, SourceLaunchRole::controller);
    auto checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    ancillary = {};
    ++native_calls_;
    const auto result = receive_operations_.receive(receive_operations_.context, channel, bytes, &ancillary);
    // All returned nonnegative owners enter existing integer cells before the
    // first clock, credential, payload or result validation after recvmsg.
    for (std::size_t i = 0; i < std::min(ancillary.count, received_.size()); ++i) {
        const auto descriptor = ancillary.descriptors[i];
        bool alias = descriptor < 0 ||
            std::find(receive_borrowed_.begin(), receive_borrowed_.end(), descriptor) != receive_borrowed_.end() ||
            std::find(received_.begin(), received_.end(), descriptor) != received_.end();
        for (const auto& role : slots_)
            for (std::size_t j = 0; j < role.descriptor_count; ++j)
                alias = alias || (role.descriptors[j].acquired && !role.descriptors[j].consumed &&
                                  role.descriptors[j].descriptor == descriptor);
        if (alias) { ancillary.malformed = true; continue; }
        received_[received_count_] = descriptor;
        received_consumed_[received_count_] = false;
        received_close_[received_count_] = {};
        ++received_count_;
    }
    if (ancillary.count > received_.size()) ancillary.malformed = true;
    if (result.value < 0 || result.error_number) {
        if ((result.error_number == EAGAIN || result.error_number == EWOULDBLOCK || result.error_number == EINTR) &&
            received_count_ == 0 && ancillary.count == 0 && !ancillary.malformed && !ancillary.truncated)
            return result; // One attempt; the enclosing bounded loop charges retries.
        static_cast<void>(refuse(SourceLaunchReason::ancillary, SourceLaunchOperation::receive_rights,
                                SourceLaunchRole::controller, result, channel));
        static_cast<void>(close_received());
        return std::unexpected{primary_->reason};
    }
    if (ancillary.truncated || ancillary.malformed) {
        static_cast<void>(refuse(SourceLaunchReason::ancillary, SourceLaunchOperation::receive_rights,
                                SourceLaunchRole::controller, result, channel));
        static_cast<void>(close_received());
        return std::unexpected{primary_->reason};
    }
    checked = check_guard(false);
    if (!checked) {
        static_cast<void>(close_received());
        return std::unexpected{primary_->reason};
    }
    return result;
}
auto SourceLaunchInvocation::close_received() noexcept -> std::expected<void, SourceLaunchReason> {
    bool failed = receive_uncertain_;
    for (std::size_t i = 0; i < received_count_; ++i) {
        if (received_consumed_[i] || received_[i] < 0) continue;
        received_consumed_[i] = true;
        ++native_calls_;
        received_close_[i] = receive_operations_.close(receive_operations_.context, received_[i]);
        if (!success(received_close_[i])) {
            failed = true; receive_uncertain_ = true; uncertain_ = true;
            static_cast<void>(refuse(SourceLaunchReason::close, SourceLaunchOperation::close,
                                    SourceLaunchRole::controller, received_close_[i], received_[i]));
        }
    }
    if (failed) return std::unexpected{primary_->reason};
    received_count_ = 0;
    received_.fill(-1);
    received_consumed_.fill(false);
    return {};
}
auto SourceLaunchInvocation::publication_directory(bool root,
    const detail::TopologySourceOperations& operations) noexcept
    -> std::expected<std::uint64_t, SourceLaunchReason> {
    if (!receive_lease_ || received_count_ != 1U || received_consumed_[0] ||
        stage_ != SourceLaunchStage::publication || primary_ || uncertain_ ||
        directory_raw_descriptor_ >= 0 || !operations.open_at || !operations.stat_fd ||
        !operations.entries || !operations.close)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::directory_open, SourceLaunchRole::controller);
    for (const auto role : {SourceLaunchRole::collector, SourceLaunchRole::validator, SourceLaunchRole::git})
        if (slots_[index(role)].view.occupied)
            return refuse(SourceLaunchReason::state, SourceLaunchOperation::directory_open, role);
    directory_operations_ = operations; // Immutable close leaf precedes open.
    std::uint64_t mask{};
    std::size_t visited{}, refills{};
    const auto record = [&](SourceLaunchReason reason, SourceLaunchOperation operation, Raw raw,
                            int descriptor) noexcept {
        static_cast<void>(refuse(reason, operation, SourceLaunchRole::controller, raw, descriptor));
    };
    const auto run = [&]() noexcept -> bool {
        if (!check_guard(false)) return false;
        struct stat input{};
        ++native_calls_;
        const auto input_stat = operations.stat_fd(operations.context, received_[0], &input);
        if (!success(input_stat) || !S_ISDIR(input.st_mode)) {
            record(SourceLaunchReason::association, SourceLaunchOperation::directory_stat, input_stat, received_[0]);
            return false;
        }
        if (!check_guard(false)) return false;
        ++native_calls_;
        const auto opened = operations.open_at(operations.context, received_[0], ".",
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (opened.value >= 0) {
            directory_raw_descriptor_ = opened.value;
            if (opened.value <= std::numeric_limits<int>::max()) {
                const auto descriptor = static_cast<int>(opened.value);
                bool alias = std::find(receive_borrowed_.begin(), receive_borrowed_.end(), descriptor) !=
                    receive_borrowed_.end() || std::find(received_.begin(), received_.end(), descriptor) != received_.end();
                for (const auto& role : slots_)
                    for (std::size_t i = 0; i < role.descriptor_count; ++i)
                        alias = alias || (role.descriptors[i].acquired && !role.descriptors[i].consumed &&
                                          role.descriptors[i].descriptor == descriptor);
                if (alias) {
                    directory_raw_descriptor_ = -1; // Existing owner, never a duplicate consuming close.
                    record(SourceLaunchReason::association, SourceLaunchOperation::directory_open, opened, descriptor);
                    return false;
                }
                directory_descriptor_ = descriptor; // No fallible registration.
            } else { receive_uncertain_ = true; uncertain_ = true; }
        }
        if (opened.value < 0 || opened.error_number || directory_descriptor_ < 0) {
            record(SourceLaunchReason::pipe, SourceLaunchOperation::directory_open, opened, received_[0]);
            return false;
        }
        if (!check_guard(false)) return false;
        struct stat held{};
        ++native_calls_;
        const auto held_stat = operations.stat_fd(operations.context, directory_descriptor_, &held);
        if (!success(held_stat) || !S_ISDIR(held.st_mode) || input.st_dev != held.st_dev || input.st_ino != held.st_ino) {
            record(SourceLaunchReason::association, SourceLaunchOperation::directory_stat, held_stat, directory_descriptor_);
            return false;
        }
        if (!check_guard(false)) return false;
        for (;;) {
            if (refills >= 8U) {
                record(SourceLaunchReason::capacity, SourceLaunchOperation::directory_read, {}, directory_descriptor_);
                return false;
            }
            if (!check_guard(false)) return false;
            ++refills; ++native_calls_;
            const auto read = operations.entries(operations.context, directory_descriptor_, directory_bytes_);
            if (read.value < 0 || read.error_number || static_cast<unsigned long>(read.value) > directory_bytes_.size()) {
                record(SourceLaunchReason::ancillary, SourceLaunchOperation::directory_read, read, directory_descriptor_);
                return false;
            }
            if (!check_guard(false)) return false;
            if (read.value == 0) return true;
            const auto count = static_cast<std::size_t>(read.value);
            const auto parsed = source_launch_directory_names(
                std::span{directory_bytes_}.first(count), phase_, root, mask, visited);
            if (!parsed) {
                record(parsed.error(), SourceLaunchOperation::directory_parse, read, directory_descriptor_);
                return false;
            }
        }
    };
    const bool accepted = run();
    // A malformed directory or a clock failure still consumes all known
    // sibling owners once. A consuming-close failure retains this bank/lease.
    if (directory_descriptor_ >= 0 && !directory_consumed_) {
        directory_consumed_ = true;
        ++native_calls_;
        const auto closed = directory_operations_.close(directory_operations_.context, directory_descriptor_);
        if (!success(closed)) {
            receive_uncertain_ = true; uncertain_ = true;
            record(SourceLaunchReason::close, SourceLaunchOperation::close, closed, directory_descriptor_);
        } else { directory_descriptor_ = -1; directory_raw_descriptor_ = -1; directory_consumed_ = false; }
    }
    static_cast<void>(close_received());
    if (!accepted || primary_ || receive_uncertain_) return std::unexpected{primary_->reason};
    return mask;
}
auto SourceLaunchInvocation::retire_receive_bank() noexcept -> std::expected<void, SourceLaunchReason> {
    if (!receive_lease_ || directory_raw_descriptor_ >= 0 || received_count_ || receive_uncertain_)
        return refuse(SourceLaunchReason::uncertainty, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    for (const auto& role : slots_)
        if (role.view.occupied)
            return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    const auto retired = receive_lease_->retire();
    if (!retired) return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::controller);
    receive_lease_.reset();
    return {};
}
auto SourceLaunchInvocation::acquire_pair(SourceLaunchRole role, std::array<int, 2>& pair,
    bool gate, std::span<const int> borrowed) noexcept
    -> std::expected<void, SourceLaunchReason> {
    auto& stored = slots_[index(role)];
    if (stored.descriptor_count > stored.descriptors.size() - 2U)
        return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::pipe, role);
    auto checked = check_guard(false);
    if (!checked) return checked;
    ++native_calls_;
    const auto result = gate ? operations_.gate_pair(operations_.context, pair.data())
                             : operations_.pipe(operations_.context, pair.data());
    // All actually returned owners are registered before any post-call guard.
    bool alias{};
    for (std::size_t i = 0; i < pair.size(); ++i) {
        if (pair[i] < 0) continue;
        bool existing = std::find(borrowed.begin(), borrowed.end(), pair[i]) != borrowed.end();
        for (const auto& slot : slots_)
            for (std::size_t j = 0; j < slot.descriptor_count; ++j)
                existing = existing || (slot.descriptors[j].acquired && !slot.descriptors[j].consumed &&
                    slot.descriptors[j].descriptor == pair[i]);
        if (existing) { alias = true; continue; }
        stored.descriptors[stored.descriptor_count++] = {
            pair[i], role, stored.view.generation, true, false, false, 0};
    }
    if (alias) return refuse(SourceLaunchReason::association, SourceLaunchOperation::pipe, role, result);
    if (!success(result) || pair[0] < 0 || pair[1] < 0 || pair[0] == pair[1])
        return refuse(SourceLaunchReason::pipe, SourceLaunchOperation::pipe, role, result);
    return check_guard(false);
}
auto SourceLaunchInvocation::launch(SourceLaunchRole role, Resources demand,
    const SourceLaunchSpec& spec) noexcept -> std::expected<int, SourceLaunchReason> {
    if (index(role) >= slots_.size() || !allowed(role) || primary_ || uncertain_ ||
        spec.image < 3 || !spec.arguments || !spec.arguments[0] || !spec.environment ||
        spec.expected_parent <= 0 || spec.working_directory < -1 || spec.standard_input < -1 ||
        (spec.working_directory >= 0 && spec.working_directory < 3) ||
        (spec.standard_input >= 0 && spec.standard_input < 3) ||
        !launch_deadline_ || launch_role_ != role ||
        spec.origin_ns != launch_deadline_->origin_ns ||
        spec.entry_end_ns != launch_deadline_->end_ns ||
        spec.phase != phase_ || spec.phase_generation != phase_generation_ ||
        spec.mappings.size() > 13 || spec.inherited_descriptors.size() > descriptor_capacity ||
        demand.processes != 1 || demand.descriptors < 18)
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::reserve, role);
    auto& stored = slots_[index(role)];
    if (stored.view.occupied || stored.view.generation == std::numeric_limits<std::uint64_t>::max())
        return refuse(SourceLaunchReason::sequence, SourceLaunchOperation::reserve, role);
    std::size_t occupied{};
    for (const auto& entry : slots_) occupied += entry.view.occupied ? 1U : 0U;
    if (occupied >= 3) return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    for (std::size_t i = 0; i < spec.mappings.size(); ++i) {
        const auto mapping = spec.mappings[i];
        if (mapping.source < 3 || mapping.target < 3 || mapping.target >= 32 ||
            mapping.target == spec.image)
            return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::reserve, role);
        for (std::size_t j = 0; j < spec.mappings.size(); ++j)
            if (mapping.target == spec.mappings[j].source ||
                (i != j && mapping.target == spec.mappings[j].target))
                return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
        for (const auto inherited : spec.inherited_descriptors)
            if (mapping.target == inherited)
                return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
    }
    std::array<int, descriptor_capacity> existing{};
    std::size_t existing_count{};
    const auto existing_descriptor = [&](int descriptor) noexcept -> bool {
        if (descriptor < 0) return false;
        if (std::find(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(existing_count),
                      descriptor) != existing.begin() + static_cast<std::ptrdiff_t>(existing_count))
            return true;
        if (existing_count == existing.size()) return false;
        existing[existing_count++] = descriptor;
        return true;
    };
    for (const auto& entry : slots_)
        for (std::size_t i = 0; i < entry.descriptor_count; ++i)
            if (entry.descriptors[i].acquired && !entry.descriptors[i].consumed &&
                !existing_descriptor(entry.descriptors[i].descriptor))
                return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    for (const auto descriptor : spec.inherited_descriptors)
        if (!existing_descriptor(descriptor))
            return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    for (const auto standard : {0, 1, 2})
        if (std::find(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(existing_count),
                      standard) == existing.begin() + static_cast<std::ptrdiff_t>(existing_count))
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
    if (std::find(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(existing_count),
                  spec.image) == existing.begin() + static_cast<std::ptrdiff_t>(existing_count))
        return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
    for (const auto descriptor : {spec.working_directory, spec.standard_input})
        if (descriptor >= 0 && std::find(existing.begin(),
            existing.begin() + static_cast<std::ptrdiff_t>(existing_count), descriptor) ==
            existing.begin() + static_cast<std::ptrdiff_t>(existing_count))
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
    for (const auto mapping : spec.mappings)
        if (std::find(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(existing_count),
                      mapping.source) == existing.begin() + static_cast<std::ptrdiff_t>(existing_count))
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::reserve, role);
    // Nine new parent and nine child copies, all inherited child copies, and
    // up to three new stdio and thirteen custom target copies coexist before
    // child source retirement. Unknown runtime copies still gate activation.
    // Unknown inherited/runtime copies remain a separate activation blocker.
    if (existing_count > descriptor_capacity - 9U ||
        demand.descriptors < 21U + existing_count + spec.mappings.size())
        return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    auto checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    auto lease = ledger_->reserve(demand);
    if (!lease) return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    const auto generation = stored.view.generation + 1U;
    stored.view = {};
    stored.view.generation = generation;
    stored.view.phase = phase_;
    stored.view.phase_generation = phase_generation_;
    stored.view.occupied = true;
    stored.descriptor_count = 0;
    stored.lease.emplace(std::move(*lease));
    std::array<std::array<int, 2>, 4> pipes{};
    for (std::size_t i = 0; i < pipes.size(); ++i) {
        auto& pair = pipes[i];
        pair = {-1, -1};
        auto acquired = acquire_pair(role, pair, i < 2, spec.inherited_descriptors);
        if (!acquired) return std::unexpected{acquired.error()};
    }
    checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    ++native_calls_;
    const auto input = operations_.open_null(operations_.context);
    bool input_alias{};
    if (input.value >= 0 && input.value <= std::numeric_limits<int>::max()) {
        const auto descriptor = static_cast<int>(input.value);
        input_alias = std::find(spec.inherited_descriptors.begin(), spec.inherited_descriptors.end(), descriptor) !=
                      spec.inherited_descriptors.end();
        for (const auto& slot : slots_)
            for (std::size_t i = 0; i < slot.descriptor_count; ++i)
                input_alias = input_alias || (slot.descriptors[i].acquired && !slot.descriptors[i].consumed &&
                    slot.descriptors[i].descriptor == descriptor);
        if (!input_alias)
            stored.descriptors[stored.descriptor_count++] = {
                static_cast<int>(input.value), role, generation, true, false, false, 0};
    }
    if (input_alias) return refuse(SourceLaunchReason::association, SourceLaunchOperation::pipe, role, input);
    if (input.value < 0 || input.value > std::numeric_limits<int>::max() || input.error_number)
    {
        if (input.value > std::numeric_limits<int>::max()) {
            stored.view.uncertainty = true;
            uncertain_ = true;
        }
        return refuse(SourceLaunchReason::pipe, SourceLaunchOperation::pipe, role, input);
    }
    checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    std::array<SourceLaunchDescriptorMapping, 16> mappings{};
    mappings[0] = {spec.standard_input >= 0 ? spec.standard_input : static_cast<int>(input.value), 0};
    mappings[1] = {pipes[2][1], 1};
    mappings[2] = {pipes[3][1], 2};
    std::copy(spec.mappings.begin(), spec.mappings.end(), mappings.begin() + 3);
    std::array<int, role_count * descriptor_capacity + descriptor_capacity> inherited{};
    std::size_t inherited_count{};
    const auto register_copy = [&](int descriptor) noexcept {
        if (std::find(inherited.begin(), inherited.begin() + static_cast<std::ptrdiff_t>(inherited_count),
                      descriptor) == inherited.begin() + static_cast<std::ptrdiff_t>(inherited_count))
            inherited[inherited_count++] = descriptor;
    };
    for (const auto& entry : slots_)
        for (std::size_t i = 0; i < entry.descriptor_count; ++i)
            if (entry.descriptors[i].acquired && !entry.descriptors[i].consumed)
                register_copy(entry.descriptors[i].descriptor);
    for (const auto descriptor : spec.inherited_descriptors) register_copy(descriptor);
    // No standard target can overwrite a held image/setup/source acquired in
    // a previously closed stdio slot. The enclosing entry must own 0,1,2
    // before acquisition; every unexpected result remains registered here.
    for (std::size_t i = 0; i < stored.descriptor_count; ++i)
        if (stored.descriptors[i].descriptor < 3)
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::child_setup, role);
    for (const auto mapping : spec.mappings)
        if (std::find(inherited.begin(), inherited.begin() + static_cast<std::ptrdiff_t>(inherited_count),
                      mapping.target) != inherited.begin() + static_cast<std::ptrdiff_t>(inherited_count))
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::child_setup, role);
    auto child_spec = spec;
    child_spec.mappings = std::span{mappings}.first(spec.mappings.size() + 3U);
    child_spec.inherited_descriptors = std::span{inherited}.first(inherited_count);
    stored.view.streams = {pipes[2][0], pipes[3][0], pipes[1][0]};
    ++native_calls_;
    const auto created = operations_.fork(operations_.context);
    if (created.value > 0) {
        stored.view.raw_pid = created.value; // Actual immediate adoption before domain checks.
        if (created.value <= std::numeric_limits<int>::max())
            stored.view.pid = static_cast<int>(created.value);
    }
    if (created.value == 0 && created.error_number == 0) {
        operations_.child_entry(operations_.context, child_spec, pipes[0][0], pipes[1][1]);
        std::terminate();
    }
    if (created.value <= 0 || created.value > std::numeric_limits<int>::max() || created.error_number) {
        stored.view.uncertainty = created.value != -1 || created.error_number <= 0;
        uncertain_ = stored.view.uncertainty;
        return refuse(SourceLaunchReason::fork, SourceLaunchOperation::fork, role, created);
    }
    checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    ++native_calls_;
    const auto grouped = operations_.group(operations_.context, stored.view.pid);
    if (!success(grouped)) return refuse(SourceLaunchReason::group, SourceLaunchOperation::group, role, grouped);
    stored.view.group_confirmed = true;
    checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    ++native_calls_;
    const auto released = operations_.gate(operations_.context, pipes[0][1]);
    if (released.value != 1 || released.error_number)
        return refuse(SourceLaunchReason::gate, SourceLaunchOperation::gate, role, released);
    stored.view.gate_released = true;
    bool parent_close_failed{};
    for (const auto descriptor : {pipes[0][0], pipes[0][1], pipes[1][1],
                                  pipes[2][1], pipes[3][1], static_cast<int>(input.value)}) {
        auto closed = close_descriptor(role, descriptor);
        parent_close_failed = parent_close_failed || !closed;
    }
    checked = check_guard(false);
    if (!checked) return std::unexpected{checked.error()};
    if (parent_close_failed) return std::unexpected{SourceLaunchReason::close};
    launch_deadline_.reset();
    return stored.view.pid;
}
auto SourceLaunchInvocation::reserve_external(SourceLaunchRole role, Resources demand,
    std::optional<Resources> serial_bootstrap) noexcept -> std::expected<void, SourceLaunchReason> {
    if (role == SourceLaunchRole::controller || index(role) >= slots_.size() || !allowed(role) ||
        primary_ || uncertain_ || !launch_deadline_ || launch_role_ != role || demand.processes != 1U ||
        (serial_bootstrap && (role != SourceLaunchRole::collector || stage_ != SourceLaunchStage::bootstrap ||
                             serial_bootstrap->processes != 1U)))
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, role);
    auto& stored = slots_[index(role)];
    if (stored.view.occupied || stored.lease || stored.bootstrap_lease ||
        stored.view.generation == UINT64_MAX)
        return refuse(SourceLaunchReason::sequence, SourceLaunchOperation::reserve, role);
    const auto checked = check_guard(false);
    if (!checked) return checked;
    auto reserved = ledger_->reserve(demand);
    if (!reserved) return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
    const auto generation = stored.view.generation + 1U;
    stored.view = {};
    stored.view.generation = generation; stored.view.phase = phase_;
    stored.view.phase_generation = phase_generation_; stored.view.occupied = true;
    stored.external = true;
    stored.lease.emplace(std::move(*reserved));
    if (serial_bootstrap) {
        auto bootstrap = ledger_->reserve(*serial_bootstrap);
        if (!bootstrap) return refuse(SourceLaunchReason::capacity, SourceLaunchOperation::reserve, role);
        stored.bootstrap_lease.emplace(std::move(*bootstrap));
    }
    return {};
}
auto SourceLaunchInvocation::adopt_external(SourceLaunchRole role, long pid) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size())
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::fork, role);
    auto& stored = slots_[index(role)];
    if (!stored.external || !stored.view.occupied || stored.view.raw_pid || !launch_deadline_ || launch_role_ != role)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::fork, role);
    if (pid > 0) {
        stored.view.raw_pid = pid;
        if (pid <= INT_MAX) stored.view.pid = static_cast<int>(pid);
    }
    if (pid <= 0 || pid > INT_MAX) {
        stored.view.uncertainty = true; uncertain_ = true;
        return refuse(SourceLaunchReason::fork, SourceLaunchOperation::fork, role, {pid, 0});
    }
    for (const auto& other : slots_)
        if (&other != &stored && other.view.occupied && other.view.raw_pid == pid) {
            stored.view.uncertainty = true; uncertain_ = true;
            return refuse(SourceLaunchReason::association, SourceLaunchOperation::fork, role, {pid, 0});
        }
    // The authenticated trusted Popen owner reports this PID only after the
    // actual child's entry READY. Startup remains fenced until this handoff.
    const auto checked = check_guard(false);
    if (!checked) return checked;
    stored.view.gate_released = true;
    launch_deadline_.reset();
    return {};
}
auto SourceLaunchInvocation::confirm_external_terminal(SourceLaunchRole role, int pid, int status) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size())
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::wait, role);
    auto& stored = slots_[index(role)];
    if (!stored.external || !stored.view.occupied || stored.view.terminal ||
        pid != stored.view.pid || pid <= 0)
        return refuse(SourceLaunchReason::association, SourceLaunchOperation::wait, role);
    const auto checked = check_guard(true);
    if (!checked) return checked;
    // Only the authenticated actual Popen wait owner can reach this source
    // protocol edge; no caller scalar constitutes producer retirement.
    auto parsed = detail::parse_fixed_process_status(status);
    if (!parsed) return refuse(SourceLaunchReason::wait, SourceLaunchOperation::wait, role, {status, 0});
    stored.view.native_wait_status = status; stored.view.status = *parsed; stored.view.terminal = true;
    return {};
}
auto SourceLaunchInvocation::retire_serial_bootstrap() noexcept -> std::expected<void, SourceLaunchReason> {
    auto& stored = slots_[index(SourceLaunchRole::collector)];
    if (stage_ != SourceLaunchStage::bootstrap || !stored.external || !stored.view.occupied ||
        stored.view.pid <= 0 || !stored.bootstrap_lease || primary_ || uncertain_)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::collector);
    const auto checked = check_guard(false);
    if (!checked) return checked;
    if (!stored.bootstrap_lease->retire())
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, SourceLaunchRole::collector);
    stored.bootstrap_lease.reset();
    return {};
}
auto SourceLaunchInvocation::request_group_stop(SourceLaunchRole role, ProcessSignal signal) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size() ||
        (signal != ProcessSignal::terminate && signal != ProcessSignal::kill))
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::signal, role);
    const auto& stored = slots_[index(role)];
    if (!stored.view.occupied || stored.view.pid <= 0 || !stored.view.group_confirmed || stored.external)
        return refuse(SourceLaunchReason::association, SourceLaunchOperation::signal, role);
    const auto checked = check_guard(true);
    if (!checked) return checked;
    ++native_calls_;
    const auto sent = operations_.process.send_signal(operations_.process.context, -stored.view.pid,
        signal == ProcessSignal::terminate ? SIGTERM : SIGKILL);
    if (!success(sent)) return refuse(SourceLaunchReason::signal, SourceLaunchOperation::signal, role, sent);
    return check_guard(true);
}
auto SourceLaunchInvocation::reap_orphans() noexcept -> std::expected<bool, SourceLaunchReason> {
    const auto leader = phase_ == SourceLaunchPhase::q0b ? SourceLaunchRole::collector
                                                       : SourceLaunchRole::controller;
    if (!slots_[index(leader)].view.terminal)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::wait, leader);
    const auto checked = check_guard(true);
    if (!checked) return std::unexpected{checked.error()};
    int status{};
    ++native_calls_;
    const auto result = operations_.process.wait_pid(operations_.process.context, -1, &status, WNOHANG);
    if (result.value == 0 && !result.error_number) return false;
    if (result.value < 0 && result.error_number == EINTR) return false;
    if (result.value < 0 && result.error_number == ECHILD) {
        // Every known external Popen owner either reaped its child before its
        // own death, or its actual orphan was reaped above. No child remains.
        for (auto& stored : slots_)
            if (stored.external && stored.view.occupied) stored.view.terminal = true;
        return true;
    }
    if (result.value > 0 && !result.error_number && detail::parse_fixed_process_status(status)) {
        for (auto& stored : slots_)
            if (stored.external && stored.view.occupied && stored.view.raw_pid == result.value) {
                stored.view.native_wait_status = status;
                stored.view.status = *detail::parse_fixed_process_status(status);
                stored.view.terminal = true;
                return false;
            }
        // The child can become our actual orphan if its Popen owner dies
        // before relaying LAUNCHED. This wait already consumed the real PID;
        // retain its unassociated failure, never assign it a producer role,
        // and continue consuming siblings until the actual ECHILD boundary.
        static_cast<void>(refuse(SourceLaunchReason::association, SourceLaunchOperation::wait,
                                leader, result));
        return false;
    }
    uncertain_ = true;
    return refuse(SourceLaunchReason::wait, SourceLaunchOperation::wait, leader, result);
}
auto SourceLaunchInvocation::observe_terminal(SourceLaunchRole role) noexcept
    -> std::expected<bool, SourceLaunchReason> {
    if (index(role) >= slots_.size())
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::wait, role);
    auto& stored = slots_[index(role)];
    if (stored.external)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::wait, role);
    if (!stored.view.occupied || stored.view.pid <= 0)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::wait, role);
    if (stored.view.terminal) return true;
    auto checked = check_guard(true);
    if (!checked) return std::unexpected{checked.error()};
    int status{};
    ++native_calls_;
    const auto waited = operations_.process.wait_pid(operations_.process.context,
                                                    stored.view.pid, &status, WNOHANG);
    if (waited.value == 0 && waited.error_number == 0) {
        checked = check_guard(true);
        if (!checked) return std::unexpected{checked.error()};
        return false;
    }
    if (waited.value == stored.view.pid && waited.error_number == 0) {
        stored.view.native_wait_status = status;
        auto parsed = detail::parse_fixed_process_status(status);
        if (parsed) {
            stored.view.status = *parsed;
            stored.view.terminal = true;
            stored.view.uncertainty = false;
            for (std::size_t i = 0; i < stored.descriptor_count; ++i)
                stored.view.uncertainty = stored.view.uncertainty || stored.descriptors[i].uncertain;
            uncertain_ = false;
            for (const auto& entry : slots_)
                uncertain_ = uncertain_ || entry.view.uncertainty;
            checked = check_guard(true);
            if (!checked) return std::unexpected{checked.error()};
            return true;
        }
    }
    stored.view.uncertainty = true;
    uncertain_ = true;
    return refuse(SourceLaunchReason::wait, SourceLaunchOperation::wait, role, waited);
}
auto SourceLaunchInvocation::request_stop(SourceLaunchRole role, ProcessSignal signal) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size() ||
        (signal != ProcessSignal::terminate && signal != ProcessSignal::kill))
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::signal, role);
    auto& stored = slots_[index(role)];
    if (!stored.view.occupied || stored.view.pid <= 0 || stored.view.terminal)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::signal, role);
    auto checked = check_guard(true);
    if (!checked) return checked;
    ++native_calls_;
    const auto signalled = operations_.process.send_signal(operations_.process.context,
        stored.view.pid, signal == ProcessSignal::terminate ? SIGTERM : SIGKILL);
    if (success(signalled)) return check_guard(true);
    return refuse(SourceLaunchReason::signal, SourceLaunchOperation::signal, role, signalled);
}
auto SourceLaunchInvocation::close_descriptor(SourceLaunchRole role, int descriptor) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size())
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::close, role);
    auto& stored = slots_[index(role)];
    for (std::size_t i = 0; i < stored.descriptor_count; ++i) {
        auto& record = stored.descriptors[i];
        if (record.descriptor != descriptor || !record.acquired || record.consumed) continue;
        // Consume before native close. A failed close is never retried and its
        // reservation is never released on the assumption that it closed.
        record.consumed = true;
        ++native_calls_;
        const auto closed = operations_.close(operations_.context, descriptor);
        if (success(closed)) return {};
        record.close_error = closed.error_number;
        record.uncertain = true;
        stored.view.uncertainty = true;
        uncertain_ = true;
        return refuse(SourceLaunchReason::close, SourceLaunchOperation::close, role, closed, descriptor);
    }
    return refuse(SourceLaunchReason::state, SourceLaunchOperation::close, role);
}
auto SourceLaunchInvocation::retire(SourceLaunchRole role) noexcept
    -> std::expected<void, SourceLaunchReason> {
    if (index(role) >= slots_.size())
        return refuse(SourceLaunchReason::invalid_input, SourceLaunchOperation::wait, role);
    auto& stored = slots_[index(role)];
    if (!stored.view.occupied || !stored.lease)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::wait, role);
    if (stored.view.pid > 0 && !stored.view.terminal)
        return refuse(SourceLaunchReason::state, SourceLaunchOperation::wait, role);
    bool close_failed{};
    for (std::size_t i = 0; i < stored.descriptor_count; ++i) {
        auto& record = stored.descriptors[i];
        if (!record.consumed) {
            auto closed = close_descriptor(role, record.descriptor);
            close_failed = close_failed || !closed;
        }
        close_failed = close_failed || record.uncertain;
    }
    if (close_failed) return std::unexpected{SourceLaunchReason::close};
    if (stored.view.uncertainty)
        return refuse(SourceLaunchReason::uncertainty, SourceLaunchOperation::wait, role);
    if (stored.bootstrap_lease) {
        if (!stored.bootstrap_lease->retire())
            return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, role);
        stored.bootstrap_lease.reset();
    }
    auto released = stored.lease->retire();
    if (!released) return refuse(SourceLaunchReason::state, SourceLaunchOperation::reserve, role);
    stored.lease.reset();
    stored.view.occupied = false;
    if (launch_deadline_ && launch_role_ == role) launch_deadline_.reset();
    return {};
}
auto SourceLaunchInvocation::slot(SourceLaunchRole role) const noexcept -> SourceLaunchSlotView {
    return index(role) < slots_.size() ? slots_[index(role)].view : SourceLaunchSlotView{};
}
auto SourceLaunchInvocation::descriptors(SourceLaunchRole role) const noexcept
    -> std::span<const SourceLaunchDescriptorView> {
    if (index(role) >= slots_.size()) return {};
    const auto& stored = slots_[index(role)];
    return std::span{stored.descriptors}.first(stored.descriptor_count);
}
auto SourceLaunchInvocation::primary() const noexcept -> const std::optional<SourceLaunchFact>& {
    return primary_;
}
auto SourceLaunchInvocation::secondary() const noexcept -> std::span<const SourceLaunchFact> {
    return std::span{secondary_}.first(secondary_count_);
}
auto SourceLaunchInvocation::has_owned_resources() const noexcept -> bool {
    if ((receive_lease_ && receive_lease_->active()) || received_count_ ||
        directory_raw_descriptor_ >= 0 || receive_uncertain_) return true;
    for (const auto& stored : slots_)
        if (stored.view.occupied || (stored.lease && stored.lease->active()) ||
            (stored.bootstrap_lease && stored.bootstrap_lease->active())) return true;
    return false;
}
auto SourceLaunchInvocation::uncertain() const noexcept -> bool { return uncertain_; }
auto production_source_launch_operations() noexcept -> SourceLaunchOperations {
    return {nullptr, native_pipe, native_socket_pair, native_null, native_fork, native_group, native_gate,
            native_close, native_child, detail::production_process_operations()};
}
auto production_source_launch_channel_operations() noexcept -> SourceLaunchChannelOperations {
    return {nullptr, native_socket_pair, native_credentials, native_receive, native_send, native_close};
}
auto production_source_launch_stream_operations() noexcept -> SourceLaunchStreamOperations {
    return {
        +[](int fd, std::span<const std::byte> bytes) noexcept -> Raw {
            const auto result = ::write(fd, bytes.data(), bytes.size());
            return {result, result < 0 ? errno : 0};
        },
        +[](int fd, std::span<std::byte> bytes, std::uint64_t offset) noexcept -> Raw {
            if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
                return {-1, EOVERFLOW};
            const auto result = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
            return {result, result < 0 ? errno : 0};
        },
        +[](int milliseconds) noexcept -> Raw {
            const auto result = ::poll(nullptr, 0, milliseconds);
            return {result, result < 0 ? errno : 0};
        },
        +[]() noexcept { return static_cast<std::uint32_t>(::geteuid()); },
        +[]() noexcept { for (;;) ::pause(); },
        +[]() noexcept { return static_cast<int>(::getpid()); },
        +[](bool set, int* value) noexcept -> Raw {
            if (!value) return {-1, EINVAL};
            const auto result = set ? ::prctl(PR_SET_CHILD_SUBREAPER, *value) :
                                      ::prctl(PR_GET_CHILD_SUBREAPER, value);
            return {result, result < 0 ? errno : 0};
        },
        +[](std::span<std::byte> bytes) noexcept -> Raw {
            const auto result = ::getrandom(bytes.data(), bytes.size(), 0);
            return {result, result < 0 ? errno : 0};
        },
        +[](int parent, const char* name) noexcept -> Raw {
            const auto result = ::mkdirat(parent, name, 0700);
            return {result, result < 0 ? errno : 0};
        },
        +[](int fd, bool set, int flags) noexcept -> Raw {
            const auto result = ::fcntl(fd, set ? F_SETFL : F_GETFL, flags);
            return {result, result < 0 ? errno : 0};
        },
        +[](int fd, int milliseconds, short* events) noexcept -> Raw {
            if (!events || fd < 0 || milliseconds < 0 || milliseconds > 100) return {-1, EINVAL};
            pollfd descriptor{fd, POLLOUT, 0};
            const auto result = ::poll(&descriptor, 1, milliseconds);
            const auto number = result < 0 ? errno : 0;
            *events = descriptor.revents;
            return {result, number};
        }
    };
}

auto source_launch_descriptor_names(std::span<const char> bytes, std::span<const int> known,
    std::size_t& visited) noexcept -> std::expected<void, SourceLaunchReason> {
    for (std::size_t offset{}; offset < bytes.size();) {
        if (++visited > 64U || bytes.size() - offset < 20U)
            return std::unexpected{SourceLaunchReason::capacity};
        std::uint64_t inode{};
        std::uint16_t length{};
        std::memcpy(&inode, bytes.data() + offset, sizeof(inode));
        std::memcpy(&length, bytes.data() + offset + 16U, sizeof(length));
        if (length < 20U || length % 8U || length > bytes.size() - offset)
            return std::unexpected{SourceLaunchReason::input};
        const auto* begin = bytes.data() + offset + 19U;
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, length - 19U));
        if (!end) return std::unexpected{SourceLaunchReason::input};
        const std::string_view name{begin, static_cast<std::size_t>(end - begin)};
        if (inode && name != "." && name != "..") {
            int descriptor{};
            const auto parsed = std::from_chars(name.data(), name.data() + name.size(), descriptor);
            if (name.empty() || descriptor < 0 || parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() ||
                std::find(known.begin(), known.end(), descriptor) == known.end())
                return std::unexpected{SourceLaunchReason::association};
        }
        offset += length;
    }
    return {};
}
auto source_launch_directory_names(std::span<const char> bytes, SourceLaunchPhase phase,
    bool root, std::uint64_t& mask, std::size_t& visited) noexcept
    -> std::expected<void, SourceLaunchReason> {
    constexpr std::array<std::string_view, 11> names{
        "manifest.json", "checksums.sha256", "host-environment.json", "q0b",
        "q1", "q2", "q3", "q4", "q5", "q6", "q7"};
    const auto last_bit = static_cast<std::size_t>(phase) + 4U;
    if (last_bit < 9U || last_bit > names.size()) return std::unexpected{SourceLaunchReason::invalid_input};
    std::size_t offset{};
    while (offset < bytes.size()) {
        if (visited >= 64U || bytes.size() - offset < 20U) return std::unexpected{SourceLaunchReason::capacity};
        ++visited;
        std::uint64_t inode{}; std::uint16_t length{};
        std::memcpy(&inode, bytes.data() + offset, sizeof(inode));
        std::memcpy(&length, bytes.data() + offset + 16U, sizeof(length));
        if (length < 20U || length > bytes.size() - offset || length % 8U)
            return std::unexpected{SourceLaunchReason::ancillary};
        const auto* first = bytes.data() + offset + 19U;
        const auto* end = static_cast<const char*>(std::memchr(first, '\0', length - 19U));
        if (!end) return std::unexpected{SourceLaunchReason::ancillary};
        const std::string_view name{first, static_cast<std::size_t>(end - first)};
        offset += length;
        if (!inode || name == "." || name == "..") continue;
        auto bit = names.size();
        if (!root) { if (name == "evidence.json") bit = 0; }
        else for (std::size_t index{}; index < last_bit; ++index) if (name == names[index]) { bit = index; break; }
        if (bit == names.size() || (mask & (UINT64_C(1) << bit)))
            return std::unexpected{SourceLaunchReason::association};
        mask |= UINT64_C(1) << bit;
    }
    return {};
}
} // namespace epycbench::platform_linux::host_work
