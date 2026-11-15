#include "monitoring/host_q1_git_command_owner.hpp"
#include "monitoring/host_q1_git_command_owner_test_access.hpp"
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <system_error>
#include <unistd.h>
#include <utility>
namespace epycbench::monitoring::qualification::detail {
namespace {
#ifndef EPYCBENCH_GIT_EXECUTABLE
#error "EPYCBENCH_GIT_EXECUTABLE must be configured"
#endif
auto error(std::string_view c, std::optional<int> e = std::nullopt) -> core::Error {
    std::optional<core::OsError> os;
    if (e)
        os = core::OsError::capture(std::error_code{*e, std::generic_category()});
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q1_plan_authority.repository_rejected"},
            .message = "Q1 Git executable authority rejected",
            .operation = "authenticate_monitoring_q1_plan",
            .os_error = std::move(os),
            .context = {{"classification", c}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto cleanup_error(int e) -> core::Error {
    auto r = error("cleanup_failure", e);
    r.code = core::StableErrorCode{"monitoring.q1_plan_authority.cleanup_failed"};
    return r;
}
int open_executable(void*, const char* p, int f) noexcept { return ::open(p, f); }
int stat_descriptor(void*, int d, struct stat* s) noexcept { return ::fstat(d, s); }
int stat_path(void*, const char* p, struct stat* s) noexcept {
    return ::fstatat(AT_FDCWD, p, s, AT_SYMLINK_NOFOLLOW);
}
int close_descriptor(void*, int d) noexcept { return ::close(d); }
unsigned int effective_user(void*) noexcept { return static_cast<unsigned int>(::geteuid()); }
void after_open(void*) {}
void before_recheck_projection(void*) {}
void before_close_projection(void*) {}
auto identity(const struct stat& s) noexcept -> MonitoringQ1GitExecutableIdentity {
    return {static_cast<std::uint64_t>(s.st_dev), static_cast<std::uint64_t>(s.st_ino),
            static_cast<std::uint32_t>(s.st_mode), static_cast<std::uint32_t>(s.st_uid)};
}
auto same(MonitoringQ1GitExecutableIdentity a, MonitoringQ1GitExecutableIdentity b) noexcept
    -> bool {
    return a.device == b.device && a.inode == b.inode && a.mode == b.mode && a.owner == b.owner;
}
auto trusted(const struct stat& s, unsigned int euid) noexcept -> bool {
    const auto owner = static_cast<unsigned int>(s.st_uid);
    return S_ISREG(s.st_mode) && (owner == 0U || owner == euid) &&
           (s.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 &&
           (s.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}
} // namespace
MonitoringQ1GitExecutable::MonitoringQ1GitExecutable(int d, std::filesystem::path p,
                                                     MonitoringQ1GitExecutableIdentity i,
                                                     Operations o, core::Error recheck_standard,
                                                     core::Error recheck_unknown,
                                                     core::Error retire_standard,
                                                     core::Error retire_unknown) noexcept
    : descriptor_{d}, path_{std::move(p)}, identity_{i}, operations_{o},
      recheck_contained_standard_{std::move(recheck_standard)},
      recheck_contained_unknown_{std::move(recheck_unknown)},
      retire_contained_standard_{std::move(retire_standard)},
      retire_contained_unknown_{std::move(retire_unknown)} {}
MonitoringQ1GitExecutable::MonitoringQ1GitExecutable(MonitoringQ1GitExecutable&& o) noexcept
    : descriptor_{std::exchange(o.descriptor_, -1)}, path_{std::move(o.path_)},
      identity_{o.identity_}, operations_{o.operations_}, source_launch_{o.source_launch_},
      recheck_contained_standard_{std::move(o.recheck_contained_standard_)},
      recheck_contained_unknown_{std::move(o.recheck_contained_unknown_)},
      retire_contained_standard_{std::move(o.retire_contained_standard_)},
      retire_contained_unknown_{std::move(o.retire_contained_unknown_)} {}
MonitoringQ1GitExecutable::~MonitoringQ1GitExecutable() noexcept {
    if (descriptor_ >= 0)
        static_cast<void>(operations_.close_descriptor(operations_.context, descriptor_));
}
auto MonitoringQ1GitExecutable::recheck() const -> std::expected<bool, core::Error> {
    struct stat held{};
    if (operations_.stat_descriptor(operations_.context, descriptor_, &held) != 0) {
        const int saved = errno;
        try {
            operations_.before_recheck_projection(operations_.context);
            return std::unexpected{error("git_identity", saved)};
        } catch (const std::exception&) {
            return std::unexpected{std::move(recheck_contained_standard_)};
        } catch (...) {
            return std::unexpected{std::move(recheck_contained_unknown_)};
        }
    }
    struct stat path{};
    if (operations_.stat_path(operations_.context, path_.c_str(), &path) != 0) {
        const int saved = errno;
        try {
            operations_.before_recheck_projection(operations_.context);
            return std::unexpected{error("git_identity", saved)};
        } catch (const std::exception&) {
            return std::unexpected{std::move(recheck_contained_standard_)};
        } catch (...) {
            return std::unexpected{std::move(recheck_contained_unknown_)};
        }
    }
    return trusted(held, operations_.effective_user(operations_.context)) &&
           same(identity_, identity(held)) && same(identity_, identity(path));
}
auto MonitoringQ1GitExecutable::retire() -> std::expected<void, core::Error> {
    if (descriptor_ < 0)
        return std::unexpected{error("git_identity")};
    const int d = std::exchange(descriptor_, -1);
    if (operations_.close_descriptor(operations_.context, d) != 0) {
        const int close_errno = errno;
        try {
            operations_.before_close_projection(operations_.context);
            return std::unexpected{cleanup_error(close_errno)};
        } catch (const std::exception&) {
            return std::unexpected{std::move(retire_contained_standard_)};
        } catch (...) {
            return std::unexpected{std::move(retire_contained_unknown_)};
        }
    }
    return {};
}
auto MonitoringQ1GitCommandOwner::retain_configured_executable()
    -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure> {
    MonitoringQ1GitExecutable::Operations operations{
        nullptr,    open_executable,           stat_descriptor,
        stat_path,  close_descriptor,          effective_user,
        after_open, before_recheck_projection, before_close_projection};
    return retain(operations, EPYCBENCH_GIT_EXECUTABLE);
}
auto MonitoringQ1GitCommandOwner::configured_executable_path() noexcept -> std::string_view {
    return EPYCBENCH_GIT_EXECUTABLE;
}
auto MonitoringQ1GitCommandOwner::retain_executable_for_test(
    const MonitoringQ1GitCommandTestOperations& test, const std::filesystem::path& p)
    -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure> {
    MonitoringQ1GitExecutable::Operations operations{
        test.context,    test.open_executable,           test.stat_descriptor,
        test.stat_path,  test.close_descriptor,          test.effective_user,
        test.after_open, test.before_recheck_projection, test.before_close_projection};
    return retain(operations, p);
}
auto MonitoringQ1GitCommandOwner::retain(MonitoringQ1GitExecutable::Operations o,
                                         const std::filesystem::path& p)
    -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure> {
    if (!o.open_executable || !o.stat_descriptor || !o.stat_path || !o.close_descriptor ||
        !o.effective_user || !o.after_open || !o.before_recheck_projection ||
        !o.before_close_projection || !p.is_absolute())
        return std::unexpected{MonitoringQ1GitCommandFailure{error("git_executable"), {}}};
    auto contained_standard = error("contained_standard_exception");
    contained_standard.code =
        core::StableErrorCode{"monitoring.q1_plan_authority.contained_failure"};
    auto contained_unknown = error("contained_unknown_exception");
    contained_unknown.code =
        core::StableErrorCode{"monitoring.q1_plan_authority.contained_failure"};
    auto cleanup_contained_standard = contained_standard;
    auto cleanup_contained_unknown = contained_unknown;
    auto recheck_contained_standard = contained_standard;
    auto recheck_contained_unknown = contained_unknown;
    auto retire_contained_standard = contained_standard;
    auto retire_contained_unknown = contained_unknown;
    auto retained_path = p;
    std::vector<core::Error> cleanup;
    cleanup.reserve(1);
    const int d = o.open_executable(o.context, p.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (d < 0)
        return std::unexpected{MonitoringQ1GitCommandFailure{error("git_executable", errno), {}}};
    std::optional<core::Error> primary;
    std::optional<MonitoringQ1GitExecutable> value;
    try {
        o.after_open(o.context);
        struct stat held{};
        if (o.stat_descriptor(o.context, d, &held) != 0)
            primary = error("git_executable", errno);
        else if (!trusted(held, o.effective_user(o.context)))
            primary = error("git_executable");
        else {
            struct stat path{};
            if (o.stat_path(o.context, p.c_str(), &path) != 0)
                primary = error("git_identity", errno);
            else if (!same(identity(held), identity(path)))
                primary = error("git_identity");
            else
                value.emplace(MonitoringQ1GitExecutable{
                    d, std::move(retained_path), identity(held), o,
                    std::move(recheck_contained_standard), std::move(recheck_contained_unknown),
                    std::move(retire_contained_standard), std::move(retire_contained_unknown)});
        }
    } catch (const std::exception&) {
        primary = std::move(contained_standard);
    } catch (...) {
        primary = std::move(contained_unknown);
    }
    if (value)
        return std::move(*value);
    if (o.close_descriptor(o.context, d) != 0) {
        const int close_errno = errno;
        try {
            o.before_close_projection(o.context);
            cleanup.push_back(cleanup_error(close_errno));
        } catch (const std::exception&) {
            cleanup.push_back(std::move(cleanup_contained_standard));
        } catch (...) {
            cleanup.push_back(std::move(cleanup_contained_unknown));
        }
    }
    return std::unexpected{MonitoringQ1GitCommandFailure{std::move(*primary), std::move(cleanup)}};
}

auto MonitoringQ1GitCommandOwnerTestAccess::retain_configured_executable()
    -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure> {
    return MonitoringQ1GitCommandOwner::retain_configured_executable();
}
auto MonitoringQ1GitCommandOwnerTestAccess::configured_executable_path() noexcept
    -> std::string_view {
    return MonitoringQ1GitCommandOwner::configured_executable_path();
}
auto MonitoringQ1GitCommandOwnerTestAccess::retain_executable(
    const MonitoringQ1GitCommandTestOperations& operations, const std::filesystem::path& path)
    -> std::expected<MonitoringQ1GitExecutable, MonitoringQ1GitCommandFailure> {
    return MonitoringQ1GitCommandOwner::retain_executable_for_test(operations, path);
}
auto MonitoringQ1GitCommandOwnerTestAccess::descriptor(
    const MonitoringQ1GitExecutable& value) noexcept -> int {
    return value.descriptor_;
}
auto MonitoringQ1GitCommandOwnerTestAccess::path(const MonitoringQ1GitExecutable& value) noexcept
    -> const std::filesystem::path& {
    return value.path_;
}
auto MonitoringQ1GitCommandOwnerTestAccess::identity(
    const MonitoringQ1GitExecutable& value) noexcept -> MonitoringQ1GitExecutableIdentity {
    return value.identity_;
}
} // namespace epycbench::monitoring::qualification::detail
