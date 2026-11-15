#include "monitoring/host_q1_git_spawn.hpp"
#include "monitoring/host_frozen_source_launch.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr auto blob_invocation = static_cast<MonitoringQ1GitInvocation>(255);
constexpr auto fixed_tree_invocation = static_cast<MonitoringQ1GitInvocation>(254);
constexpr auto q4_historical_invocation = static_cast<MonitoringQ1GitInvocation>(253);
constexpr std::array<std::string_view, 6> q4_historical_paths{
    "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json",
    "schemas/monitoring-q4-frequency-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q4-frequency-evidence-v1.schema.json",
    "schemas/monitoring-host-q4-backend-result-v1.schema.json",
    "schemas/monitoring-host-q4-backend-error-v1.schema.json",
    "schemas/monitoring-host-q4-backend-authored-v1.schema.json"};
constexpr std::uint32_t setup_magic = 0x51473147U;
struct SetupRecord final {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t disposition;
    std::int32_t error;
};
static_assert(sizeof(SetupRecord) == 12);

[[noreturn]] void child_failure(const MonitoringQ1GitSpawnOperations& operations, int setup,
                                std::uint16_t disposition, int error) noexcept {
    const SetupRecord record{setup_magic, 1U, disposition, error > 0 ? error : EIO};
    const char* cursor = reinterpret_cast<const char*>(&record);
    std::size_t remaining = sizeof(record);
    while (remaining != 0) {
        const auto written = operations.write_setup(operations.context, setup, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    operations.exit_child(operations.context, 127);
    _exit(127);
}

int poll_operation(void*, MonitoringQ1GitPollEvent* events, std::size_t count) noexcept {
    std::array<struct pollfd, 3> native{};
    for (std::size_t i = 0; i < count; ++i)
        native[i] = {events[i].descriptor, POLLIN, 0};
    const int result = ::poll(native.data(), static_cast<nfds_t>(count), -1);
    if (result >= 0)
        for (std::size_t i = 0; i < count; ++i)
            events[i].revents = native[i].revents;
    return result;
}
int source_poll_operation(void*, MonitoringQ1GitPollEvent* events, std::size_t count) noexcept {
    std::array<struct pollfd, 3> native{};
    if (count > native.size()) { errno = EINVAL; return -1; }
    for (std::size_t i = 0; i < count; ++i) native[i] = {events[i].descriptor, POLLIN, 0};
    const auto result = ::poll(native.data(), static_cast<nfds_t>(count), 100);
    if (result >= 0)
        for (std::size_t i = 0; i < count; ++i) events[i].revents = native[i].revents;
    return result;
}
long read_operation(void*, int fd, void* bytes, std::size_t size) noexcept {
    return static_cast<long>(::read(fd, bytes, size));
}
int kill_operation(void*, int pid, int signal) noexcept { return ::kill(pid, signal); }
int wait_operation(void*, int pid, int* status) noexcept { return ::waitpid(pid, status, 0); }
[[noreturn]] void await_outer(void*) noexcept {
    for (;;)
        ::pause();
}
int close_operation(void*, int fd) noexcept { return ::close(fd); }
int pipe_operation(void*, int* descriptors, int flags) noexcept {
    return ::pipe2(descriptors, flags);
}
int open_null_operation(void*) noexcept { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
int fork_operation(void*) noexcept { return ::fork(); }
int chdir_operation(void*, int descriptor) noexcept { return ::fchdir(descriptor); }
int duplicate_operation(void*, int source, int destination) noexcept {
    return ::dup2(source, destination);
}
int close_range_operation(void*) noexcept {
#ifdef SYS_close_range
    return static_cast<int>(::syscall(SYS_close_range, 3U, UINT_MAX, CLOSE_RANGE_CLOEXEC));
#else
    errno = ENOSYS;
    return -1;
#endif
}
int execute_operation(void*, int descriptor, char* const* arguments,
                      char* const* environment) noexcept {
    return ::execveat(descriptor, "", arguments, environment, AT_EMPTY_PATH);
}
long write_operation(void*, int descriptor, const void* bytes, std::size_t size) noexcept {
    return static_cast<long>(::write(descriptor, bytes, size));
}
void exit_operation(void*, int status) noexcept { _exit(status); }

struct SpawnOwner final {
    int pid{-1};
    std::array<int, 3> reads{-1, -1, -1};
    MonitoringQ1GitBoundedChildOperations operations{};
    bool active{true};
    ~SpawnOwner() noexcept {
        if (!active)
            return;
        static_cast<void>(operations.kill(operations.context, pid, SIGKILL));
        std::array<bool, 3> open{true, true, true};
        std::array<char, 4096> discard{};
        while (open[0] || open[1] || open[2]) {
            std::array<MonitoringQ1GitPollEvent, 3> events{};
            std::size_t count = 0;
            for (std::size_t i = 0; i < reads.size(); ++i)
                if (open[i])
                    events[count++] = {reads[i], 0};
            if (operations.poll(operations.context, events.data(), count) <= 0)
                break;
            for (std::size_t j = 0; j < count; ++j) {
                std::size_t i = 0;
                while (i < reads.size() && reads[i] != events[j].descriptor)
                    ++i;
                if (i == reads.size())
                    continue;
                if (operations.read(operations.context, reads[i], discard.data(), discard.size()) <=
                    0)
                    open[i] = false;
            }
        }
        int status{};
        for (;;) {
            const int waited = operations.wait(operations.context, pid, &status);
            if (waited == pid)
                break;
            if (waited < 0 && errno == EINTR)
                continue;
            operations.await_outer_termination(operations.context);
            std::terminate();
        }
        for (auto it = reads.rbegin(); it != reads.rend(); ++it)
            if (*it >= 0)
                static_cast<void>(operations.close(operations.context, *it));
    }
};
void adopt(void* context) noexcept { static_cast<SpawnOwner*>(context)->active = false; }

auto spawn_error() -> MonitoringQ1GitCommandFailure {
    return {core::Error{
                .category = core::ErrorCategory::monitoring,
                .code = core::StableErrorCode{"monitoring.q1_plan_authority.repository_rejected"},
                .message = "Q1 Git child setup failed",
                .operation = "authenticate_monitoring_q1_plan",
                .os_error = core::OsError::capture(std::error_code{errno, std::generic_category()}),
                .context = {{"classification", "git_setup"}},
                .severity = core::ErrorSeverity::error,
                .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto identity_error(core::Error error) -> MonitoringQ1GitCommandFailure {
    return {std::move(error), {}};
}
auto valid_fixed_path(const std::string_view path) noexcept -> bool {
    if (path.empty() || path.size() > 511 || path.front() == '/' || path.back() == '/' ||
        path.find('\0') != std::string_view::npos)
        return false;
    std::size_t start{};
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}
auto cleanup_error(int error_number) -> core::Error {
    auto result = spawn_error().primary;
    result.code = core::StableErrorCode{"monitoring.q1_plan_authority.cleanup_failed"};
    result.context.at("classification") = "cleanup_failure";
    result.os_error =
        core::OsError::capture(std::error_code{error_number, std::generic_category()});
    return result;
}
} // namespace

auto MonitoringQ1GitSpawn::run(const MonitoringQ1GitExecutable& executable,
                               const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                               MonitoringQ1GitInvocation invocation)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_with_operations(executable, root, invocation, operations);
}

auto MonitoringQ1GitSpawn::validate_blob_token(const MonitoringQ1GitObjectId& object_id,
                                               const void* binding, std::size_t head_width,
                                               std::size_t slot) noexcept -> bool {
    return object_id.binding_ == binding && object_id.width_ == head_width &&
           object_id.slot_ == slot && object_id.value_.size() == head_width &&
           object_id.stdout_limit_ > 0 && object_id.stdout_limit_ <= (2U << 20U) &&
           std::ranges::all_of(object_id.value_, [](unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

auto MonitoringQ1GitSpawn::run_blob(const MonitoringQ1GitExecutable& executable,
                                    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                                    const MonitoringQ1GitObjectId& object_id, const void* binding,
                                    std::size_t head_width, std::size_t slot)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const bool valid = validate_blob_token(object_id, binding, head_width, slot);
    if (!valid) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_blob_with_operations(executable, root, object_id, binding, head_width, slot,
                                    operations);
}
auto MonitoringQ1GitSpawn::run_historical_tree(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    MonitoringQ1GitInvocation invocation, std::string_view commit)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_with_operations(executable, root, invocation, operations, commit);
}
auto MonitoringQ1GitSpawn::run_q4_historical_tree(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    MonitoringQ4HistoricalSelector selector, std::string_view commit)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const MonitoringQ1GitSpawnOperations operations{
        nullptr, pipe_operation, open_null_operation, fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation, close_operation};
    return run_q4_historical_tree_with_operations(executable, root, selector, commit, operations);
}
auto MonitoringQ1GitSpawn::run_q4_historical_tree_with_operations(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    MonitoringQ4HistoricalSelector selector, std::string_view commit,
    const MonitoringQ1GitSpawnOperations& operations)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const auto index = static_cast<std::size_t>(selector);
    if (index >= q4_historical_paths.size() || (commit.size() != 40U && commit.size() != 64U) ||
        !std::ranges::all_of(commit, [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    return run_with_operations(executable, root, q4_historical_invocation, operations,
                               commit, {}, q4_historical_paths[index]);
}
auto MonitoringQ1GitSpawn::run_historical_ancestry(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root, std::string_view ancestor,
    std::string_view descendant)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_with_operations(executable, root, MonitoringQ1GitInvocation::historical_ancestry,
                               operations, ancestor, descendant);
}
auto MonitoringQ1GitSpawn::run_exact_object_type(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root, std::string_view object)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_with_operations(executable, root, MonitoringQ1GitInvocation::exact_object_type,
                               operations, object);
}
auto MonitoringQ1GitSpawn::run_tree_path(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root, const std::string_view path)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (!valid_fixed_path(path)) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    const MonitoringQ1GitSpawnOperations operations{
        nullptr,         pipe_operation,      open_null_operation,   fork_operation,
        chdir_operation, duplicate_operation, close_range_operation, execute_operation,
        write_operation, exit_operation,      close_operation};
    return run_tree_path_with_operations(executable, root, path, operations);
}
auto MonitoringQ1GitSpawn::validate_fixed_path(const std::string_view path) noexcept -> bool {
    return valid_fixed_path(path);
}
auto MonitoringQ1GitSpawn::run_tree_path_with_operations(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root, const std::string_view path,
    const MonitoringQ1GitSpawnOperations& operations)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (!valid_fixed_path(path)) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    return run_with_operations(executable, root, fixed_tree_invocation, operations, {}, {}, path);
}

auto MonitoringQ1GitSpawn::run_blob_with_operations(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitObjectId& object_id, const void* binding, std::size_t head_width,
    std::size_t slot, const MonitoringQ1GitSpawnOperations& operations)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (!validate_blob_token(object_id, binding, head_width, slot)) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    return run_with_operations(executable, root, blob_invocation, operations, object_id.value_, {},
                               {}, object_id.stdout_limit_);
}

auto MonitoringQ1GitSpawn::build_arguments(MonitoringQ1GitInvocation invocation,
    char* object, char* secondary, char* path) noexcept -> std::array<char*, 15> {
    std::array<char*, 15> argv{};
    argv[0] = const_cast<char*>("git");
    argv[1] = const_cast<char*>("--no-pager");
    argv[2] = const_cast<char*>("-c");
    argv[3] = const_cast<char*>("color.ui=false");
    argv[4] = const_cast<char*>("-c");
    argv[5] = const_cast<char*>("core.fsmonitor=false");
    argv[6] = const_cast<char*>("-c");
    argv[7] = const_cast<char*>("core.untrackedCache=false");
    if (invocation == MonitoringQ1GitInvocation::head_commit) {
        argv[8] = const_cast<char*>("rev-parse");
        argv[9] = const_cast<char*>("--verify");
        argv[10] = const_cast<char*>("HEAD^{commit}");
    } else if (invocation == MonitoringQ1GitInvocation::clean_status) {
        argv[8] = const_cast<char*>("status");
        argv[9] = const_cast<char*>("--porcelain=v1");
        argv[10] = const_cast<char*>("-z");
        argv[11] = const_cast<char*>("--untracked-files=all");
    } else if (invocation == MonitoringQ1GitInvocation::historical_ancestry) {
        argv[8] = const_cast<char*>("merge-base");
        argv[9] = const_cast<char*>("--is-ancestor");
        argv[10] = object;
        argv[11] = secondary;
    } else if (invocation == MonitoringQ1GitInvocation::exact_object_type) {
        argv[8] = const_cast<char*>("cat-file");
        argv[9] = const_cast<char*>("-t");
        argv[10] = object;
    } else if (invocation != blob_invocation) {
        argv[8] = const_cast<char*>("ls-tree");
        argv[9] = const_cast<char*>("-z");
        argv[10] = const_cast<char*>("--full-tree");
        const bool historical =
            invocation == MonitoringQ1GitInvocation::historical_tree_profile ||
            invocation == MonitoringQ1GitInvocation::historical_tree_q1_plan ||
            invocation == MonitoringQ1GitInvocation::historical_tree_q2_plan ||
            invocation == MonitoringQ1GitInvocation::historical_tree_q3_plan ||
            invocation == q4_historical_invocation;
        argv[11] = historical ? object : const_cast<char*>("HEAD");
        argv[12] = const_cast<char*>("--");
        if (invocation == fixed_tree_invocation || invocation == q4_historical_invocation)
            argv[13] = path;
        else if (invocation == MonitoringQ1GitInvocation::tree_policy)
            argv[13] = const_cast<char*>("config/monitoring-qualification/host-v2-policy.json");
        else if (invocation == MonitoringQ1GitInvocation::tree_profile ||
                 invocation == MonitoringQ1GitInvocation::historical_tree_profile)
            argv[13] =
                const_cast<char*>("config/monitoring-qualification/host-v2-profile.json");
        else if (invocation == MonitoringQ1GitInvocation::tree_plan ||
                 invocation == MonitoringQ1GitInvocation::historical_tree_q1_plan)
            argv[13] = const_cast<char*>(
                "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json");
        else if (invocation == MonitoringQ1GitInvocation::tree_q2_plan ||
                 invocation == MonitoringQ1GitInvocation::historical_tree_q2_plan)
            argv[13] = const_cast<char*>("config/monitoring-qualification/"
                                         "host-v2-q2-context-switch-collection-plan.json");
        else
            argv[13] = const_cast<char*>("config/monitoring-qualification/"
                                         "host-v2-q3-temperature-collection-plan.json");
    } else {
        argv[8] = const_cast<char*>("cat-file");
        argv[9] = const_cast<char*>("blob");
        argv[10] = object;
    }
    return argv;
}

auto MonitoringQ1GitSpawn::run_with_operations(
    const MonitoringQ1GitExecutable& executable,
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    MonitoringQ1GitInvocation invocation, const MonitoringQ1GitSpawnOperations& spawn,
    std::string_view blob_id, std::string_view secondary_id, std::string_view fixed_path,
    const std::size_t stdout_limit)
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (blob_id.size() > 64U || secondary_id.size() > 64U || fixed_path.size() > 511U) {
        auto failure = spawn_error();
        failure.primary.os_error.reset();
        return std::unexpected{std::move(failure)};
    }
    std::array<char, 65> blob_storage{};
    std::ranges::copy(blob_id, blob_storage.begin());
    std::array<char, 65> secondary_storage{};
    std::ranges::copy(secondary_id, secondary_storage.begin());
    std::array<char, 512> path_storage{};
    std::ranges::copy(fixed_path, path_storage.begin());
    if (!spawn.pipe || !spawn.open_null || !spawn.fork || !spawn.change_directory ||
        !spawn.duplicate || !spawn.close_range || !spawn.execute || !spawn.write_setup ||
        !spawn.exit_child || !spawn.close_descriptor) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    if (invocation != MonitoringQ1GitInvocation::head_commit &&
        invocation != MonitoringQ1GitInvocation::clean_status &&
        invocation != MonitoringQ1GitInvocation::tree_policy &&
        invocation != MonitoringQ1GitInvocation::tree_profile &&
        invocation != MonitoringQ1GitInvocation::tree_plan &&
        invocation != MonitoringQ1GitInvocation::tree_q2_plan &&
        invocation != MonitoringQ1GitInvocation::tree_q3_plan &&
        !((invocation == MonitoringQ1GitInvocation::historical_tree_profile ||
           invocation == MonitoringQ1GitInvocation::historical_tree_q1_plan ||
           invocation == MonitoringQ1GitInvocation::historical_tree_q2_plan ||
           invocation == MonitoringQ1GitInvocation::historical_tree_q3_plan) &&
          (blob_id.size() == 40 || blob_id.size() == 64)) &&
        !(invocation == MonitoringQ1GitInvocation::historical_ancestry &&
          (blob_id.size() == 40 || blob_id.size() == 64) &&
          secondary_id.size() == blob_id.size()) &&
        !(invocation == MonitoringQ1GitInvocation::exact_object_type &&
          (blob_id.size() == 40 || blob_id.size() == 64)) &&
        !(invocation == blob_invocation && (blob_id.size() == 40 || blob_id.size() == 64)) &&
        !(invocation == fixed_tree_invocation && valid_fixed_path(fixed_path)) &&
        !(invocation == q4_historical_invocation &&
          (blob_id.size() == 40U || blob_id.size() == 64U) &&
          std::ranges::find(q4_historical_paths, fixed_path) != q4_historical_paths.end())) {
        auto error = spawn_error();
        error.primary.os_error.reset();
        return std::unexpected{std::move(error)};
    }
    if (executable.source_launch_ && executable.source_launch_->supervises_git())
        return executable.source_launch_->git(executable, root.descriptor(),
            static_cast<std::uint16_t>(invocation), blob_id, secondary_id, fixed_path, stdout_limit);
    if (executable.source_launch_ && !executable.source_launch_->guard()) {
        errno = ECANCELED;
        return std::unexpected{spawn_error()};
    }
    auto prepared = MonitoringQ1GitBoundedChild::prepare(stdout_limit);
    std::array<std::array<int, 2>, 3> pipes{};
    for (auto& pipe : pipes)
        pipe = {-1, -1};
    int null_fd = -1;
    struct PreForkCleanup final {
        std::array<int, 7> errors{};
        std::size_t count{};
    };
    auto close_setup = [&]() noexcept {
        PreForkCleanup cleanup;
        std::array<int, 7> acquired{pipes[0][0], pipes[0][1], pipes[1][0], pipes[1][1],
                                    pipes[2][0], pipes[2][1], null_fd};
        for (auto it = acquired.rbegin(); it != acquired.rend(); ++it)
            if (*it >= 0 && spawn.close_descriptor(spawn.context, *it) != 0)
                cleanup.errors[cleanup.count++] = errno;
        null_fd = -1;
        for (auto& pipe : pipes)
            pipe = {-1, -1};
        return cleanup;
    };
    auto pre_fork_failure = [&](int primary_error) {
        const auto cleanup_records = close_setup();
        errno = primary_error;
        auto failure = spawn_error();
        failure.primary.context.at("classification") = "git_spawn";
        failure.cleanup.reserve(cleanup_records.count);
        for (std::size_t i = 0; i < cleanup_records.count; ++i)
            failure.cleanup.push_back(cleanup_error(cleanup_records.errors[i]));
        return std::unexpected{std::move(failure)};
    };
    for (auto& pipe : pipes)
        if (spawn.pipe(spawn.context, pipe.data(), O_CLOEXEC) != 0) {
            return pre_fork_failure(errno);
        }
    null_fd = spawn.open_null(spawn.context);
    if (null_fd < 0) {
        return pre_fork_failure(errno);
    }
    const pid_t pid = spawn.fork(spawn.context);
    if (pid < 0) {
        return pre_fork_failure(errno);
    }
    if (pid == 0) {
        if (spawn.change_directory(spawn.context, root.descriptor()) != 0)
            child_failure(spawn, pipes[2][1], 1U, errno);
        if (spawn.duplicate(spawn.context, null_fd, STDIN_FILENO) < 0 ||
            spawn.duplicate(spawn.context, pipes[0][1], STDOUT_FILENO) < 0 ||
            spawn.duplicate(spawn.context, pipes[1][1], STDERR_FILENO) < 0)
            child_failure(spawn, pipes[2][1], 2U, errno);
        if (spawn.close_range(spawn.context) != 0)
            child_failure(spawn, pipes[2][1], 3U, errno);
        auto argv = build_arguments(invocation, blob_storage.data(), secondary_storage.data(), path_storage.data());
        std::array<char*, 21> source_arguments{};
        if (executable.source_launch_) {
            std::copy_n(argv.begin(), 8U, source_arguments.begin());
            constexpr std::array<std::string_view, 6> profile{
                "-c", "core.hooksPath=/dev/null", "-c", "core.preloadIndex=false", "-c", "index.threads=1"};
            for (std::size_t index{}; index < profile.size(); ++index)
                source_arguments[8U + index] = const_cast<char*>(profile[index].data());
            std::copy(argv.begin() + 8, argv.end(), source_arguments.begin() + 14);
        }
        std::array<char*, 12> environment{const_cast<char*>("LC_ALL=C"),
                               const_cast<char*>("GIT_CONFIG_NOSYSTEM=1"),
                               const_cast<char*>("GIT_CONFIG_SYSTEM=/dev/null"),
                               const_cast<char*>("GIT_CONFIG_GLOBAL=/dev/null"),
                               const_cast<char*>("GIT_ATTR_NOSYSTEM=1"),
                               const_cast<char*>("GIT_PAGER=cat"),
                               const_cast<char*>("PAGER=cat"),
                               const_cast<char*>("GIT_TERMINAL_PROMPT=0"),
                               const_cast<char*>("GIT_NO_REPLACE_OBJECTS=1"),
                               nullptr};
        if (executable.source_launch_) {
            environment[9] = const_cast<char*>("GIT_OPTIONAL_LOCKS=0");
            environment[10] = const_cast<char*>("GIT_NO_LAZY_FETCH=1");
        }
        spawn.execute(spawn.context, executable.descriptor_,
            executable.source_launch_ ? source_arguments.data() : argv.data(), environment.data());
        child_failure(spawn, pipes[2][1], 4U, errno);
    }
    auto child_operations = spawn.child_operations;
    if (!child_operations.poll)
        child_operations = {nullptr, executable.source_launch_ ? source_poll_operation : poll_operation, read_operation,
                            kill_operation,  wait_operation, await_outer,
                            close_operation, nullptr,        nullptr};
    // The practical source route uses the same configured Git process owner.
    // Its synchronous polling/read leaves retain the actual source token and
    // absolute fence; consuming signal/wait/close remain available on failure.
    struct SourceChildContext final {
        const HostFrozenSourceLaunchBinding* source;
        MonitoringQ1GitBoundedChildOperations original;
    } source_child{executable.source_launch_, child_operations};
    if (source_child.source) {
        child_operations.context = &source_child;
        child_operations.poll = +[](void* opaque, MonitoringQ1GitPollEvent* events, std::size_t count) noexcept {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            if (!context.source->guard()) { errno = ECANCELED; return -1; }
            const auto result = context.original.poll(context.original.context, events, count);
            if (result >= 0 && !context.source->guard()) { errno = ECANCELED; return -1; }
            return result;
        };
        child_operations.read = +[](void* opaque, int descriptor, void* data, std::size_t count) noexcept -> long {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            if (!context.source->guard()) { errno = ECANCELED; return -1; }
            const auto result = context.original.read(context.original.context, descriptor, data, count);
            if (result >= 0 && !context.source->guard()) { errno = ECANCELED; return -1; }
            return result;
        };
        child_operations.kill = +[](void* opaque, int child_pid, int signal) noexcept {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            return context.original.kill(context.original.context, child_pid, signal);
        };
        child_operations.wait = +[](void* opaque, int child_pid, int* status) noexcept {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            return context.original.wait(context.original.context, child_pid, status);
        };
        child_operations.await_outer_termination = +[](void* opaque) noexcept {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            context.original.await_outer_termination(context.original.context);
        };
        child_operations.close = +[](void* opaque, int descriptor) noexcept {
            auto& context = *static_cast<SourceChildContext*>(opaque);
            return context.original.close(context.original.context, descriptor);
        };
    }
    SpawnOwner owner{pid, {pipes[0][0], pipes[1][0], pipes[2][0]}, child_operations, true};
    MonitoringQ1GitInitialCloseFailures parent_close_errors;
    for (auto& pipe : pipes) {
        if (spawn.close_descriptor(spawn.context, pipe[1]) != 0)
            parent_close_errors.errors[parent_close_errors.count++] = errno;
        pipe[1] = -1;
    }
    if (spawn.close_descriptor(spawn.context, null_fd) != 0)
        parent_close_errors.errors[parent_close_errors.count++] = errno;
    null_fd = -1;
    child_operations.adopt = adopt;
    child_operations.adopt_context = &owner;
    auto result = MonitoringQ1GitBoundedChild::collect(
        std::move(prepared), {pid, owner.reads[0], owner.reads[1], owner.reads[2]},
        child_operations, parent_close_errors);
    auto rechecked = executable.recheck();
    if (!rechecked) {
        if (!result) {
            result.error().cleanup.push_back(std::move(rechecked.error()));
            return result;
        }
        return std::unexpected{identity_error(std::move(rechecked.error()))};
    }
    if (!*rechecked) {
        auto error = spawn_error();
        error.primary.context.at("classification") = "git_identity";
        error.primary.os_error.reset();
        if (!result) {
            result.error().cleanup.push_back(std::move(error.primary));
            return result;
        }
        return std::unexpected{std::move(error)};
    }
    if (!result)
        return result;
    return result;
}

} // namespace epycbench::monitoring::qualification::detail
