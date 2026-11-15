#include "platform_linux/host_work_exec_entry.hpp"

#include "platform_linux/host_frozen_control_preparation.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace epycbench::platform_linux::host_work {
namespace {
using Raw = detail::RawStatusResult;
using HeldMetadata = detail::ImageMetadata;
auto ok(Raw value) noexcept -> bool { return value.value == 0 && value.error_number == 0; }
auto same(const HeldMetadata& a, const HeldMetadata& b) noexcept -> bool {
    return a.device == b.device && a.inode == b.inode && a.mode == b.mode &&
        a.uid == b.uid && a.gid == b.gid && a.size == b.size && a.links == b.links &&
        a.modified_seconds == b.modified_seconds && a.modified_nanoseconds == b.modified_nanoseconds &&
        a.changed_seconds == b.changed_seconds && a.changed_nanoseconds == b.changed_nanoseconds;
}
auto timestamps(const HeldMetadata& m) noexcept -> bool {
    return m.modified_nanoseconds >= 0 && m.modified_nanoseconds < 1000000000 &&
        m.changed_nanoseconds >= 0 && m.changed_nanoseconds < 1000000000;
}
auto socket_endpoint(int descriptor, int expected_type) noexcept -> Raw {
    int domain{}, type{}; socklen_t length = sizeof(int);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0)
        return {-1, errno};
    if (length != sizeof(int) || domain != AF_UNIX) return {-1, EINVAL};
    length = sizeof(int);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &length) != 0)
        return {-1, errno};
    if (length != sizeof(int) || type != expected_type) return {-1, EINVAL};
    sockaddr_un peer{}; length = sizeof(peer);
    if (::getpeername(descriptor, reinterpret_cast<sockaddr*>(&peer), &length) != 0)
        return {-1, errno};
    return peer.sun_family == AF_UNIX ? Raw{0, 0} : Raw{-1, EINVAL};
}
struct Run final {
    ExecOperations exec;
    LimitOperations limits;
    std::optional<ExecFailure> failure;
    std::uint64_t end{}, last{};
    ExecPhase phase{ExecPhase::state};
    void fail(ExecPhase current, ExecReason reason, std::optional<Reason> lower = {}) noexcept {
        if (!failure) failure.emplace(ExecFailure{current, reason, lower, {}, 0});
    }
    auto check() noexcept -> bool {
        if (failure) return false;
        if (limits.guard.cancelled && limits.guard.cancelled(limits.guard.context)) {
            fail(phase, ExecReason::cancelled); return false;
        }
        std::uint64_t now{};
        if (!ok(limits.guard.now(limits.guard.context, &now)) || now < last) {
            fail(phase, ExecReason::clock); return false;
        }
        last = now;
        if (end != 0 && now >= end) {
            fail(phase, ExecReason::deadline); return false;
        }
        if (limits.guard.cancelled(limits.guard.context)) {
            fail(phase, ExecReason::cancelled); return false;
        }
        return true;
    }
    auto status(ExecPhase current, Raw result) noexcept -> bool {
        if (!ok(result)) fail(current, ExecReason::native_failure);
        return check();
    }
    auto capabilities(int descriptor, ExecPhase current) noexcept -> bool {
        phase = current;
        if (!check()) return false;
        const auto result = exec.capability_size(exec.context, descriptor);
        if (result.value != -1 || result.error_number != ENODATA)
            fail(current, ExecReason::authentication);
        return check();
    }
    auto metadata(int descriptor, HeldMetadata& output, ExecPhase current) noexcept -> bool {
        phase = current;
        return check() && status(current, exec.metadata(exec.context, descriptor, &output));
    }
    auto flags(int descriptor, int& output, ExecPhase current) noexcept -> bool {
        phase = current;
        if (!check()) return false;
        const auto result = exec.file_flags(exec.context, descriptor);
        if (result.value < 0 || result.value > std::numeric_limits<int>::max() || result.error_number)
            fail(current, ExecReason::native_failure);
        if (!check()) return false;
        output = static_cast<int>(result.value);
        return true;
    }
    // Every lower native operation is fenced by the ORIGINAL entry deadline.
    // The latch preserves its precise reason if the lower owner projects a
    // blocked wrapper operation to a generic limit/clock failure.
    static auto identity(void* pointer, int* pid, int* parent, std::uint64_t* uid) noexcept -> Raw {
        auto& run = *static_cast<Run*>(pointer);
        run.phase = ExecPhase::identity;
        if (!run.check()) return {-1, ECANCELED};
        const auto result = run.limits.identity(run.limits.context, pid, parent, uid);
        if (!ok(result)) run.fail(ExecPhase::identity, ExecReason::authentication);
        return run.check() ? result : Raw{-1, ECANCELED};
    }
    static auto get(void* pointer, NativeLimit which, LimitPair* value) noexcept -> Raw {
        auto& run = *static_cast<Run*>(pointer);
        run.phase = ExecPhase::limits;
        if (!run.check()) return {-1, ECANCELED};
        const auto result = run.limits.get(run.limits.context, which, value);
        if (!ok(result)) run.fail(ExecPhase::limits, ExecReason::limit_failure, Reason::limit_get);
        return run.check() ? result : Raw{-1, ECANCELED};
    }
    static auto set(void* pointer, NativeLimit which, LimitPair value) noexcept -> Raw {
        auto& run = *static_cast<Run*>(pointer);
        run.phase = ExecPhase::limits;
        if (!run.check()) return {-1, ECANCELED};
        const auto result = run.limits.set(run.limits.context, which, value);
        if (!ok(result)) run.fail(ExecPhase::limits, ExecReason::limit_failure, Reason::limit_set);
        return run.check() ? result : Raw{-1, ECANCELED};
    }
    static auto now(void* pointer, std::uint64_t* output) noexcept -> Raw {
        auto& run = *static_cast<Run*>(pointer);
        if (!run.check()) return {-1, ECANCELED};
        *output = run.last; return {0, 0};
    }
    static auto cancelled(void* pointer) noexcept -> bool {
        auto& run = *static_cast<Run*>(pointer);
        static_cast<void>(run.check());
        return run.failure && run.failure->reason == ExecReason::cancelled;
    }
};
} // namespace

HostWorkExecEntry::HostWorkExecEntry(int parent, int target, int projection, int stage, int entry,
    std::array<char*, 33> arguments, ExecOperations operations, LimitOperations limits,
    std::optional<SourceDeadline> source_deadline) noexcept
    : parent_{parent}, target_{target}, projection_{projection}, stage_{stage}, entry_{entry}, arguments_{arguments},
      operations_{operations}, limits_{limits}, source_deadline_{source_deadline} {}
HostWorkExecEntry::HostWorkExecEntry(HostWorkExecEntry&& other) noexcept
    : parent_{std::exchange(other.parent_, 0)}, target_{std::exchange(other.target_, -1)},
      projection_{std::exchange(other.projection_, -1)}, stage_{std::exchange(other.stage_, -1)},
      entry_{std::exchange(other.entry_, -1)}, arguments_{other.arguments_},
      operations_{other.operations_}, limits_{other.limits_},
      source_deadline_{std::exchange(other.source_deadline_, std::nullopt)} {}
HostWorkExecEntry::~HostWorkExecEntry() noexcept {
    if (target_ >= 0) static_cast<void>(operations_.close(operations_.context, target_));
    if (projection_ >= 0) static_cast<void>(operations_.close(operations_.context, projection_));
    if (stage_ >= 0) static_cast<void>(operations_.close(operations_.context, stage_));
    if (entry_ >= 0) static_cast<void>(operations_.close(operations_.context, entry_));
}
auto HostWorkExecEntry::prepare(int parent, int target, int projection,
    std::span<char* const> arguments, ExecOperations exec, LimitOperations limits) noexcept
    -> std::expected<HostWorkExecEntry, ExecFailure> {
    return prepare_extended(parent, target, projection, -1, -1, arguments, exec, limits);
}
auto HostWorkExecEntry::prepare_extended(int parent, int target, int projection, int stage, int entry,
    std::span<char* const> arguments, ExecOperations exec, LimitOperations limits) noexcept
    -> std::expected<HostWorkExecEntry, ExecFailure> {
    return prepare_impl(parent, target, projection, stage, entry, arguments, exec,
        limits, std::nullopt, false);
}
auto HostWorkExecEntry::prepare_impl(int parent, int target, int projection, int stage, int entry,
    std::span<char* const> arguments, ExecOperations exec, LimitOperations limits,
    std::optional<SourceDeadline> deadline, bool without_projection) noexcept
    -> std::expected<HostWorkExecEntry, ExecFailure> {
    const auto reject = [](ExecReason reason) -> std::unexpected<ExecFailure> {
        return std::unexpected{ExecFailure{ExecPhase::preparation, reason, {}, {}, 0}};
    };
    if ((deadline && (entry < 3 || deadline->end <= deadline->origin ||
            deadline->end - deadline->origin > UINT64_C(5000000000))) ||
        (without_projection && (!deadline || projection != -1 || stage < 3)))
        return reject(ExecReason::invalid_input);
    if (parent <= 0 || target < 3 || (!without_projection && projection < 3) ||
        target == projection || arguments.empty() ||
        !exec.metadata || !exec.file_flags || !exec.capability_size || !exec.cloexec ||
        !exec.execute || !exec.close || !limits.identity || !limits.get || !limits.set ||
        !limits.guard.now || !limits.guard.cancelled || limits.maximum_finite == 0)
        return reject(ExecReason::invalid_input);
    const auto endpoint = deadline ? exec.source_socket_endpoint : exec.socket_endpoint;
    if ((stage != -1 && (stage < 3 || stage == target || stage == projection || !endpoint)) ||
        (entry != -1 && (entry < 3 || entry == target || entry == projection || entry == stage || !exec.running_metadata)))
        return reject(ExecReason::invalid_input);
    if (stage >= 1024 || entry >= 1024 || target >= 1024 || projection >= 1024 || arguments.size() > 32) return reject(ExecReason::capacity);
    std::array<char*, 33> prepared{};
    std::size_t remaining{32768};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (!arguments[i] || (i == 0 && arguments[i][0] == '\0')) return reject(ExecReason::invalid_input);
        std::size_t bytes{};
        while (bytes < remaining && arguments[i][bytes] != '\0') ++bytes;
        if (bytes == remaining) return reject(ExecReason::capacity);
        remaining -= bytes + 1;
        prepared[i] = arguments[i];
    }
    return HostWorkExecEntry{parent, target, projection, stage, entry, prepared, exec, limits, deadline};
}
auto HostWorkExecEntry::prepare_source_extended(int parent, int target, int projection,
    int stage, int entry, std::uint64_t origin, std::uint64_t end,
    std::span<char* const> arguments, ExecOperations exec, LimitOperations limits) noexcept
    -> std::expected<HostWorkExecEntry, ExecFailure> {
    // Invalid preparation transfers no inherited descriptor ownership. Bound
    // subtraction only after ordering; a phase-clipped high interval need not
    // have enough uint64 headroom for origin + the full five seconds.
    return prepare_impl(parent, target, projection, stage, entry, arguments,
        exec, limits, SourceDeadline{origin, end}, false);
}
auto HostWorkExecEntry::prepare_source_without_projection(int parent, int target,
    int stage, int entry, std::uint64_t origin, std::uint64_t end,
    std::span<char* const> arguments, ExecOperations exec, LimitOperations limits) noexcept
    -> std::expected<HostWorkExecEntry, ExecFailure> {
    return prepare_impl(parent, target, -1, stage, entry, arguments,
        exec, limits, SourceDeadline{origin, end}, true);
}
auto HostWorkExecEntry::run() noexcept -> ExecFailure {
    if (!parent_) return {ExecPhase::state, ExecReason::invalid_state, {}, {}, 0};
    const int parent = std::exchange(parent_, 0);
    const int target = std::exchange(target_, -1);
    const int projection = std::exchange(projection_, -1);
    const int stage = std::exchange(stage_, -1);
    int entry = std::exchange(entry_, -1);
    Run run{operations_, limits_, {}, source_deadline_ ? source_deadline_->end : 0,
        source_deadline_ ? source_deadline_->origin : 0, ExecPhase::state};
    const auto work = [&]() {
        if (!run.check()) return;
        if (!source_deadline_) {
            if (run.last > UINT64_MAX - UINT64_C(5000000000)) {
                run.fail(ExecPhase::state, ExecReason::clock); return;
            }
            run.end = run.last + UINT64_C(5000000000);
        }
        int pid{}, actual_parent{}; std::uint64_t uid{};
        if (!ok(Run::identity(&run, &pid, &actual_parent, &uid))) return;
        if (uid == 0 || pid <= 0 || actual_parent != parent || pid == parent) {
            run.fail(ExecPhase::identity, ExecReason::authentication); return;
        }
        HeldMetadata original_stage{}, original_entry{};
        if (entry >= 0) {
            HeldMetadata running{};
            if (!run.metadata(entry, original_entry, ExecPhase::entry)) return;
            run.phase = ExecPhase::entry;
            if (!run.check() || !run.status(ExecPhase::entry,
                operations_.running_metadata(operations_.context, &running))) return;
            if (!same(original_entry, running) || (running.mode & S_IFMT) != S_IFREG ||
                running.size <= 0 || running.links != 1 || !timestamps(running) ||
                (running.uid != 0 && running.uid != uid) ||
                (running.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
                (running.mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) != 0) {
                run.fail(ExecPhase::entry, ExecReason::authentication); return;
            }
            if (!run.capabilities(entry, ExecPhase::entry)) return;
        }
        if (stage >= 0) {
            int flags{};
            if (!run.metadata(stage, original_stage, ExecPhase::stage) ||
                !run.flags(stage, flags, ExecPhase::stage)) return;
            if ((original_stage.mode & S_IFMT) != S_IFSOCK || (flags & O_ACCMODE) != O_RDWR ||
                (flags & (O_PATH | O_APPEND)) != 0) {
                run.fail(ExecPhase::stage, ExecReason::authentication); return;
            }
            run.phase = ExecPhase::stage;
            if (!run.check() || !run.status(ExecPhase::stage,
                (source_deadline_ ? operations_.source_socket_endpoint : operations_.socket_endpoint)
                    (operations_.context, stage))) return;
        }
        HeldMetadata original_target{}, original_projection{};
        if (!run.metadata(target, original_target, ExecPhase::target)) return;
        if ((original_target.mode & S_IFMT) != S_IFREG || original_target.size <= 0 ||
            original_target.links != 1 || !timestamps(original_target) ||
            (original_target.uid != 0 && original_target.uid != uid) ||
            (original_target.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
            (original_target.mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) != 0) {
            run.fail(ExecPhase::target, ExecReason::authentication); return;
        }
        if (!run.capabilities(target, ExecPhase::target)) return;
        if (projection >= 0) {
            if (!run.metadata(projection, original_projection, ExecPhase::projection)) return;
            if ((original_projection.mode & (S_IFMT | 07777)) != (S_IFREG | 0600) ||
                original_projection.size != 0 || original_projection.uid != uid ||
                original_projection.links != 1 || !timestamps(original_projection)) {
                run.fail(ExecPhase::projection, ExecReason::authentication); return;
            }
        }
        int target_flags{}, projection_flags{};
        if (!run.flags(target, target_flags, ExecPhase::target)) return;
        if (projection >= 0 && !run.flags(projection, projection_flags, ExecPhase::projection)) return;
        if ((target_flags & O_PATH) != 0 || (target_flags & O_ACCMODE) != O_RDONLY ||
            (projection >= 0 && ((projection_flags & O_PATH) != 0 || (projection_flags & O_APPEND) != 0 ||
            ((projection_flags & O_ACCMODE) != O_WRONLY && (projection_flags & O_ACCMODE) != O_RDWR)))) {
            run.fail(ExecPhase::flags, ExecReason::authentication); return;
        }
        auto admission = ChildLimitAdmission::prepare(parent,
            {core::ByteCount::from_base_units(UINT64_C(21474836480)),
             core::ByteCount::from_base_units(8388608), 1024});
        if (!admission) { run.fail(ExecPhase::limits, ExecReason::limit_failure, admission.error()); return; }
        const LimitOperations fenced{&run, Run::identity, Run::get, Run::set,
            limits_.maximum_finite, {&run, Run::now, Run::cancelled}};
        const auto receipt = admission->apply(fenced);
        if (!receipt) run.fail(ExecPhase::limits, ExecReason::limit_failure, receipt.error());
        if (!run.check()) return;
        for (const auto& [descriptor, close_on_exec] : {std::pair{target, true}, std::pair{projection, false}}) {
            if (descriptor < 0) continue;
            run.phase = ExecPhase::flags;
            if (!run.check() || !run.status(ExecPhase::flags,
                operations_.cloexec(operations_.context, descriptor, close_on_exec))) return;
        }
        if (stage >= 0) {
            run.phase = ExecPhase::flags;
            if (!run.check() || !run.status(ExecPhase::flags,
                operations_.cloexec(operations_.context, stage, false))) return;
        }
        HeldMetadata rechecked{};
        if (!run.metadata(target, rechecked, ExecPhase::recheck)) return;
        if (!same(original_target, rechecked)) { run.fail(ExecPhase::recheck, ExecReason::authentication); return; }
        if (projection >= 0) {
            if (!run.metadata(projection, rechecked, ExecPhase::recheck)) return;
            if (!same(original_projection, rechecked)) { run.fail(ExecPhase::recheck, ExecReason::authentication); return; }
        }
        if (!run.capabilities(target, ExecPhase::recheck) || !run.check()) return;
        if (stage >= 0) {
            if (!run.metadata(stage, rechecked, ExecPhase::recheck)) return;
            if (!same(original_stage, rechecked)) {
                run.fail(ExecPhase::recheck, ExecReason::authentication); return;
            }
            if (!run.check() || !run.status(ExecPhase::recheck,
                (source_deadline_ ? operations_.source_socket_endpoint : operations_.socket_endpoint)
                    (operations_.context, stage))) return;
        }
        if (entry >= 0) {
            HeldMetadata running{};
            if (!run.metadata(entry, rechecked, ExecPhase::entry)) return;
            run.phase = ExecPhase::entry;
            if (!run.check() || !run.status(ExecPhase::entry,
                operations_.running_metadata(operations_.context, &running))) return;
            if (!same(original_entry, rechecked) || !same(original_entry, running)) {
                run.fail(ExecPhase::entry, ExecReason::authentication); return;
            }
            if (!run.capabilities(entry, ExecPhase::entry) || !run.check()) return;
            const auto closed = operations_.close(operations_.context, entry);
            entry = -1; // Consuming close: never retry even when it fails.
            if (!ok(closed)) {
                run.fail(ExecPhase::entry, ExecReason::native_failure);
                run.failure->cleanup[run.failure->cleanup_count++] = closed;
                return;
            }
            if (!run.check()) return;
        }
        run.phase = ExecPhase::execute;
        static_cast<void>(operations_.execute(operations_.context, target, arguments_.data()));
        // fexecve may only return on failure. Never interpret zero as success.
        run.fail(ExecPhase::execute, ExecReason::exec_returned);
    };
    work();
    if (!run.failure) run.fail(ExecPhase::state, ExecReason::invalid_state);
    for (const int descriptor : {target, projection, stage, entry}) {
        if (descriptor < 0) continue;
        const auto closed = operations_.close(operations_.context, descriptor);
        if (!ok(closed)) run.failure->cleanup[run.failure->cleanup_count++] = closed;
    }
    return *run.failure;
}
auto production_source_exec_limit_operations() noexcept -> LimitOperations {
    auto operations = production_limit_operations();
    operations.guard.now = [](void*, std::uint64_t* output) noexcept -> Raw {
        const auto now = frozen_control_raw_now();
        if (!now) return {-1, now.error().native_error};
        *output = *now;
        return {0, 0};
    };
    return operations;
}
auto production_exec_operations() noexcept -> ExecOperations {
    return {nullptr,
        [](void*, int descriptor, HeldMetadata* output) noexcept -> Raw {
            struct stat value{};
            if (::fstat(descriptor, &value) != 0) return {-1, errno};
            *output = {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
                value.st_mode, value.st_uid, value.st_gid, value.st_size, value.st_mtim.tv_sec,
                value.st_mtim.tv_nsec, value.st_nlink, value.st_ctim.tv_sec, value.st_ctim.tv_nsec};
            return {0, 0};
        },
        [](void*, int descriptor) noexcept -> Raw {
            const int result = ::fcntl(descriptor, F_GETFL); return {result, result < 0 ? errno : 0};
        },
        [](void*, int descriptor) noexcept -> Raw {
            const auto result = ::fgetxattr(descriptor, "security.capability", nullptr, 0);
            return {result, result < 0 ? errno : 0};
        },
        [](void*, int descriptor, bool enabled) noexcept -> Raw {
            const int flags = ::fcntl(descriptor, F_GETFD);
            if (flags < 0) return {-1, errno};
            const int result = ::fcntl(descriptor, F_SETFD, enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC);
            return {result, result != 0 ? errno : 0};
        },
        [](void*, int descriptor, char* const* arguments) noexcept -> Raw {
            const int result = ::fexecve(descriptor, arguments, environ); return {result, errno};
        },
        [](void*, int descriptor) noexcept -> Raw {
            const int result = ::close(descriptor); return {result, result != 0 ? errno : 0};
        },
        [](void*, HeldMetadata* output) noexcept -> Raw {
            struct stat value{};
            if (::stat("/proc/self/exe", &value) != 0) return {-1, errno};
            *output = {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
                value.st_mode, value.st_uid, value.st_gid, value.st_size, value.st_mtim.tv_sec,
                value.st_mtim.tv_nsec, value.st_nlink, value.st_ctim.tv_sec, value.st_ctim.tv_nsec};
            return {0, 0};
        },
        [](void*, int descriptor) noexcept -> Raw {
            return socket_endpoint(descriptor, SOCK_STREAM);
        },
        [](void*, int descriptor) noexcept -> Raw {
            return socket_endpoint(descriptor, SOCK_SEQPACKET);
        }};
}
} // namespace epycbench::platform_linux::host_work
