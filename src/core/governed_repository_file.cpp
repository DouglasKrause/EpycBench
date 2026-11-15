#include "epycbench/core/governed_repository_file.hpp"

#include "governed_repository_file_test_access.hpp"
#include "governed_repository_source_access.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace epycbench::core {
auto detail::GovernedRepositorySourceAccess::make(std::filesystem::path relative,
                                                 std::string bytes) noexcept
    -> GovernedRepositoryFile {
    return GovernedRepositoryFile{std::move(relative), std::move(bytes)};
}
namespace {

class Descriptor {
  public:
    explicit Descriptor(int value = -1) noexcept : value_{value} {}
    Descriptor(const Descriptor&) = delete;
    auto operator=(const Descriptor&) -> Descriptor& = delete;
    Descriptor(Descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
    auto operator=(Descriptor&& other) noexcept -> Descriptor& {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~Descriptor() { reset(); }
    [[nodiscard]] auto get() const noexcept -> int { return value_; }
    [[nodiscard]] auto release() noexcept -> int { return std::exchange(value_, -1); }

  private:
    void reset() noexcept {
        if (value_ >= 0) {
            (void)::close(value_);
            value_ = -1;
        }
    }
    int value_;
};

enum class SpellingKind { root, relative };

struct ValidatedSpelling {
    std::vector<std::string> components;
};

[[nodiscard]] auto make_error(std::string code, std::string message, std::string operation,
                              std::optional<int> os_error = std::nullopt,
                              nlohmann::json context = nlohmann::json::object()) -> Error {
    std::optional<OsError> captured;
    if (os_error) {
        captured = OsError::capture(std::error_code{*os_error, std::generic_category()});
    }
    return {.category = ErrorCategory::core,
            .code = StableErrorCode{std::move(code)},
            .message = std::move(message),
            .operation = std::move(operation),
            .os_error = std::move(captured),
            .context = std::move(context),
            .severity = ErrorSeverity::error,
            .continuation = ContinuationPolicy::abort_command};
}

[[nodiscard]] auto syntax_error(SpellingKind kind, std::string reason) -> Error {
    return make_error(kind == SpellingKind::root ? "governed_repository_root_invalid"
                                                 : "governed_relative_path_invalid",
                      kind == SpellingKind::root ? "Invalid governed repository root spelling"
                                                 : "Invalid governed relative path spelling",
                      kind == SpellingKind::root ? "create_governed_repository_root"
                                                 : "read_governed_repository_file",
                      std::nullopt, {{"reason", std::move(reason)}});
}

[[nodiscard]] auto validate_spelling(const std::filesystem::path& path, SpellingKind kind)
    -> std::expected<ValidatedSpelling, Error> {
    const auto& spelling = path.native();
    if (spelling.find('\0') != std::string::npos) {
        return std::unexpected{syntax_error(kind, "embedded_nul")};
    }
    if (spelling.empty()) {
        return std::unexpected{syntax_error(kind, "empty")};
    }
    const bool root = kind == SpellingKind::root;
    if (root ? spelling.front() != '/' : spelling.front() == '/') {
        return std::unexpected{syntax_error(kind, root ? "not_absolute" : "absolute")};
    }
    if (root && spelling == "/") {
        return ValidatedSpelling{};
    }
    if (spelling.back() == '/') {
        return std::unexpected{syntax_error(kind, "trailing_separator")};
    }
    const std::size_t begin = root ? 1U : 0U;
    std::vector<std::string> components;
    std::size_t component_begin = begin;
    for (std::size_t index = begin; index <= spelling.size(); ++index) {
        if (index != spelling.size() && spelling[index] != '/') {
            continue;
        }
        const auto component = spelling.substr(component_begin, index - component_begin);
        if (component.empty()) {
            return std::unexpected{syntax_error(kind, "empty_component")};
        }
        if (component == ".") {
            return std::unexpected{syntax_error(kind, "dot_component")};
        }
        if (component == "..") {
            return std::unexpected{syntax_error(kind, "parent_component")};
        }
        components.push_back(component);
        component_begin = index + 1U;
    }
    return ValidatedSpelling{std::move(components)};
}

[[nodiscard]] auto is_symlink_at(int directory, const std::string& component) noexcept -> bool {
    struct stat status {};
    return ::fstatat(directory, component.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISLNK(status.st_mode);
}

} // namespace

auto GovernedRepositoryRoot::read_opened_regular_file(int descriptor,
                                                      const std::filesystem::path& relative) noexcept
    -> std::expected<GovernedRepositoryFile, Error> {
    try {
        struct stat status {};
        if (::fstat(descriptor, &status) < 0) {
            const int error = errno;
            return std::unexpected{make_error("governed_file_read_failed",
                                              "Unable to inspect governed file",
                                              "read_governed_repository_file", error,
                                              {{"path", relative.native()}})};
        }
        if (!S_ISREG(status.st_mode)) {
            return std::unexpected{make_error("governed_target_not_regular_file",
                                              "Governed target is not a regular file",
                                              "read_governed_repository_file", std::nullopt,
                                              {{"path", relative.native()}})};
        }
        std::string bytes;
        std::array<char, 65536> buffer{};
        for (;;) {
            const auto count = ::read(descriptor, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int error = errno;
                return std::unexpected{make_error("governed_file_read_failed",
                                                  "Unable to read governed file",
                                                  "read_governed_repository_file", error,
                                                  {{"path", relative.native()}})};
            }
            if (count == 0) {
                break;
            }
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return GovernedRepositoryFile{relative, std::move(bytes)};
    } catch (const std::exception&) {
        return std::unexpected{make_error("governed_file_read_failed",
                                          "Unable to retain governed file bytes",
                                          "read_governed_repository_file")};
    } catch (...) {
        return std::unexpected{make_error("governed_file_read_failed",
                                          "Unable to retain governed file bytes",
                                          "read_governed_repository_file")};
    }
}

GovernedRepositoryRoot::GovernedRepositoryRoot(GovernedRepositoryRoot&& other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)},
      accepted_root_{std::move(other.accepted_root_)} {}

GovernedRepositoryRoot::~GovernedRepositoryRoot() {
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
    }
}

auto GovernedRepositoryRoot::create(const std::filesystem::path& root) noexcept
    -> std::expected<GovernedRepositoryRoot, Error> {
    try {
        auto validated = validate_spelling(root, SpellingKind::root);
        if (!validated) {
            return std::unexpected{std::move(validated.error())};
        }
        Descriptor current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (current.get() < 0) {
            const int error = errno;
            return std::unexpected{make_error("governed_repository_root_invalid",
                                              "Unable to open filesystem root",
                                              "create_governed_repository_root", error)};
        }
        for (const auto& component : validated->components) {
            if (is_symlink_at(current.get(), component)) {
                return std::unexpected{make_error("governed_repository_root_symlink_rejected",
                                                  "Governed repository root traverses a symlink",
                                                  "create_governed_repository_root")};
            }
            Descriptor next{::openat(current.get(), component.c_str(),
                                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
            if (next.get() < 0) {
                const int error = errno;
                const bool symlink = error == ELOOP || is_symlink_at(current.get(), component);
                return std::unexpected{make_error(
                    symlink ? "governed_repository_root_symlink_rejected"
                            : "governed_repository_root_invalid",
                    symlink ? "Governed repository root traverses a symlink"
                            : "Governed repository root is unavailable or not a directory",
                    "create_governed_repository_root", error)};
            }
            current = std::move(next);
        }
        return GovernedRepositoryRoot{current.release(), root};
    } catch (const std::exception&) {
        return std::unexpected{make_error("governed_repository_root_invalid",
                                          "Unable to establish governed repository root",
                                          "create_governed_repository_root")};
    } catch (...) {
        return std::unexpected{make_error("governed_repository_root_invalid",
                                          "Unable to establish governed repository root",
                                          "create_governed_repository_root")};
    }
}

auto GovernedRepositoryRoot::read_regular_file(
    const std::filesystem::path& repository_relative_path) const noexcept
    -> std::expected<GovernedRepositoryFile, Error> {
    try {
        auto validated = validate_spelling(repository_relative_path, SpellingKind::relative);
        if (!validated) {
            return std::unexpected{std::move(validated.error())};
        }
        int parent = descriptor_;
        Descriptor owned_parent;
        for (std::size_t index = 0; index + 1U < validated->components.size(); ++index) {
            const auto& component = validated->components[index];
            if (is_symlink_at(parent, component)) {
                return std::unexpected{make_error("governed_symlink_rejected",
                                                  "Governed path traverses a symlink",
                                                  "read_governed_repository_file", std::nullopt,
                                                  {{"path", repository_relative_path.native()}})};
            }
            Descriptor next{::openat(parent, component.c_str(),
                                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
            if (next.get() < 0) {
                const int error = errno;
                const bool symlink = error == ELOOP || is_symlink_at(parent, component);
                return std::unexpected{make_error(
                    symlink ? "governed_symlink_rejected"
                            : "governed_path_component_open_failed",
                    symlink ? "Governed path traverses a symlink"
                            : "Unable to open governed path component",
                    "read_governed_repository_file", error,
                    {{"path", repository_relative_path.native()}})};
            }
            owned_parent = std::move(next);
            parent = owned_parent.get();
        }
        const auto& final = validated->components.back();
        if (is_symlink_at(parent, final)) {
            return std::unexpected{make_error("governed_symlink_rejected",
                                              "Governed target is a symlink",
                                              "read_governed_repository_file", std::nullopt,
                                              {{"path", repository_relative_path.native()}})};
        }
        Descriptor file{
            ::openat(parent, final.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
        if (file.get() < 0) {
            const int error = errno;
            const bool symlink = is_symlink_at(parent, final);
            return std::unexpected{detail::GovernedRepositoryFileTestAccess::
                                       classify_final_open_failure(
                                           error, symlink, repository_relative_path)};
        }
        return read_opened_regular_file(file.get(), repository_relative_path);
    } catch (const std::exception&) {
        return std::unexpected{make_error("governed_file_read_failed",
                                          "Unable to read governed repository file",
                                          "read_governed_repository_file")};
    } catch (...) {
        return std::unexpected{make_error("governed_file_read_failed",
                                          "Unable to read governed repository file",
                                          "read_governed_repository_file")};
    }
}

namespace detail {
auto GovernedRepositoryFileTestAccess::read_opened_regular_file(
    int descriptor, const std::filesystem::path& repository_relative_path) noexcept
    -> std::expected<GovernedRepositoryFile, Error> {
    return GovernedRepositoryRoot::read_opened_regular_file(descriptor,
                                                             repository_relative_path);
}

auto GovernedRepositoryFileTestAccess::classify_final_open_failure(
    int authoritative_errno, bool final_symlink,
    const std::filesystem::path& repository_relative_path) -> Error {
    if (final_symlink) {
        return make_error("governed_symlink_rejected", "Governed target is a symlink",
                          "read_governed_repository_file", authoritative_errno,
                          {{"path", repository_relative_path.native()}});
    }
    if (authoritative_errno == ENOENT) {
        return make_error("governed_target_not_found", "Unable to open governed target",
                          "read_governed_repository_file", authoritative_errno,
                          {{"path", repository_relative_path.native()}});
    }
    return make_error("governed_target_open_failed", "Unable to open governed target",
                      "read_governed_repository_file", authoritative_errno,
                      {{"classification", "final_target_open_failed"},
                       {"path", repository_relative_path.native()}});
}
} // namespace detail

} // namespace epycbench::core
