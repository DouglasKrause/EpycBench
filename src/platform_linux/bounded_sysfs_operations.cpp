#include "platform_linux/bounded_sysfs_operations.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace epycbench::platform_linux::detail {
namespace {

struct LinuxDirectoryEntry final {
    std::uint64_t inode;
    std::int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[1];
};

auto native_next(void*, int descriptor, BoundedSysfsEnumerationState* state,
                 char* output, std::size_t output_size) noexcept -> int {
    for (;;) {
        if (state->cursor >= state->used) {
            const auto count =
                ::syscall(SYS_getdents64, descriptor, state->bytes.data(), state->bytes.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return -1;
            if (count == 0)
                return 0;
            state->used = static_cast<std::size_t>(count);
            state->cursor = 0;
        }
        if (state->used - state->cursor < sizeof(LinuxDirectoryEntry)) {
            errno = EIO;
            return -1;
        }
        const auto* entry = reinterpret_cast<const LinuxDirectoryEntry*>(
            state->bytes.data() + state->cursor);
        if (entry->record_length < offsetof(LinuxDirectoryEntry, name) + 1U ||
            entry->record_length > state->used - state->cursor) {
            errno = EIO;
            return -1;
        }
        state->cursor += entry->record_length;
        const auto available = static_cast<std::size_t>(entry->record_length) -
                               offsetof(LinuxDirectoryEntry, name);
        const auto length = strnlen(entry->name, available);
        if (length == available || length + 1U > output_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if ((length == 1U && entry->name[0] == '.') ||
            (length == 2U && entry->name[0] == '.' && entry->name[1] == '.'))
            continue;
        std::copy_n(entry->name, static_cast<std::ptrdiff_t>(length + 1U), output);
        return 1;
    }
}

} // namespace

auto BoundedSysfsDispositionStock::prepare(std::size_t operation_capacity,
                                           std::size_t retirement_capacity,
                                           BoundedSysfsStockObservation* observation)
    -> BoundedSysfsDispositionStock {
    if (observation != nullptr)
        *observation = {operation_capacity, retirement_capacity};
    return BoundedSysfsDispositionStock{
        std::vector<BoundedSysfsDisposition>(operation_capacity),
        std::vector<BoundedSysfsDisposition>(retirement_capacity), observation};
}

auto BoundedSysfsDispositionStock::record_operation(int native_error) noexcept -> bool {
    if (operation_count_ == operations_.size()) {
        if (observation_ != nullptr)
            observation_->operation_exhausted = true;
        return false;
    }
    operations_[operation_count_++] = {
        native_error == 0 ? BoundedSysfsDispositionKind::success
                          : BoundedSysfsDispositionKind::native_failure,
        native_error, true};
    if (observation_ != nullptr)
        observation_->operation_count = operation_count_;
    return true;
}

auto BoundedSysfsDispositionStock::record_operation(
    BoundedSysfsCloseOutcome outcome) noexcept -> bool {
    if (operation_count_ == operations_.size()) {
        if (observation_ != nullptr)
            observation_->operation_exhausted = true;
        return false;
    }
    operations_[operation_count_++] = {
        outcome.kind, outcome.native_error, true};
    if (observation_ != nullptr)
        observation_->operation_count = operation_count_;
    return true;
}

auto BoundedSysfsDispositionStock::record_retirement(
    BoundedSysfsCloseOutcome outcome) noexcept -> bool {
    if (retirement_count_ == retirements_.size()) {
        if (observation_ != nullptr)
            observation_->retirement_exhausted = true;
        return false;
    }
    retirements_[retirement_count_++] = {
        outcome.kind, outcome.native_error, true};
    if (observation_ != nullptr)
        observation_->retirement_count = retirement_count_;
    return true;
}

auto production_bounded_sysfs_operations() noexcept -> BoundedSysfsOperations {
    auto operations = BoundedSysfsOperations{
        nullptr,
        [](void*, const char* path, int flags) noexcept { return ::open(path, flags); },
        [](void*, int parent, const char* name, int flags) noexcept {
            return ::openat(parent, name, flags);
        },
        [](void*, int descriptor, struct stat* value) noexcept {
            return ::fstat(descriptor, value);
        },
        [](void*, int parent, const char* name, struct stat* value) noexcept {
            return ::fstatat(parent, name, value, AT_SYMLINK_NOFOLLOW);
        },
        native_next,
        [](void*, int descriptor, void* output, std::size_t size,
           std::int64_t offset) noexcept -> std::ptrdiff_t {
            return ::pread(descriptor, output, size, static_cast<off_t>(offset));
        },
        [](void*, int descriptor) noexcept -> BoundedSysfsCloseOutcome {
            if (::close(descriptor) == 0)
                return {};
            const auto saved = errno;
            return {BoundedSysfsDispositionKind::native_failure, saved};
        }};
    operations.effective_uid = [](void*) noexcept { return ::geteuid(); };
    operations.open_hwmon_class_link = [](void*, int parent, const char* name,
                                         int flags) noexcept {
        const open_how how{.flags = static_cast<decltype(open_how::flags)>(flags),
                           .mode = 0U,
                           .resolve = RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS};
        return static_cast<int>(::syscall(SYS_openat2, parent, name, &how, sizeof(how)));
    };
    operations.stat_hwmon_class_target = [](void*, int parent, const char* name,
                                            struct stat* value) noexcept {
        return ::fstatat(parent, name, value, 0);
    };
    return operations;
}

auto bounded_sysfs_identity(const struct stat& value) noexcept -> BoundedSysfsIdentity {
    return {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_uid),
            static_cast<std::uint64_t>(value.st_mode),
            static_cast<std::uint64_t>(value.st_nlink)};
}
auto bounded_sysfs_same_identity(const struct stat& left,
                                 const struct stat& right) noexcept -> bool {
    return bounded_sysfs_identity(left) == bounded_sysfs_identity(right);
}
auto bounded_sysfs_directory_identity(const struct stat& value) noexcept -> bool {
    return S_ISDIR(value.st_mode) && value.st_nlink > 0;
}
auto bounded_sysfs_file_identity(const struct stat& value) noexcept -> bool {
    return S_ISREG(value.st_mode) && value.st_nlink == 1;
}

} // namespace epycbench::platform_linux::detail
