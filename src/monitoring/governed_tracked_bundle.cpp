#include "monitoring/governed_tracked_bundle.hpp"

#include <cerrno>
#include <fcntl.h>
#include <type_traits>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
static_assert(std::is_nothrow_move_constructible_v<std::vector<core::Sha256Digest>>);
static_assert(std::is_nothrow_move_constructible_v<MonitoringGovernedTrackedBundleCore>);
namespace {
constexpr std::array<std::string_view, 18> paths{
    "config/monitoring-qualification/host-v2-profile.json",
    "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json",
    "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json",
    "schemas/monitoring-q2-context-switch-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q2-backend-result-v1.schema.json",
    "schemas/monitoring-host-q2-backend-error-v1.schema.json",
    "schemas/monitoring-host-q2-backend-authored-v1.schema.json",
    "config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json",
    "schemas/monitoring-q3-temperature-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q3-temperature-evidence-v1.schema.json",
    "schemas/monitoring-host-q3-backend-result-v1.schema.json",
    "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json",
    "schemas/monitoring-q4-frequency-collection-plan-v1.schema.json",
    "schemas/monitoring-host-q4-frequency-evidence-v1.schema.json",
    "schemas/monitoring-host-q4-backend-result-v1.schema.json",
    "schemas/monitoring-host-q4-backend-error-v1.schema.json",
    "schemas/monitoring-host-q4-backend-authored-v1.schema.json",
    "schemas/monitoring-host-q2-backend-result-v1-evidence-v2.schema.json"};
constexpr std::array<std::size_t, 18> caps{
    1U << 20U, 65536U,    65536U, 2U << 20U, 2U << 20U, 2U << 20U, 2U << 20U, 65536U, 2U << 20U,
    2U << 20U, 2U << 20U, 65536U, 65536U,    65536U,    65536U,    65536U,    65536U,
    2U << 20U};
constexpr std::array<int, 18> parents{2, 2, 2, 3, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3, 3, 3, 3};
constexpr std::array<std::string_view, 11> failure_classes{
    "operations", "tracked_entry",     "directory",       "leaf",    "identity", "read", "growth",
    "mutation",   "contained_failure", "cleanup_failure", "inactive"};

auto leaf(std::string_view path) noexcept -> std::string_view {
    return path.substr(path.rfind('/') + 1U);
}
auto identity(const struct stat& value) noexcept -> MonitoringGovernedTrackedIdentity {
    return {static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size),
            static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),
            static_cast<std::uint32_t>(value.st_nlink),
            static_cast<std::int64_t>(value.st_mtim.tv_sec),
            static_cast<std::int64_t>(value.st_mtim.tv_nsec),
            static_cast<std::int64_t>(value.st_ctim.tv_sec),
            static_cast<std::int64_t>(value.st_ctim.tv_nsec)};
}
auto failure(std::string classification, int native = 0) -> MonitoringQ1GitCommandFailure {
    std::optional<core::OsError> os;
    if (native != 0)
        os = core::OsError::capture(std::error_code{native, std::generic_category()});
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q4_plan_authority.repository_rejected"},
             .message = "Q4 governed tracked bundle rejected",
             .operation = "authenticate_monitoring_q4_plan",
             .os_error = std::move(os),
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto failure_kind(const std::string_view value) noexcept
    -> MonitoringGovernedTrackedBundleFailureKind {
    if (value == "operations")
        return MonitoringGovernedTrackedBundleFailureKind::operations;
    if (value == "tracked_entry")
        return MonitoringGovernedTrackedBundleFailureKind::tracked_entry;
    if (value == "directory")
        return MonitoringGovernedTrackedBundleFailureKind::directory;
    if (value == "leaf")
        return MonitoringGovernedTrackedBundleFailureKind::leaf;
    if (value == "identity")
        return MonitoringGovernedTrackedBundleFailureKind::identity;
    if (value == "read")
        return MonitoringGovernedTrackedBundleFailureKind::read;
    if (value == "growth")
        return MonitoringGovernedTrackedBundleFailureKind::growth;
    if (value == "mutation")
        return MonitoringGovernedTrackedBundleFailureKind::mutation;
    if (value == "cleanup_failure")
        return MonitoringGovernedTrackedBundleFailureKind::cleanup_failure;
    if (value == "inactive")
        return MonitoringGovernedTrackedBundleFailureKind::inactive;
    return MonitoringGovernedTrackedBundleFailureKind::contained_failure;
}
auto failure_index(const std::string_view value) noexcept -> std::size_t {
    const auto found = std::ranges::find(failure_classes, value);
    return found == failure_classes.end()
               ? 8U
               : static_cast<std::size_t>(found - failure_classes.begin());
}
auto valid_directory(const struct stat& value, unsigned int euid) noexcept -> bool {
    return S_ISDIR(value.st_mode) && value.st_uid == euid && (value.st_mode & 0022) == 0;
}
auto exact_mode(std::size_t slot) noexcept -> std::optional<unsigned int> {
    if (slot <= 2U)
        return 0600U;
    if (slot == 7U || slot == 11U)
        return 0644U;
    return std::nullopt;
}
auto valid_leaf(const struct stat& value, unsigned int euid, std::size_t slot) noexcept -> bool {
    if (!S_ISREG(value.st_mode) || value.st_uid != euid || value.st_nlink != 1 ||
        value.st_size < 0 || static_cast<std::uint64_t>(value.st_size) > caps[slot])
        return false;
    const auto required = exact_mode(slot);
    return !required || (static_cast<unsigned int>(value.st_mode) & 07777U) == *required;
}
auto named_parent(std::size_t role) noexcept -> std::size_t {
    constexpr std::array<std::size_t, 4> directory_parents{0U, 0U, 1U, 0U};
    if (role < 4U)
        return directory_parents[role];
    return static_cast<std::size_t>(parents[role - 4U]);
}
auto role_name(std::size_t role) noexcept -> std::string_view {
    constexpr std::array<std::string_view, 4> directories{".", "config", "monitoring-qualification",
                                                          "schemas"};
    return role < 4U ? directories[role] : leaf(paths[role - 4U]);
}
} // namespace

MonitoringGovernedTrackedBundleCore::MonitoringGovernedTrackedBundleCore(
    std::array<int, descriptor_count> descriptors,
    std::array<MonitoringGovernedTrackedIdentity, descriptor_count> identities,
    std::array<std::string, leaf_count> bytes, std::vector<core::Sha256Digest> digests,
    std::string head, std::shared_ptr<const void> binding, Operations operations,
    MonitoringQ1GitCommandFailure retirement_diagnostic,
    std::array<core::Error, descriptor_count> retirement_cleanup) noexcept
    : descriptors_{descriptors}, identities_{identities}, bytes_{std::move(bytes)},
      digests_{std::move(digests)}, head_{std::move(head)}, capture_binding_{std::move(binding)},
      operations_{operations}, retirement_diagnostic_{std::move(retirement_diagnostic)},
      retirement_cleanup_{std::move(retirement_cleanup)} {
    active_.set();
}
MonitoringGovernedTrackedBundleCore::MonitoringGovernedTrackedBundleCore(
    MonitoringGovernedTrackedBundleCore&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {})}, identities_{other.identities_},
      bytes_{std::move(other.bytes_)}, digests_{std::move(other.digests_)},
      head_{std::move(other.head_)}, capture_binding_{std::move(other.capture_binding_)},
      operations_{other.operations_},
      retirement_diagnostic_{std::move(other.retirement_diagnostic_)},
      retirement_cleanup_{std::move(other.retirement_cleanup_)},
      active_{std::exchange(other.active_, {})}, confirmed_{std::exchange(other.confirmed_, {})},
      uncertain_{std::exchange(other.uncertain_, {})} {
    other.descriptors_.fill(-1);
}
MonitoringGovernedTrackedBundleCore::~MonitoringGovernedTrackedBundleCore() noexcept {
    if (active_.any())
        static_cast<void>(retire());
}

auto MonitoringGovernedTrackedBundleCore::retain(int root,
                                                 MonitoringCurrentRevisionTrackedCapture&& capture,
                                                 Operations operations)
    -> std::expected<MonitoringGovernedTrackedBundleCore, MonitoringGovernedTrackedBundleFailure> {
    std::array<int, descriptor_count> descriptors{};
    descriptors.fill(-1);
    std::array<MonitoringQ1GitCommandFailure, failure_classes.size()> diagnostic_stock{};
    std::array<core::Error, descriptor_count> retirement_cleanup{};
    try {
        for (std::size_t index{}; index < failure_classes.size(); ++index) {
            diagnostic_stock[index] = failure(std::string{failure_classes[index]});
            diagnostic_stock[index].cleanup.reserve(descriptor_count);
        }
        for (auto& value : retirement_cleanup)
            value = failure("cleanup_failure").primary;
    } catch (...) {
        MonitoringGovernedTrackedBundleFailure contained{};
        contained.kind = MonitoringGovernedTrackedBundleFailureKind::contained_failure;
        return std::unexpected{std::move(contained)};
    }
    const auto reject = [&](std::string classification, int native = 0) {
        MonitoringGovernedTrackedBundleFailure projected{};
        projected.kind = failure_kind(classification);
        projected.native_error = native;
        for (std::size_t reverse{}; reverse < descriptor_count; ++reverse) {
            const auto role = descriptor_count - reverse - 1U;
            const int descriptor = std::exchange(descriptors[role], -1);
            if (descriptor < 0)
                continue;
            if (operations.close_fd && operations.close_fd(operations.context, descriptor) == 0) {
                projected.retirement.confirmed.set(role);
            } else {
                projected.retirement.uncertain.set(role);
                const auto index = projected.retirement.cleanup_count++;
                projected.retirement.cleanup_roles[index] = static_cast<std::uint8_t>(role);
                projected.retirement.cleanup_errors[index] = operations.close_fd ? errno : 0;
            }
        }
        try {
            if (operations.fault)
                operations.fault(operations.context, 37U);
            projected.diagnostic = std::move(diagnostic_stock[failure_index(classification)]);
            if (native != 0)
                projected.diagnostic.primary.os_error =
                    core::OsError::capture(std::error_code{native, std::generic_category()});
            for (std::size_t index{}; index < projected.retirement.cleanup_count; ++index)
                projected.diagnostic.cleanup.push_back(
                    failure("cleanup_failure", projected.retirement.cleanup_errors[index]).primary);
        } catch (...) {
            projected.diagnostic.primary.category = core::ErrorCategory::monitoring;
            projected.diagnostic.primary.severity = core::ErrorSeverity::error;
            projected.diagnostic.primary.continuation = core::ContinuationPolicy::abort_command;
        }
        return std::expected<MonitoringGovernedTrackedBundleCore,
                             MonitoringGovernedTrackedBundleFailure>{
            std::unexpected{std::move(projected)}};
    };
    if (!operations.open_at || !operations.stat_fd || !operations.stat_at || !operations.read_at ||
        !operations.close_fd || !operations.effective_user || !operations.fault)
        return reject("operations");
    try {
        operations.fault(operations.context, 0);
        if (capture.records_.size() != leaf_count)
            return reject("tracked_entry");
        for (std::size_t slot{}; slot < leaf_count; ++slot)
            if (capture.records_[slot].path_ != paths[slot] ||
                capture.records_[slot].bytes_.size() > caps[slot])
                return reject("tracked_entry");
        constexpr std::array<std::string_view, 4> directory_names{
            ".", "config", "monitoring-qualification", "schemas"};
        constexpr std::array<int, 4> directory_parents{-1, 0, 1, 0};
        for (std::size_t role{}; role < directory_count; ++role) {
            const int parent =
                role == 0U ? root : descriptors[static_cast<std::size_t>(directory_parents[role])];
            descriptors[role] =
                operations.open_at(operations.context, parent, directory_names[role].data(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (descriptors[role] < 0)
                return reject("directory", errno);
        }
        std::array<MonitoringGovernedTrackedIdentity, descriptor_count> identities{};
        const auto euid = operations.effective_user(operations.context);
        for (std::size_t role{}; role < directory_count; ++role) {
            struct stat value{};
            if (operations.stat_fd(operations.context, descriptors[role], &value) != 0 ||
                !valid_directory(value, euid))
                return reject("identity", errno);
            identities[role] = identity(value);
        }
        std::array<std::string, leaf_count> bytes{};
        std::vector<core::Sha256Digest> digests;
        digests.reserve(leaf_count);
        for (std::size_t slot{}; slot < leaf_count; ++slot) {
            const std::size_t role = directory_count + slot;
            descriptors[role] = operations.open_at(
                operations.context, descriptors[static_cast<std::size_t>(parents[slot])],
                leaf(paths[slot]).data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (descriptors[role] < 0)
                return reject("leaf", errno);
            struct stat value{};
            if (operations.stat_fd(operations.context, descriptors[role], &value) != 0 ||
                !valid_leaf(value, euid, slot))
                return reject("identity", errno);
            identities[role] = identity(value);
            operations.fault(operations.context, static_cast<unsigned int>(1U + slot));
            bytes[slot].resize(static_cast<std::size_t>(value.st_size));
            std::size_t offset{};
            while (offset < bytes[slot].size()) {
                const auto count = operations.read_at(
                    operations.context, descriptors[role], bytes[slot].data() + offset,
                    bytes[slot].size() - offset, static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return reject("read", count < 0 ? errno : EIO);
                if (static_cast<std::size_t>(count) > bytes[slot].size() - offset)
                    return reject("growth", EFBIG);
                offset += static_cast<std::size_t>(count);
            }
            char probe{};
            ssize_t trailing{};
            do {
                trailing = operations.read_at(operations.context, descriptors[role], &probe, 1,
                                              static_cast<off_t>(offset));
            } while (trailing < 0 && errno == EINTR);
            if (trailing != 0)
                return reject("growth", trailing < 0 ? errno : EFBIG);
            if (bytes[slot] != capture.records_[slot].bytes_)
                return reject("tracked_entry");
            operations.fault(operations.context, static_cast<unsigned int>(19U + slot));
            digests.push_back(core::sha256(bytes[slot]));
        }
        for (std::size_t role{}; role < descriptor_count; ++role) {
            struct stat held{}, named{};
            if (operations.stat_fd(operations.context, descriptors[role], &held) != 0 ||
                operations.stat_at(operations.context, descriptors[named_parent(role)],
                                   role_name(role).data(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                identity(held) != identities[role] || identity(named) != identities[role])
                return reject("mutation", errno);
        }
        operations.fault(operations.context, 37U);
        return MonitoringGovernedTrackedBundleCore{
            descriptors,
            identities,
            std::move(bytes),
            std::move(digests),
            std::move(capture.head_),
            std::move(capture.frame_),
            operations,
            std::move(diagnostic_stock[failure_index("cleanup_failure")]),
            std::move(retirement_cleanup)};
    } catch (...) {
        return reject("contained_failure");
    }
}

auto MonitoringGovernedTrackedBundleCore::recheck() const
    -> std::expected<bool, MonitoringGovernedTrackedBundleFailure> {
    const auto rejected = [&](MonitoringGovernedTrackedBundleFailureKind kind, int native = 0) {
        MonitoringGovernedTrackedBundleFailure result{};
        result.kind = kind;
        result.native_error = native;
        result.retirement.active = active_;
        return std::expected<bool, MonitoringGovernedTrackedBundleFailure>{
            std::unexpected{std::move(result)}};
    };
    if (!active_.all())
        return rejected(MonitoringGovernedTrackedBundleFailureKind::inactive);
    try {
        operations_.fault(operations_.context, 40U);
        const auto euid = operations_.effective_user(operations_.context);
        for (std::size_t role{}; role < descriptor_count; ++role) {
            struct stat held{}, named{};
            if (operations_.stat_fd(operations_.context, descriptors_[role], &held) != 0 ||
                operations_.stat_at(operations_.context, descriptors_[named_parent(role)],
                                    role_name(role).data(), &named, AT_SYMLINK_NOFOLLOW) != 0)
                return rejected(MonitoringGovernedTrackedBundleFailureKind::mutation, errno);
            const bool valid = role < directory_count
                                   ? valid_directory(held, euid)
                                   : valid_leaf(held, euid, role - directory_count);
            if (!valid || identity(held) != identities_[role] ||
                identity(named) != identities_[role])
                return false;
        }
        for (std::size_t slot{}; slot < leaf_count; ++slot) {
            operations_.fault(operations_.context, static_cast<unsigned int>(41U + slot));
            std::string repeated(bytes_[slot].size(), '\0');
            std::size_t offset{};
            while (offset < repeated.size()) {
                const auto count = operations_.read_at(
                    operations_.context, descriptors_[directory_count + slot],
                    repeated.data() + offset, repeated.size() - offset, static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return rejected(MonitoringGovernedTrackedBundleFailureKind::read,
                                    count < 0 ? errno : EIO);
                if (static_cast<std::size_t>(count) > repeated.size() - offset)
                    return false;
                offset += static_cast<std::size_t>(count);
            }
            char probe{};
            ssize_t trailing{};
            do {
                trailing =
                    operations_.read_at(operations_.context, descriptors_[directory_count + slot],
                                        &probe, 1, static_cast<off_t>(offset));
            } while (trailing < 0 && errno == EINTR);
            if (trailing < 0)
                return rejected(MonitoringGovernedTrackedBundleFailureKind::read, errno);
            if (trailing != 0 || repeated != bytes_[slot])
                return false;
        }
        for (std::size_t role{}; role < descriptor_count; ++role) {
            operations_.fault(operations_.context, static_cast<unsigned int>(59U + role));
            struct stat held{}, named{};
            if (operations_.stat_fd(operations_.context, descriptors_[role], &held) != 0 ||
                operations_.stat_at(operations_.context, descriptors_[named_parent(role)],
                                    role_name(role).data(), &named, AT_SYMLINK_NOFOLLOW) != 0)
                return rejected(MonitoringGovernedTrackedBundleFailureKind::mutation, errno);
            if (identity(held) != identities_[role] || identity(named) != identities_[role])
                return false;
        }
        return true;
    } catch (...) {
        return rejected(MonitoringGovernedTrackedBundleFailureKind::contained_failure);
    }
}

auto MonitoringGovernedTrackedBundleCore::retire() noexcept
    -> std::expected<void, MonitoringGovernedTrackedBundleFailure> {
    if (!active_.any())
        return std::unexpected{MonitoringGovernedTrackedBundleFailure{
            .kind = MonitoringGovernedTrackedBundleFailureKind::inactive,
            .retirement = retirement_}};
    for (std::size_t reverse{}; reverse < descriptor_count; ++reverse) {
        const std::size_t role = descriptor_count - reverse - 1U;
        if (!active_.test(role))
            continue;
        active_.reset(role);
        const int descriptor = std::exchange(descriptors_[role], -1);
        if (operations_.close_fd(operations_.context, descriptor) == 0) {
            confirmed_.set(role);
            retirement_.confirmed.set(role);
        } else {
            uncertain_.set(role);
            retirement_.uncertain.set(role);
            const auto index = retirement_.cleanup_count++;
            retirement_.cleanup_roles[index] = static_cast<std::uint8_t>(role);
            retirement_.cleanup_errors[index] = errno;
        }
    }
    retirement_.active = active_;
    if (retirement_.cleanup_count != 0U) {
        MonitoringGovernedTrackedBundleFailure projected{};
        projected.kind = MonitoringGovernedTrackedBundleFailureKind::cleanup_failure;
        projected.native_error = retirement_.cleanup_errors[0];
        projected.retirement = retirement_;
        try {
            if (operations_.fault)
                operations_.fault(operations_.context, 36U);
            projected.diagnostic = std::move(retirement_diagnostic_);
            projected.diagnostic.primary.os_error = core::OsError::capture(
                std::error_code{retirement_.cleanup_errors[0], std::generic_category()});
            for (std::size_t index = 1; index < retirement_.cleanup_count; ++index) {
                auto error = std::move(retirement_cleanup_[index]);
                error.os_error = core::OsError::capture(
                    std::error_code{retirement_.cleanup_errors[index], std::generic_category()});
                projected.diagnostic.cleanup.push_back(std::move(error));
            }
        } catch (...) {
            projected.diagnostic.primary.category = core::ErrorCategory::monitoring;
            projected.diagnostic.primary.severity = core::ErrorSeverity::error;
            projected.diagnostic.primary.continuation = core::ContinuationPolicy::abort_command;
        }
        return std::unexpected{std::move(projected)};
    }
    return {};
}

} // namespace epycbench::monitoring::qualification::detail
