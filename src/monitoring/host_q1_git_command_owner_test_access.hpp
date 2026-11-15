#pragma once
#include "monitoring/host_q1_git_command_owner.hpp"
namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ1GitCommandTestOperations final {
    void* context{};
    int (*open_executable)(void*, const char*, int) noexcept {};
    int (*stat_descriptor)(void*, int, struct stat*) noexcept {};
    int (*stat_path)(void*, const char*, struct stat*) noexcept {};
    int (*close_descriptor)(void*, int) noexcept {};
    unsigned int (*effective_user)(void*) noexcept {};
    void (*after_open)(void*){};
    void (*before_recheck_projection)(void*){};
    void (*before_close_projection)(void*){};
};
class MonitoringQ1GitCommandOwnerTestAccess final {
  public:
    static auto retain_configured_executable()
        -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure>;
    static auto configured_executable_path() noexcept -> std::string_view;
    static auto retain_executable(const MonitoringQ1GitCommandTestOperations&,
                                  const std::filesystem::path&)
        -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure>;
    static auto descriptor(const MonitoringQ1GitExecutable&) noexcept -> int;
    static auto path(const MonitoringQ1GitExecutable&) noexcept -> const std::filesystem::path&;
    static auto identity(const MonitoringQ1GitExecutable&) noexcept
        -> MonitoringQ1GitExecutableIdentity;
};
} // namespace epycbench::monitoring::qualification::detail
