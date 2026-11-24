#include "platform_linux/experiment_operations.hpp"

#include "platform_linux/process_ipc_operations.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace epycbench::platform_linux::experiment_operations {
namespace {

constexpr char pending_name[] = ".epycbench-pending";
constexpr char run_owner_name[] = ".epycbench-run-owner";
constexpr std::string_view runtime_variable = "XDG_RUNTIME_DIR=";
constexpr std::size_t max_path = 4095;
constexpr std::size_t transfer_bytes = 64U * 1024U;

auto failure(std::string_view operation, std::string_view message, int native_error)
    -> std::unexpected<core::Error> {
    std::optional<core::OsError> os_error;
    if (native_error) {
        os_error = core::OsError::capture({native_error, std::generic_category()});
    }
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{"linux.experiment." + std::string{operation}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
    }};
}

auto valid_leaf(std::string_view name) noexcept -> bool {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
           name.find('/') == name.npos && name.find('\0') == name.npos;
}

auto public_leaf(std::string_view name) noexcept -> bool {
    return valid_leaf(name) && name != pending_name && name != run_owner_name;
}

auto absolute_path(const std::filesystem::path& path)
    -> std::expected<std::filesystem::path, core::Error> {
    std::error_code error;
    auto result = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        return failure("path", "Cannot normalize the experiment path", error.value());
    }
    if (result.native().size() > max_path || result.native().find('\0') != std::string::npos) {
        return failure("path", "Invalid experiment path", EINVAL);
    }
    return result;
}

auto opened(int descriptor, std::string_view operation) -> std::expected<FileDescriptor, core::Error> {
    if (descriptor < 0) {
        return failure(operation, "Cannot open experiment storage", errno);
    }
    return FileDescriptor::adopt(descriptor);
}

constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr int read_flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;

auto open_absolute_directory(const std::filesystem::path& path)
    -> std::expected<FileDescriptor, core::Error> {
    auto current = opened(::open("/", directory_flags), "open_directory");
    if (!current) return current;
    for (const auto& part : path.relative_path()) {
        const auto& name = part.native();
        if (name.empty()) continue;
        if (!valid_leaf(name)) {
            return failure("open_directory", "Invalid experiment directory component", EINVAL);
        }
        auto next = opened(::openat(current->native_handle(), name.c_str(), directory_flags),
                           "open_directory");
        if (!next) return next;
        current = std::move(next);
    }
    return current;
}

auto private_directory(int descriptor) -> std::expected<void, core::Error> {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return failure("directory_metadata", "Cannot inspect the experiment directory", errno);
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777U) != 0700U) {
        return failure("directory_metadata", "Experiment directory must be owned and mode 0700",
                       EACCES);
    }
    return {};
}

auto private_file(int descriptor) -> std::expected<struct stat, core::Error> {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        return failure("file_metadata", "Cannot inspect the experiment file", errno);
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & 07777U) != 0600U) {
        return failure("file_metadata", "Experiment file must be owned, unshared and mode 0600",
                       EACCES);
    }
    return status;
}

auto lock(int descriptor, int flags) noexcept -> int {
    int result;
    do {
        result = ::flock(descriptor, flags);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? 0 : errno;
}

auto sync(int descriptor) noexcept -> int {
    int result;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? 0 : errno;
}

auto check_previous_run_owner(int directory) -> std::expected<void, core::Error> {
    const int descriptor = ::openat(directory, run_owner_name, read_flags);
    if (descriptor < 0 && errno == ENOENT) return {};
    auto lease = opened(descriptor, "open_run_lease");
    if (!lease) return std::unexpected{std::move(lease.error())};
    auto checked = private_file(lease->native_handle());
    if (!checked) return std::unexpected{std::move(checked.error())};
    if (const int error = lock(lease->native_handle(), LOCK_EX | LOCK_NB); error != 0) {
        return failure("lock_run_lease", "A previous Run may still hold the experiment lease",
                       error);
    }
    return lease->close();
}

struct PendingFile {
    int directory;
    bool exists{true};
    ~PendingFile() {
        if (exists) static_cast<void>(::unlinkat(directory, pending_name, 0));
    }
};

auto same_snapshot(const struct stat& initial, const struct stat& current) noexcept -> bool {
    return initial.st_dev == current.st_dev && initial.st_ino == current.st_ino &&
           initial.st_size == current.st_size && initial.st_mtim.tv_sec == current.st_mtim.tv_sec &&
           initial.st_mtim.tv_nsec == current.st_mtim.tv_nsec &&
           initial.st_ctim.tv_sec == current.st_ctim.tv_sec &&
           initial.st_ctim.tv_nsec == current.st_ctim.tv_nsec;
}

auto verify_named_directory(int descriptor, const std::filesystem::path& path)
    -> std::expected<void, core::Error> {
    auto named = open_absolute_directory(path);
    if (!named) return std::unexpected{std::move(named.error())};
    struct stat opened_status {};
    struct stat named_status {};
    if (::fstat(descriptor, &opened_status) != 0 || ::fstat(named->native_handle(), &named_status) != 0) {
        return failure("directory_metadata", "Cannot compare the held and named directories", errno);
    }
    if (opened_status.st_dev != named_status.st_dev || opened_status.st_ino != named_status.st_ino) {
        return failure("directory_metadata", "Experiment path no longer names its held directory",
                       EIO);
    }
    return private_directory(descriptor);
}

struct SpawnActions {
    posix_spawn_file_actions_t value{};
    bool initialized{};
    ~SpawnActions() {
        if (initialized) static_cast<void>(::posix_spawn_file_actions_destroy(&value));
    }
};

struct SpawnAttributes {
    posix_spawnattr_t value{};
    bool initialized{};
    ~SpawnAttributes() {
        if (initialized) static_cast<void>(::posix_spawnattr_destroy(&value));
    }
};

auto requested(Cancellation cancellation) noexcept -> bool {
    return cancellation.requested && cancellation.requested(cancellation.context);
}

struct WaitReport {
    int status{};
    int error{};
    int signal_error{};
    bool reaped{};
    bool cancelled{};
};

auto wait_for_run(pid_t pid, Cancellation cancellation) noexcept -> WaitReport {
    WaitReport result;
    const auto send_terminate = [&] {
        if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) result.signal_error = errno;
    };

    // Block across the cancellation check and atomically unblock in ppoll so
    // an arriving signal can wake the coordinator without a periodic timer.
    sigset_t cancellation_signals{};
    sigset_t previous{};
    ::sigemptyset(&cancellation_signals);
    ::sigaddset(&cancellation_signals, SIGINT);
    ::sigaddset(&cancellation_signals, SIGTERM);
    const auto mask_error = ::pthread_sigmask(SIG_BLOCK, &cancellation_signals, &previous);
    const auto raw_pidfd = ::syscall(SYS_pidfd_open, pid, 0U);
    if (mask_error != 0) result.error = mask_error;
    if (raw_pidfd < 0 && result.error == 0) result.error = errno;
    if (result.error != 0) send_terminate();
    if (raw_pidfd >= 0) {
        const auto pidfd = static_cast<int>(raw_pidfd);
        struct pollfd event {pidfd, POLLIN, 0};
        while (result.error == 0) {
            if (!result.cancelled && requested(cancellation)) {
                result.cancelled = true;
                send_terminate();
            }
            if (::ppoll(&event, 1, nullptr, &previous) > 0) {
                if ((event.revents & POLLIN) == 0) result.error = EIO;
                break;
            }
            if (errno != EINTR) result.error = errno;
        }
        static_cast<void>(::close(pidfd));
        if (result.error != 0 && mask_error == 0) send_terminate();
    }
    if (mask_error == 0) {
        const auto restored = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (restored != 0 && result.error == 0) result.error = restored;
    }

    pid_t waited;
    do {
        waited = ::waitpid(pid, &result.status, 0);
    } while (waited < 0 && errno == EINTR);
    result.reaped = waited == pid;
    if (!result.reaped && result.error == 0) result.error = waited < 0 ? errno : ECHILD;
    return result;
}

} // namespace

Directory::Directory(std::filesystem::path path, FileDescriptor descriptor) noexcept
    : path_{std::move(path)}, descriptor_{std::move(descriptor)} {}

auto Directory::acquire(const std::filesystem::path& path)
    -> std::expected<Directory, core::Error> {
    auto absolute = absolute_path(path);
    if (!absolute) return std::unexpected{std::move(absolute.error())};
    auto descriptor = open_absolute_directory(*absolute);
    if (!descriptor) return std::unexpected{std::move(descriptor.error())};
    const int fd = descriptor->native_handle();
    auto checked = private_directory(fd);
    if (!checked) return std::unexpected{std::move(checked.error())};
    if (const int error = lock(fd, LOCK_EX | LOCK_NB); error != 0) {
        return failure("lock_directory", "Cannot acquire exclusive experiment directory ownership",
                       error);
    }
    auto idle = check_previous_run_owner(fd);
    if (!idle) return std::unexpected{std::move(idle.error())};
    return Directory{std::move(*absolute), std::move(*descriptor)};
}

auto Directory::write_new(std::string_view name, std::string_view bytes,
                          std::size_t max_bytes) const -> std::expected<void, core::Error> {
    if (!public_leaf(name) || bytes.size() > max_bytes) {
        return failure("write_file", "Invalid or oversized experiment file", EINVAL);
    }
    const std::string leaf{name};
    const int directory = descriptor_.native_handle();
    if (::unlinkat(directory, pending_name, 0) != 0 && errno != ENOENT) {
        return failure("remove_pending", "Cannot clear an unpublished experiment file", errno);
    }
    constexpr int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    auto opened_file = opened(::openat(directory, pending_name, flags, 0600), "create_file");
    if (!opened_file) return std::unexpected{std::move(opened_file.error())};
    PendingFile pending{directory};
    const int fd = opened_file->native_handle();
    if (::fchmod(fd, 0600) != 0) {
        return failure("file_metadata", "Cannot set the experiment file mode", errno);
    }
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(bytes.size() - offset, transfer_bytes);
        const auto written = ::write(fd, bytes.data() + offset, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return failure("write_file", "Cannot write the experiment file",
                                         written == 0 ? EIO : errno);
        offset += static_cast<std::size_t>(written);
    }
    if (const int error = sync(fd); error != 0) {
        return failure("sync_file", "Cannot persist the experiment file contents", error);
    }
    auto closed = opened_file->close();
    if (!closed) return closed;
    if (::syscall(SYS_renameat2, directory, pending_name, directory, leaf.c_str(),
                  RENAME_NOREPLACE) != 0) {
        return failure("publish_file", "Cannot publish a new experiment file", errno);
    }
    pending.exists = false;
    if (const int error = sync(directory); error != 0) {
        return failure("sync_directory", "Experiment file is published with uncertain durability",
                       error);
    }
    return {};
}

auto Directory::read(std::string_view name, std::size_t max_bytes) const
    -> std::expected<std::string, core::Error> {
    if (!public_leaf(name)) {
        return failure("read_file", "Invalid experiment filename", EINVAL);
    }
    const std::string leaf{name};
    auto file = opened(::openat(descriptor_.native_handle(), leaf.c_str(), read_flags), "open_file");
    if (!file) return std::unexpected{std::move(file.error())};
    const int fd = file->native_handle();
    auto initial = private_file(fd);
    if (!initial) return std::unexpected{std::move(initial.error())};
    if (initial->st_size < 0 || std::cmp_greater(initial->st_size, max_bytes)) {
        return failure("read_file", "Experiment file exceeds its size bound", EFBIG);
    }
    std::string bytes(static_cast<std::size_t>(initial->st_size), '\0');
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(bytes.size() - offset, transfer_bytes);
        const auto length = ::read(fd, bytes.data() + offset, count);
        if (length < 0 && errno == EINTR) continue;
        if (length <= 0) return failure("read_file", "Cannot read the complete experiment file",
                                       length == 0 ? EIO : errno);
        offset += static_cast<std::size_t>(length);
    }
    auto current = private_file(fd);
    if (!current) return std::unexpected{std::move(current.error())};
    if (!same_snapshot(*initial, *current)) {
        return failure("read_file", "Experiment file changed during reading", EIO);
    }
    auto closed = file->close();
    if (!closed) return std::unexpected{std::move(closed.error())};
    return bytes;
}

auto Directory::sync_file(std::string_view name) const -> std::expected<void, core::Error> {
    if (!public_leaf(name)) {
        return failure("sync_file", "Invalid experiment filename", EINVAL);
    }
    const std::string leaf{name};
    const int directory = descriptor_.native_handle();
    auto file = opened(::openat(directory, leaf.c_str(), read_flags), "open_file");
    if (!file) return std::unexpected{std::move(file.error())};
    const int fd = file->native_handle();
    auto checked = private_file(fd);
    if (!checked) return std::unexpected{std::move(checked.error())};
    if (const int error = sync(fd); error != 0) {
        return failure("sync_file", "Cannot persist the experiment file contents", error);
    }
    auto closed = file->close();
    if (!closed) return closed;
    if (const int error = sync(directory); error != 0) {
        return failure("sync_directory", "Cannot persist the experiment file location", error);
    }
    return {};
}

auto Directory::list(std::size_t max_entries) const
    -> std::expected<std::vector<std::string>, core::Error> {
    const int descriptor = ::openat(descriptor_.native_handle(), ".", directory_flags);
    if (descriptor < 0) return failure("list_files", "Cannot open experiment directory", errno);
    std::unique_ptr<DIR, int (*)(DIR*)> stream{::fdopendir(descriptor), ::closedir};
    if (!stream) {
        const int error = errno;
        static_cast<void>(::close(descriptor));
        return failure("list_files", "Cannot open experiment directory stream", error);
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (!entry) {
            if (errno != 0) return failure("list_files", "Cannot list experiment files", errno);
            break;
        }
        const std::string_view name{entry->d_name};
        if (!public_leaf(name)) continue;
        if (names.size() == max_entries) {
            return failure("list_files", "Experiment directory exceeds its entry bound", EFBIG);
        }
        names.emplace_back(name);
    }
    if (::closedir(stream.release()) != 0) {
        return failure("list_files", "Cannot close experiment directory stream", errno);
    }
    std::ranges::sort(names);
    return names;
}

auto Directory::create_subdirectory(std::string_view name) const
    -> std::expected<std::filesystem::path, core::Error> {
    if (!public_leaf(name)) return failure("create_directory", "Invalid subdirectory name", EINVAL);
    const std::string leaf{name};
    auto path = path_ / leaf;
    if (path.native().size() > max_path) {
        return failure("create_directory", "Experiment subdirectory path is too long", ENAMETOOLONG);
    }
    const int directory = descriptor_.native_handle();
    if (::mkdirat(directory, leaf.c_str(), 0700) != 0) {
        return failure("create_directory", "Cannot create a new private subdirectory", errno);
    }
    if (const int error = sync(directory); error != 0) {
        return failure("sync_directory", "New subdirectory has uncertain durability", error);
    }
    return path;
}

auto run(const Directory& directory, const std::filesystem::path& scenario,
         std::string_view expected_plan_digest,
         const std::optional<std::filesystem::path>& module_directory,
         const std::filesystem::path& runtime_directory, Cancellation cancellation,
         const std::optional<std::filesystem::path>& supplied_workers)
    -> std::expected<RunExit, core::Error> {
    if (requested(cancellation)) return RunExit{.exit_code = {}, .signal = {}, .cancelled = true};
    auto scenario_path = absolute_path(scenario);
    if (!scenario_path) return std::unexpected{std::move(scenario_path.error())};
    auto runtime_path = absolute_path(runtime_directory);
    if (!runtime_path) return std::unexpected{std::move(runtime_path.error())};
    if (!expected_plan_digest.starts_with("sha256:") || expected_plan_digest.size() != 71 ||
        expected_plan_digest.find_first_not_of("0123456789abcdef", 7) != std::string_view::npos) {
        return failure("run_arguments", "Invalid expected plan digest", EINVAL);
    }
    auto runtime_owner = open_absolute_directory(*runtime_path);
    if (!runtime_owner) return std::unexpected{std::move(runtime_owner.error())};
    auto checked = private_directory(runtime_owner->native_handle());
    if (!checked) return std::unexpected{std::move(checked.error())};

    std::vector<std::string> words{"epycbench", "run", "--expect-plan", std::string{expected_plan_digest}};
    if (module_directory) {
        auto modules_path = absolute_path(*module_directory);
        if (!modules_path) return std::unexpected{std::move(modules_path.error())};
        words.emplace_back("--module-dir");
        words.emplace_back(modules_path->native());
    }
    if (supplied_workers) {
        auto fixture = absolute_path(*supplied_workers);
        if (!fixture) return std::unexpected{std::move(fixture.error())};
        words.emplace_back("--supplied-workers");
        words.emplace_back(fixture->native());
    }
    words.emplace_back(scenario_path->native());
    std::vector<char*> arguments;
    arguments.reserve(words.size() + 1);
    for (auto& word : words) arguments.push_back(word.data());
    arguments.push_back(nullptr);
    std::string runtime_environment = std::string{runtime_variable} + runtime_path->native();
    std::vector<char*> environment;
    for (auto** variable = environ; variable && *variable; ++variable) {
        if (!std::string_view{*variable}.starts_with(runtime_variable)) {
            environment.push_back(*variable);
        }
    }
    environment.push_back(runtime_environment.data());
    environment.push_back(nullptr);

    struct sigaction child_signals {};
    if (::sigaction(SIGCHLD, nullptr, &child_signals) != 0) {
        return failure("run_signals", "Cannot inspect Run child signal disposition", errno);
    }
    if (child_signals.sa_handler != SIG_DFL || (child_signals.sa_flags & SA_NOCLDWAIT) != 0) {
        return failure("run_signals", "Run requires exclusive child retirement observation", EINVAL);
    }

    const int directory_fd = directory.descriptor_.native_handle();
    auto named = verify_named_directory(directory_fd, directory.path());
    if (!named) return std::unexpected{std::move(named.error())};
    constexpr int lease_flags = O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    int lease_descriptor = ::openat(directory_fd, run_owner_name, lease_flags | O_CREAT | O_EXCL,
                                    0600);
    const bool new_lease = lease_descriptor >= 0;
    if (!new_lease && errno == EEXIST) {
        lease_descriptor = ::openat(directory_fd, run_owner_name, lease_flags);
    }
    auto lease = opened(lease_descriptor, "open_run_lease");
    if (!lease) return std::unexpected{std::move(lease.error())};
    if (new_lease && ::fchmod(lease->native_handle(), 0600) != 0) {
        return failure("run_lease_metadata", "Cannot set the Run lease mode", errno);
    }
    auto lease_identity = private_file(lease->native_handle());
    if (!lease_identity) return std::unexpected{std::move(lease_identity.error())};
    if (const int error = lock(lease->native_handle(), LOCK_EX | LOCK_NB); error != 0) {
        return failure("lock_run_lease", "Cannot admit a Run while its lease is busy", error);
    }
    auto retirement = opened(::openat(directory_fd, run_owner_name, read_flags), "open_run_lease");
    if (!retirement) return std::unexpected{std::move(retirement.error())};
    auto retirement_identity = private_file(retirement->native_handle());
    if (!retirement_identity) return std::unexpected{std::move(retirement_identity.error())};
    if (!same_snapshot(*lease_identity, *retirement_identity)) {
        return failure("run_lease_metadata", "Run lease changed while opening its observer", EIO);
    }
    if (const int error = sync(directory_fd); error != 0) {
        return failure("sync_run_lease", "Cannot persist the Run lease location", error);
    }

    SpawnActions actions;
    int error = ::posix_spawn_file_actions_init(&actions.value);
    if (error != 0) return failure("run_spawn_setup", "Cannot initialize Run file actions", error);
    actions.initialized = true;
    // The ordinary Run and its worker do not read stdin. Their copy of this
    // lease survives both execs and keeps restart admission closed until they
    // have finished, even when either owning parent terminates unexpectedly.
    error = ::posix_spawn_file_actions_adddup2(&actions.value, lease->native_handle(), STDIN_FILENO);
    if (error == 0) error = ::posix_spawn_file_actions_addclosefrom_np(&actions.value, 3);
    if (error != 0) return failure("run_spawn_setup", "Cannot arrange child lease inheritance", error);

    SpawnAttributes attributes;
    error = ::posix_spawnattr_init(&attributes.value);
    if (error != 0) return failure("run_spawn_setup", "Cannot initialize Run spawn attributes", error);
    attributes.initialized = true;
    error = ::posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP);
    if (error == 0) error = ::posix_spawnattr_setpgroup(&attributes.value, 0);
    if (error != 0) return failure("run_spawn_setup", "Cannot isolate the child signal group", error);

    if (requested(cancellation)) return RunExit{.exit_code = {}, .signal = {}, .cancelled = true};
    pid_t pid{};
    error = ::posix_spawn(&pid, "/proc/self/exe", &actions.value, &attributes.value,
                          arguments.data(), environment.data());
    if (error != 0) return failure("run_spawn", "Cannot start the ordinary Run command", error);
    if (pid <= 0) return failure("run_spawn", "Run started without a valid process identity", ECHILD);
    const auto closed = detail::DescriptorAccess::close_raw(*lease);
    const auto waited = wait_for_run(pid, cancellation);
    const auto lease_error = lock(retirement->native_handle(), LOCK_EX);
    if (waited.error != 0 || !waited.reaped) {
        return failure("run_wait", "Cannot establish the exact Run exit status", waited.error);
    }
    if (lease_error != 0) {
        return failure("run_retirement", "Cannot establish retirement of the Run lease", lease_error);
    }
    if (closed.error_number != 0) {
        return failure("run_retirement", "Cannot close the coordinator's Run lease copy",
                       closed.error_number);
    }
    if (waited.signal_error != 0) {
        return failure("run_signal", "Cannot forward cancellation to the Run owner",
                       waited.signal_error);
    }
    if (WIFEXITED(waited.status)) {
        return RunExit{.exit_code = WEXITSTATUS(waited.status), .signal = {},
                       .cancelled = waited.cancelled};
    }
    if (WIFSIGNALED(waited.status)) {
        return RunExit{.exit_code = {}, .signal = WTERMSIG(waited.status),
                       .cancelled = waited.cancelled};
    }
    return failure("run_wait", "Unexpected terminal Run status", ECHILD);
}

} // namespace epycbench::platform_linux::experiment_operations
