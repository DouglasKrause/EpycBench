#pragma once
#include "epycbench/core/error.hpp"
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <sys/stat.h>
#include <vector>
#include "monitoring/governed_plan_decode.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1PlanV2AuthoritySession;
class HostFrozenSourceLaunchBinding;
struct MonitoringQ1GitExecutableIdentity final {
    std::uint64_t device{}, inode{};
    std::uint32_t mode{}, owner{};
};
struct MonitoringQ1GitCommandFailure final {
    MonitoringQ1GitCommandFailure()=default;
    MonitoringQ1GitCommandFailure(core::Error value,std::vector<core::Error> retired)
      :primary(std::move(value)),cleanup(std::move(retired)){}
    core::Error primary;
    std::vector<core::Error> cleanup;
    std::optional<MonitoringGovernedPlanAuthoredDiagnostic> governed_authored;
};
struct MonitoringQ1GitCommandTestOperations;
class MonitoringQ1GitExecutable final {
  public:
    MonitoringQ1GitExecutable(const MonitoringQ1GitExecutable&) = delete;
    MonitoringQ1GitExecutable& operator=(const MonitoringQ1GitExecutable&) = delete;
    MonitoringQ1GitExecutable(MonitoringQ1GitExecutable&&) noexcept;
    MonitoringQ1GitExecutable& operator=(MonitoringQ1GitExecutable&&) = delete;
    ~MonitoringQ1GitExecutable() noexcept;
    [[nodiscard]] auto recheck() const -> std::expected<bool, core::Error>;
    [[nodiscard]] auto retire() -> std::expected<void, core::Error>;
    [[nodiscard]] auto held_identity() const noexcept -> MonitoringQ1GitExecutableIdentity {
        return identity_;
    }

  private:
    struct Operations final {
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
    MonitoringQ1GitExecutable(int, std::filesystem::path, MonitoringQ1GitExecutableIdentity,
                              Operations, core::Error, core::Error, core::Error,
                              core::Error) noexcept;
    int descriptor_{-1};
    std::filesystem::path path_;
    MonitoringQ1GitExecutableIdentity identity_{};
    Operations operations_{};
    // Source-only synchronous borrow. The stable invocation binding outlives
    // this executable and every repository/Session algorithm that uses it.
    const HostFrozenSourceLaunchBinding* source_launch_{};
    mutable core::Error recheck_contained_standard_;
    mutable core::Error recheck_contained_unknown_;
    core::Error retire_contained_standard_;
    core::Error retire_contained_unknown_;
    friend class MonitoringQ1GitCommandOwner;
    friend class MonitoringQ1GitCommandOwnerTestAccess;
    friend class MonitoringQ1GitSpawn;
    friend class MonitoringQ1PlanV2AuthoritySession;
};
class MonitoringQ1GitCommandOwner final {
  private:
    friend class MonitoringQ1PlanV2AuthoritySession;
    friend class MonitoringQ1GitCommandOwnerTestAccess;
    [[nodiscard]] static auto retain_configured_executable()
        -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto configured_executable_path() noexcept -> std::string_view;
    [[nodiscard]] static auto
    retain_executable_for_test(const MonitoringQ1GitCommandTestOperations&,
                               const std::filesystem::path&)
        -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto retain(MonitoringQ1GitExecutable::Operations,
                                     const std::filesystem::path&)
        -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure>;
};
} // namespace epycbench::monitoring::qualification::detail
