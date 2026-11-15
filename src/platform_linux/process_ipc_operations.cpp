#include "platform_linux/process_ipc_operations.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace epycbench::platform_linux::detail {
namespace {

auto native_image_pid(void*) noexcept -> RawStatusResult { return {::getpid(), 0}; }
auto native_image_open(void*, const char* path, int flags) noexcept -> RawStatusResult {
    const int result = ::open(path, flags);
    return {result, result < 0 ? errno : 0};
}
auto native_image_stat(void*, int fd, ImageMetadata* output) noexcept -> RawStatusResult {
    struct stat value{};
    const int result = ::fstat(fd, &value);
    if (result == 0) {
        *output = {static_cast<std::uint64_t>(value.st_dev),
                   static_cast<std::uint64_t>(value.st_ino),
                   static_cast<std::uint64_t>(value.st_mode),
                   static_cast<std::uint64_t>(value.st_uid),
                   static_cast<std::uint64_t>(value.st_gid),
                   static_cast<std::int64_t>(value.st_size),
                   static_cast<std::int64_t>(value.st_mtim.tv_sec),
                   static_cast<std::int64_t>(value.st_mtim.tv_nsec),
                   static_cast<std::uint64_t>(value.st_nlink),
                   static_cast<std::int64_t>(value.st_ctim.tv_sec),
                   static_cast<std::int64_t>(value.st_ctim.tv_nsec)};
    }
    return {result, result < 0 ? errno : 0};
}

auto native_close(void*, int descriptor) noexcept -> RawStatusResult {
    const auto value = ::close(descriptor);
    return {value, value == -1 ? errno : 0};
}

auto native_duplicate(void*, int descriptor, int minimum) noexcept -> RawStatusResult {
    const auto value = ::fcntl(descriptor, F_DUPFD_CLOEXEC, minimum);
    return {value, value == -1 ? errno : 0};
}

auto native_socket_pair(void*, int domain, int type, int protocol,
                        int descriptors[2]) noexcept -> RawStatusResult {
    const auto value = ::socketpair(domain, type, protocol, descriptors);
    return {value, value == -1 ? errno : 0};
}

auto native_receive(void*, int descriptor, void* buffer, std::size_t size,
                    int flags) noexcept -> RawStatusResult {
    const auto value = ::recv(descriptor, buffer, size, flags);
    return {value, value == -1 ? errno : 0};
}

auto native_send(void*, int descriptor, const void* buffer, std::size_t size,
                 int flags) noexcept -> RawStatusResult {
    const auto value = ::send(descriptor, buffer, size, flags);
    return {value, value == -1 ? errno : 0};
}

auto native_poll(void*, int descriptor, short events, int timeout,
                 short* returned_events) noexcept -> RawStatusResult {
    pollfd item{.fd = descriptor, .events = events, .revents = 0};
    const auto value = ::poll(&item, 1, timeout);
    *returned_events = item.revents;
    return {value, value == -1 ? errno : 0};
}

auto native_shutdown(void*, int descriptor, int how) noexcept -> RawStatusResult {
    const auto value = ::shutdown(descriptor, how);
    return {value, value == -1 ? errno : 0};
}

auto native_spawn_initialize(void*, void** actions) noexcept -> RawSpawnResult {
    auto* storage = new (std::nothrow) posix_spawn_file_actions_t;
    if (storage == nullptr) {
        return {ENOMEM, ENOMEM};
    }
    const auto value = ::posix_spawn_file_actions_init(storage);
    if (value != 0) {
        delete storage;
        return {value, value};
    }
    *actions = storage;
    return {0, 0};
}

auto native_spawn_add_dup2(void*, void* actions, int source, int destination) noexcept
    -> RawSpawnResult {
    const auto value = ::posix_spawn_file_actions_adddup2(
        static_cast<posix_spawn_file_actions_t*>(actions), source, destination);
    return {value, value};
}

auto native_spawn_add_close_from(void*, void* actions, int first) noexcept -> RawSpawnResult {
    const auto value = ::posix_spawn_file_actions_addclosefrom_np(
        static_cast<posix_spawn_file_actions_t*>(actions), first);
    return {value, value};
}

auto native_spawn_destroy(void*, void* actions) noexcept -> RawSpawnResult {
    auto* native = static_cast<posix_spawn_file_actions_t*>(actions);
    const auto value = ::posix_spawn_file_actions_destroy(native);
    delete native;
    return {value, value};
}

auto native_spawn(void*, int* pid, const char* executable, void* actions,
                  char* const* arguments, char* const* environment) noexcept -> RawSpawnResult {
    pid_t native_pid{};
    const auto value = ::posix_spawn(&native_pid, executable,
                                     static_cast<posix_spawn_file_actions_t*>(actions), nullptr,
                                     arguments, environment);
    if (value == 0) {
        *pid = native_pid;
    }
    return {value, value};
}

auto native_wait_pid(void*, int pid, int* status, int options) noexcept -> RawStatusResult {
    const auto value = ::waitpid(static_cast<pid_t>(pid), status, options);
    return {value, value == -1 ? errno : 0};
}

auto native_signal(void*, int pid, int signal) noexcept -> RawStatusResult {
    const auto value = ::kill(static_cast<pid_t>(pid), signal);
    return {value, value == -1 ? errno : 0};
}

} // namespace

auto production_image_operations() noexcept -> ImageOperations {
    return {nullptr, native_image_pid, native_image_open, native_image_stat};
}
auto production_descriptor_operations() noexcept -> DescriptorOperations {
    return {nullptr, native_close, native_duplicate};
}
auto production_ipc_operations() noexcept -> IpcOperations {
    return {nullptr, native_socket_pair, native_receive, native_send, native_poll,
            native_shutdown};
}
auto production_spawn_operations() noexcept -> SpawnOperations {
    return {nullptr, native_spawn_initialize, native_spawn_add_dup2,
            native_spawn_add_close_from, native_spawn_destroy, native_spawn};
}
auto production_process_operations() noexcept -> ProcessOperations {
    return {nullptr, native_wait_pid, native_signal};
}

} // namespace epycbench::platform_linux::detail
