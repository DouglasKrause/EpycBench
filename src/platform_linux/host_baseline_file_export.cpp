#include "platform_linux/host_baseline_file_export.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace epycbench::platform_linux::host_baseline_file {
namespace {
using NativeResult = detail::RawStatusResult;

auto uid(void*) noexcept -> std::uint64_t { return static_cast<std::uint64_t>(::geteuid()); }
auto metadata(void*, int fd, Metadata* value) noexcept -> NativeResult {
    struct stat info{};
    const auto result = ::fstat(fd, &info);
    if (result != 0) return {result, errno};
    if (info.st_size < 0) return {-1, EIO};
    *value = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
              static_cast<std::uint64_t>(info.st_mode), static_cast<std::uint64_t>(info.st_uid),
              static_cast<std::uint64_t>(info.st_nlink), static_cast<std::uint64_t>(info.st_size)};
    return {0, 0};
}
auto access(void*, int fd) noexcept -> NativeResult {
    const auto result = ::fcntl(fd, F_GETFL);
    return {result, result < 0 ? errno : 0};
}
auto write_at(void*, int fd, std::span<const std::byte> bytes, std::uint64_t offset) noexcept
    -> NativeResult {
    const auto result = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    return {static_cast<long>(result), result < 0 ? errno : 0};
}
auto sync(void*, int fd) noexcept -> NativeResult {
    const auto result = ::fsync(fd);
    return {result, result < 0 ? errno : 0};
}
auto timestamp(const detail::ClockOperations& operations) noexcept
    -> std::expected<std::uint64_t, Reason> {
    const auto value = operations.clock_gettime_raw(operations.context, CLOCK_MONOTONIC);
    if (value.native_result != 0 || value.error_number != 0 || value.timestamp.tv_sec < 0 ||
        value.timestamp.tv_nsec < 0 || value.timestamp.tv_nsec >= 1000000000)
        return std::unexpected{Reason::clock};
    const auto seconds = static_cast<std::uint64_t>(value.timestamp.tv_sec);
    const auto ns = static_cast<std::uint64_t>(value.timestamp.tv_nsec);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (seconds > (maximum - ns) / UINT64_C(1000000000))
        return std::unexpected{Reason::clock};
    return seconds * UINT64_C(1000000000) + ns;
}
auto same_identity(const Metadata& first, const Metadata& second) noexcept -> bool {
    return first.device == second.device && first.inode == second.inode &&
           first.mode == second.mode && first.uid == second.uid && first.links == second.links;
}
} // namespace

auto production_operations() noexcept -> Operations {
    return {nullptr, uid, metadata, access, write_at, sync, detail::production_clock_operations()};
}
auto export_bytes(FileDescriptor fd, std::span<const std::byte> bytes, Limits limits) noexcept
    -> std::expected<Receipt, Failure> {
    return export_bytes_with(std::move(fd), bytes, limits, production_operations());
}

auto export_bytes_with(FileDescriptor fd, std::span<const std::byte> bytes,
    Limits limits, const Operations& operations) noexcept -> std::expected<Receipt, Failure> {
    std::size_t written{}, calls{};
    std::optional<std::uint64_t> deadline;
    const auto within_deadline = [&]() -> std::expected<void, Reason> {
        auto now = timestamp(operations.clock);
        if (!now) return std::unexpected{now.error()};
        if (*now >= *deadline) return std::unexpected{Reason::deadline};
        return {};
    };
    const auto operation = [&]() -> std::expected<Receipt, Reason> {
        if (!fd.active() || fd.native_handle() < 3 || bytes.empty() ||
            !operations.effective_uid || !operations.metadata || !operations.access ||
            !operations.write_at || !operations.sync || !operations.clock.clock_gettime_raw)
            return std::unexpected{Reason::invalid_input};
        if (limits.bytes == 0 || limits.bytes > 524288 || bytes.size() > limits.bytes ||
            limits.write_calls == 0 || limits.write_calls > 1024 ||
            limits.duration.value() == 0 || limits.duration.value() > UINT64_C(5000000000) ||
            bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return std::unexpected{Reason::capacity};
        auto now = timestamp(operations.clock);
        if (!now) return std::unexpected{now.error()};
        if (*now > std::numeric_limits<std::uint64_t>::max() - limits.duration.value())
            return std::unexpected{Reason::clock};
        deadline = *now + limits.duration.value();
        const auto effective_uid = operations.effective_uid(operations.context);
        auto ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        if (effective_uid == 0) return std::unexpected{Reason::admission};
        Metadata initial{};
        auto result = operations.metadata(operations.context, fd.native_handle(), &initial);
        if (result.value != 0 || result.error_number != 0) return std::unexpected{Reason::metadata};
        ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        if ((initial.mode & S_IFMT) != S_IFREG || (initial.mode & 07777U) != 0600U ||
            initial.uid != effective_uid || initial.links != 1 || initial.bytes != 0)
            return std::unexpected{Reason::admission};
        result = operations.access(operations.context, fd.native_handle());
        if (result.value < 0 || result.error_number != 0) return std::unexpected{Reason::access};
        ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        if (result.value > std::numeric_limits<int>::max()) return std::unexpected{Reason::access};
        const auto flags = static_cast<int>(result.value);
        if (((flags & O_ACCMODE) != O_WRONLY && (flags & O_ACCMODE) != O_RDWR) || (flags & O_APPEND))
            return std::unexpected{Reason::admission};
        while (written != bytes.size()) {
            ready = within_deadline();
            if (!ready) return std::unexpected{ready.error()};
            if (calls == limits.write_calls) return std::unexpected{Reason::capacity};
            ++calls;
            result = operations.write_at(operations.context, fd.native_handle(), bytes.subspan(written),
                                         static_cast<std::uint64_t>(written));
            if (result.value == -1 && result.error_number == EINTR) {
                ready = within_deadline();
                if (!ready) return std::unexpected{ready.error()};
                continue;
            }
            if (result.value < 0 || result.error_number != 0) return std::unexpected{Reason::write};
            if (result.value == 0 || static_cast<std::uint64_t>(result.value) > bytes.size() - written)
                return std::unexpected{Reason::progress};
            written += static_cast<std::size_t>(result.value);
            ready = within_deadline();
            if (!ready) return std::unexpected{ready.error()};
        }
        ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        result = operations.sync(operations.context, fd.native_handle());
        if (result.value != 0 || result.error_number != 0) return std::unexpected{Reason::sync};
        ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        Metadata final{};
        result = operations.metadata(operations.context, fd.native_handle(), &final);
        if (result.value != 0 || result.error_number != 0) return std::unexpected{Reason::metadata};
        ready = within_deadline();
        if (!ready) return std::unexpected{ready.error()};
        if (!same_identity(initial, final) || final.bytes != bytes.size())
            return std::unexpected{Reason::identity};
        return Receipt{core::ByteCount::from_base_units(static_cast<std::uint64_t>(written)), calls};
    };
    auto result = operation();
    if (result) {
        auto ready = within_deadline();
        if (!ready) result = std::unexpected{ready.error()};
    }
    const auto closed = detail::DescriptorAccess::close_raw(fd);
    const bool close_failed = closed.value != 0 || closed.error_number != 0;
    if (!result) return std::unexpected{Failure{result.error(),
        close_failed ? std::optional{Reason::close} : std::nullopt, written, calls}};
    if (close_failed) return std::unexpected{Failure{Reason::close, std::nullopt, written, calls}};
    auto ready = within_deadline();
    if (!ready) return std::unexpected{Failure{ready.error(), std::nullopt, written, calls}};
    return *result;
}
} // namespace epycbench::platform_linux::host_baseline_file
