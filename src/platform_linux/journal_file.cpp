#include "platform_linux/journal_file.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::journal_file {
namespace detail {
namespace {
auto native_open(void*, int parent, const char* name, int flags, unsigned mode) noexcept -> Raw {
    const auto value = ::openat(parent, name, flags, static_cast<mode_t>(mode));
    return {value, value < 0 ? errno : 0};
}
auto native_stat(void*, int descriptor, Metadata* out) noexcept -> Raw {
    struct stat value{};
    const auto result = ::fstat(descriptor, &value);
    if (result < 0)
        return {result, errno};
    *out = {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_uid),
            static_cast<std::uint64_t>(value.st_nlink),
            static_cast<std::int64_t>(value.st_size),
            static_cast<std::int64_t>(value.st_mtim.tv_sec),
            static_cast<std::int64_t>(value.st_mtim.tv_nsec),
            static_cast<std::int64_t>(value.st_ctim.tv_sec),
            static_cast<std::int64_t>(value.st_ctim.tv_nsec),
            static_cast<std::uint32_t>(value.st_mode & 07777U),
            S_ISREG(value.st_mode),
            S_ISDIR(value.st_mode)};
    return {0, 0};
}
auto native_read(void*, int fd, void* data, std::size_t size) noexcept -> Raw {
    const auto v = ::read(fd, data, size);
    return {v, v < 0 ? errno : 0};
}
auto native_write(void*, int fd, const void* data, std::size_t size) noexcept -> Raw {
    const auto v = ::write(fd, data, size);
    return {v, v < 0 ? errno : 0};
}
auto native_sync(void*, int fd) noexcept -> Raw {
    const auto v = ::fsync(fd);
    return {v, v < 0 ? errno : 0};
}
auto native_close(void*, int fd) noexcept -> Raw {
    const auto v = ::close(fd);
    return {v, v < 0 ? errno : 0};
}
auto native_uid(void*) noexcept -> Raw { return {static_cast<long>(::geteuid()), 0}; }
auto native_rename(void*, int directory, const char* source, const char* destination) noexcept
    -> Raw {
    const auto value =
        ::syscall(SYS_renameat2, directory, source, directory, destination, RENAME_NOREPLACE);
    return {value, value < 0 ? errno : 0};
}
} // namespace
auto production_operations() noexcept -> Operations {
    return {nullptr,      native_open, native_stat,  native_read,
            native_write, native_sync, native_close, native_uid, native_rename};
}
} // namespace detail
struct File::State {
    Limits limits;
    detail::Operations ops;
    std::optional<FileDescriptor> directory, file;
    detail::Metadata snapshot;
    Progress progress;
    CloseReport closed;
    std::optional<Issue> failure;
    std::unique_ptr<char[]> leaf;
    std::array<char, 256> destination{};
    bool rename_attempted{};
};
namespace {
auto fail(File::State& s, Phase p, Reason r, int number = 0) -> Failure {
    s.progress.live = false;
    if (!s.failure)
        s.failure = Issue{p, r, number};
    return {*s.failure, {}, s.progress.created};
}
auto issue(Phase p, Reason r) -> Failure { return {{p, r, 0}, {}, false}; }
auto charge(File::State& s, Phase phase, bool interrupted = false) -> bool {
    if (s.progress.operations >= s.limits.operations) {
        (void)fail(s, phase, interrupted ? Reason::interrupted_budget : Reason::capacity);
        return false;
    }
    ++s.progress.operations;
    return true;
}
auto invoke(File::State& s, Phase phase, const auto& call) -> detail::Raw {
    bool interrupted{};
    while (charge(s, phase, interrupted)) {
        const auto result = call();
        if (result.value == -1 && result.error_number == EINTR) {
            interrupted = true;
            continue;
        }
        return result;
    }
    return {-1, 0};
}
auto close_state(File::State& s) noexcept -> CloseReport {
    for (auto* value : {&s.file, &s.directory})
        if (*value && (*value)->active()) {
            ++s.closed.attempts;
            const auto raw = platform_linux::detail::DescriptorAccess::close_raw(**value);
            if (raw.value != 0 || raw.error_number != 0)
                s.closed.issues[s.closed.count++] = {Phase::close, Reason::io, raw.error_number};
        }
    s.progress.live = false;
    return s.closed;
}
auto stop(File::State& s, Failure f) -> std::expected<File, Failure> {
    f.cleanup = close_state(s);
    f.created = s.progress.created;
    return std::unexpected{f};
}
auto admitted_fd(File::State& s, long raw) -> std::optional<FileDescriptor> {
    if (raw < 0 || raw > std::numeric_limits<int>::max())
        return {};
    const platform_linux::detail::DescriptorOperations operations{s.ops.context, s.ops.close,
                                                                  nullptr};
    auto fd = platform_linux::detail::DescriptorAccess::adopt(static_cast<int>(raw), operations);
    if (!fd)
        return {};
    return std::move(*fd);
}
auto status(File::State& s, Phase phase, int fd, detail::Metadata& metadata)
    -> std::expected<void, Failure> {
    const auto raw = invoke(s, phase, [&] { return s.ops.stat(s.ops.context, fd, &metadata); });
    if (s.failure)
        return std::unexpected{fail(s, phase, Reason::capacity)};
    if (raw.value != 0 || raw.error_number)
        return std::unexpected{fail(s, phase, Reason::io, raw.error_number)};
    return {};
}
} // namespace
File::File(std::unique_ptr<State> value) noexcept : state_(std::move(value)) {}
File::~File() = default;
File::File(File&&) noexcept = default;
auto File::operator=(File&&) noexcept -> File& = default;
auto File::storage(Limits l, std::size_t leaf) -> std::expected<std::size_t, Failure> {
    if (l.component_bytes == 0U || l.component_bytes > 255U || l.transfer_bytes == 0U ||
        l.transfer_bytes > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()) ||
        l.file_bytes > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return std::unexpected{issue(Phase::argument, Reason::limits)};
    auto total = sizeof(State);
    for (const auto value : {l.root_bytes, leaf, std::size_t{2}}) {
        if (value > SIZE_MAX - total)
            return std::unexpected{issue(Phase::argument, Reason::overflow)};
        total += value;
    }
    return total;
}
auto detail::Access::open_impl(std::string_view root, std::string_view leaf, Limits l, bool writing,
                          Operations ops, bool root_only) -> std::expected<File, Failure> {
    if (!ops.open_at || !ops.stat || !ops.read || !ops.write || !ops.sync || !ops.close ||
        !ops.effective_uid)
        return std::unexpected{issue(Phase::argument, Reason::limits)};
    if (l.component_bytes == 0 || l.component_bytes > 255U || l.transfer_bytes == 0 ||
        l.transfer_bytes > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()) ||
        l.file_bytes == SIZE_MAX)
        return std::unexpected{issue(Phase::argument, Reason::limits)};
    if (root.size() < 2U || root.front() != '/' || root.back() == '/' ||
        root.find('\0') != root.npos || (!root_only && leaf.empty()) || leaf == "." || leaf == ".." ||
        leaf.find_first_of("/\0", 0, 2) != leaf.npos)
        return std::unexpected{issue(Phase::argument, Reason::limits)};
    if (root.size() > l.root_bytes || leaf.size() > l.component_bytes || l.descriptors < 2U ||
        (writing && l.files < 1U))
        return std::unexpected{issue(Phase::argument, Reason::capacity)};
    std::size_t components{};
    for (std::size_t begin = 1; begin < root.size();) {
        auto end = root.find('/', begin);
        if (end == root.npos)
            end = root.size();
        const auto part = root.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..")
            return std::unexpected{issue(Phase::argument, Reason::limits)};
        if (part.size() > l.component_bytes || ++components > l.root_components)
            return std::unexpected{issue(Phase::argument, Reason::capacity)};
        begin = end + 1U;
    }
    if (!File::storage(l, leaf.size()))
        return std::unexpected{issue(Phase::argument, Reason::overflow)};
    auto state = std::make_unique<File::State>();
    state->limits = l;
    state->ops = ops;
    state->progress.live = true;
    state->progress.writable = writing;
    auto path = std::make_unique<char[]>(root.size() + 1U);
    std::memcpy(path.get(), root.data(), root.size());
    path[root.size()] = '\0';
    state->leaf = std::make_unique<char[]>(leaf.size() + 1U);
    if (!leaf.empty()) std::memcpy(state->leaf.get(), leaf.data(), leaf.size());
    state->leaf[leaf.size()] = '\0';
    auto& s = *state;
    const auto uid = invoke(s, Phase::root, [&] { return ops.effective_uid(ops.context); });
    if (uid.value < 0 || uid.error_number || s.failure)
        return stop(s, fail(s, Phase::root, Reason::io, uid.error_number));
    const auto first = invoke(s, Phase::root, [&] {
        return ops.open_at(ops.context, AT_FDCWD, "/",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0U);
    });
    s.directory = admitted_fd(s, first.value);
    if (!s.directory || first.error_number || s.failure)
        return stop(s, fail(s, Phase::root, Reason::io, first.error_number));
    for (std::size_t begin = 1; begin < root.size();) {
        auto end = root.find('/', begin);
        if (end == root.npos)
            end = root.size();
        path[end] = '\0';
        const auto raw = invoke(s, Phase::root, [&] {
            return ops.open_at(ops.context, s.directory->native_handle(), path.get() + begin,
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0U);
        });
        s.file = admitted_fd(s, raw.value);
        if (!s.file || raw.error_number || s.failure)
            return stop(s, fail(s, Phase::root, Reason::io, raw.error_number));
        if (!charge(s, Phase::root))
            return stop(s, fail(s, Phase::root, Reason::capacity));
        const auto retired = platform_linux::detail::DescriptorAccess::close_raw(*s.directory);
        if (retired.value || retired.error_number)
            return stop(s, fail(s, Phase::root, Reason::io, retired.error_number));
        s.directory = std::move(s.file);
        s.file.reset();
        begin = end + 1U;
    }
    Metadata directory{};
    auto checked = status(s, Phase::root, s.directory->native_handle(), directory);
    if (!checked)
        return stop(s, checked.error());
    if (!directory.directory)
        return stop(s, fail(s, Phase::root, Reason::type));
    if (directory.owner != static_cast<std::uint64_t>(uid.value))
        return stop(s, fail(s, Phase::root, Reason::owner));
    if (directory.permissions != 0700U)
        return stop(s, fail(s, Phase::root, Reason::mode));
    if (root_only)
        return File{std::move(state)};
    const auto flags = writing ? (O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW)
                               : (O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    const auto raw = invoke(s, Phase::open, [&] {
        return ops.open_at(ops.context, s.directory->native_handle(), s.leaf.get(), flags, 0600U);
    });
    s.file = admitted_fd(s, raw.value);
    s.progress.created = writing && s.file.has_value();
    if (!s.file || raw.error_number || s.failure)
        return stop(s, fail(s, Phase::open,
                            raw.error_number == EEXIST ? Reason::collision : Reason::io,
                            raw.error_number));
    checked = status(s, Phase::stat, s.file->native_handle(), s.snapshot);
    if (!checked)
        return stop(s, checked.error());
    if (!s.snapshot.regular)
        return stop(s, fail(s, Phase::stat, Reason::type));
    if (s.snapshot.owner != static_cast<std::uint64_t>(uid.value))
        return stop(s, fail(s, Phase::stat, Reason::owner));
    if (s.snapshot.permissions != 0600U)
        return stop(s, fail(s, Phase::stat, Reason::mode));
    if (s.snapshot.links != 1U)
        return stop(s, fail(s, Phase::stat, Reason::link));
    if (s.snapshot.size < 0 || static_cast<std::uint64_t>(s.snapshot.size) > l.file_bytes ||
        (writing && s.snapshot.size != 0))
        return stop(s, fail(s, Phase::stat, Reason::capacity));
    s.progress.initial_size = static_cast<std::size_t>(s.snapshot.size);
    return File{std::move(state)};
}
auto detail::Access::open(std::string_view root, std::string_view leaf, Limits limits, bool writing,
                         Operations operations) -> std::expected<File, Failure> {
    return open_impl(root, leaf, limits, writing, operations, false);
}
auto detail::Access::preflight_root(std::string_view root, Limits limits, Operations operations)
    -> std::expected<void, Failure> {
    auto admitted = open_impl(root, {}, limits, false, operations, true);
    if (!admitted) return std::unexpected{admitted.error()};
    const auto closed = admitted->close();
    if (closed.count) {
        CloseReport remaining{};
        remaining.attempts = closed.attempts;
        for (std::size_t i = 1; i < closed.count; ++i)
            remaining.issues[remaining.count++] = closed.issues[i];
        return std::unexpected{Failure{closed.issues[0], remaining, false}};
    }
    return {};
}
auto File::preflight_root(std::string_view root, Limits limits) -> std::expected<void, Failure> {
    return detail::Access::preflight_root(root, limits, detail::production_operations());
}
auto File::create(std::string_view root, std::string_view leaf, Limits l)
    -> std::expected<File, Failure> {
    return detail::Access::open(root, leaf, l, true, detail::production_operations());
}
auto File::open_read_only(std::string_view root, std::string_view leaf, Limits l)
    -> std::expected<File, Failure> {
    return detail::Access::open(root, leaf, l, false, detail::production_operations());
}
auto File::write_some(std::span<const std::byte> bytes) -> std::expected<std::size_t, Failure> {
    if (!state_ || !state_->progress.live || !state_->progress.writable)
        return std::unexpected{issue(Phase::write, Reason::state)};
    auto& s = *state_;
    if (bytes.empty())
        return std::unexpected{fail(s, Phase::write, Reason::no_progress)};
    if (bytes.size() > s.limits.file_bytes - s.progress.bytes_written)
        return std::unexpected{fail(s, Phase::write, Reason::capacity)};
    const auto count = std::min(bytes.size(), s.limits.transfer_bytes);
    const auto raw = invoke(s, Phase::write, [&] {
        return s.ops.write(s.ops.context, s.file->native_handle(), bytes.data(), count);
    });
    if (s.failure)
        return std::unexpected{fail(s, Phase::write, Reason::capacity)};
    if (raw.value < 0 || raw.error_number)
        return std::unexpected{fail(s, Phase::write, Reason::io, raw.error_number)};
    if (raw.value == 0)
        return std::unexpected{fail(s, Phase::write, Reason::no_progress)};
    if (static_cast<std::uint64_t>(raw.value) > count)
        return std::unexpected{fail(s, Phase::write, Reason::over_report)};
    const auto actual = static_cast<std::size_t>(raw.value);
    s.progress.bytes_written += actual;
    return actual;
}
auto File::read_some(std::span<std::byte> bytes) -> std::expected<std::size_t, Failure> {
    if (!state_ || !state_->progress.live || state_->progress.writable)
        return std::unexpected{issue(Phase::read, Reason::state)};
    auto& s = *state_;
    if (bytes.empty())
        return std::unexpected{fail(s, Phase::read, Reason::no_progress)};
    const auto remaining = s.progress.initial_size + 1U - s.progress.bytes_read;
    if (remaining == 0U)
        return std::unexpected{fail(s, Phase::read, Reason::capacity)};
    const auto count = std::min({bytes.size(), s.limits.transfer_bytes, remaining});
    const auto raw = invoke(s, Phase::read, [&] {
        return s.ops.read(s.ops.context, s.file->native_handle(), bytes.data(), count);
    });
    if (s.failure)
        return std::unexpected{fail(s, Phase::read, Reason::capacity)};
    if (raw.value < 0 || raw.error_number)
        return std::unexpected{fail(s, Phase::read, Reason::io, raw.error_number)};
    if (static_cast<std::uint64_t>(raw.value) > count)
        return std::unexpected{fail(s, Phase::read, Reason::over_report)};
    const auto actual = static_cast<std::size_t>(raw.value);
    s.progress.bytes_read += actual;
    return actual;
}
namespace {
auto synchronize(File::State& s, bool directory) -> std::expected<void, Failure> {
    const auto phase = directory ? Phase::directory_sync : Phase::file_sync;
    if (!s.progress.live || !s.progress.writable)
        return std::unexpected{issue(phase, Reason::state)};
    const auto raw = invoke(s, phase, [&] {
        return s.ops.sync(s.ops.context, (directory ? s.directory : s.file)->native_handle());
    });
    if (s.failure)
        return std::unexpected{fail(s, phase, Reason::capacity)};
    if (raw.value || raw.error_number)
        return std::unexpected{fail(s, phase, Reason::io, raw.error_number)};
    return {};
}
} // namespace
auto File::rename_no_replace(std::string_view destination) -> std::expected<void, Failure> {
    if (!state_ || !state_->progress.live || !state_->progress.writable || state_->rename_attempted)
        return std::unexpected{issue(Phase::rename, Reason::state)};
    auto& s = *state_;
    if (destination.empty() || destination == "." || destination == ".." ||
        destination.find_first_of("/\0", 0, 2) != destination.npos)
        return std::unexpected{fail(s, Phase::rename, Reason::limits)};
    if (destination.size() > s.limits.component_bytes)
        return std::unexpected{fail(s, Phase::rename, Reason::capacity)};
    if (!s.ops.rename_no_replace)
        return std::unexpected{fail(s, Phase::rename, Reason::state)};
    std::memcpy(s.destination.data(), destination.data(), destination.size());
    s.destination[destination.size()] = '\0';
    if (!charge(s, Phase::rename))
        return std::unexpected{fail(s, Phase::rename, Reason::capacity)};
    s.rename_attempted = true;
    // Never retry: an interrupted rename can have an uncertain externally visible outcome.
    const auto raw = s.ops.rename_no_replace(s.ops.context, s.directory->native_handle(),
                                             s.leaf.get(), s.destination.data());
    if (raw.value || raw.error_number)
        return std::unexpected{fail(s, Phase::rename,
                                    raw.error_number == EEXIST ? Reason::collision : Reason::io,
                                    raw.error_number)};
    return {};
}
auto File::sync_file() -> std::expected<void, Failure> {
    if (!state_)
        return std::unexpected{issue(Phase::file_sync, Reason::state)};
    return synchronize(*state_, false);
}
auto File::sync_directory() -> std::expected<void, Failure> {
    if (!state_)
        return std::unexpected{issue(Phase::directory_sync, Reason::state)};
    return synchronize(*state_, true);
}
auto File::verify_snapshot() -> std::expected<void, Failure> {
    if (!state_ || !state_->progress.live || state_->progress.writable)
        return std::unexpected{issue(Phase::snapshot, Reason::state)};
    auto& s = *state_;
    detail::Metadata current{};
    auto result = status(s, Phase::snapshot, s.file->native_handle(), current);
    if (!result)
        return result;
    if (current != s.snapshot)
        return std::unexpected{fail(s, Phase::snapshot, Reason::mutation)};
    return {};
}
auto File::close() noexcept -> CloseReport { return state_ ? close_state(*state_) : CloseReport{}; }
auto File::progress() const noexcept -> Progress { return state_ ? state_->progress : Progress{}; }
} // namespace epycbench::platform_linux::journal_file
