#pragma once
#include "monitoring/host_q1_git_bounded_child.hpp"
#include <cstdint>
namespace epycbench::monitoring::qualification::detail {
using MonitoringQ1GitBoundedChildTestOperations = MonitoringQ1GitBoundedChildOperations;
class MonitoringQ1GitBoundedChildTestAccess final {
  public:
    static void prepare_only(std::size_t stdout_limit) {
        static_cast<void>(MonitoringQ1GitBoundedChild::prepare(stdout_limit));
    }
    static auto collect(std::array<int, 4> c, const MonitoringQ1GitBoundedChildTestOperations& o,
                        MonitoringQ1GitInitialCloseFailures initial = {}) {
        return MonitoringQ1GitBoundedChild::collect(MonitoringQ1GitBoundedChild::prepare(), c, o,
                                                    initial);
    }
    static auto collect_with_stdout_limit(std::array<int, 4> c,
                                          const MonitoringQ1GitBoundedChildTestOperations& o,
                                          std::size_t stdout_limit,
                                          MonitoringQ1GitInitialCloseFailures initial = {}) {
        return MonitoringQ1GitBoundedChild::collect(
            MonitoringQ1GitBoundedChild::prepare(stdout_limit), c, o, initial);
    }
    static auto setup_record(std::int32_t error, std::uint16_t disposition = 1U)
        -> std::array<char, 12>;
    static void emergency_retire(std::array<int, 4>,
                                 const MonitoringQ1GitBoundedChildTestOperations&) noexcept;
};
} // namespace epycbench::monitoring::qualification::detail
