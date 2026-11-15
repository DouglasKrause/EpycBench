#include "monitoring/host_q1_public_input_authority.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::size_t cap = 1U << 20U;
auto native_open(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_fstat(void*, int fd, struct stat* value) noexcept -> int { return ::fstat(fd, value); }
auto native_stat(void*, int parent, const char* name, struct stat* value, int flags) noexcept
    -> int {
    return ::fstatat(parent, name, value, flags);
}
auto native_read(void*, int fd, void* bytes, std::size_t size, off_t offset) noexcept -> ssize_t {
    return ::pread(fd, bytes, size, offset);
}
auto native_close(void*, int fd) noexcept -> int { return ::close(fd); }
auto native_euid(void*) noexcept -> unsigned int { return ::geteuid(); }
void native_fault(void*, unsigned int) {}
auto identity(const struct stat& value) noexcept -> MonitoringQ1PublicInputIdentity {
    return {static_cast<std::uint64_t>(value.st_dev),  static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),  static_cast<std::uint32_t>(value.st_nlink)};
}
auto error(std::string classification, std::string code, int native = 0)
    -> MonitoringQ1GitCommandFailure {
    std::optional<core::OsError> os;
    if (native != 0)
        os = core::OsError::capture(std::error_code{native, std::generic_category()});
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{std::move(code)},
             .message = "Q1 governed public input rejected",
             .operation = "authenticate_monitoring_q1_plan",
             .os_error = std::move(os),
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto close_all(std::array<int, 6>& fds,
               const MonitoringQ1PublicInputAuthority::Operations& operations) noexcept {
    std::array<int, 6> failures{};
    std::size_t count{};
    for (auto it = fds.rbegin(); it != fds.rend(); ++it) {
        const int fd = std::exchange(*it, -1);
        if (fd >= 0 && operations.close_fd(operations.context, fd) != 0)
            failures[count++] = errno;
    }
    return std::pair{failures, count};
}
auto append_close(MonitoringQ1GitCommandFailure& failure,
                  const std::pair<std::array<int, 6>, std::size_t>& closes) {
    for (std::size_t index = 0; index < closes.second; ++index)
        failure.cleanup.push_back(error("cleanup_failure",
                                        "monitoring.q1_plan_authority.cleanup_failed",
                                        closes.first[index])
                                      .primary);
}
auto read_exact(int fd, std::uint64_t size,
                const MonitoringQ1PublicInputAuthority::Operations& operations)
    -> std::expected<std::string, int> {
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto got = operations.read_at(operations.context, fd, bytes.data() + offset,
                                            bytes.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return std::unexpected{got < 0 ? errno : EIO};
        offset += static_cast<std::size_t>(got);
    }
    char extra{};
    ssize_t trailing{};
    do {
        trailing =
            operations.read_at(operations.context, fd, &extra, 1, static_cast<off_t>(offset));
    } while (trailing < 0 && errno == EINTR);
    if (trailing < 0)
        return std::unexpected{errno};
    if (trailing != 0)
        return std::unexpected{EFBIG};
    return bytes;
}
} // namespace

MonitoringQ1PublicInputAuthority::MonitoringQ1PublicInputAuthority(
    std::array<int, 6> descriptors, std::array<MonitoringQ1PublicInputIdentity, 6> identities,
    std::array<std::string, 3> bytes, std::array<std::string, 3> digests, std::string captured_head,
    std::shared_ptr<const void> capture_binding, Operations operations) noexcept
    : descriptors_{descriptors}, identities_{identities}, bytes_{std::move(bytes)},
      digests_{std::move(digests)}, captured_head_{std::move(captured_head)},
      capture_binding_{std::move(capture_binding)}, operations_{operations} {}
MonitoringQ1PublicInputAuthority::MonitoringQ1PublicInputAuthority(
    MonitoringQ1PublicInputAuthority&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {-1, -1, -1, -1, -1, -1})},
      identities_{other.identities_}, bytes_{std::move(other.bytes_)},
      digests_{std::move(other.digests_)}, captured_head_{std::move(other.captured_head_)},
      capture_binding_{std::move(other.capture_binding_)}, operations_{other.operations_} {}
MonitoringQ1PublicInputAuthority::~MonitoringQ1PublicInputAuthority() noexcept {
    static_cast<void>(close_all(descriptors_, operations_));
}
auto MonitoringQ1PublicInputAuthority::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1GitFixedResults& git)
    -> std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure> {
    return retain_with(root.descriptor(), git,
                       {nullptr, native_open, native_fstat, native_stat, native_read, native_close,
                        native_euid, native_fault});
}
auto MonitoringQ1PublicInputAuthority::retain_with(int root, const MonitoringQ1GitFixedResults& git,
                                                   Operations operations)
    -> std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure> {
    std::array<int, 6> fds{-1, -1, -1, -1, -1, -1};
    auto reject = [&](std::string_view classification, int native = 0) {
        const auto closes = close_all(fds, operations);
        auto failure = error(std::string{classification},
                             "monitoring.q1_plan_authority.public_input_rejected", native);
        append_close(failure, closes);
        return std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure>{
            std::unexpected{std::move(failure)}};
    };
    auto tracked_reject = [&]() {
        const auto closes = close_all(fds, operations);
        auto failure = error("tracked_entry", "monitoring.q1_plan_authority.repository_rejected");
        append_close(failure, closes);
        return std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure>{
            std::unexpected{std::move(failure)}};
    };
    try {
        if (!operations.open_at || !operations.stat_fd || !operations.stat_at ||
            !operations.read_at || !operations.close_fd || !operations.effective_user ||
            !operations.fault)
            return reject("policy");
        operations.fault(operations.context, 0);
        if (git.records.size() != 3)
            return reject("policy");
        fds[0] = operations.open_at(operations.context, root, ".",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fds[0] < 0)
            return reject("policy", errno);
        fds[1] = operations.open_at(operations.context, fds[0], "config",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fds[1] < 0)
            return reject("policy", errno);
        fds[2] = operations.open_at(operations.context, fds[1], "monitoring-qualification",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fds[2] < 0)
            return reject("policy", errno);
        std::array<MonitoringQ1PublicInputIdentity, 6> identities{};
        const auto euid = operations.effective_user(operations.context);
        for (std::size_t index = 0; index < 3; ++index) {
            struct stat value{};
            if (operations.stat_fd(operations.context, fds[index], &value) != 0)
                return reject("policy", errno);
            if (!S_ISDIR(value.st_mode) || value.st_uid != euid || (value.st_mode & 0022) != 0)
                return reject("policy");
            identities[index] = identity(value);
        }
        constexpr std::array names{"host-v2-policy.json", "host-v2-profile.json",
                                   "host-v2-q1-page-fault-collection-plan.json"};
        constexpr std::array paths{
            "config/monitoring-qualification/host-v2-policy.json",
            "config/monitoring-qualification/host-v2-profile.json",
            "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json"};
        constexpr std::array modes{0644U, 0600U, 0600U};
        constexpr std::array classifications{"policy", "profile", "plan"};
        std::array<std::string, 3> bytes{};
        std::array<std::string, 3> digests{};
        for (std::size_t index = 0; index < 3; ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 1));
            if (git.records[index].path_ != paths[index])
                return tracked_reject();
            fds[index + 3] = operations.open_at(operations.context, fds[2], names[index],
                                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fds[index + 3] < 0)
                return reject(classifications[index], errno);
            struct stat before{}, after{};
            if (operations.stat_fd(operations.context, fds[index + 3], &before) != 0)
                return reject(classifications[index], errno);
            if (!S_ISREG(before.st_mode) || before.st_uid != euid || before.st_nlink != 1 ||
                (static_cast<unsigned int>(before.st_mode) & 07777U) != modes[index] ||
                before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > cap)
                return reject(classifications[index]);
            auto read =
                read_exact(fds[index + 3], static_cast<std::uint64_t>(before.st_size), operations);
            if (!read)
                return reject(classifications[index], read.error());
            if (operations.stat_fd(operations.context, fds[index + 3], &after) != 0)
                return reject(classifications[index], errno);
            if (identity(before) != identity(after))
                return reject(classifications[index]);
            if (*read != git.records[index].bytes_)
                return tracked_reject();
            identities[index + 3] = identity(before);
            bytes[index] = std::move(*read);
            digests[index] = core::sha256(bytes[index]).to_hex();
        }
        return MonitoringQ1PublicInputAuthority{
            fds,       identities, std::move(bytes), std::move(digests),
            git.head_, git.frame_, operations};
    } catch (const std::exception&) {
        const auto closes = close_all(fds, operations);
        auto failure =
            error("contained_standard_exception", "monitoring.q1_plan_authority.contained_failure");
        append_close(failure, closes);
        return std::unexpected{std::move(failure)};
    } catch (...) {
        const auto closes = close_all(fds, operations);
        auto failure =
            error("contained_unknown_exception", "monitoring.q1_plan_authority.contained_failure");
        append_close(failure, closes);
        return std::unexpected{std::move(failure)};
    }
}

auto MonitoringQ1PublicInputAuthority::recheck() const
    -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    try {
        operations_.fault(operations_.context, 10);
        constexpr std::array names{".",
                                   "config",
                                   "monitoring-qualification",
                                   "host-v2-policy.json",
                                   "host-v2-profile.json",
                                   "host-v2-q1-page-fault-collection-plan.json"};
        for (std::size_t index = 0; index < descriptors_.size(); ++index) {
            operations_.fault(operations_.context, static_cast<unsigned int>(11 + index));
            struct stat held{}, named{};
            if (operations_.stat_fd(operations_.context, descriptors_[index], &held) != 0)
                return std::unexpected{error(
                    "authority_changed", "monitoring.q1_plan_authority.authority_changed", errno)};
            const int parent =
                index == 0 ? descriptors_[0] : descriptors_[index < 3 ? index - 1 : 2];
            if (operations_.stat_at(operations_.context, parent, names[index], &named,
                                    AT_SYMLINK_NOFOLLOW) != 0)
                return std::unexpected{error(
                    "authority_changed", "monitoring.q1_plan_authority.authority_changed", errno)};
            if (identity(held) != identities_[index] || identity(named) != identities_[index])
                return false;
            if (index >= 3) {
                auto current =
                    read_exact(descriptors_[index], identities_[index].size, operations_);
                if (!current)
                    return std::unexpected{error("authority_changed",
                                                 "monitoring.q1_plan_authority.authority_changed",
                                                 current.error())};
                struct stat after{};
                if (operations_.stat_fd(operations_.context, descriptors_[index], &after) != 0)
                    return std::unexpected{error("authority_changed",
                                                 "monitoring.q1_plan_authority.authority_changed",
                                                 errno)};
                if (identity(after) != identities_[index] || *current != bytes_[index - 3])
                    return false;
            }
        }
        return true;
    } catch (const std::exception&) {
        return std::unexpected{error("contained_standard_exception",
                                     "monitoring.q1_plan_authority.contained_failure")};
    } catch (...) {
        return std::unexpected{
            error("contained_unknown_exception", "monitoring.q1_plan_authority.contained_failure")};
    }
}
auto MonitoringQ1PublicInputAuthority::retire()
    -> std::expected<void, MonitoringQ1GitCommandFailure> {
    try {
        operations_.fault(operations_.context, 20);
        auto closes = close_all(descriptors_, operations_);
        if (closes.second == 0)
            return {};
        auto failure = error("cleanup_failure", "monitoring.q1_plan_authority.cleanup_failed",
                             closes.first[0]);
        for (std::size_t index = 1; index < closes.second; ++index)
            failure.cleanup.push_back(error("cleanup_failure",
                                            "monitoring.q1_plan_authority.cleanup_failed",
                                            closes.first[index])
                                          .primary);
        return std::unexpected{std::move(failure)};
    } catch (const std::exception&) {
        const auto closes = close_all(descriptors_, operations_);
        auto failure =
            error("contained_standard_exception", "monitoring.q1_plan_authority.contained_failure");
        append_close(failure, closes);
        return std::unexpected{std::move(failure)};
    } catch (...) {
        const auto closes = close_all(descriptors_, operations_);
        auto failure =
            error("contained_unknown_exception", "monitoring.q1_plan_authority.contained_failure");
        append_close(failure, closes);
        return std::unexpected{std::move(failure)};
    }
}
} // namespace epycbench::monitoring::qualification::detail
