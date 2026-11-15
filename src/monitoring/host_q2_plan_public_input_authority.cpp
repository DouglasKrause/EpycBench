#include "monitoring/host_q2_plan_public_input_authority.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto bootstrap_failure() noexcept -> MonitoringQ1GitCommandFailure {
    MonitoringQ1GitCommandFailure result{};
    result.primary.category = core::ErrorCategory::monitoring;
    result.primary.severity = core::ErrorSeverity::error;
    result.primary.continuation = core::ContinuationPolicy::abort_command;
    return result;
}
auto native_open(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_stat(void*, int fd, struct stat* value) noexcept -> int { return ::fstat(fd, value); }
auto native_stat_at(void*, int parent, const char* name, struct stat* value, int flags) noexcept
    -> int {
    return ::fstatat(parent, name, value, flags);
}
auto native_read(void*, int fd, void* data, std::size_t size, off_t offset) noexcept -> ssize_t {
    return ::pread(fd, data, size, offset);
}
auto native_close(void*, int fd) noexcept -> int { return ::close(fd); }
auto native_euid(void*) noexcept -> unsigned int { return ::geteuid(); }
void native_fault(void*, unsigned int) {}
} // namespace

auto MonitoringQ2PlanPublicInputAuthority::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitFixedResults& git)
    -> std::expected<MonitoringQ2PlanPublicInputAuthority, MonitoringQ1GitCommandFailure> {
    try {
        return retain_with(root.descriptor(), git,
                           {nullptr, native_open, native_stat, native_stat_at, native_read,
                            native_close, native_euid, native_fault});
    } catch (...) {
        return std::unexpected{bootstrap_failure()};
    }
}

auto MonitoringQ2PlanPublicInputAuthority::retain_with(
    int root, const MonitoringQ1GitFixedResults& git, Operations operations)
    -> std::expected<MonitoringQ2PlanPublicInputAuthority, MonitoringQ1GitCommandFailure> {
    constexpr MonitoringGovernedPlanInputCore::Specification specification{
        "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
        "host-v2-q2-context-switch-collection-plan.json",
        "monitoring.q2.plan_authority.public_input_rejected",
        "Q2 governed plan input rejected",
        "authenticate_monitoring_q2_plan",
        4U,
        3U,
        {},
        false,
        [](std::string_view) -> std::expected<std::shared_ptr<const void>, MonitoringGovernedPlanDecodeFailure> {
            return std::shared_ptr<const void>{};
        }};
    auto core = MonitoringGovernedPlanInputCore::retain(root, git, operations, specification);
    if (!core) return std::unexpected{std::move(core.error())};
    return MonitoringQ2PlanPublicInputAuthority{std::move(*core)};
}

} // namespace epycbench::monitoring::qualification::detail
