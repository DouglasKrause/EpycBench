#include "monitoring/host_q4_complete_q3_predecessor_authority.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include "monitoring/host_q2_context_switch_evidence.hpp"
#include "monitoring/host_q3_temperature_evidence_json.hpp"
#include <nlohmann/json.hpp>
#include "core/json_ownership.hpp"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Kind = MonitoringQ4CompleteQ3PrimaryKind;
auto fixed(Kind kind, int native = 0) noexcept -> MonitoringQ4CompleteQ3AuthorityFailure {
    return {.primary = kind, .native_error = native};
}
void append(MonitoringQ4CompleteQ3Retirement& value, MonitoringQ4PredecessorRole role,
            MonitoringQ4PredecessorCleanupSource source, int native) noexcept {
    if (value.cleanup_count < value.cleanup_facts.size())
        value.cleanup_facts[value.cleanup_count++] = {role, source, native};
    else
        value.cleanup_overflowed = true;
}
auto identity(const struct stat& value) noexcept
    -> MonitoringQ4CompleteQ3PredecessorAuthority::Identity {
    return {static_cast<std::uint64_t>(value.st_dev),  static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),  static_cast<std::uint32_t>(value.st_nlink)};
}
int duplicate(void*, int fd) noexcept { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }
int open_at(void*, int parent, const char* name, int flags) noexcept {
    return ::openat(parent, name, flags);
}
int stat_fd(void*, int fd, struct stat* value) noexcept { return ::fstat(fd, value); }
int stat_at(void*, int fd, const char* name, struct stat* value, int flags) noexcept {
    return ::fstatat(fd, name, value, flags);
}
ssize_t read_at(void*, int fd, void* output, std::size_t size, off_t offset) noexcept {
    return ::pread(fd, output, size, offset);
}
int close_fd(void*, int fd) noexcept { return ::close(fd); }
unsigned int effective_user(void*) noexcept { return ::geteuid(); }
void no_fault(void*, unsigned int) {}

struct DirectoryGuard final {
    DIR* value{};
    ~DirectoryGuard() noexcept {
        if (value)
            static_cast<void>(::closedir(value));
    }
};
thread_local void (*inventory_test_fault)() {};
thread_local int (*inventory_test_close)(void*) noexcept {};
auto consume_inventory_directory(DIR* value) noexcept -> int {
    return inventory_test_close ? inventory_test_close(value) : ::closedir(value);
}
auto inventory(void*, int descriptor)
    -> std::expected<std::set<std::string>,
                     MonitoringQ4CompleteQ3PredecessorAuthority::InventoryFailure> {
    // A duplicate shares the caller's directory offset; each scan needs its own description.
    const int copy = ::openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (copy < 0)
        return std::unexpected{MonitoringQ4CompleteQ3PredecessorAuthority::InventoryFailure{
            Kind::inventory, errno, std::nullopt}};
    DIR* opened = ::fdopendir(copy);
    if (!opened) {
        const int primary = errno;
        const int closed = ::close(copy);
        const int cleanup = errno;
        MonitoringQ4CompleteQ3PredecessorAuthority::InventoryFailure failure{Kind::inventory,
                                                                             primary, std::nullopt};
        if (closed != 0)
            failure.cleanup = MonitoringQ4PredecessorCleanupFact{
                MonitoringQ4PredecessorRole::qid_inventory,
                MonitoringQ4PredecessorCleanupSource::inventory, cleanup};
        return std::unexpected{failure};
    }
    DirectoryGuard guard{opened};
    try {
        if (inventory_test_fault) inventory_test_fault();
        std::set<std::string> result;
        errno = 0;
        while (const auto* entry = ::readdir(opened)) {
            const std::string_view name{entry->d_name};
            if (name != "." && name != "..")
                result.emplace(name);
            errno = 0;
        }
        const int read_error = errno;
        guard.value = nullptr;
        const int closed = consume_inventory_directory(opened);
        const int close_error = errno;
        if (read_error != 0 || closed != 0) {
            MonitoringQ4CompleteQ3PredecessorAuthority::InventoryFailure failure{
                Kind::inventory, read_error != 0 ? read_error : close_error, std::nullopt};
            return std::unexpected{failure};
        }
        return result;
    } catch (...) {
        guard.value = nullptr;
        const int closed = consume_inventory_directory(opened);
        const int close_error = errno;
        MonitoringQ4CompleteQ3PredecessorAuthority::InventoryFailure failure{Kind::contained, 0,
                                                                             std::nullopt};
        if (closed != 0)
            failure.cleanup = MonitoringQ4PredecessorCleanupFact{
                MonitoringQ4PredecessorRole::qid_inventory,
                MonitoringQ4PredecessorCleanupSource::inventory, close_error};
        return std::unexpected{failure};
    }
}
constexpr std::array<std::size_t, 7> caps{1U << 20U,   8U << 20U, 8U << 20U, 16U << 20U,
                                          512U << 10U, 1U << 20U, 1U << 20U};
constexpr std::array<std::size_t, 14> parents{0, 1, 2, 3, 3, 3, 3, 3, 4, 5, 6, 7, 3, 3};
constexpr std::array<const char*, 14> names{".local",
                                            "monitoring-qualification",
                                            "MON-007.host-v2.20260906-05",
                                            "q0b",
                                            "q1",
                                            "q2",
                                            "q3",
                                            "host-environment.json",
                                            "evidence.json",
                                            "evidence.json",
                                            "evidence.json",
                                            "evidence.json",
                                            "manifest.json",
                                            "checksums.sha256"};
constexpr std::array<std::size_t, 7> file_roles{8, 9, 10, 11, 12, 13, 14};
auto expected_inventory(std::size_t index, bool completed = false) -> std::set<std::string> {
    if (index == 0) {
        auto result = std::set<std::string>{
            "checksums.sha256", "host-environment.json", "manifest.json", "q0b", "q1", "q2", "q3"};
        if (completed) result.emplace("q4");
        return result;
    }
    return {"evidence.json"};
}
template <class Operations>
auto read_all(int fd, std::uint64_t size, std::size_t cap, const Operations& operations)
    -> std::expected<std::string, MonitoringQ4CompleteQ3AuthorityFailure> {
    if (size > cap)
        return std::unexpected{fixed(Kind::predicate)};
    try {
        std::string result(static_cast<std::size_t>(size), '\0');
        std::size_t offset{};
        while (offset < result.size()) {
            const auto count =
                operations.read_at(operations.context, fd, result.data() + offset,
                                   result.size() - offset, static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return std::unexpected{fixed(Kind::read, errno)};
            if (count == 0)
                return std::unexpected{fixed(Kind::eof, EIO)};
            if (static_cast<std::size_t>(count) > result.size() - offset)
                return std::unexpected{fixed(Kind::read, EOVERFLOW)};
            offset += static_cast<std::size_t>(count);
        }
        char probe{};
        ssize_t count{};
        do {
            count =
                operations.read_at(operations.context, fd, &probe, 1, static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            return std::unexpected{fixed(Kind::read, errno)};
        if (count != 0)
            return std::unexpected{fixed(Kind::growth, EFBIG)};
        return result;
    } catch (...) {
        return std::unexpected{fixed(Kind::contained)};
    }
}
constexpr std::array<std::string_view, 19> classifications{"operations",
                                                           "snapshot_open",
                                                           "snapshot_stat",
                                                           "snapshot_predicate",
                                                           "snapshot_inventory",
                                                           "snapshot_inventory_changed",
                                                           "snapshot_read",
                                                           "snapshot_eof",
                                                           "snapshot_growth",
                                                           "snapshot_authentication",
                                                           "snapshot_recheck",
                                                           "snapshot_held_changed",
                                                           "snapshot_named_recheck",
                                                           "snapshot_named_changed",
                                                           "snapshot_bytes_changed",
                                                           "inactive",
                                                           "contained",
                                                           "snapshot_cleanup",
                                                           "projection_failed"};
auto make_error(Kind kind, int native) -> core::Error {
    core::Error value{
        .category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.q4.complete_q3_predecessor_authority.invalid"},
        .message = "Q4 complete Q3 predecessor authority rejected",
        .operation = "authenticate_monitoring_q4_complete_q3_predecessor_authority",
        .os_error = std::nullopt,
        .context = {{"classification",
                     std::string{classifications[static_cast<std::size_t>(kind)]}}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command};
    if (native != 0)
        value.os_error = core::OsError::capture(std::error_code{native, std::generic_category()});
    return value;
}
} // namespace

MonitoringQ4CompleteQ3PredecessorAuthority::MonitoringQ4CompleteQ3PredecessorAuthority() noexcept {
    descriptors_.fill(-1);
}
MonitoringQ4CompleteQ3PredecessorAuthority::MonitoringQ4CompleteQ3PredecessorAuthority(
    MonitoringQ4CompleteQ3PredecessorAuthority&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {})}, identities_{other.identities_},
      bytes_{std::move(other.bytes_)}, value_{std::move(other.value_)},
      candidate_{std::exchange(other.candidate_, nullptr)},
      campaign_values_{other.campaign_values_}, provisional_commits_{std::move(other.provisional_commits_)},
      binding_attempted_{other.binding_attempted_}, binding_completed_{other.binding_completed_},
      history_{std::move(other.history_)}, expected_{std::move(other.expected_)},
      operations_{other.operations_}, active_{std::exchange(other.active_, {})},
      confirmed_{other.confirmed_}, uncertain_{other.uncertain_}, retirement_{other.retirement_} {
    completed_descriptors_ = std::exchange(other.completed_descriptors_, std::array<int, 2>{-1, -1});
    completed_identities_ = other.completed_identities_;
    completed_active_ = std::exchange(other.completed_active_, {});
    completed_confirmed_ = other.completed_confirmed_;
    completed_uncertain_ = other.completed_uncertain_;
    q3_prefix_ = std::move(other.q3_prefix_);
    q4_bytes_ = std::move(other.q4_bytes_);
    producer_commit_ = std::move(other.producer_commit_);
    completed_snapshot_ = other.completed_snapshot_;
    other.descriptors_.fill(-1);
}
MonitoringQ4CompleteQ3PredecessorAuthority::~MonitoringQ4CompleteQ3PredecessorAuthority() noexcept {
    if (active_.any() || completed_active_.any())
        static_cast<void>(retire());
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::retire() noexcept
    -> MonitoringQ4CompleteQ3Retirement {
    for (std::size_t reverse{}; reverse < 2U; ++reverse) {
        const auto index = 1U - reverse;
        if (!completed_active_.test(index)) continue;
        const int fd = std::exchange(completed_descriptors_[index], -1);
        completed_active_.reset(index);
        if (operations_.close_fd(operations_.context, fd) == 0)
            completed_confirmed_.set(index);
        else {
            completed_uncertain_.set(index);
            append(retirement_, index == 0U ? MonitoringQ4PredecessorRole::q4_directory :
                MonitoringQ4PredecessorRole::q4_evidence,
                MonitoringQ4PredecessorCleanupSource::descriptor, errno);
        }
    }
    for (std::size_t reverse{}; reverse < descriptor_count; ++reverse) {
        const auto role = descriptor_count - 1U - reverse;
        if (!active_.test(role))
            continue;
        const int descriptor = std::exchange(descriptors_[role], -1);
        active_.reset(role);
        if (operations_.close_fd(operations_.context, descriptor) == 0)
            confirmed_.set(role);
        else {
            uncertain_.set(role);
            append(retirement_, static_cast<MonitoringQ4PredecessorRole>(role),
                   MonitoringQ4PredecessorCleanupSource::descriptor, errno);
        }
    }
    retirement_.confirmed = confirmed_;
    retirement_.uncertain = uncertain_;
    retirement_.active = active_;
    retirement_.completed_confirmed = completed_confirmed_;
    retirement_.completed_uncertain = completed_uncertain_;
    retirement_.completed_active = completed_active_;
    return retirement_;
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::project_failure(
    const MonitoringQ4CompleteQ3AuthorityFailure& failure)
    -> std::expected<MonitoringQ4CompleteQ3ProjectedFailure,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return project_failure_with(failure, {nullptr, no_fault});
}
auto MonitoringQ4CompleteQ3PredecessorAuthority::project_failure_with(
    const MonitoringQ4CompleteQ3AuthorityFailure& failure, ProjectionOperations operations)
    -> std::expected<MonitoringQ4CompleteQ3ProjectedFailure,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    if (failure.retirement.active.any() || failure.retirement.completed_active.any()) {
        auto projected = fixed(Kind::projection_failed);
        projected.retirement = failure.retirement;
        return std::unexpected{projected};
    }
    try {
        operations.fault(operations.context, 0U);
        MonitoringQ4CompleteQ3ProjectedFailure result{
            make_error(failure.primary, failure.native_error)};
        result.retirement = failure.retirement;
        result.cleanup_count = failure.retirement.cleanup_count;
        for (std::size_t index{}; index < result.cleanup_count; ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(index + 1U));
            const auto fact = failure.retirement.cleanup_facts[index];
            result.cleanup[index].emplace(MonitoringQ4PredecessorCleanupRecord{
                fact.role, fact.source, make_error(Kind::cleanup, fact.native_error)});
        }
        return result;
    } catch (...) {
        auto projected = fixed(Kind::projection_failed);
        projected.retirement = failure.retirement;
        return std::unexpected{projected};
    }
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::production_inventory(void* context, int descriptor)
    -> std::expected<std::set<std::string>, InventoryFailure> {
    return inventory(context, descriptor);
}

void MonitoringQ4CompleteQ3PredecessorAuthority::set_inventory_test_leaves(
    void (*fault)(), int (*close)(void*) noexcept) noexcept {
    inventory_test_fault = fault;
    inventory_test_close = close;
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return retain_with(root.descriptor(), candidate, history,
                       {nullptr, duplicate, open_at, stat_fd, stat_at, read_at, production_inventory, close_fd,
                        effective_user, no_fault},
                       nullptr);
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::retain_with(
    int root, const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history, Operations operations,
    const MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests* expected)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return retain_impl(root, candidate, &history, operations, expected, nullptr);
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::capture_campaign(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ1ValidatedPublicValues& values)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return capture_with(root.descriptor(), candidate, values,
        {nullptr, duplicate, open_at, stat_fd, stat_at, read_at, production_inventory,
         close_fd, effective_user, no_fault});
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::capture_with(
    int root, const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ1ValidatedPublicValues& values, Operations operations)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return retain_impl(root, candidate, nullptr, operations, nullptr, &values);
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::capture_completed(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ1ValidatedPublicValues& values)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    return retain_impl(root.descriptor(), candidate, nullptr,
        {nullptr, duplicate, open_at, stat_fd, stat_at, read_at, production_inventory,
         close_fd, effective_user, no_fault}, nullptr, &values, true);
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::retain_impl(
    int root, const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue* history, Operations operations,
    const MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests* expected,
    const MonitoringQ1ValidatedPublicValues* values, bool completed)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    MonitoringQ4CompleteQ3PredecessorAuthority result;
    result.operations_ = operations;
    result.campaign_values_ = values;
    result.candidate_ = &candidate;
    result.completed_snapshot_ = completed;
    const auto reject = [&](MonitoringQ4CompleteQ3AuthorityFailure failure) noexcept {
        const auto retired = result.retire();
        for (std::size_t index{}; index < retired.cleanup_count; ++index)
            append(failure.retirement, retired.cleanup_facts[index].role,
                   retired.cleanup_facts[index].source, retired.cleanup_facts[index].native_error);
        failure.retirement.confirmed = retired.confirmed;
        failure.retirement.uncertain = retired.uncertain;
        failure.retirement.active = retired.active;
        failure.retirement.completed_confirmed = retired.completed_confirmed;
        failure.retirement.completed_uncertain = retired.completed_uncertain;
        failure.retirement.completed_active = retired.completed_active;
        failure.retirement.cleanup_overflowed |= retired.cleanup_overflowed;
        return std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                             MonitoringQ4CompleteQ3AuthorityFailure>{std::unexpected{failure}};
    };
    if (!operations.duplicate || !operations.open_at || !operations.stat_fd ||
        !operations.stat_at || !operations.read_at || !operations.inventory ||
        !operations.close_fd || !operations.effective_user || !operations.fault)
        return reject(fixed(Kind::operations));
    try {
        if (completed && (!values || history || expected))
            return reject(fixed(Kind::authentication));
        if (expected)
            result.expected_ = *expected;
        // Rechecks outlive caller history moves; retain its authenticated facts and binding.
        if (history)
            result.history_.emplace(MonitoringQ4HistoricalGitValue{history->commits_, history->binding_});
        if (values && (!MonitoringQ4CompleteQ3PredecessorAuthenticator::candidate_active(candidate) || (values->qualification_id() != "MON-007.host-v2.campaign-20261005-01" && values->qualification_id() != "MON-007.host-v2.campaign-20261018-01" && values->qualification_id() != "MON-007.host-v2.campaign-20261020-01" && values->qualification_id() != "MON-007.host-v2.campaign-20261024-01" && values->qualification_id() != "MON-007.host-v2.campaign-20261102-01") ||
            values->profile_sha256() != candidate.profile().sha256.to_hex()))
            return reject(fixed(Kind::authentication));
        operations.fault(operations.context, 0U);
        result.descriptors_[0] = operations.duplicate(operations.context, root);
        if (result.descriptors_[0] < 0)
            return reject(fixed(Kind::open, errno));
        result.active_.set(0);
        constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        constexpr int file_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        for (std::size_t index{}; index < names.size(); ++index) {
            const auto role = index + 1U;
            operations.fault(operations.context, static_cast<unsigned int>(role));
            result.descriptors_[role] =
                operations.open_at(operations.context, result.descriptors_[parents[index]],
                                   values && index == 2U ? values->qualification_id().data() : names[index],
                                   role < 8U ? directory_flags : file_flags);
            if (result.descriptors_[role] < 0)
                return reject(fixed(Kind::open, errno));
            result.active_.set(role);
        }
        const auto owner =
            static_cast<std::uint32_t>(operations.effective_user(operations.context));
        for (std::size_t role{}; role < descriptor_count; ++role) {
            operations.fault(operations.context, static_cast<unsigned int>(15U + role));
            struct stat status{};
            if (operations.stat_fd(operations.context, result.descriptors_[role], &status) != 0)
                return reject(fixed(Kind::stat, errno));
            result.identities_[role] = identity(status);
            if (role == 0U)
                continue;
            const bool directory = role < 8U;
            if ((directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) ||
                static_cast<std::uint32_t>(status.st_uid) != owner ||
                (!directory && status.st_nlink != 1) ||
                (static_cast<std::uint32_t>(status.st_mode) & 07777U) !=
                    (directory ? 0700U : 0600U) ||
                (!directory && (status.st_size < 0 ||
                                static_cast<std::uint64_t>(status.st_size) > caps[role - 8U])))
                return reject(fixed(Kind::predicate));
        }
        for (std::size_t index{}; index < 5U; ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(30U + index));
            auto actual = operations.inventory(operations.context, result.descriptors_[index + 3U]);
            if (!actual) {
                auto failure = fixed(actual.error().primary, actual.error().native_error);
                if (actual.error().cleanup) {
                    auto fact = *actual.error().cleanup;
                    fact.role = static_cast<MonitoringQ4PredecessorRole>(15U + index);
                    append(failure.retirement, fact.role, fact.source, fact.native_error);
                }
                return reject(failure);
            }
            if (*actual != expected_inventory(index, completed))
                return reject(fixed(Kind::inventory_changed));
        }
        if (completed) {
            result.completed_descriptors_[0] = operations.open_at(
                operations.context, result.descriptors_[3], "q4", directory_flags);
            if (result.completed_descriptors_[0] < 0) return reject(fixed(Kind::open, errno));
            result.completed_active_.set(0);
            result.completed_descriptors_[1] = operations.open_at(
                operations.context, result.completed_descriptors_[0], "evidence.json", file_flags);
            if (result.completed_descriptors_[1] < 0) return reject(fixed(Kind::open, errno));
            result.completed_active_.set(1);
            for (std::size_t i{}; i < 2U; ++i) {
                struct stat status{};
                if (operations.stat_fd(operations.context, result.completed_descriptors_[i], &status) != 0)
                    return reject(fixed(Kind::stat, errno));
                if ((i == 0U ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) ||
                    static_cast<std::uint32_t>(status.st_uid) != owner ||
                    (static_cast<std::uint32_t>(status.st_mode) & 07777U) != (i == 0U ? 0700U : 0600U) ||
                    (i != 0U && (status.st_nlink != 1 || status.st_size <= 0)))
                    return reject(fixed(Kind::predicate));
                result.completed_identities_[i] = identity(status);
            }
            // Eight exact leaves, including manifest/checksums. This local tree bound
            // does not authenticate historical live allocation or other attempt roots.
            constexpr std::uint64_t retained_bytes = 268435456U;
            static_assert(8U <= 64U);
            std::uint64_t total{};
            for (const auto role : file_roles) {
                const auto size = result.identities_[role].size;
                if (size == 0U || size > retained_bytes - total)
                    return reject(fixed(Kind::predicate));
                total += size;
            }
            const auto size = result.completed_identities_[1].size;
            if (size > retained_bytes - total) return reject(fixed(Kind::predicate));
            auto actual = operations.inventory(operations.context, result.completed_descriptors_[0]);
            if (!actual) {
                auto failure = fixed(actual.error().primary, actual.error().native_error);
                if (actual.error().cleanup) append(failure.retirement,
                    MonitoringQ4PredecessorRole::q4_inventory,
                    actual.error().cleanup->source, actual.error().cleanup->native_error);
                return reject(failure);
            }
            if (*actual != expected_inventory(1U)) return reject(fixed(Kind::inventory_changed));
            auto q4 = read_all(result.completed_descriptors_[1], size,
                static_cast<std::size_t>(retained_bytes - total), operations);
            if (!q4) return reject(q4.error());
            result.q4_bytes_ = std::move(*q4);
        }
        for (std::size_t index{}; index < 7U; ++index) {
            operations.fault(operations.context, static_cast<unsigned int>(35U + index));
            auto bytes =
                read_all(result.descriptors_[file_roles[index]],
                         result.identities_[file_roles[index]].size, caps[index], operations);
            if (!bytes)
                return reject(bytes.error());
            result.bytes_[index] = std::move(*bytes);
        }
        if (completed) {
            auto prefix = MonitoringQ4CompleteQ3PredecessorAuthenticator::completed_prefix(
                result.bytes_, result.q4_bytes_, candidate, *values);
            if (!prefix) return reject(fixed(Kind::authentication));
            result.q3_prefix_ = std::move(prefix->manifest_checksums);
            result.producer_commit_ = std::move(prefix->producer_commit);
        }
        MonitoringQ4CompleteQ3PredecessorInput input{
            result.bytes_[0], result.bytes_[1], result.bytes_[2], result.bytes_[3],
            result.bytes_[4], result.bytes_[5], result.bytes_[6]};
        operations.fault(operations.context, 42U);
        if (values && !history) {
            // Claims are provisional: only the later Git and same-byte semantic
            // binding can produce a predecessor value. No descriptor is recaptured.
            using Artifact = MonitoringQ4CompleteQ3PredecessorAuthenticator::CanonicalArtifactRole;
            if (!MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_environment(result.bytes_[0]) ||
                !MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_outer_canonical(
                    Artifact::q1_evidence, result.bytes_[2]) ||
                !MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_outer_canonical(
                    Artifact::q2_evidence, result.bytes_[3]) ||
                !MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_outer_canonical(
                    Artifact::q3_evidence, result.bytes_[4]) ||
                !validate_monitoring_q2_context_switch_evidence(result.bytes_[3]) ||
                !validate_monitoring_q3_temperature_evidence_json(result.bytes_[4]))
                return reject(fixed(Kind::authentication));
            auto q1 = nlohmann::json::parse(result.bytes_[2]);
            core::detail::JsonCleanup q1_cleanup{q1};
            auto q2 = nlohmann::json::parse(result.bytes_[3]);
            core::detail::JsonCleanup q2_cleanup{q2};
            auto q3 = nlohmann::json::parse(result.bytes_[4]);
            core::detail::JsonCleanup q3_cleanup{q3};
            if (!q1.at("repository").is_object() || q1.at("repository").size() != 2U ||
                !q1.at("repository").contains("commit") || !q1.at("repository").contains("dirty") ||
                q1.at("repository").at("dirty") != false ||
                q1.at("qualification_id") != values->qualification_id() ||
                q2.at("qualification_id") != values->qualification_id() ||
                q3.at("qualification_id") != values->qualification_id())
                return reject(fixed(Kind::authentication));
            result.provisional_commits_ = {values->q0().repository_commit,
                q1.at("repository").at("commit").get<std::string>(),
                q2.at("provenance").at("repository_commit").get<std::string>(),
                q3.at("provenance").at("repository_commit").get<std::string>()};
            auto checked = result.recheck();
            if (!checked || !*checked)
                return reject(checked ? fixed(Kind::held_changed) : checked.error());
            return result;
        }
        auto authenticated =
            expected ? MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_with(
                           std::move(input), candidate, *result.history_, *expected)
                     : MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate(
                           std::move(input), candidate, *result.history_);
        if (!authenticated)
            return reject(fixed(Kind::authentication));
        result.value_.emplace(std::move(*authenticated));
        auto checked = result.recheck();
        if (!checked || !*checked)
            return reject(checked ? fixed(Kind::held_changed) : checked.error());
        return result;
    } catch (...) {
        return reject(fixed(Kind::contained));
    }
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::recheck() const noexcept
    -> std::expected<bool, MonitoringQ4CompleteQ3AuthorityFailure> {
    if (!active_.all() || (completed_snapshot_ && !completed_active_.all()))
        return std::unexpected{fixed(Kind::inactive)};
    const auto failure = [&](MonitoringQ4CompleteQ3AuthorityFailure value) {
        value.retirement.confirmed = confirmed_;
        value.retirement.uncertain = uncertain_;
        value.retirement.active = active_;
        value.retirement.completed_confirmed = completed_confirmed_;
        value.retirement.completed_uncertain = completed_uncertain_;
        value.retirement.completed_active = completed_active_;
        return std::expected<bool, MonitoringQ4CompleteQ3AuthorityFailure>{std::unexpected{value}};
    };
    try {
        for (std::size_t role{}; role < descriptor_count; ++role) {
            operations_.fault(operations_.context, static_cast<unsigned int>(100U + role));
            struct stat status{};
            if (operations_.stat_fd(operations_.context, descriptors_[role], &status) != 0)
                return failure(fixed(Kind::held_recheck, errno));
            if (identity(status) != identities_[role])
                return false;
        }
        for (std::size_t index{}; index < names.size(); ++index) {
            operations_.fault(operations_.context, static_cast<unsigned int>(115U + index));
            struct stat status{};
            if (operations_.stat_at(operations_.context, descriptors_[parents[index]],
                                    campaign_values_ && index == 2U ? campaign_values_->qualification_id().data() : names[index],
                                    &status, AT_SYMLINK_NOFOLLOW) != 0)
                return failure(fixed(Kind::named_recheck, errno));
            if (identity(status) != identities_[index + 1U])
                return false;
        }
        for (std::size_t index{}; index < 5U; ++index) {
            operations_.fault(operations_.context, static_cast<unsigned int>(129U + index));
            auto actual = operations_.inventory(operations_.context, descriptors_[index + 3U]);
            if (!actual) {
                auto value = fixed(actual.error().primary, actual.error().native_error);
                value.retirement.active = active_;
                value.retirement.completed_confirmed = completed_confirmed_;
                value.retirement.completed_uncertain = completed_uncertain_;
                value.retirement.completed_active = completed_active_;
                if (actual.error().cleanup) {
                    const auto fact = *actual.error().cleanup;
                    append(value.retirement, static_cast<MonitoringQ4PredecessorRole>(15U + index),
                           fact.source, fact.native_error);
                }
                return std::unexpected{value};
            }
            if (*actual != expected_inventory(index, completed_snapshot_))
                return false;
        }
        if (completed_snapshot_) {
            for (std::size_t i{}; i < 2U; ++i) {
                struct stat held{}, named{};
                if (operations_.stat_fd(operations_.context, completed_descriptors_[i], &held) != 0)
                    return failure(fixed(Kind::held_recheck, errno));
                if (identity(held) != completed_identities_[i]) return false;
                const auto parent = i == 0U ? descriptors_[3] : completed_descriptors_[0];
                const auto* name = i == 0U ? "q4" : "evidence.json";
                if (operations_.stat_at(operations_.context, parent, name, &named,
                                        AT_SYMLINK_NOFOLLOW) != 0)
                    return failure(fixed(Kind::named_recheck, errno));
                if (identity(named) != completed_identities_[i]) return false;
            }
            auto inventory_result = operations_.inventory(operations_.context, completed_descriptors_[0]);
            if (!inventory_result) {
                auto fact = fixed(inventory_result.error().primary, inventory_result.error().native_error);
                if (inventory_result.error().cleanup) append(fact.retirement,
                    MonitoringQ4PredecessorRole::q4_inventory,
                    inventory_result.error().cleanup->source, inventory_result.error().cleanup->native_error);
                return failure(fact);
            }
            if (*inventory_result != expected_inventory(1U)) return false;
            auto raw = read_all(completed_descriptors_[1], completed_identities_[1].size,
                                268435456U, operations_);
            if (!raw) return failure(raw.error());
            if (*raw != q4_bytes_) return false;
        }
        std::array<std::string, 7> reread{};
        for (std::size_t index{}; index < 7U; ++index) {
            operations_.fault(operations_.context, static_cast<unsigned int>(134U + index));
            auto bytes = read_all(descriptors_[file_roles[index]],
                                  identities_[file_roles[index]].size, caps[index], operations_);
            if (!bytes)
                return failure(bytes.error());
            reread[index] = std::move(*bytes);
            if (reread[index] != bytes_[index])
                return false;
        }
        if (completed_snapshot_) {
            auto prefix = MonitoringQ4CompleteQ3PredecessorAuthenticator::completed_prefix(
                reread, q4_bytes_, *candidate_, *campaign_values_);
            if (!prefix || prefix->manifest_checksums != q3_prefix_ ||
                prefix->producer_commit != producer_commit_) return false;
        }
        if (campaign_values_ && !history_)
            return true; // Capture-only; finalization remains unavailable before binding.
        MonitoringQ4CompleteQ3PredecessorInput input{reread[0], reread[1], reread[2], reread[3],
            reread[4], completed_snapshot_ ? q3_prefix_[0] : reread[5],
            completed_snapshot_ ? q3_prefix_[1] : reread[6]};
        operations_.fault(operations_.context, 141U);
        auto authenticated = campaign_values_
            ? MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_campaign(
                  std::move(input), *candidate_, *history_, *campaign_values_)
            : expected_ ? MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_with(
                            std::move(input), *candidate_, *history_, *expected_)
                      : MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate(
                            std::move(input), *candidate_, *history_);
        if (!authenticated)
            return failure(fixed(Kind::authentication));
        return true;
    } catch (...) {
        return failure(fixed(Kind::contained));
    }
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::bind_campaign(
    const MonitoringQ4HistoricalGitValue& history)
    -> std::expected<void, MonitoringQ4CompleteQ3AuthorityFailure> {
    if (!campaign_values_ || !active_.all() ||
        (completed_snapshot_ && !completed_active_.all()) || history_ || value_ || binding_attempted_)
        return std::unexpected{fixed(Kind::inactive)};
    binding_attempted_ = true; // Every entered semantic binding attempt is consuming.
    try {
        const std::array<std::string_view, 4> commits{history.q0_commit(), history.q1_commit(),
                                                     history.q2_commit(), history.q3_commit()};
        for (std::size_t index{}; index < commits.size(); ++index)
            if (commits[index] != provisional_commits_[index])
                return std::unexpected{fixed(Kind::authentication)};
        if (completed_snapshot_ && history.completed_q4_producer_commit() != producer_commit_)
            return std::unexpected{fixed(Kind::authentication)};
        auto checked = recheck();
        if (!checked || !*checked)
            return std::unexpected{checked ? fixed(Kind::held_changed) : checked.error()};
        history_.emplace(MonitoringQ4HistoricalGitValue{history.commits_, history.binding_});
        history_->completed_q4_producer_ = history.completed_q4_producer_;
        MonitoringQ4CompleteQ3PredecessorInput input{bytes_[0], bytes_[1], bytes_[2], bytes_[3],
            bytes_[4], completed_snapshot_ ? q3_prefix_[0] : bytes_[5],
            completed_snapshot_ ? q3_prefix_[1] : bytes_[6]};
        auto value = MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_campaign(
            std::move(input), *candidate_, *history_, *campaign_values_);
        if (!value)
            return std::unexpected{fixed(Kind::authentication)};
        value_.emplace(std::move(*value));
        checked = recheck();
        if (!checked || !*checked)
            return std::unexpected{checked ? fixed(Kind::held_changed) : checked.error()};
        binding_completed_ = true;
        return {};
    } catch (...) { return std::unexpected{fixed(Kind::contained)}; }
}

auto MonitoringQ4CompleteQ3PredecessorAuthority::finalize() noexcept
    -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                     MonitoringQ4CompleteQ3AuthorityFailure> {
    auto checked = recheck();
    if (!value_ || (campaign_values_ && !binding_completed_) || !checked || !*checked) {
        auto result = checked ? fixed(Kind::held_changed) : checked.error();
        const auto retired = retire();
        for (std::size_t index{}; index < retired.cleanup_count; ++index)
            append(result.retirement, retired.cleanup_facts[index].role,
                   retired.cleanup_facts[index].source, retired.cleanup_facts[index].native_error);
        result.retirement.confirmed = retired.confirmed;
        result.retirement.uncertain = retired.uncertain;
        result.retirement.active = retired.active;
        result.retirement.completed_confirmed = retired.completed_confirmed;
        result.retirement.completed_uncertain = retired.completed_uncertain;
        result.retirement.completed_active = retired.completed_active;
        return std::unexpected{result};
    }
    const auto retired = retire();
    if (!retired.confirmed.all() || retired.uncertain.any() || retired.active.any() ||
        (completed_snapshot_ && !retired.completed_confirmed.all()) ||
        retired.completed_uncertain.any() || retired.completed_active.any()) {
        auto result = fixed(Kind::cleanup);
        result.retirement = retired;
        return std::unexpected{result};
    }
    if (!value_)
        return std::unexpected{fixed(Kind::inactive)};
    return std::move(*value_);
}
} // namespace epycbench::monitoring::qualification::detail
