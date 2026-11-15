#include "monitoring/host_q4_kernel_maximum_authority.hpp"
#include "monitoring/host_q4_monitoring_capacity_adapter.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr auto path = "/sys/devices/system/cpu/kernel_max";

auto native_open(void*, const char* value, int flags) noexcept -> int {
    return ::open(value, flags);
}
auto native_stat_fd(void*, int descriptor, struct stat* value) noexcept -> int {
    return ::fstat(descriptor, value);
}
auto native_stat_named(void*, const char* value, struct stat* status) noexcept -> int {
    return ::fstatat(AT_FDCWD, value, status, AT_SYMLINK_NOFOLLOW);
}
auto native_read(void*, int descriptor, void* bytes, std::size_t count,
                 std::int64_t offset) noexcept -> ssize_t {
    return ::pread(descriptor, bytes, count, static_cast<off_t>(offset));
}
auto native_close(void*, int descriptor) noexcept -> int { return ::close(descriptor); }

using Reason = MonitoringQ4CapacityReason;
auto identity_reason(const struct stat& left, const struct stat& right,
                     const std::array<Reason, 5>& reasons) noexcept -> Reason {
    if (left.st_dev != right.st_dev) return reasons[0];
    if (left.st_ino != right.st_ino) return reasons[1];
    if (left.st_uid != right.st_uid) return reasons[2];
    if (left.st_mode != right.st_mode) return reasons[3];
    if (left.st_nlink != right.st_nlink) return reasons[4];
    return Reason::none;
}
auto failure(MonitoringQ4KernelMaximumFailureKind kind, int native = 0,
             Reason reason = Reason::none) noexcept {
    return MonitoringQ4KernelMaximumFailure{kind, native, false, std::nullopt, reason};
}
} // namespace

MonitoringQ4KernelMaximumAuthority::MonitoringQ4KernelMaximumAuthority(
    int descriptor, struct stat identity, Operations operations,
    std::shared_ptr<const void> binding,
    std::optional<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority> prepared) noexcept
    : descriptor_{descriptor}, identity_{identity}, operations_{operations},
      binding_{std::move(binding)}, prepared_{std::move(prepared)} {}

MonitoringQ4AuthenticatedKernelMaximum::MonitoringQ4AuthenticatedKernelMaximum(
    MonitoringQ4AuthenticatedKernelMaximum&& other) noexcept
    : value_{other.value_}, binding_{std::move(other.binding_)},
      authority_{std::move(other.authority_)},
      consumed_{std::exchange(other.consumed_, true)} {}

auto MonitoringQ4AuthenticatedKernelMaximum::take(
    const std::shared_ptr<const void>& expected) noexcept
    -> std::optional<MonitoringQ4AuthenticatedKernelMaximumPayload> {
    if (consumed_ || !binding_ || binding_ != expected || !authority_) {
        consumed_ = true;
        binding_.reset();
        authority_.reset();
        return std::nullopt;
    }
    consumed_ = true;
    binding_.reset();
    return MonitoringQ4AuthenticatedKernelMaximumPayload{value_, std::move(*authority_)};
}

MonitoringQ4KernelMaximumAuthority::MonitoringQ4KernelMaximumAuthority(
    MonitoringQ4KernelMaximumAuthority&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)}, identity_{other.identity_},
      operations_{other.operations_}, binding_{std::move(other.binding_)},
      prepared_{std::move(other.prepared_)},
      terminal_{std::exchange(other.terminal_, true)} {}

MonitoringQ4KernelMaximumAuthority::~MonitoringQ4KernelMaximumAuthority() noexcept {
    if (descriptor_ >= 0)
        static_cast<void>(operations_.close(operations_.context, descriptor_));
}

auto MonitoringQ4KernelMaximumAuthority::retain(
    const MonitoringQ4KernelMaximumReady& ready) noexcept
    -> std::expected<MonitoringQ4KernelMaximumAuthority, MonitoringQ4KernelMaximumFailure> {
    auto prepared = MonitoringQ4MonitoringCapacityAdapter::prepare(ready);
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto retained = retain_with({nullptr, native_open, native_stat_fd, native_stat_named,
                                 native_read, native_close, nullptr});
    if (retained) {
        retained->binding_ = ready.kernel_maximum_binding_;
        retained->prepared_.emplace(std::move(*prepared));
    }
    return retained;
}

auto MonitoringQ4KernelMaximumAuthority::retain_with(Operations operations) noexcept
    -> std::expected<MonitoringQ4KernelMaximumAuthority, MonitoringQ4KernelMaximumFailure> {
    const int descriptor =
        operations.open(operations.context, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::open, errno, Reason::kernel_retain_open)};
    bool owned = true;
    const auto reject = [&](MonitoringQ4KernelMaximumFailure value) noexcept
        -> std::expected<MonitoringQ4KernelMaximumAuthority, MonitoringQ4KernelMaximumFailure> {
        if (owned) {
            owned = false;
            if (operations.close(operations.context, descriptor) != 0)
                value.cleanup = MonitoringQ4KernelMaximumCleanup{
                    MonitoringQ4KernelMaximumFailureKind::close, errno, true};
        }
        return std::unexpected{value};
    };
    try {
        if (operations.fault)
            operations.fault(operations.context, 1U);
        struct stat held{}, named{};
        if (operations.stat_fd(operations.context, descriptor, &held) != 0)
            return reject(failure(MonitoringQ4KernelMaximumFailureKind::held_stat, errno, Reason::kernel_retain_held_stat));
        if (operations.stat_named(operations.context, path, &named) != 0)
            return reject(failure(MonitoringQ4KernelMaximumFailureKind::named_stat, errno, Reason::kernel_retain_named_stat));
        if (!S_ISREG(held.st_mode))
            return reject(failure(MonitoringQ4KernelMaximumFailureKind::identity, 0, Reason::kernel_retain_type));
        if (held.st_nlink != 1)
            return reject(failure(MonitoringQ4KernelMaximumFailureKind::identity, 0, Reason::kernel_retain_links));
        const auto identity = identity_reason(held, named, {Reason::kernel_retain_device,
            Reason::kernel_retain_inode, Reason::kernel_retain_owner, Reason::kernel_retain_mode,
            Reason::kernel_retain_identity_links});
        if (identity != Reason::none)
            return reject(failure(MonitoringQ4KernelMaximumFailureKind::identity, 0, identity));
        if (operations.fault)
            operations.fault(operations.context, 2U);
        owned = false;
        return MonitoringQ4KernelMaximumAuthority{descriptor, held, operations, {}, std::nullopt};
    } catch (...) {
        return reject(failure(MonitoringQ4KernelMaximumFailureKind::contained));
    }
}

auto MonitoringQ4KernelMaximumAuthority::reject(MonitoringQ4KernelMaximumFailureKind kind,
                                                int native, Reason reason) noexcept
    -> std::expected<MonitoringQ4AuthenticatedKernelMaximum, MonitoringQ4KernelMaximumFailure> {
    auto value = failure(kind, native, reason);
    terminal_ = true;
    if (descriptor_ >= 0) {
        const int held = std::exchange(descriptor_, -1);
        if (operations_.close(operations_.context, held) != 0)
            value.cleanup = MonitoringQ4KernelMaximumCleanup{
                MonitoringQ4KernelMaximumFailureKind::close, errno, true};
    }
    return std::unexpected{value};
}

auto MonitoringQ4KernelMaximumAuthority::authenticate_and_retire() noexcept
    -> std::expected<MonitoringQ4AuthenticatedKernelMaximum, MonitoringQ4KernelMaximumFailure> {
    if (terminal_ || descriptor_ < 0)
        return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::inactive)};
    try {
        if (operations_.fault)
            operations_.fault(operations_.context, 3U);
        std::array<char, 11> bytes{};
        std::size_t used{};
        while (used < bytes.size()) {
            const auto count =
                operations_.read(operations_.context, descriptor_, bytes.data() + used,
                                 bytes.size() - used, static_cast<std::int64_t>(used));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                return reject(MonitoringQ4KernelMaximumFailureKind::read, errno, Reason::kernel_body_read);
            }
            if (count == 0)
                break;
            if (static_cast<std::size_t>(count) > bytes.size() - used)
                return reject(MonitoringQ4KernelMaximumFailureKind::read_contract, 0, Reason::kernel_body_overreport);
            used += static_cast<std::size_t>(count);
        }
        char probe{};
        for (;;) {
            const auto count = operations_.read(operations_.context, descriptor_, &probe, 1,
                                                static_cast<std::int64_t>(used));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return reject(MonitoringQ4KernelMaximumFailureKind::read, errno, Reason::kernel_probe_read);
            if (count > 1)
                return reject(MonitoringQ4KernelMaximumFailureKind::read_contract, 0, Reason::kernel_probe_overreport);
            if (count != 0)
                return reject(MonitoringQ4KernelMaximumFailureKind::byte_limit, 0, Reason::kernel_probe_not_eof);
            break;
        }
        struct stat held{}, named{};
        if (operations_.stat_fd(operations_.context, descriptor_, &held) != 0)
            return reject(MonitoringQ4KernelMaximumFailureKind::held_stat, errno, Reason::kernel_after_held_stat);
        if (operations_.stat_named(operations_.context, path, &named) != 0)
            return reject(MonitoringQ4KernelMaximumFailureKind::named_stat, errno, Reason::kernel_after_named_stat);
        if (!S_ISREG(held.st_mode))
            return reject(MonitoringQ4KernelMaximumFailureKind::identity, 0, Reason::kernel_after_type);
        if (held.st_nlink != 1)
            return reject(MonitoringQ4KernelMaximumFailureKind::identity, 0, Reason::kernel_after_links);
        const auto after_held = identity_reason(identity_, held, {Reason::kernel_after_held_device, Reason::kernel_after_held_inode, Reason::kernel_after_held_owner, Reason::kernel_after_held_mode, Reason::kernel_after_held_links});
        if (after_held != Reason::none)
            return reject(MonitoringQ4KernelMaximumFailureKind::identity, 0, after_held);
        const auto after_named = identity_reason(identity_, named, {Reason::kernel_after_named_device, Reason::kernel_after_named_inode, Reason::kernel_after_named_owner, Reason::kernel_after_named_mode, Reason::kernel_after_named_links});
        if (after_named != Reason::none)
            return reject(MonitoringQ4KernelMaximumFailureKind::identity, 0, after_named);
        if (used < 2U)
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_short);
        if (bytes[used - 1U] != '\n')
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_lf);
        const auto digits = std::string_view{bytes.data(), used - 1U};
        if (digits.size() > 1U && digits.front() == '0')
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_leading_zero);
        if (digits.size() > 10U)
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_width);
        std::uint64_t parsed{};
        const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
        if (converted.ec != std::errc{})
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_parse);
        if (converted.ptr != digits.data() + digits.size())
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_trailing);
        if (parsed > std::numeric_limits<std::uint32_t>::max())
            return reject(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0, Reason::kernel_value_range);
        if (operations_.fault)
            operations_.fault(operations_.context, 4U);
        const int held_descriptor = std::exchange(descriptor_, -1);
        terminal_ = true;
        if (operations_.close(operations_.context, held_descriptor) != 0) {
            auto close_failure = failure(MonitoringQ4KernelMaximumFailureKind::close, errno);
            close_failure.primary_uncertain = true;
            return std::unexpected{close_failure};
        }
        if (!prepared_)
            return reject(MonitoringQ4KernelMaximumFailureKind::inactive);
        const auto maximum = static_cast<std::uint32_t>(parsed);
        auto authority = MonitoringQ4MonitoringCapacityAdapter::seal(
            std::move(*prepared_), maximum);
        prepared_.reset();
        if (!authority)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, 0,
                                           monitoring_q4_linux_capacity_reason(authority.error().reason))};
        return MonitoringQ4AuthenticatedKernelMaximum{
            MonitoringQ4KernelMaximum{maximum}, std::move(binding_), std::move(*authority)};
    } catch (...) {
        return reject(MonitoringQ4KernelMaximumFailureKind::contained);
    }
}

} // namespace epycbench::monitoring::qualification::detail
