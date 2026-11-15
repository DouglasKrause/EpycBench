#pragma once
#include "monitoring/host_q2_terminal_containment.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2TerminalContainmentTestAccess final {
  public:
    static auto reserve(core::Error stock) noexcept {
        return MonitoringQ2TerminalContainment::reserve(std::move(stock));
    }
    static auto reserve() noexcept {
        return reserve(
            core::Error{.category = core::ErrorCategory::monitoring,
                        .code = core::StableErrorCode{"monitoring.q2.containment.test_reserve"},
                        .message = "test reserve",
                        .operation = "monitoring_q2_terminal_containment",
                        .os_error = std::nullopt,
                        .context = {},
                        .severity = core::ErrorSeverity::error,
                        .continuation = core::ContinuationPolicy::abort_command});
    }
    static auto contain(MonitoringQ2TerminalContainmentStock&& stock,
                        MonitoringQ2BridgeRetirementRecovery&& recovery,
                        MonitoringQ2ContextSwitchSourceOwner&& source,
                        MonitoringQ2CollectorPlan&& plan, MonitoringQ2CollectionStage stage,
                        core::Error primary,
                        std::optional<MonitoringQ2HelperProvider> provider = {}) noexcept {
        std::array<std::optional<core::Error>, 64> cleanup{};
        return MonitoringQ2TerminalContainment::contain(
            std::move(stock), std::move(recovery), std::move(source), std::move(plan), stage,
            std::move(primary), std::move(cleanup), 0U, false, std::move(provider));
    }
    static auto
    contain_with_cleanup(MonitoringQ2TerminalContainmentStock&& stock,
                         MonitoringQ2BridgeRetirementRecovery&& recovery,
                         MonitoringQ2ContextSwitchSourceOwner&& source,
                         MonitoringQ2CollectorPlan&& plan, MonitoringQ2CollectionStage stage,
                         core::Error primary, std::array<std::optional<core::Error>, 64>&& cleanup,
                         std::size_t cleanup_count, bool overflowed = false,
                         std::optional<MonitoringQ2HelperProvider> provider = {}) noexcept {
        return MonitoringQ2TerminalContainment::contain(
            std::move(stock), std::move(recovery), std::move(source), std::move(plan), stage,
            std::move(primary), std::move(cleanup), cleanup_count, overflowed, std::move(provider));
    }
    [[noreturn]] static void
    consume(MonitoringQ2TerminalContainment&& value,
            MonitoringQ2TerminalContainmentOperations operations) noexcept {
        consume_monitoring_q2_terminal_containment_with(std::move(value), operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
