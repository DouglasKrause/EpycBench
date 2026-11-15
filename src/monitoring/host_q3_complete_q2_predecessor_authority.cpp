#include "monitoring/host_q3_complete_q2_predecessor_authority.hpp"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto error(std::string classification, int native = 0) -> core::Error {
    core::Error result{.category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.q3.complete_q2_predecessor.invalid"},
        .message = "Monitoring Q3 complete Q2 predecessor is invalid",
        .operation = "authenticate_monitoring_q3_complete_q2_predecessor", .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command};
    if (native != 0)
        result.os_error = core::OsError::capture(std::error_code{native, std::generic_category()});
    return result;
}
auto failure(std::string classification, int native = 0)
    -> MonitoringQ3CompleteQ2PredecessorAuthorityFailure {
    return {error(std::move(classification), native)};
}
void append(MonitoringQ3CompleteQ2PredecessorAuthorityFailure& target, core::Error value) noexcept {
    if (target.cleanup_count == target.cleanup.size()) std::terminate();
    target.cleanup[target.cleanup_count++].emplace(std::move(value));
}
auto converted(MonitoringQ3CompleteQ2PredecessorFailure source)
    -> MonitoringQ3CompleteQ2PredecessorAuthorityFailure {
    MonitoringQ3CompleteQ2PredecessorAuthorityFailure result{std::move(source.primary)};
    if (source.cleanup_count > source.cleanup.size()) std::terminate();
    for (std::size_t index{}; index < source.cleanup_count; ++index) {
        if (!source.cleanup[index]) std::terminate();
        append(result, std::move(*source.cleanup[index]));
    }
    return result;
}
void append(MonitoringQ3CompleteQ2PredecessorAuthorityFailure& target,
            MonitoringQ3CompleteQ2PredecessorAuthorityFailure source) noexcept {
    append(target, std::move(source.primary));
    for (std::size_t index{}; index < source.cleanup_count; ++index) {
        if (!source.cleanup[index]) std::terminate();
        append(target, std::move(*source.cleanup[index]));
    }
}
auto identity(const struct stat& value)
    -> MonitoringQ3CompleteQ2PredecessorAuthority::SnapshotIdentity {
    return {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
        static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
        static_cast<std::uint32_t>(value.st_uid), static_cast<std::uint32_t>(value.st_nlink)};
}
auto native_inventory(void*, int descriptor)
    -> std::expected<std::set<std::string>, MonitoringQ3CompleteQ2PredecessorAuthority::InventoryFailure> {
    const int duplicate = ::dup(descriptor);
    if (duplicate < 0)
        return std::unexpected{MonitoringQ3CompleteQ2PredecessorAuthority::InventoryFailure{
            error("snapshot_inventory", errno), std::nullopt}};
    DIR* directory = ::fdopendir(duplicate);
    if (!directory) {
        const int saved = errno;
        auto result = MonitoringQ3CompleteQ2PredecessorAuthority::InventoryFailure{
            error("snapshot_inventory", saved), std::nullopt};
        if (::close(duplicate) != 0) result.cleanup.emplace(error("snapshot_cleanup", errno));
        return std::unexpected{std::move(result)};
    }
    std::set<std::string> names;
    ::rewinddir(directory);
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        const std::string_view name{entry->d_name};
        if (name != "." && name != "..") names.emplace(name);
        errno = 0;
    }
    const int read_error = errno;
    const int closed = ::closedir(directory);
    if (read_error != 0 || closed != 0)
        return std::unexpected{MonitoringQ3CompleteQ2PredecessorAuthority::InventoryFailure{
            error("snapshot_inventory", read_error != 0 ? read_error : errno), std::nullopt}};
    return names;
}
auto native_duplicate(void*, int value) noexcept -> int { return ::fcntl(value, F_DUPFD_CLOEXEC, 0); }
auto native_open(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_stat(void*, int fd, struct stat* value) noexcept -> int { return ::fstat(fd, value); }
auto native_stat_at(void*, int fd, const char* name, struct stat* value, int flags) noexcept -> int {
    return ::fstatat(fd, name, value, flags);
}
auto native_read(void*, int fd, void* bytes, std::size_t size, off_t offset) noexcept -> ssize_t {
    return ::pread(fd, bytes, size, offset);
}
auto native_close(void*, int fd) noexcept -> int { return ::close(fd); }
auto native_user(void*) noexcept -> unsigned int { return ::geteuid(); }
void native_fault(void*, unsigned int) {}
template <typename Operations>
auto read_all(int fd, std::uint64_t authenticated_size, std::size_t limit,
              const Operations& operations)
    -> std::expected<std::string, core::Error> {
    if (authenticated_size > limit)
        return std::unexpected{error("snapshot_size")};
    std::string bytes(static_cast<std::size_t>(authenticated_size), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = operations.read_at(operations.context, fd, bytes.data() + offset,
            bytes.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::unexpected{error("snapshot_read", count < 0 ? errno : EIO)};
        offset += static_cast<std::size_t>(count);
    }
    char extra{};
    ssize_t count{};
    do { count = operations.read_at(operations.context, fd, &extra, 1, static_cast<off_t>(offset)); }
    while (count < 0 && errno == EINTR);
    if (count != 0) return std::unexpected{error("snapshot_growth", count < 0 ? errno : EFBIG)};
    return bytes;
}
} // namespace

MonitoringQ3CompleteQ2PredecessorBinding::MonitoringQ3CompleteQ2PredecessorBinding(
    std::string_view qualification_id, std::string_view current_commit, std::string_view profile,
    std::string_view q1_plan, std::string_view q2_plan, std::string_view q3_plan,
    MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history,
    const MonitoringQ1ValidatedPublicValues* campaign_values) noexcept
    : qualification_id_{qualification_id}, current_commit_{current_commit}, profile_{profile},
      q1_plan_{q1_plan}, q2_plan_{q2_plan}, q3_plan_{q3_plan}, history_{history},
      campaign_values_{campaign_values} {}

MonitoringQ3CompleteQ2PredecessorAuthority::MonitoringQ3CompleteQ2PredecessorAuthority() noexcept {
    descriptors_.fill(-1);
}
MonitoringQ3CompleteQ2PredecessorAuthority::MonitoringQ3CompleteQ2PredecessorAuthority(
    MonitoringQ3CompleteQ2PredecessorAuthority&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {})}, identities_{other.identities_},
      bytes_{std::move(other.bytes_)}, qualification_id_{std::move(other.qualification_id_)},
      current_commit_{std::move(other.current_commit_)}, profile_{std::move(other.profile_)},
      q1_plan_{std::move(other.q1_plan_)}, q2_plan_{std::move(other.q2_plan_)},
      q3_plan_{std::move(other.q3_plan_)}, value_{std::move(other.value_)}, history_{other.history_},
      expected_{std::move(other.expected_)}, campaign_values_{other.campaign_values_}, operations_{other.operations_},
      active_{std::exchange(other.active_, false)} {
    other.descriptors_.fill(-1);
}
MonitoringQ3CompleteQ2PredecessorAuthority::~MonitoringQ3CompleteQ2PredecessorAuthority() noexcept {
    if (active_) static_cast<void>(retire());
}
auto MonitoringQ3CompleteQ2PredecessorAuthority::retire()
    -> std::expected<void, MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    if (!active_) return std::unexpected{failure("inactive")};
    active_ = false;
    std::optional<MonitoringQ3CompleteQ2PredecessorAuthorityFailure> result;
    for (auto iterator = descriptors_.rbegin(); iterator != descriptors_.rend(); ++iterator) {
        const int descriptor = std::exchange(*iterator, -1);
        if (descriptor >= 0 && operations_.close_fd(operations_.context, descriptor) != 0) {
            if (!result) result.emplace(failure("snapshot_cleanup", errno));
            else append(*result, error("snapshot_cleanup", errno));
        }
    }
    if (result) return std::unexpected{std::move(*result)};
    return {};
}

auto MonitoringQ3CompleteQ2PredecessorAuthority::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ3CompleteQ2PredecessorBinding& binding)
    -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                     MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    return retain_with(root.descriptor(), binding,
        {nullptr, native_duplicate, native_open, native_stat, native_stat_at, native_read,
         native_inventory, native_close, native_user, native_fault}, nullptr);
}
auto MonitoringQ3CompleteQ2PredecessorAuthority::retain_with_native_expected(
    int root, const MonitoringQ3CompleteQ2PredecessorBinding& binding,
    const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected)
    -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                     MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    return retain_with(root, binding,
        {nullptr, native_duplicate, native_open, native_stat, native_stat_at, native_read,
         native_inventory, native_close, native_user, native_fault}, &expected);
}
auto MonitoringQ3CompleteQ2PredecessorAuthority::retain_with_native_fault(
    int root, const MonitoringQ3CompleteQ2PredecessorBinding& binding,
    const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected,
    void* context, void (*fault)(void*, unsigned int))
    -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                     MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    return retain_with(root, binding,
        {context, native_duplicate, native_open, native_stat, native_stat_at, native_read,
         native_inventory, native_close, native_user, fault}, &expected);
}

auto MonitoringQ3CompleteQ2PredecessorAuthority::retain_with(
    int root, const MonitoringQ3CompleteQ2PredecessorBinding& binding, Operations operations,
    const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests* expected)
    -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                     MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    MonitoringQ3CompleteQ2PredecessorAuthority result;
    result.operations_ = operations;
    const auto reject = [&](MonitoringQ3CompleteQ2PredecessorAuthorityFailure rejected) {
        for (auto iterator = result.descriptors_.rbegin(); iterator != result.descriptors_.rend(); ++iterator) {
            const int descriptor = std::exchange(*iterator, -1);
            if (descriptor >= 0 && operations.close_fd(operations.context, descriptor) != 0)
                append(rejected, error("snapshot_cleanup", errno));
        }
        return std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                             MonitoringQ3CompleteQ2PredecessorAuthorityFailure>{
            std::unexpected{std::move(rejected)}};
    };
    try {
        constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        constexpr int file_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        operations.fault(operations.context, 0);
        result.descriptors_[0] = operations.duplicate(operations.context, root);
        if (result.descriptors_[0] < 0) return reject(failure("snapshot_open", errno));
        constexpr std::array directory_parents{0U, 1U, 2U, 3U, 3U, 3U};
        const std::array<std::string, 6> directory_names{".local", "monitoring-qualification",
            std::string{binding.qualification_id_}, "q0b", "q1", "q2"};
        for (std::size_t index{}; index < directory_names.size(); ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 1U));
            result.descriptors_[index + 1U] = operations.open_at(operations.context,
                result.descriptors_[directory_parents[index]], directory_names[index].c_str(), directory_flags);
            if (result.descriptors_[index + 1U] < 0) return reject(failure("snapshot_open", errno));
        }
        constexpr std::array file_parents{3U, 4U, 5U, 6U, 3U, 3U};
        constexpr std::array file_names{"host-environment.json", "evidence.json", "evidence.json",
                                         "evidence.json", "manifest.json", "checksums.sha256"};
        for (std::size_t index{}; index < file_names.size(); ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 7U));
            result.descriptors_[index + 7U] = operations.open_at(operations.context,
                result.descriptors_[file_parents[index]], file_names[index], file_flags);
            if (result.descriptors_[index + 7U] < 0) return reject(failure("snapshot_open", errno));
        }
        const std::array expected_inventory{
            std::set<std::string>{"checksums.sha256", "host-environment.json", "manifest.json", "q0b", "q1", "q2"},
            std::set<std::string>{"evidence.json"}, std::set<std::string>{"evidence.json"},
            std::set<std::string>{"evidence.json"}};
        for (std::size_t index{}; index < expected_inventory.size(); ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 13U));
            auto names = operations.inventory(operations.context, result.descriptors_[index + 3U]);
            if (!names) {
                auto rejected = failure("snapshot_inventory");
                rejected.primary = std::move(names.error().primary);
                if (names.error().cleanup) append(rejected, std::move(*names.error().cleanup));
                return reject(std::move(rejected));
            }
            if (*names != expected_inventory[index]) return reject(failure("snapshot_inventory"));
        }
        constexpr std::array<std::size_t, 6> limits{1U << 20U, 8U << 20U, 8U << 20U,
                                                    16U << 20U, 1U << 20U, 1U << 20U};
        const auto owner = static_cast<std::uint32_t>(operations.effective_user(operations.context));
        for (std::size_t index{}; index < result.descriptors_.size(); ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 17U));
            struct stat status{};
            if (operations.stat_fd(operations.context, result.descriptors_[index], &status) != 0)
                return reject(failure("snapshot_stat", errno));
            result.identities_[index] = identity(status);
            if (index == 0U) continue;
            const bool directory = index <= 6U;
            if ((directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) ||
                static_cast<std::uint32_t>(status.st_uid) != owner ||
                (!directory && status.st_nlink != 1) ||
                (!directory && (status.st_size < 0 ||
                    static_cast<std::uint64_t>(status.st_size) > limits[index - 7U])) ||
                (static_cast<std::uint32_t>(status.st_mode) & 07777U) != (directory ? 0700U : 0600U))
                return reject(failure("snapshot_predicate"));
        }
        for (std::size_t index{}; index < result.bytes_.size(); ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 30U));
            auto bytes = read_all(result.descriptors_[index + 7U],
                result.identities_[index + 7U].size, limits[index], operations);
            if (!bytes) return reject(MonitoringQ3CompleteQ2PredecessorAuthorityFailure{
                std::move(bytes.error())});
            result.bytes_[index] = std::move(*bytes);
        }
        MonitoringQ3CompleteQ2PredecessorInput input{std::string{binding.qualification_id_},
            std::string{binding.current_commit_}, std::string{binding.profile_},
            std::string{binding.q1_plan_}, std::string{binding.q2_plan_}, std::string{binding.q3_plan_},
            result.bytes_[0], result.bytes_[1], result.bytes_[2], result.bytes_[3], result.bytes_[4],
            result.bytes_[5]};
        auto authenticated = binding.campaign_values_
            ? MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_campaign(
                  std::move(input), binding.history_, *binding.campaign_values_)
            : expected
            ? MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_with(
                  std::move(input), binding.history_, *expected)
            : MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate(std::move(input), binding.history_);
        if (!authenticated) {
            return reject(converted(std::move(authenticated.error())));
        }
        result.value_ = std::move(*authenticated);
        result.qualification_id_ = binding.qualification_id_;
        result.current_commit_ = binding.current_commit_;
        result.profile_ = binding.profile_;
        result.q1_plan_ = binding.q1_plan_;
        result.q2_plan_ = binding.q2_plan_;
        result.q3_plan_ = binding.q3_plan_;
        result.history_ = binding.history_;
        result.campaign_values_ = binding.campaign_values_;
        if (expected) result.expected_ = *expected;
        result.active_ = true;
        auto final = result.recheck();
        if (!final || !*final) {
            auto rejected = final ? failure("snapshot_changed") : std::move(final.error());
            auto retired = result.retire();
            if (!retired) append(rejected, std::move(retired.error()));
            return std::unexpected{std::move(rejected)};
        }
        return result;
    } catch (...) { return reject(failure("contained")); }
}

auto MonitoringQ3CompleteQ2PredecessorAuthority::recheck() const
    -> std::expected<bool, MonitoringQ3CompleteQ2PredecessorAuthorityFailure> {
    if (!active_) return std::unexpected{failure("inactive")};
    try {
        for (std::size_t index{}; index < descriptors_.size(); ++index) {
            struct stat status{};
            if (operations_.stat_fd(operations_.context, descriptors_[index], &status) != 0)
                return std::unexpected{failure("snapshot_recheck", errno)};
            if (identity(status) != identities_[index]) return std::unexpected{failure("snapshot_held_changed")};
        }
        const std::array<std::size_t, 12> parents{0, 1, 2, 3, 3, 3, 3, 4, 5, 6, 3, 3};
        const std::array<const char*, 12> names{".local", "monitoring-qualification", nullptr, "q0b", "q1", "q2",
            "host-environment.json", "evidence.json", "evidence.json", "evidence.json", "manifest.json", "checksums.sha256"};
        for (std::size_t index{}; index < parents.size(); ++index) {
            struct stat status{};
            const char* name = index == 2U ? qualification_id_.c_str() : names[index];
            if (operations_.stat_at(operations_.context, descriptors_[parents[index]], name, &status,
                                    AT_SYMLINK_NOFOLLOW) != 0)
                return std::unexpected{failure("snapshot_named_recheck", errno)};
            if (identity(status) != identities_[index + 1U])
                return std::unexpected{failure("snapshot_named_changed")};
        }
        const std::array expected_inventory{
            std::set<std::string>{"checksums.sha256", "host-environment.json", "manifest.json", "q0b", "q1", "q2"},
            std::set<std::string>{"evidence.json"}, std::set<std::string>{"evidence.json"},
            std::set<std::string>{"evidence.json"}};
        for (std::size_t index{}; index < expected_inventory.size(); ++index) {
            auto inventory_value = operations_.inventory(operations_.context, descriptors_[index + 3U]);
            if (!inventory_value) {
                MonitoringQ3CompleteQ2PredecessorAuthorityFailure rejected{
                    std::move(inventory_value.error().primary)};
                if (inventory_value.error().cleanup)
                    append(rejected, std::move(*inventory_value.error().cleanup));
                return std::unexpected{std::move(rejected)};
            }
            if (*inventory_value != expected_inventory[index])
                return std::unexpected{failure("snapshot_inventory_changed")};
        }
        constexpr std::array<std::size_t, 6> limits{1U << 20U, 8U << 20U, 8U << 20U,
                                                    16U << 20U, 1U << 20U, 1U << 20U};
        for (std::size_t index{}; index < bytes_.size(); ++index) {
            auto bytes = read_all(descriptors_[index + 7U], identities_[index + 7U].size,
                                  limits[index], operations_);
            if (!bytes) return std::unexpected{
                MonitoringQ3CompleteQ2PredecessorAuthorityFailure{std::move(bytes.error())}};
            if (*bytes != bytes_[index]) return std::unexpected{failure("snapshot_bytes_changed")};
        }
        MonitoringQ3CompleteQ2PredecessorInput input{qualification_id_, current_commit_, profile_, q1_plan_,
            q2_plan_, q3_plan_, bytes_[0], bytes_[1], bytes_[2], bytes_[3], bytes_[4], bytes_[5]};
        auto authenticated = campaign_values_
            ? MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_campaign(
                  std::move(input), history_, *campaign_values_)
            : expected_
            ? MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_with(
                  std::move(input), history_, *expected_)
            : MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate(std::move(input), history_);
        if (!authenticated) {
            return std::unexpected{converted(std::move(authenticated.error()))};
        }
        return true;
    } catch (...) { return std::unexpected{failure("contained")}; }
}

} // namespace epycbench::monitoring::qualification::detail
