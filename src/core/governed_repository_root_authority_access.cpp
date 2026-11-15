#include "governed_repository_root_authority_access.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace epycbench::core::detail {
namespace {
auto failure(std::string_view code, std::string_view classification, std::string_view operation,
             std::optional<int> os_error = std::nullopt) -> Error {
    std::optional<OsError> captured;
    if (os_error)
        captured = OsError::capture(std::error_code{*os_error, std::generic_category()});
    return {.category = ErrorCategory::core,
            .code = StableErrorCode{std::string{code}},
            .message = "Governed repository root authority operation failed",
            .operation = std::string{operation},
            .os_error = std::move(captured),
            .context = {{"classification", classification}},
            .severity = ErrorSeverity::error,
            .continuation = ContinuationPolicy::abort_command};
}
auto stat_identity(const struct stat& value) noexcept -> GovernedRepositoryRootIdentity {
    return {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint32_t>(value.st_mode), static_cast<std::uint32_t>(value.st_uid)};
}
auto same(const GovernedRepositoryRootIdentity& left,
          const GovernedRepositoryRootIdentity& right) noexcept -> bool {
    return left.device == right.device && left.inode == right.inode && left.mode == right.mode &&
           left.owner == right.owner;
}
int duplicate(void*, int descriptor) noexcept { return ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0); }
int stat_descriptor(void*, int descriptor, struct stat* value) noexcept {
    return ::fstat(descriptor, value);
}
int open_root(void*) noexcept {
    return ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
int open_directory_at(void*, int parent, const char* name) noexcept {
    return ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
int close_descriptor(void*, int descriptor) noexcept { return ::close(descriptor); }
std::uint32_t effective_user(void*) noexcept { return static_cast<std::uint32_t>(::geteuid()); }
const GovernedRepositoryRootOperations production_operations{
    nullptr,           duplicate,        stat_descriptor, open_root,
    open_directory_at, close_descriptor, effective_user};
class DescriptorGuard final {
  public:
    DescriptorGuard(int descriptor, GovernedRepositoryRootOperations operations) noexcept
        : descriptor_{descriptor}, operations_{operations} {}
    DescriptorGuard(const DescriptorGuard&) = delete;
    DescriptorGuard(DescriptorGuard&& other) noexcept
        : descriptor_{std::exchange(other.descriptor_, -1)}, operations_{other.operations_} {}
    auto operator=(DescriptorGuard&& other) noexcept -> DescriptorGuard& {
        if (this != &other) {
            if (descriptor_ >= 0)
                static_cast<void>(operations_.close_descriptor(operations_.context, descriptor_));
            descriptor_ = std::exchange(other.descriptor_, -1);
            operations_ = other.operations_;
        }
        return *this;
    }
    ~DescriptorGuard() noexcept {
        if (descriptor_ >= 0)
            static_cast<void>(operations_.close_descriptor(operations_.context, descriptor_));
    }
    auto get() const noexcept -> int { return descriptor_; }
    auto release() noexcept -> int { return std::exchange(descriptor_, -1); }

  private:
    int descriptor_;
    GovernedRepositoryRootOperations operations_;
};
} // namespace

OwnedGovernedRepositoryRootDescriptor::OwnedGovernedRepositoryRootDescriptor(
    int descriptor, std::filesystem::path path, GovernedRepositoryRootIdentity accepted,
    GovernedRepositoryRootOperations operations) noexcept
    : descriptor_{descriptor}, accepted_path_{std::move(path)}, identity_{accepted},
      operations_{operations} {}
OwnedGovernedRepositoryRootDescriptor::OwnedGovernedRepositoryRootDescriptor(
    OwnedGovernedRepositoryRootDescriptor&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      accepted_path_{std::move(other.accepted_path_)}, identity_{other.identity_},
      operations_{other.operations_} {}
OwnedGovernedRepositoryRootDescriptor::~OwnedGovernedRepositoryRootDescriptor() noexcept {
    if (descriptor_ >= 0)
        static_cast<void>(operations_.close_descriptor(operations_.context, descriptor_));
}
auto OwnedGovernedRepositoryRootDescriptor::retire() -> std::expected<void, Error> {
    if (descriptor_ < 0)
        return std::unexpected{failure("governed_repository_root_retire_invalid",
                                       "repository_authority_state_invalid",
                                       "retire_governed_repository_root")};
    const int owned = std::exchange(descriptor_, -1);
    if (operations_.close_descriptor(operations_.context, owned) != 0)
        return std::unexpected{failure("governed_repository_root_close_failed",
                                       "repository_authority_close_failed",
                                       "retire_governed_repository_root", errno)};
    return {};
}
auto OwnedGovernedRepositoryRootDescriptor::still_matches_held_descriptor() const
    -> std::expected<bool, Error> {
    struct stat value{};
    if (operations_.stat_descriptor(operations_.context, descriptor_, &value) != 0)
        return std::unexpected{failure("governed_repository_root_recheck_failed",
                                       "repository_authority_syscall_failed",
                                       "recheck_governed_repository_root", errno)};
    return same(identity_, stat_identity(value));
}
auto OwnedGovernedRepositoryRootDescriptor::still_matches_pathname() const
    -> GovernedRepositoryRootCheckResult {
    GovernedRepositoryRootCheckResult result;
    const auto close_owned = [&](DescriptorGuard& descriptor) {
        const int owned = descriptor.release();
        if (operations_.close_descriptor(operations_.context, owned) != 0)
            result.cleanup.push_back(failure("governed_repository_root_close_failed",
                                             "repository_authority_close_failed",
                                             "recheck_governed_repository_root", errno));
    };
    const int root_descriptor = operations_.open_root(operations_.context);
    if (root_descriptor < 0) {
        result.primary = failure("governed_repository_root_recheck_failed",
                                 "repository_authority_syscall_failed",
                                 "recheck_governed_repository_root", errno);
        return result;
    }
    DescriptorGuard current{root_descriptor, operations_};
    for (const auto& component : accepted_path_.relative_path()) {
        const int child =
            operations_.open_directory_at(operations_.context, current.get(), component.c_str());
        const int opened_error = errno;
        std::optional<DescriptorGuard> child_owner;
        if (child >= 0)
            child_owner.emplace(child, operations_);
        close_owned(current);
        if (child < 0) {
            result.primary = failure("governed_repository_root_recheck_failed",
                                     "repository_authority_syscall_failed",
                                     "recheck_governed_repository_root", opened_error);
            return result;
        }
        current = std::move(*child_owner);
    }
    struct stat value{};
    const int status = operations_.stat_descriptor(operations_.context, current.get(), &value);
    const int status_error = errno;
    close_owned(current);
    if (status != 0)
        result.primary = failure("governed_repository_root_recheck_failed",
                                 "repository_authority_syscall_failed",
                                 "recheck_governed_repository_root", status_error);
    else
        result.matches = S_ISDIR(value.st_mode) && same(identity_, stat_identity(value));
    return result;
}
auto GovernedRepositoryRootAuthorityAccess::retain(const GovernedRepositoryRoot& root)
    -> GovernedRepositoryRootRetainResult {
    return retain_with_operations(root, production_operations);
}
auto GovernedRepositoryRootAuthorityAccess::retain_with_operations(
    const GovernedRepositoryRoot& root, GovernedRepositoryRootOperations operations)
    -> GovernedRepositoryRootRetainResult {
    GovernedRepositoryRootRetainResult result;
    if (operations.duplicate == nullptr || operations.stat_descriptor == nullptr ||
        operations.open_root == nullptr || operations.open_directory_at == nullptr ||
        operations.close_descriptor == nullptr || operations.effective_user == nullptr) {
        result.primary =
            failure("governed_repository_root_operations_invalid",
                    "repository_authority_semantic_invalid", "retain_governed_repository_root");
        return result;
    }
    const auto duplicated = operations.duplicate(operations.context, root.descriptor_);
    if (duplicated < 0) {
        result.primary =
            failure("governed_repository_root_retain_failed", "repository_authority_syscall_failed",
                    "retain_governed_repository_root", errno);
        return result;
    }
    DescriptorGuard owner{duplicated, operations};
    const auto close_with_evidence = [&] {
        const int owned = owner.release();
        if (operations.close_descriptor(operations.context, owned) != 0)
            result.cleanup.push_back(failure("governed_repository_root_close_failed",
                                             "repository_authority_close_failed",
                                             "retain_governed_repository_root", errno));
    };
    struct stat value{};
    if (operations.stat_descriptor(operations.context, duplicated, &value) != 0) {
        const auto saved = errno;
        result.primary =
            failure("governed_repository_root_retain_failed", "repository_authority_syscall_failed",
                    "retain_governed_repository_root", saved);
        close_with_evidence();
        return result;
    }
    if (!S_ISDIR(value.st_mode) ||
        static_cast<std::uint32_t>(value.st_uid) != operations.effective_user(operations.context)) {
        result.primary =
            failure("governed_repository_root_semantic_invalid",
                    "repository_authority_semantic_invalid", "retain_governed_repository_root");
        close_with_evidence();
        return result;
    }
    OwnedGovernedRepositoryRootDescriptor retained{duplicated, root.accepted_root_,
                                                   stat_identity(value), operations};
    static_cast<void>(owner.release());
    result.value.emplace(std::move(retained));
    return result;
}
} // namespace epycbench::core::detail
