#pragma once
#include "monitoring/host_q4_capacity_reason.hpp"

#include "monitoring/host_q4_authority_session.hpp"
#include "monitoring/host_q4_structural_capacity.hpp"
#include "platform_linux/authenticated_monitoring_capacity_authority.hpp"

#include <expected>
#include <optional>
#include <sys/stat.h>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4KernelMaximumFailureKind : unsigned char {
    open,
    held_stat,
    named_stat,
    identity,
    read,
    read_contract,
    byte_limit,
    invalid_value,
    close,
    contained,
    inactive
};

struct MonitoringQ4KernelMaximumCleanup final {
    MonitoringQ4KernelMaximumFailureKind kind{MonitoringQ4KernelMaximumFailureKind::close};
    int native_error{};
    bool uncertain{};
};

struct MonitoringQ4KernelMaximumFailure final {
    MonitoringQ4KernelMaximumFailureKind primary{};
    int native_error{};
    bool primary_uncertain{};
    std::optional<MonitoringQ4KernelMaximumCleanup> cleanup{};
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
};

class MonitoringQ4KernelMaximumAuthorityTestAccess;
class MonitoringQ4SourceReadyTestAccess;
struct MonitoringQ4AuthenticatedKernelMaximumPayload final {
    MonitoringQ4KernelMaximum maximum;
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority;
};
class MonitoringQ4AuthenticatedKernelMaximum final {
  public:
    MonitoringQ4AuthenticatedKernelMaximum(const MonitoringQ4AuthenticatedKernelMaximum&) = delete;
    MonitoringQ4AuthenticatedKernelMaximum(MonitoringQ4AuthenticatedKernelMaximum&&) noexcept;
    auto operator=(const MonitoringQ4AuthenticatedKernelMaximum&)
        -> MonitoringQ4AuthenticatedKernelMaximum& = delete;
    auto operator=(MonitoringQ4AuthenticatedKernelMaximum&&)
        -> MonitoringQ4AuthenticatedKernelMaximum& = delete;

  private:
    MonitoringQ4AuthenticatedKernelMaximum(MonitoringQ4KernelMaximum value,
                                           std::shared_ptr<const void> binding) noexcept
        : value_{value}, binding_{std::move(binding)} {}
    MonitoringQ4AuthenticatedKernelMaximum(
        MonitoringQ4KernelMaximum value, std::shared_ptr<const void> binding,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority) noexcept
        : value_{value}, binding_{std::move(binding)}, authority_{std::move(authority)} {}
    [[nodiscard]] auto take(const std::shared_ptr<const void>&) noexcept
        -> std::optional<MonitoringQ4AuthenticatedKernelMaximumPayload>;
    MonitoringQ4KernelMaximum value_{};
    std::shared_ptr<const void> binding_;
    std::optional<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority> authority_;
    bool consumed_{};
    friend class MonitoringQ4KernelMaximumAuthority;
    friend class MonitoringQ4KernelMaximumAuthorityTestAccess;
    friend class MonitoringQ4SourceReadyTransition;
    friend class MonitoringQ4SourceReadyTestAccess;
};

class MonitoringQ4KernelMaximumAuthority final {
  public:
    MonitoringQ4KernelMaximumAuthority(const MonitoringQ4KernelMaximumAuthority&) = delete;
    MonitoringQ4KernelMaximumAuthority(MonitoringQ4KernelMaximumAuthority&&) noexcept;
    auto operator=(const MonitoringQ4KernelMaximumAuthority&)
        -> MonitoringQ4KernelMaximumAuthority& = delete;
    auto operator=(MonitoringQ4KernelMaximumAuthority&&)
        -> MonitoringQ4KernelMaximumAuthority& = delete;
    ~MonitoringQ4KernelMaximumAuthority() noexcept;

    [[nodiscard]] static auto retain(const MonitoringQ4KernelMaximumReady&) noexcept
        -> std::expected<MonitoringQ4KernelMaximumAuthority, MonitoringQ4KernelMaximumFailure>;
    [[nodiscard]] auto authenticate_and_retire() noexcept
        -> std::expected<MonitoringQ4AuthenticatedKernelMaximum, MonitoringQ4KernelMaximumFailure>;

  private:
    struct Operations final {
        void* context{};
        int (*open)(void*, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_named)(void*, const char*, struct stat*) noexcept {};
        ssize_t (*read)(void*, int, void*, std::size_t, std::int64_t) noexcept {};
        int (*close)(void*, int) noexcept {};
        void (*fault)(void*, unsigned){};
    };
    static auto retain_with(Operations) noexcept
        -> std::expected<MonitoringQ4KernelMaximumAuthority, MonitoringQ4KernelMaximumFailure>;
    MonitoringQ4KernelMaximumAuthority(int, struct stat, Operations,
                                       std::shared_ptr<const void>,
                                       std::optional<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority>) noexcept;
    [[nodiscard]] auto reject(MonitoringQ4KernelMaximumFailureKind, int = 0,
                              MonitoringQ4CapacityReason = MonitoringQ4CapacityReason::none) noexcept
        -> std::expected<MonitoringQ4AuthenticatedKernelMaximum, MonitoringQ4KernelMaximumFailure>;
    int descriptor_{-1};
    struct stat identity_{};
    Operations operations_{};
    std::shared_ptr<const void> binding_;
    std::optional<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority> prepared_;
    bool terminal_{};
    friend class MonitoringQ4KernelMaximumAuthorityTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
