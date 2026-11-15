#include "monitoring/host_q0b_governed_parent.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/host_work_exec_entry.hpp"
#include "runtime/controller_process_production_authority.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <utility>
#include <unistd.h>

// Direct governed baseline parent. Main supplies its sole authority, actual
// cancellation token and unchanged original RAW launch/work intervals.
namespace epycbench::monitoring::qualification::detail {
namespace {
namespace work = platform_linux::host_work;
using Raw = platform_linux::detail::RawStatusResult;
using Role = work::SourceLaunchRole;
using Phase = work::SourceLaunchPhase;
using Operation = work::SourceLaunchOperation;
using Reason = work::SourceLaunchReason;
constexpr std::uint64_t gib = UINT64_C(1073741824);
constexpr std::uint64_t mib = UINT64_C(1048576);
constexpr std::uint64_t work_span = UINT64_C(300000000000);
constexpr std::uint64_t cleanup_span = UINT64_C(30000000000);
constexpr std::uint64_t stop_span = UINT64_C(5000000000);
constexpr std::string_view command = "__monitoring-host-q0b-governed-parent";

struct Arguments final {
    const char* repository{};
    const char* qualification{};
    int projection{-1}, image{-1}, entry{-1}, parent{};
    std::uint64_t origin{}, work_end{}, launch_origin{}, launch_end{};
};
template<class Integer> auto scalar(const char* raw, Integer& value) noexcept -> bool {
    if (!raw || !*raw) return false;
    const std::string_view text{raw};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// Every inherited FD is owned by these cells before any metadata, resource
// allocation or signal operation. Fixed failure stock and token/clock borrows
// remain live through confirmed retirement or this actor's terminal retention.
struct Parent final {
    runtime::RunnerCancellation& token;
    HostFrozenSourceLaunchClock& clock;
    Arguments arguments;
    const HostQ0bGovernedParentObserver* observer{};
    std::array<int, 3> inherited;
    std::array<bool, 3> consumed{};
    std::optional<work::ResourceLedger> ledger;
    std::optional<work::ResourceLease> baseline;
    std::optional<work::SourceLaunchInvocation> invocation;
    platform_linux::detail::ControllerSignalScope signals;
    work::SourceLaunchOperations original{work::production_source_launch_operations()};
    work::SourceLaunchStreamOperations streams{work::production_source_launch_stream_operations()};
    platform_linux::detail::TopologySourceOperations files{
        platform_linux::detail::production_topology_source_operations()};
    std::optional<work::SourceLaunchFact> first;
    // All accepted lower facts plus the fixed parent acquisition/restoration
    // stock survive lower-owner destruction; no duplicate late import.
    std::array<work::SourceLaunchFact, work::SourceLaunchInvocation::failure_capacity + 64U> later{};
    std::size_t later_count{};
    bool imported_primary{};
    std::size_t imported_secondary{};
    std::optional<platform_linux::detail::SignalScopeFailure> signal_failure;
    std::optional<work::SourceLaunchChildFailure> child_failure;
    std::array<std::size_t, 3> output_bytes{};
    std::array<char, 4096> buffer{};
    std::array<std::array<char, 4096>, 2> relay{};
    std::array<std::size_t, 2> relay_size{}, relay_offset{};
    std::array<int, 2> output_flags{};
    std::array<bool, 2> output_flags_owned{}, output_restore_attempted{}, output_flags_verified{};
    std::array<std::byte, 32> setup{};
    std::size_t setup_bytes{};
    std::uint64_t last{}, cleanup_end{};
    long actual_fork{};
    int prior_subreaper{};
    bool signal_owned{}, subreaper_owned{}, uncertain{}, notified{}, successful{}, launch_confirmed{};
    sigset_t pipe_original_mask{};
    bool pipe_mask_owned{};

    Parent(runtime::RunnerCancellation& cancellation, HostFrozenSourceLaunchClock& sample,
        Arguments value, std::uint64_t main_entry) noexcept
        : token{cancellation}, clock{sample}, arguments{value},
          inherited{value.projection, value.image, value.entry}, last{main_entry} {
        for (std::size_t index{}; index < inherited.size(); ++index)
            for (std::size_t previous{}; previous < index; ++previous)
                if (inherited[index] >= 0 && inherited[index] == inherited[previous]) {
                    inherited[index] = -1;
                    static_cast<void>(fail(Reason::association, Operation::input_stat));
                }
    }
    Parent(const Parent&) = delete;
    ~Parent() noexcept {
        if (invocation || baseline || signal_owned || subreaper_owned || pipe_mask_owned ||
            std::ranges::any_of(output_flags_owned, [](bool owned) { return owned; }) ||
            std::ranges::any_of(inherited, [](int fd) { return fd >= 0; })) std::terminate();
    }
    void remember(work::SourceLaunchFact fact) noexcept {
        if (!first) first = fact;
        else if (later_count < later.size()) later[later_count++] = fact;
        else uncertain = true;
    }
    auto fail(Reason reason, Operation operation, Raw raw = {}, int descriptor = -1) noexcept -> bool {
        remember({reason, operation, Role::collector, 1, raw.value, raw.error_number, descriptor});
        return false;
    }
    void import_lower() noexcept {
        if (!invocation) return;
        if (invocation->primary() && !imported_primary) {
            remember(*invocation->primary()); imported_primary = true;
        }
        const auto facts = invocation->secondary();
        for (; imported_secondary < facts.size(); ++imported_secondary) remember(facts[imported_secondary]);
    }
    void retain_signal(const platform_linux::detail::SignalScopeFailure& failure) noexcept {
        if (!signal_failure) signal_failure = failure;
        static_cast<void>(fail(Reason::signal, Operation::signal));
        if (failure.facts.residual) uncertain = true;
    }
    auto guard_operations() noexcept -> work::GuardOperations {
        return {this, +[](void* opaque, std::uint64_t* now) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(opaque);
            const auto sampled = work::source_launch_raw_now(self.clock.current, now, &self.clock.actual);
            if (!sampled.value && !sampled.error_number) {
                if (*now < self.last) {
                    self.clock.current = {};
                    self.clock.current.reason = Reason::clock;
                    self.clock.current.operation = work::SourceLaunchChildOperation::clock;
                    self.clock.current.raw_present = true;
                    self.clock.current.native_result = self.clock.actual.value;
                    self.clock.current.error_number = self.clock.actual.error_number;
                    static_cast<void>(self.fail(Reason::clock, Operation::guard, self.clock.actual));
                    return {-1, EINVAL};
                }
                self.last = *now;
            }
            return sampled;
        }, +[](void* opaque) noexcept {
            auto& self = *static_cast<Parent*>(opaque);
            const auto observed = self.signal_owned ? self.signals.observe()
                : platform_linux::detail::ControllerSignal::none;
            if (observed != platform_linux::detail::ControllerSignal::none)
                static_cast<void>(self.token.try_request(observed == platform_linux::detail::ControllerSignal::interrupt
                    ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
            return self.token.requested();
        }, &clock.actual};
    }
    auto guard(bool retirement = false) noexcept -> bool {
        if (!retirement && first) return false;
        if (!retirement && token.requested()) return fail(Reason::cancelled, Operation::guard);
        auto operations = guard_operations();
        std::uint64_t now{};
        const auto raw = operations.now(operations.context, &now);
        if (raw.value || raw.error_number || now < last)
            return fail(Reason::clock, Operation::guard, clock.actual);
        last = now;
        const auto end = retirement ? cleanup_end : (launch_confirmed ? arguments.work_end :
                                                     std::min(arguments.work_end, arguments.launch_end));
        if (now >= end) return fail(Reason::deadline, Operation::guard);
        if (!retirement && operations.cancelled(operations.context))
            return fail(Reason::cancelled, Operation::guard);
        return true;
    }
    auto close_inherited(std::size_t slot) noexcept -> bool {
        if (inherited[slot] < 0 || consumed[slot]) return true;
        const auto descriptor = inherited[slot];
        consumed[slot] = true;
        const auto raw = files.close(files.context, descriptor);
        if (raw.value || raw.error_number) {
            uncertain = true;
            return fail(Reason::close, Operation::close, raw, descriptor);
        }
        inherited[slot] = -1;
        return true;
    }
    auto prepare_outputs() noexcept -> bool {
        // Query both original descriptions before changing either, including
        // the case where stdout/stderr are duplicates of the same description.
        for (std::size_t index{}; index < output_flags.size(); ++index) {
            if (!guard()) return false;
            const auto queried = streams.output_flags(static_cast<int>(index) + 1, false, 0);
            if (queried.value < 0 || queried.value > INT32_MAX || queried.error_number)
                return fail(Reason::input, Operation::input_stat, queried, static_cast<int>(index) + 1);
            output_flags[index] = static_cast<int>(queried.value);
            if ((output_flags[index] & O_ACCMODE) == O_RDONLY)
                return fail(Reason::association, Operation::input_stat, queried, static_cast<int>(index) + 1);
        }
        for (std::size_t index{}; index < output_flags.size(); ++index) {
            if (!guard()) return false;
            const auto descriptor = static_cast<int>(index) + 1;
            output_flags_owned[index] = true; // Restoration owner before the mutating leaf.
            const auto installed = streams.output_flags(descriptor, true, output_flags[index] | O_NONBLOCK);
            if (installed.value || installed.error_number)
                return fail(Reason::input, Operation::input_stat, installed, descriptor);
            if (!guard()) return false;
            const auto verified = streams.output_flags(descriptor, false, 0);
            if (verified.value < 0 || verified.error_number || verified.value != (output_flags[index] | O_NONBLOCK))
                return fail(Reason::association, Operation::input_stat, verified, descriptor);
            output_flags_verified[index] = true;
        }
        return guard();
    }
    auto restore_outputs() noexcept -> bool {
        bool confirmed = true;
        for (std::size_t index{}; index < output_flags.size(); ++index) {
            if (!output_flags_owned[index]) continue;
            if (output_restore_attempted[index]) { confirmed = false; continue; }
            output_restore_attempted[index] = true;
            output_flags_verified[index] = false;
            const auto descriptor = static_cast<int>(index) + 1;
            const auto restored = streams.output_flags(descriptor, true, output_flags[index]);
            if (restored.value || restored.error_number) {
                uncertain = true; confirmed = false;
                static_cast<void>(fail(Reason::uncertainty, Operation::input_stat, restored, descriptor));
            } else output_flags_owned[index] = false;
        }
        // Verify only after restoring both potentially aliased descriptions.
        for (std::size_t index{}; index < output_flags.size(); ++index) {
            if (!output_restore_attempted[index] || output_flags_owned[index]) continue;
            const auto descriptor = static_cast<int>(index) + 1;
            const auto verified = streams.output_flags(descriptor, false, 0);
            if (verified.value != output_flags[index] || verified.error_number) {
                output_flags_owned[index] = true; uncertain = true; confirmed = false;
                static_cast<void>(fail(Reason::uncertainty, Operation::input_stat, verified, descriptor));
            }
        }
        return confirmed;
    }
    auto write_output(int descriptor, std::span<const std::byte> bytes) noexcept -> Raw {
        // This actor is single-threaded. Block only around this one write and
        // consume only the SIGPIPE newly generated by that actual write; the
        // inherited disposition and mask are restored, not globally ignored.
        sigset_t pipe{}, pending{};
        ::sigemptyset(&pipe); ::sigaddset(&pipe, SIGPIPE);
        const auto blocked = ::pthread_sigmask(SIG_BLOCK, &pipe, &pipe_original_mask);
        if (blocked) {
            static_cast<void>(fail(Reason::signal, Operation::signal, {blocked, 0}, descriptor));
            return {-1, blocked};
        }
        pipe_mask_owned = true;
        const auto before = ::sigpending(&pending);
        const auto before_error = before < 0 ? errno : 0;
        Raw written{-1, EINVAL};
        bool may_restore = before == 0 && ::sigismember(&pending, SIGPIPE) == 0;
        if (!may_restore)
            static_cast<void>(fail(Reason::signal, Operation::signal,
                {before, before_error}, descriptor));
        else {
            written = streams.write(descriptor, bytes);
            if ((written.value < 0 || written.error_number) && written.error_number != EAGAIN &&
                written.error_number != EWOULDBLOCK && written.error_number != EINTR)
                static_cast<void>(fail(Reason::input, Operation::input_read, written, descriptor));
            const auto after = ::sigpending(&pending);
            if (after != 0) {
                may_restore = false;
                static_cast<void>(fail(Reason::signal, Operation::signal, {after, errno}, descriptor));
            }
            else if (::sigismember(&pending, SIGPIPE) == 1) {
                timespec immediate{};
                const auto consumed_signal = ::sigtimedwait(&pipe, nullptr, &immediate);
                if (consumed_signal != SIGPIPE) {
                    may_restore = false;
                    static_cast<void>(fail(Reason::signal, Operation::signal,
                        {consumed_signal, consumed_signal < 0 ? errno : 0}, descriptor));
                }
            }
        }
        if (!may_restore) {
            // A failed sample/consume cannot prove it safe to unblock a newly
            // generated SIGPIPE. Keep the actual mask owner and enclosing
            // graph engaged; external termination is the recovery boundary.
            uncertain = true;
            return {-1, written.error_number ? written.error_number : EIO};
        }
        const auto restored = ::pthread_sigmask(SIG_SETMASK, &pipe_original_mask, nullptr);
        if (restored) {
            uncertain = true;
            static_cast<void>(fail(Reason::uncertainty, Operation::signal, {restored, 0}, descriptor));
        } else pipe_mask_owned = false;
        if (first || uncertain || pipe_mask_owned) return {-1, written.error_number ? written.error_number : EIO};
        return written;
    }
    void diagnose() noexcept {
        // Best-available fixed facts only while this known inherited stream is
        // verified nonblocking. Never restore flags then perform blocking IO.
        if (!first || !output_flags_verified[1] || pipe_mask_owned || !guard(true)) return;
        std::array<char, 192> text{};
        const auto size = std::snprintf(text.data(), text.size(),
            "q0b_parent_refusal reason=%u operation=%u result=%ld errno=%d fd=%d\n",
            static_cast<unsigned>(first->reason), static_cast<unsigned>(first->operation),
            first->native_result, first->error_number, first->descriptor);
        if (size <= 0 || static_cast<std::size_t>(size) >= text.size()) return;
        static_cast<void>(write_output(2, std::as_bytes(std::span{text}.first(static_cast<std::size_t>(size)))));
        // A partial/EAGAIN diagnostic is still failure; it grants no permission
        // to wait past the original cleanup fence or resume backend relay.
    }
    auto wait_output(int descriptor, bool retirement = false) noexcept -> bool {
        if (!guard(retirement)) return false;
        const auto end = retirement ? cleanup_end : (launch_confirmed ? arguments.work_end : arguments.launch_end);
        const auto remaining = end - last;
        const auto milliseconds = static_cast<int>(std::min(UINT64_C(100),
            remaining / UINT64_C(1000000) + (remaining % UINT64_C(1000000) != 0)));
        short events{};
        const auto waited = streams.output_wait(descriptor, milliseconds, &events);
        if (waited.value < 0 && waited.error_number == EINTR) return guard(retirement);
        if (waited.value < 0 || waited.value > 1 || waited.error_number || events & (POLLERR | POLLHUP | POLLNVAL))
            return fail(Reason::input, Operation::input_read, waited, descriptor);
        return guard(retirement);
    }
    auto notify_pid(std::span<const std::byte> bytes) noexcept -> bool {
        std::size_t sent{};
        for (std::size_t turn{}; turn < 52U && sent < bytes.size(); ++turn) {
            if (!guard()) return false;
            const auto written = write_output(1, bytes.subspan(sent));
            if (first || uncertain || pipe_mask_owned) return false;
            if (written.value > 0 && !written.error_number && static_cast<std::size_t>(written.value) <= bytes.size() - sent)
                sent += static_cast<std::size_t>(written.value);
            else if (written.value < 0 && (written.error_number == EINTR || written.error_number == EAGAIN ||
                                           written.error_number == EWOULDBLOCK)) {
                if (!wait_output(1)) return false;
            } else return fail(Reason::gate, Operation::gate, written, 1);
        }
        return sent == bytes.size() && guard() && !first && !uncertain && !pipe_mask_owned;
    }
    auto launch_operations() noexcept -> work::SourceLaunchOperations {
        auto result = original;
        result.context = this;
        result.pipe = +[](void* raw, int pair[2]) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard()) return {-1, ECANCELED};
            return self.original.pipe(self.original.context, pair);
        };
        result.gate_pair = +[](void* raw, int pair[2]) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard()) return {-1, ECANCELED};
            return self.original.gate_pair(self.original.context, pair);
        };
        result.open_null = +[](void* raw) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard()) return {-1, ECANCELED};
            return self.original.open_null(self.original.context);
        };
        result.fork = +[](void* raw) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard()) return {-1, ECANCELED};
            const auto actual = self.original.fork(self.original.context);
            if (actual.value > 0) self.actual_fork = actual.value;
            return actual;
        };
        result.group = +[](void* raw, int pid) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard()) return {-1, ECANCELED};
            return self.original.group(self.original.context, pid);
        };
        result.gate = +[](void* raw, int descriptor) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw);
            if (!self.guard() || self.first || self.uncertain || self.pipe_mask_owned ||
                self.notified || self.actual_fork <= 0 || self.actual_fork > INT32_MAX) return {-1, EINVAL};
            self.notified = true;
            std::array<std::byte, 8> bytes{};
            for (std::size_t index{}; index < bytes.size(); ++index)
                bytes[7U - index] = static_cast<std::byte>((static_cast<std::uint64_t>(self.actual_fork) >> (8U * index)) & 255U);
            if (!self.notify_pid(bytes) || self.first || self.uncertain || self.pipe_mask_owned)
                return {-1, EIO};
            if (self.observer) {
                const auto observed = self.observer->before_gate
                    ? self.observer->before_gate(self.observer->context, static_cast<int>(self.actual_fork),
                        self.arguments.launch_origin, self.arguments.launch_end)
                    : Raw{-1, EINVAL};
                if (observed.value || observed.error_number) {
                    static_cast<void>(self.fail(Reason::gate, Operation::gate, observed, descriptor));
                    return {-1, observed.error_number ? observed.error_number : EIO};
                }
                if (!self.guard()) return {-1, ECANCELED};
            }
            return self.original.gate(self.original.context, descriptor);
        };
        result.close = +[](void* raw, int descriptor) noexcept -> Raw {
            auto& self = *static_cast<Parent*>(raw); return self.original.close(self.original.context, descriptor);
        };
        result.child_entry = +[](void* raw, const work::SourceLaunchSpec& spec, int gate, int setup_fd) noexcept {
            auto& self = *static_cast<Parent*>(raw); self.original.child_entry(self.original.context, spec, gate, setup_fd);
        };
        // ProcessOperations retains its actual original context; it is not the
        // newly wrapped launch context and requires no duplicate child owner.
        return result;
    }
    auto prepare() -> bool {
        if (arguments.parent <= 0 || arguments.parent != ::getppid() || streams.effective_uid() == 0 ||
            arguments.work_end <= arguments.origin || arguments.work_end - arguments.origin > work_span ||
            arguments.work_end > UINT64_MAX - cleanup_span ||
            arguments.launch_origin < arguments.origin || arguments.launch_end <= arguments.launch_origin ||
            arguments.launch_end > arguments.work_end ||
            arguments.launch_end - arguments.launch_origin > UINT64_C(5000000000) ||
            last < arguments.launch_origin || last >= arguments.launch_end)
            return fail(Reason::association, Operation::guard);
        if (!guard()) return false; // Original prelaunch fence precedes fallible preparation.
        for (std::size_t index{}; index < inherited.size(); ++index) {
            if (inherited[index] < 3 || inherited[index] >= 29)
                return fail(Reason::association, Operation::input_stat, {}, inherited[index]);
            for (std::size_t previous{}; previous < index; ++previous)
                if (inherited[index] == inherited[previous]) {
                    inherited[index] = -1; // One inherited descriptor owner.
                    return fail(Reason::association, Operation::input_stat);
                }
        }
        auto prepared = work::ResourceLedger::prepare();
        if (!prepared) return fail(Reason::capacity, Operation::reserve);
        ledger.emplace(std::move(*prepared));
        // Invoker + this parent + one serial Git. The collector grant below is
        // a partition of the same enclosing limits, not another20GiB charge.
        auto charged = ledger->reserve(work::Resources{.processes = 3, .threads = 3,
            .descriptors = 48, .stack_bytes = core::ByteCount::from_base_units(24U * mib),
            .ordinary_bytes = core::ByteCount::from_base_units(710U * mib), // Python512 + Native192 + serialGit6.
            .address_space_bytes = core::ByteCount::from_base_units(734U * mib)});
        if (!charged) return fail(Reason::capacity, Operation::reserve);
        baseline.emplace(std::move(*charged));
        if (!guard()) return false;
        const auto acquired = signals.acquire();
        if (!acquired) { retain_signal(acquired.error()); return false; }
        signal_owned = true;
        if (!guard()) return false;
        if (!prepare_outputs()) return false;
        const auto executable = work::production_exec_operations();
        std::array<platform_linux::detail::ImageMetadata, 3> metadata{};
        for (std::size_t index{}; index < inherited.size(); ++index) {
            if (!guard()) return false;
            const auto raw = executable.metadata(executable.context, inherited[index], &metadata[index]);
            if (raw.value || raw.error_number) return fail(Reason::input, Operation::input_stat, raw, inherited[index]);
            const auto& value = metadata[index];
            if (!S_ISREG(value.mode) || value.links != 1 ||
                (index == 0U ? value.uid != streams.effective_uid()
                             : value.uid != streams.effective_uid() && value.uid != 0))
                return fail(Reason::association, Operation::input_stat, raw, inherited[index]);
            if (index == 0U) {
                if ((value.mode & 07777U) != 0600U || value.size != 0)
                    return fail(Reason::association, Operation::input_stat, raw, inherited[index]);
            } else {
                if (value.size <= 0 || !(value.mode & 0111U) || (value.mode & 06022U))
                    return fail(Reason::association, Operation::input_stat, raw, inherited[index]);
                if (!guard()) return false;
                const auto capabilities = executable.capability_size(executable.context, inherited[index]);
                if (capabilities.value != -1 || capabilities.error_number != ENODATA)
                    return fail(Reason::association, Operation::input_stat, capabilities, inherited[index]);
            }
            if (!guard()) return false;
        }
        if (!guard()) return false;
        platform_linux::detail::ImageMetadata running{};
        const auto running_raw = executable.running_metadata(executable.context, &running);
        if (running_raw.value || running_raw.error_number || running.device != metadata[1].device ||
            running.inode != metadata[1].inode || running.size != metadata[1].size)
            return fail(Reason::association, Operation::input_stat, running_raw, inherited[1]);
        // Reuse the existing fixed directory parser. A fourth immediate cell
        // owns the enumeration FD before fstat/refills and one consuming close.
        // It closes before fork and is never passed to the child inventory.
        if (!guard()) return false;
        const auto directory = files.open_root(files.context, "/proc/self/fd",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int inventory_fd{-1};
        if (directory.value >= 0 && directory.value <= INT32_MAX)
            inventory_fd = static_cast<int>(directory.value);
        bool inventory_confirmed = directory.value >= 3 && directory.value <= INT32_MAX && !directory.error_number;
        if (!inventory_confirmed) static_cast<void>(fail(Reason::input, Operation::input_open, directory));
        if (directory.value > INT32_MAX) { uncertain = true; return false; }
        const std::array inventory{0, 1, 2, inherited[0], inherited[1], inherited[2], inventory_fd};
        std::size_t visited{};
        bool inventory_end{};
        for (std::size_t refill{}; inventory_confirmed && refill < 8U; ++refill) {
            if (!guard()) { inventory_confirmed = false; break; }
            const auto read = files.entries(files.context, inventory_fd, buffer);
            if (!read.value && !read.error_number) { inventory_end = true; break; }
            if (read.value < 0 || read.error_number || static_cast<std::size_t>(read.value) > buffer.size()) {
                static_cast<void>(fail(Reason::input, Operation::input_read, read, inventory_fd));
                inventory_confirmed = false; break;
            }
            const auto accepted = work::source_launch_descriptor_names(
                std::span{buffer}.first(static_cast<std::size_t>(read.value)), inventory, visited);
            if (!accepted) { static_cast<void>(fail(accepted.error(), Operation::input_read)); inventory_confirmed = false; }
        }
        if (inventory_fd >= 0) {
            const auto closed = files.close(files.context, inventory_fd);
            if (closed.value || closed.error_number) {
                uncertain = true;
                static_cast<void>(fail(Reason::close, Operation::close, closed, inventory_fd));
                inventory_confirmed = false;
            }
        }
        if (!inventory_confirmed || !inventory_end) return false;
        if (!guard()) return false;
        const auto queried = streams.subreaper(false, &prior_subreaper);
        if (queried.value || queried.error_number) return fail(Reason::state, Operation::guard, queried);
        int enabled = 1;
        if (!guard()) return false;
        const auto installed = streams.subreaper(true, &enabled);
        if (installed.value || installed.error_number) return fail(Reason::state, Operation::guard, installed);
        subreaper_owned = true;
        if (!guard()) return false;
        invocation.emplace(*ledger, guard_operations(), launch_operations(), arguments.origin, arguments.work_end,
            &clock.current, Phase::q0b, work::SourceLaunchDeadline{arguments.launch_origin, arguments.launch_end});
        const auto native_parent = std::to_string(streams.process_id());
        std::array<std::string, 25> text{
            "host-work-exec-entry", "--source-deadline", std::to_string(arguments.launch_origin),
            std::to_string(arguments.launch_end), "--held-entry", "31", "30", "29", "--", "epycbench",
            "__monitoring-host-q0b-governed-characterize", "--repository-root", arguments.repository,
            "--qualification-id", arguments.qualification, "--projection-fd", "29", "--expected-parent", native_parent,
            "--origin-raw-ns", std::to_string(arguments.origin), "--end-raw-ns", std::to_string(arguments.work_end), {}, {}};
        std::array<char*, 24> pointers{};
        for (std::size_t index{}; index < 23U; ++index) pointers[index] = text[index].data();
        std::array<char*, 3> environment{const_cast<char*>("LC_ALL=C"), const_cast<char*>("PATH=/usr/bin:/bin"), nullptr};
        const std::array mappings{work::SourceLaunchDescriptorMapping{inherited[0], 29},
            work::SourceLaunchDescriptorMapping{inherited[1], 30}, work::SourceLaunchDescriptorMapping{inherited[2], 31}};
        const std::array inherited_inventory{0, 1, 2, inherited[0], inherited[1], inherited[2]};
        const work::SourceLaunchSpec spec{inherited[2], pointers.data(), environment.data(), mappings, inherited_inventory,
            {core::ByteCount::from_base_units(20U * gib), core::ByteCount::from_base_units(8U * mib), 1024},
            streams.process_id(), arguments.launch_origin, arguments.launch_end, Phase::q0b, 1};
        const work::Resources collector{.processes = 1, .threads = 11, .measurement_workers = 8, .service_roles = 2,
            .descriptors = 128, .stack_bytes = core::ByteCount::from_base_units(88U * mib),
            .mapped_bytes = core::ByteCount::from_base_units(16U * gib),
            .ordinary_bytes = core::ByteCount::from_base_units(3U * gib),
            .address_space_bytes = core::ByteCount::from_base_units(19U * gib + 88U * mib)};
        if (!guard()) return false;
        const auto launched = invocation->launch(Role::collector, collector, spec);
        import_lower();
        if (!launched || first || uncertain || pipe_mask_owned) return false;
        const auto view = invocation->slot(Role::collector);
        if (!view.occupied || view.pid <= 0 || !view.group_confirmed || !view.gate_released)
            return fail(Reason::association, Operation::gate);
        // The original entry fence remains armed until the actual CLOEXEC
        // setup channel ends without a failure record, confirming native exec.
        return guard();
    }
    auto flush_output(std::size_t index) noexcept -> bool {
        if (first || uncertain || pipe_mask_owned) return false;
        if (relay_offset[index] == relay_size[index]) return true;
        if (!guard()) return false;
        const auto descriptor = static_cast<int>(index) + 1;
        const auto bytes = std::as_bytes(std::span{relay[index]}).subspan(relay_offset[index],
            relay_size[index] - relay_offset[index]);
        const auto written = write_output(descriptor, bytes);
        if (first || uncertain || pipe_mask_owned) return false;
        if (written.value > 0 && !written.error_number && static_cast<std::size_t>(written.value) <= bytes.size()) {
            relay_offset[index] += static_cast<std::size_t>(written.value);
            if (relay_offset[index] == relay_size[index]) relay_offset[index] = relay_size[index] = 0;
            return guard();
        }
        if (written.value < 0 && (written.error_number == EAGAIN || written.error_number == EWOULDBLOCK ||
                                  written.error_number == EINTR)) return wait_output(descriptor);
        return fail(Reason::input, Operation::input_read, written, descriptor);
    }
    auto drain(bool retirement = false) noexcept -> bool {
        const auto view = invocation->slot(Role::collector);
        for (std::size_t index{}; index < view.streams.size(); ++index) {
            if (!guard(retirement)) return false;
            if (!retirement && index < 2U) {
                if (!flush_output(index)) return false;
                if (relay_size[index]) continue; // Retain partial bytes; never overwrite or block.
            }
            const auto descriptor = view.streams[index];
            if (descriptor < 0 || std::ranges::none_of(invocation->descriptors(Role::collector),
                [descriptor](const auto& owned) { return owned.descriptor == descriptor && !owned.consumed; })) continue;
            const auto raw = files.read(files.context, descriptor, buffer);
            if (raw.value > 0 && !raw.error_number) {
                const auto count = static_cast<std::size_t>(raw.value);
                const auto limit = index == 0U ? 10U * mib : index == 1U ? mib : setup.size();
                if (count > buffer.size() || count > limit - output_bytes[index])
                    return fail(Reason::capacity, Operation::input_read, raw, descriptor);
                output_bytes[index] += count;
                if (index == 2U) {
                    std::memcpy(setup.data() + setup_bytes, buffer.data(), count); setup_bytes += count;
                    if (setup_bytes == setup.size()) {
                        const auto decoded = work::decode_source_launch_child_failure(setup);
                        if (!decoded) return fail(decoded.error(), Operation::child_setup, raw, descriptor);
                        child_failure = *decoded; // Full lower enum/argument/raw fact survives projection.
                        return fail(decoded->reason, Operation::child_setup,
                            {static_cast<long>(decoded->native_result), decoded->error_number}, decoded->native_argument);
                    }
                } else if (!retirement) {
                    std::copy_n(buffer.begin(), count, relay[index].begin());
                    relay_size[index] = count; relay_offset[index] = 0;
                    if (!flush_output(index)) return false;
                }
            } else if (!raw.value && !raw.error_number) {
                if (index == 2U && setup_bytes && setup_bytes != setup.size())
                    static_cast<void>(fail(Reason::invalid_input, Operation::child_setup, raw, descriptor));
                const auto closed = invocation->close_descriptor(Role::collector, descriptor);
                import_lower();
                if (!closed) return false;
                if (index == 2U && !retirement && !first && !setup_bytes && view.pid > 0 && view.group_confirmed && view.gate_released) {
                    // EOF and its consuming close can cross the original
                    // launch fence. Check it before publishing work mode;
                    // retirement draining never establishes startup success.
                    if (!guard()) return false;
                    launch_confirmed = true;
                }
            } else if (raw.error_number != EAGAIN && raw.error_number != EWOULDBLOCK && raw.error_number != EINTR)
                return fail(Reason::input, Operation::input_read, raw, descriptor);
        }
        return true;
    }
    auto run() noexcept -> bool {
        for (std::size_t step{}; step < 4096U && guard(); ++step) {
            if (!drain()) return false;
            const auto terminal = invocation->observe_terminal(Role::collector);
            import_lower();
            if (!terminal) return false;
            if (*terminal) {
                const auto status = invocation->slot(Role::collector).native_wait_status;
                if (!status || !WIFEXITED(*status) || WEXITSTATUS(*status) != 0 || first) return false;
                // The final pipe bytes may still be buffered after wait. Do
                // not turn a truncated backend body into Native success.
                const auto owned = invocation->descriptors(Role::collector);
                const auto view = invocation->slot(Role::collector);
                if (!relay_size[0] && !relay_size[1] && std::ranges::none_of(view.streams, [&](int fd) {
                    return std::ranges::any_of(owned,
                        [fd](const auto& item) { return item.descriptor == fd && !item.consumed; });
                })) return true;
            }
            if (!guard()) return false;
            const auto paused = streams.pause(100);
            if (paused.value < 0 && paused.error_number != EINTR)
                return fail(Reason::input, Operation::input_read, paused);
        }
        return false;
    }
    auto retire() noexcept -> bool {
        bool confirmed = !uncertain;
        bool tail_available{};
        bool children_confirmed = !invocation || invocation->slot(Role::collector).raw_pid <= 0;
        std::uint64_t start{};
        const auto sampled = work::source_launch_raw_now(clock.current, &start, &clock.actual);
        if (sampled.value || sampled.error_number || start < last || start > UINT64_MAX - cleanup_span) {
            static_cast<void>(fail(Reason::clock, Operation::guard, clock.actual)); confirmed = false;
        }
        else { last = start; cleanup_end = std::min(arguments.work_end + cleanup_span, start + cleanup_span); tail_available = true; }
        if (invocation && invocation->slot(Role::collector).raw_pid > 0 && tail_available) {
            const auto stop = [&](platform_linux::ProcessSignal signal) noexcept {
                const auto view = invocation->slot(Role::collector);
                if (view.group_confirmed) static_cast<void>(invocation->request_group_stop(Role::collector, signal));
                else if (!view.terminal) static_cast<void>(invocation->request_stop(Role::collector, signal));
                import_lower();
            };
            if (!successful) stop(platform_linux::ProcessSignal::terminate);
            bool killed{}, no_children{};
            for (std::size_t step{}; step < 304U && guard(true); ++step) {
                static_cast<void>(drain(true)); // No relay follows a latched failure.
                const auto terminal = invocation->observe_terminal(Role::collector);
                import_lower();
                if (!terminal) { confirmed = false; break; }
                if (*terminal) {
                    const auto orphan = invocation->reap_orphans();
                    import_lower();
                    if (!orphan) { confirmed = false; break; }
                    if (*orphan) { no_children = true; break; }
                    // Leader exit never implies its Git descendant is gone.
                    if (successful) stop(platform_linux::ProcessSignal::terminate);
                }
                if (!killed && last >= start + stop_span) { stop(platform_linux::ProcessSignal::kill); killed = true; }
                const auto paused = streams.pause(100);
                if (paused.value < 0 && paused.error_number != EINTR) {
                    static_cast<void>(fail(Reason::input, Operation::input_read, paused)); confirmed = false; break;
                }
            }
            children_confirmed = no_children;
            confirmed = confirmed && no_children;
        }
        if (invocation && children_confirmed) {
            const auto view = invocation->slot(Role::collector);
            if (view.raw_pid > 0 && view.group_confirmed) {
                if (!tail_available || !guard(true)) confirmed = false;
                else {
                    const auto absent = original.process.send_signal(original.process.context, -view.pid, 0);
                    if (absent.value != -1 || absent.error_number != ESRCH) {
                        uncertain = true; confirmed = false;
                        static_cast<void>(fail(Reason::uncertainty, Operation::signal, absent, -view.pid));
                    }
                }
            }
        }
        if (invocation) {
            import_lower();
            if (invocation->slot(Role::collector).occupied && !invocation->retire(Role::collector)) confirmed = false;
            import_lower(); // Retain primary and ordered consuming-close failures before reset.
            if (invocation->has_owned_resources() || invocation->uncertain()) confirmed = false;
        }
        for (std::size_t index{}; index < inherited.size(); ++index)
            if (!close_inherited(index)) confirmed = false;
        diagnose();
        if (!restore_outputs()) confirmed = false;
        if (children_confirmed && subreaper_owned) {
            auto prior = prior_subreaper;
            const auto raw = streams.subreaper(true, &prior);
            if (raw.value || raw.error_number) {
                uncertain = true; confirmed = false;
                static_cast<void>(fail(Reason::uncertainty, Operation::guard, raw));
            } else subreaper_owned = false;
        }
        if (children_confirmed && signal_owned && !pipe_mask_owned) {
            const auto closed = signals.close();
            if (!closed) {
                retain_signal(closed.error());
                if (closed.error().facts.residual) confirmed = false;
                else signal_owned = false;
            } else if (closed->residual) {
                uncertain = true; confirmed = false;
                static_cast<void>(fail(Reason::uncertainty, Operation::signal));
            } else signal_owned = false;
            if (closed && closed->observed != platform_linux::detail::ControllerSignal::none)
                static_cast<void>(fail(Reason::cancelled, Operation::guard));
        }
        if (!confirmed || uncertain || pipe_mask_owned) return false;
        invocation.reset();
        if (baseline) { if (!baseline->retire()) return false; baseline.reset(); }
        ledger.reset();
        return true;
    }
};
}

auto is_host_q0b_governed_parent(int argc, char* argv[]) noexcept -> bool {
    return argc > 1 && argv && argv[1] && std::string_view{argv[1]} == command;
}
static auto run_governed_parent(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& token,
    HostFrozenSourceLaunchClock& clock, std::uint64_t main_entry,
    const HostQ0bGovernedParentObserver* observer) noexcept -> int {
    static_cast<void>(authority); // Borrow the sole main-owned authority; no second bootstrap.
    constexpr std::array<std::string_view, 10> switches{"--repository-root", "--qualification-id", "--projection-fd",
        "--current-image-fd", "--entry-fd", "--expected-parent", "--origin-raw-ns", "--work-end-raw-ns",
        "--launch-origin-raw-ns", "--launch-end-raw-ns"};
    if (argc != 22 || !is_host_q0b_governed_parent(argc, argv)) return 64;
    for (std::size_t index{}; index < switches.size(); ++index)
        if (!argv[2U + index * 2U] || std::string_view{argv[2U + index * 2U]} != switches[index] ||
            !argv[3U + index * 2U]) return 64;
    Arguments arguments{.repository = argv[3], .qualification = argv[5]};
    if (!scalar(argv[7], arguments.projection) || !scalar(argv[9], arguments.image) ||
        !scalar(argv[11], arguments.entry)) return 64;
    Parent owner{token, clock, arguments, main_entry}; // Immediate owning cells.
    owner.observer = observer;
    const bool parsed = scalar(argv[13], arguments.parent) && scalar(argv[15], arguments.origin) &&
        scalar(argv[17], arguments.work_end) && scalar(argv[19], arguments.launch_origin) &&
        scalar(argv[21], arguments.launch_end);
    owner.arguments = arguments;
    bool completed{};
    try {
        const std::string_view repository{arguments.repository}, qid{arguments.qualification};
        if (parsed && !repository.empty() && repository.size() <= 511U && repository.front() == '/' &&
            !qid.empty() && qid.size() <= 128U && qid != "." && qid != ".." && qid.find('/') == std::string_view::npos)
            completed = owner.prepare() && owner.run();
        else static_cast<void>(owner.fail(Reason::invalid_input, Operation::guard));
    } catch (...) { static_cast<void>(owner.fail(Reason::state, Operation::guard)); }
    owner.successful = completed;
    const bool retired = owner.retire();
    if (!retired || !completed || owner.first) {
        if (!retired) owner.streams.retain_until_outer_termination();
        return 78;
    }
    return 0;
}
auto run_host_q0b_governed_parent(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& token,
    HostFrozenSourceLaunchClock& clock, std::uint64_t main_entry) noexcept -> int {
    return run_governed_parent(argc, argv, authority, token, clock, main_entry, nullptr);
}
auto run_host_q0b_governed_parent_observed(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& token,
    HostFrozenSourceLaunchClock& clock, std::uint64_t main_entry,
    const HostQ0bGovernedParentObserver& observer) noexcept -> int {
    return run_governed_parent(argc, argv, authority, token, clock, main_entry, &observer);
}
} // namespace epycbench::monitoring::qualification::detail
