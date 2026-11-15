#include "monitoring/host_q1_git_repository_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::size_t metadata_cap = 1U << 20U;
auto native_open_at(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_stat_fd(void*, int descriptor, struct stat* value) noexcept -> int {
    return ::fstat(descriptor, value);
}
auto native_stat_at(void*, int parent, const char* name, struct stat* value, int flags) noexcept
    -> int {
    return ::fstatat(parent, name, value, flags);
}
auto native_read_at(void*, int descriptor, void* bytes, std::size_t size, off_t offset) noexcept
    -> ssize_t {
    return ::pread(descriptor, bytes, size, offset);
}
auto native_close(void*, int descriptor) noexcept -> int { return ::close(descriptor); }
auto native_euid(void*) noexcept -> unsigned int { return ::geteuid(); }
void native_fault(void*, unsigned int) {}
struct CloseLedger final {
    std::array<int, 6> errors{};
    std::size_t size{};
};
auto failure(std::string_view classification, int error = 0) -> MonitoringQ1GitCommandFailure {
    std::optional<core::OsError> os;
    if (error != 0)
        os = core::OsError::capture(std::error_code{error, std::generic_category()});
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q1_plan_authority.repository_rejected"},
             .message = "Q1 Git repository metadata rejected",
             .operation = "authenticate_monitoring_q1_plan",
             .os_error = std::move(os),
             .context = {{"classification", std::string{classification}}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto append_cleanup(MonitoringQ1GitCommandFailure& result, const CloseLedger& ledger) -> void {
    result.cleanup.reserve(ledger.size);
    for (std::size_t index = 0; index < ledger.size; ++index)
        result.cleanup.push_back(failure("cleanup_failure", ledger.errors[index]).primary);
}
auto close_reverse(std::array<int, 6>& descriptors,
                   const MonitoringQ1GitRepositoryMetadata::Operations& operations) noexcept
    -> CloseLedger {
    CloseLedger ledger;
    for (auto iterator = descriptors.rbegin(); iterator != descriptors.rend(); ++iterator) {
        if (*iterator < 0)
            continue;
        const int descriptor = std::exchange(*iterator, -1);
        if (operations.close_fd(operations.context, descriptor) != 0 &&
            ledger.size != ledger.errors.size())
            ledger.errors[ledger.size++] = errno;
    }
    return ledger;
}
auto rejected(std::string_view classification, int error, std::array<int, 6>& descriptors,
              const MonitoringQ1GitRepositoryMetadata::Operations& operations)
    -> std::expected<MonitoringQ1GitRepositoryMetadata, MonitoringQ1GitCommandFailure> {
    const auto ledger = close_reverse(descriptors, operations);
    auto result = failure(classification, error);
    append_cleanup(result, ledger);
    return std::unexpected{std::move(result)};
}
auto identity(const struct stat& value) noexcept -> MonitoringQ1GitMetadataIdentity {
    return {static_cast<std::uint64_t>(value.st_dev),  static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),  static_cast<std::uint32_t>(value.st_nlink)};
}
auto trusted_directory(const struct stat& value, unsigned int euid) noexcept -> bool {
    return S_ISDIR(value.st_mode) && (value.st_uid == 0 || value.st_uid == euid) &&
           (value.st_mode & 0022) == 0;
}
auto trusted_file(const struct stat& value, unsigned int euid) noexcept -> bool {
    return S_ISREG(value.st_mode) && value.st_nlink == 1 &&
           (value.st_uid == 0 || value.st_uid == euid) && (value.st_mode & 0022) == 0 &&
           value.st_size >= 0 && static_cast<std::uint64_t>(value.st_size) <= metadata_cap;
}
auto lower(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
auto trim(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}
auto valid_name(std::string_view value) noexcept -> bool {
    if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front())))
        return false;
    return std::ranges::all_of(value,
                               [](unsigned char c) { return std::isalnum(c) != 0 || c == '-'; });
}
auto safe_config(std::string_view bytes) -> bool {
    if (bytes.find('\0') != std::string_view::npos)
        return false;
    std::string section;
    std::string subsection;
    std::string logical;
    std::size_t tokens = 0;
    logical.reserve(std::min<std::size_t>(bytes.size(), 4096U));
    while (!bytes.empty()) {
        const auto end = bytes.find('\n');
        auto physical = bytes.substr(0, end);
        bytes = end == std::string_view::npos ? std::string_view{} : bytes.substr(end + 1U);
        if (physical.size() > 4096U || logical.size() + physical.size() > 4096U)
            return false;
        bool quoted = false;
        bool escaped = false;
        std::size_t comment = physical.size();
        for (std::size_t i = 0; i < physical.size(); ++i) {
            const char c = physical[i];
            if (escaped) {
                if (quoted && c != '\\' && c != '"' && c != 'n' && c != 't' && c != 'b')
                    return false;
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"')
                quoted = !quoted;
            else if (!quoted && (c == '#' || c == ';')) {
                comment = i;
                break;
            }
        }
        if (quoted)
            return false;
        physical = physical.substr(0, comment);
        const bool continued = !physical.empty() && physical.back() == '\\' &&
                               (physical.size() < 2U || physical[physical.size() - 2U] != '\\');
        if (continued)
            physical.remove_suffix(1);
        logical.append(physical);
        if (continued)
            continue;
        auto line = trim(logical);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            logical.clear();
        if (line.empty())
            continue;
        if (line.front() == '[') {
            if (++tokens > 1024U)
                return false;
            if (line.back() != ']')
                return false;
            const auto body = trim(line.substr(1, line.size() - 2U));
            const auto separator = body.find_first_of(". \t");
            const auto base = body.substr(0, separator);
            if (!valid_name(base))
                return false;
            section = lower(std::string{base});
            subsection.clear();
            if (separator != std::string_view::npos) {
                if (body[separator] == '.') {
                    subsection = std::string{body.substr(separator + 1U)};
                    if (subsection.empty() || subsection.size() > 1024U)
                        return false;
                } else {
                    const auto quoted_subsection = trim(body.substr(separator));
                    if (quoted_subsection.size() < 2U || quoted_subsection.front() != '"')
                        return false;
                    bool escaped_subsection = false;
                    std::size_t closing = std::string_view::npos;
                    for (std::size_t index = 1; index < quoted_subsection.size(); ++index) {
                        if (escaped_subsection) {
                            escaped_subsection = false;
                        } else if (quoted_subsection[index] == '\\') {
                            escaped_subsection = true;
                        } else if (quoted_subsection[index] == '"') {
                            closing = index;
                            break;
                        }
                    }
                    if (closing == std::string_view::npos ||
                        !trim(quoted_subsection.substr(closing + 1U)).empty())
                        return false;
                    subsection = std::string{quoted_subsection.substr(1U, closing - 1U)};
                }
            }
            logical.clear();
            continue;
        }
        const auto equals = line.find('=');
        const auto key = lower(std::string{trim(line.substr(0, equals))});
        if (key.empty() || key.size() > 1024U || ++tokens > 1024U)
            return false;
        const auto value = equals == std::string_view::npos ? std::string_view{"true"}
                                                            : trim(line.substr(equals + 1U));
        if (!value.empty() && value.front() == '"') {
            bool value_escaped = false;
            std::size_t closing = std::string_view::npos;
            for (std::size_t index = 1; index < value.size(); ++index) {
                const char character = value[index];
                if (value_escaped) {
                    if (character != '\\' && character != '"' && character != 'n' &&
                        character != 't' && character != 'b')
                        return false;
                    value_escaped = false;
                } else if (character == '\\') {
                    value_escaped = true;
                } else if (character == '"') {
                    closing = index;
                    break;
                }
            }
            if (value_escaped || closing == std::string_view::npos ||
                !trim(value.substr(closing + 1U)).empty())
                return false;
        } else if (value.find_first_of("\\\"") != std::string_view::npos) {
            return false;
        }
        auto effective_section = section;
        auto effective_key = key;
        auto effective_subsection = subsection;
        if (!section.empty() && effective_key.find('.') != std::string::npos)
            return false;
        if (section.empty()) {
            const auto dot = effective_key.find('.');
            if (dot == std::string::npos)
                return false;
            effective_section = effective_key.substr(0, dot);
            effective_key.erase(0, dot + 1U);
            const auto terminal = effective_key.rfind('.');
            if (terminal != std::string::npos) {
                effective_subsection = effective_key.substr(0, terminal);
                effective_key.erase(0, terminal + 1U);
                if (effective_subsection.empty() || effective_subsection.size() > 1024U)
                    return false;
            }
        }
        if (!valid_name(effective_key))
            return false;
        const bool include_path = effective_section == "include" && effective_subsection.empty() &&
                                  effective_key == "path";
        const bool include_if_path = effective_section == "includeif" &&
                                     !effective_subsection.empty() && effective_key == "path";
        if (include_path || include_if_path)
            return false;
        if (effective_section == "core" && effective_subsection.empty() &&
            (effective_key == "worktree" || effective_key == "excludesfile" ||
             effective_key == "attributesfile"))
            return false;
        if (effective_section == "extensions" && effective_subsection.empty() &&
            effective_key == "worktreeconfig")
            return false;
        const bool filter_section =
            effective_section == "filter" || effective_section.starts_with("filter.");
        const bool filter_key = effective_key == "clean" || effective_key == "process" ||
                                effective_key == "required" || effective_key.ends_with(".clean") ||
                                effective_key.ends_with(".process") ||
                                effective_key.ends_with(".required");
        if (filter_section && !effective_subsection.empty() && filter_key)
            return false;
        logical.clear();
    }
    return logical.empty();
}
auto read_file(int descriptor, const struct stat& expected,
               const MonitoringQ1GitRepositoryMetadata::Operations& operations)
    -> std::expected<std::string, int> {
    std::string bytes(static_cast<std::size_t>(expected.st_size), '\0');
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto count = operations.read_at(operations.context, descriptor, bytes.data() + offset,
                                              bytes.size() - offset, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected{errno};
        }
        if (count == 0)
            return std::unexpected{EIO};
        offset += static_cast<std::size_t>(count);
    }
    struct stat after{};
    if (operations.stat_fd(operations.context, descriptor, &after) != 0)
        return std::unexpected{errno};
    if (identity(after) != identity(expected))
        return std::unexpected{ESTALE};
    return bytes;
}
auto absent(int parent, const char* name,
            const MonitoringQ1GitRepositoryMetadata::Operations& operations) noexcept
    -> std::expected<bool, int> {
    struct stat value{};
    if (operations.stat_at(operations.context, parent, name, &value, AT_SYMLINK_NOFOLLOW) == 0)
        return false;
    if (errno == ENOENT)
        return true;
    return std::unexpected{errno};
}
} // namespace

MonitoringQ1GitRepositoryMetadata::MonitoringQ1GitRepositoryMetadata(
    int root, std::array<int, 6> descriptors,
    std::array<MonitoringQ1GitMetadataIdentity, 6> identities, std::array<std::string, 2> bytes,
    Operations operations) noexcept
    : root_{root}, descriptors_{descriptors}, identities_{identities}, bytes_{std::move(bytes)},
      operations_{operations} {}
MonitoringQ1GitRepositoryMetadata::MonitoringQ1GitRepositoryMetadata(
    MonitoringQ1GitRepositoryMetadata&& other) noexcept
    : root_{other.root_}, descriptors_{other.descriptors_}, identities_{other.identities_},
      bytes_{std::move(other.bytes_)} {
    operations_ = other.operations_;
    other.descriptors_.fill(-1);
}
MonitoringQ1GitRepositoryMetadata::~MonitoringQ1GitRepositoryMetadata() noexcept {
    for (auto it = descriptors_.rbegin(); it != descriptors_.rend(); ++it)
        if (*it >= 0)
            static_cast<void>(operations_.close_fd(operations_.context, *it));
}

auto MonitoringQ1GitRepositoryMetadata::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root)
    -> std::expected<MonitoringQ1GitRepositoryMetadata, MonitoringQ1GitCommandFailure> {
    return retain_with(root.descriptor(), {.open_at = native_open_at,
                                           .stat_fd = native_stat_fd,
                                           .stat_at = native_stat_at,
                                           .read_at = native_read_at,
                                           .close_fd = native_close,
                                           .effective_user = native_euid,
                                           .fault_point = native_fault});
}

auto MonitoringQ1GitRepositoryMetadata::retain_with(int root, Operations operations)
    -> std::expected<MonitoringQ1GitRepositoryMetadata, MonitoringQ1GitCommandFailure> {
    std::array<int, 6> fd{-1, -1, -1, -1, -1, -1};
    try {
        const auto acquire = [&](std::size_t index, int parent, const char* name,
                                 int flags) -> std::optional<int> {
            fd[index] = operations.open_at(operations.context, parent, name, flags);
            return fd[index] < 0 ? std::optional<int>{errno} : std::nullopt;
        };
        if (auto error = acquire(0, root, ".git", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))
            return rejected("git_metadata", *error, fd, operations);
        if (auto error = acquire(1, fd[0], "info", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))
            return rejected("git_metadata", *error, fd, operations);
        if (auto error =
                acquire(2, fd[0], "objects", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))
            return rejected("git_metadata", *error, fd, operations);
        if (auto error = acquire(3, fd[2], "info", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))
            return rejected("git_metadata", *error, fd, operations);
        if (auto error =
                acquire(4, fd[0], "config", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK))
            return rejected("git_metadata", *error, fd, operations);
        if (auto error =
                acquire(5, fd[1], "exclude", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK))
            return rejected("git_metadata", *error, fd, operations);
        std::array<MonitoringQ1GitMetadataIdentity, 6> identities{};
        std::array<struct stat, 6> stats{};
        for (std::size_t i = 0; i < fd.size(); ++i) {
            if (operations.stat_fd(operations.context, fd[i], &stats[i]) != 0) {
                const int error = errno;
                return rejected("git_metadata", error, fd, operations);
            }
            if (i < 4 ? !trusted_directory(stats[i], operations.effective_user(operations.context))
                      : !trusted_file(stats[i], operations.effective_user(operations.context))) {
                return rejected("git_metadata", 0, fd, operations);
            }
            identities[i] = identity(stats[i]);
        }
        for (const auto& [parent, name] :
             {std::pair{fd[0], "commondir"}, std::pair{fd[3], "alternates"},
              std::pair{fd[1], "attributes"}}) {
            auto missing = absent(parent, name, operations);
            if (!missing) {
                const int error = missing.error();
                return rejected("git_metadata", error, fd, operations);
            }
            if (!*missing) {
                return rejected("git_metadata", 0, fd, operations);
            }
        }
        std::array<std::string, 2> bytes;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            auto read = read_file(fd[i + 4U], stats[i + 4U], operations);
            if (!read) {
                const int error = read.error();
                return rejected("git_metadata", error, fd, operations);
            }
            bytes[i] = std::move(*read);
        }
        if (!safe_config(bytes[0])) {
            return rejected("git_config", 0, fd, operations);
        }
        operations.fault_point(operations.context, 1U);
        return MonitoringQ1GitRepositoryMetadata{root, fd, identities, std::move(bytes),
                                                 operations};
    } catch (const std::exception&) {
        return rejected("contained_standard_exception", 0, fd, operations);
    } catch (...) {
        return rejected("contained_unknown_exception", 0, fd, operations);
    }
}

auto MonitoringQ1GitRepositoryMetadata::recheck() const
    -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    try {
        const std::array<int, 6> parents{root_,           descriptors_[0], descriptors_[0],
                                         descriptors_[2], descriptors_[0], descriptors_[1]};
        constexpr std::array names{".git", "info", "objects", "info", "config", "exclude"};
        for (std::size_t i = 0; i < descriptors_.size(); ++i) {
            struct stat value{};
            if (operations_.stat_fd(operations_.context, descriptors_[i], &value) != 0)
                return std::unexpected{failure("git_metadata", errno)};
            if (identity(value) != identities_[i])
                return false;
            if (operations_.stat_at(operations_.context, parents[i], names[i], &value,
                                    AT_SYMLINK_NOFOLLOW) != 0)
                return std::unexpected{failure("git_metadata", errno)};
            if (identity(value) != identities_[i])
                return false;
        }
        for (const auto& [parent, name] :
             {std::pair{descriptors_[0], "commondir"}, std::pair{descriptors_[3], "alternates"},
              std::pair{descriptors_[1], "attributes"}}) {
            auto missing = absent(parent, name, operations_);
            if (!missing)
                return std::unexpected{failure("git_metadata", missing.error())};
            if (!*missing)
                return false;
        }
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            struct stat value{};
            if (operations_.stat_fd(operations_.context, descriptors_[i + 4U], &value) != 0)
                return std::unexpected{failure("git_metadata", errno)};
            if (identity(value) != identities_[i + 4U])
                return false;
            auto read = read_file(descriptors_[i + 4U], value, operations_);
            if (!read)
                return std::unexpected{failure("git_metadata", read.error())};
            if (*read != bytes_[i])
                return false;
        }
        operations_.fault_point(operations_.context, 2U);
        return true;
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_standard_exception")};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_exception")};
    }
}

auto MonitoringQ1GitRepositoryMetadata::retire()
    -> std::expected<void, MonitoringQ1GitCommandFailure> {
    const auto ledger = close_reverse(descriptors_, operations_);
    try {
        if (ledger.size != 0) {
            auto result = failure("cleanup_failure", ledger.errors[0]);
            CloseLedger secondaries;
            secondaries.size = ledger.size - 1U;
            std::copy_n(ledger.errors.begin() + 1, secondaries.size, secondaries.errors.begin());
            append_cleanup(result, secondaries);
            return std::unexpected{std::move(result)};
        }
        operations_.fault_point(operations_.context, 3U);
        return {};
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_standard_exception")};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_exception")};
    }
}

} // namespace epycbench::monitoring::qualification::detail
