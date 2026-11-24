#include "topology/topology_cli.hpp"

#include "epycbench/topology/hwloc_source.hpp"
#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/linux_source.hpp"
#include "topology/checked_topology_access.hpp"

#include <CLI/CLI.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <ostream>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace epycbench::topology::cli {
namespace {

constexpr std::string_view fixture_relative_path{"resources/fixtures/topology/reference-epyc"};

class FileDescriptor {
  public:
    explicit FileDescriptor(int value = -1) noexcept : value_{value} {}
    FileDescriptor(const FileDescriptor&) = delete;
    auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
    auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }
    [[nodiscard]] auto get() const noexcept -> int { return value_; }
    [[nodiscard]] auto valid() const noexcept -> bool { return value_ >= 0; }
    [[nodiscard]] auto release() noexcept -> int { return std::exchange(value_, -1); }

  private:
    void reset() noexcept {
        if (value_ >= 0) {
            (void)::close(value_);
        }
        value_ = -1;
    }
    int value_;
};

auto failure(std::string code, std::string classification, ExitCode exit_code)
    -> std::unexpected<CliFailure> {
    return std::unexpected{CliFailure{std::move(code), std::move(classification), exit_code}};
}

struct ResolvedParent {
    FileDescriptor directory;
    std::string filename;
};

auto split_path(const std::filesystem::path& path) -> std::vector<std::string> {
    std::vector<std::string> components;
    for (const auto& component : path) {
        const auto text = component.string();
        if (text.empty() || text == "/") {
            continue;
        }
        components.push_back(text);
    }
    return components;
}

auto resolve_parent(const std::filesystem::path& path, bool input)
    -> std::expected<ResolvedParent, CliFailure> {
    auto components = split_path(path);
    if (components.empty()) {
        return failure("topology.cli.parent_missing", "invalid path", ExitCode::filesystem);
    }
    for (const auto& component : components) {
        if (component == "." || component == "..") {
            return failure("topology.cli.symlink_rejected", "unsafe path component",
                           ExitCode::filesystem);
        }
    }

    FileDescriptor current{
        ::open(path.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (!current.valid()) {
        return failure(input ? "topology.cli.input_open_failed" : "topology.cli.parent_missing",
                       "path traversal failed", ExitCode::filesystem);
    }
    for (std::size_t index = 0; index + 1U < components.size(); ++index) {
        struct stat component_status{};
        if (::fstatat(current.get(), components[index].c_str(), &component_status,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(component_status.st_mode)) {
            return failure("topology.cli.symlink_rejected", "symlink rejected",
                           ExitCode::filesystem);
        }
        const int next = ::openat(current.get(), components[index].c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const bool symlink = errno == ELOOP;
            return failure(
                symlink
                    ? "topology.cli.symlink_rejected"
                    : (input ? "topology.cli.input_open_failed" : "topology.cli.parent_missing"),
                symlink ? "symlink rejected" : "parent unavailable", ExitCode::filesystem);
        }
        current = FileDescriptor{next};
    }
    return ResolvedParent{std::move(current), std::move(components.back())};
}

auto read_regular_file(const std::filesystem::path& path, std::size_t maximum_document_bytes)
    -> std::expected<std::string, CliFailure> {
    auto parent = resolve_parent(path, true);
    if (!parent) {
        return std::unexpected{std::move(parent.error())};
    }
    FileDescriptor file{::openat(parent->directory.get(), parent->filename.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!file.valid()) {
        return failure(
            errno == ELOOP ? "topology.cli.symlink_rejected" : "topology.cli.input_open_failed",
            errno == ELOOP ? "symlink rejected" : "input open failed", ExitCode::filesystem);
    }
    struct stat status{};
    if (::fstat(file.get(), &status) < 0) {
        return failure("topology.cli.input_open_failed", "input inspection failed",
                       ExitCode::filesystem);
    }
    if (!S_ISREG(status.st_mode)) {
        return failure("topology.cli.input_not_regular", "input is not a regular file",
                       ExitCode::filesystem);
    }
    if (status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_document_bytes) {
        return failure("topology.serialization.limit_exceeded", "input size limit exceeded",
                       ExitCode::topology);
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 65536> buffer{};
    while (true) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failure("topology.cli.input_open_failed", "input read failed",
                           ExitCode::filesystem);
        }
        if (count == 0) {
            break;
        }
        const auto amount = static_cast<std::size_t>(count);
        if (amount > maximum_document_bytes - contents.size()) {
            return failure("topology.serialization.limit_exceeded", "input size limit exceeded",
                           ExitCode::topology);
        }
        contents.append(buffer.data(), amount);
    }
    return contents;
}

auto write_all(int descriptor, std::string_view bytes) -> bool {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

auto rename_no_replace(int directory, const char* old_name, const char* new_name) -> int {
#if defined(SYS_renameat2)
    return static_cast<int>(
        ::syscall(SYS_renameat2, directory, old_name, directory, new_name, RENAME_NOREPLACE));
#else
    errno = ENOSYS;
    return -1;
#endif
}

auto publish_regular_file(const std::filesystem::path& path, std::string_view bytes, bool force)
    -> std::expected<void, CliFailure> {
    auto parent = resolve_parent(path, false);
    if (!parent) {
        return std::unexpected{std::move(parent.error())};
    }
    struct stat destination{};
    const int status = ::fstatat(parent->directory.get(), parent->filename.c_str(), &destination,
                                 AT_SYMLINK_NOFOLLOW);
    if (status == 0) {
        if (S_ISLNK(destination.st_mode)) {
            return failure("topology.cli.symlink_rejected", "destination symlink rejected",
                           ExitCode::filesystem);
        }
        if (!S_ISREG(destination.st_mode)) {
            return failure("topology.cli.output_invalid", "destination is not a regular file",
                           ExitCode::filesystem);
        }
        if (!force) {
            return failure("topology.cli.output_exists", "destination already exists",
                           ExitCode::filesystem);
        }
    } else if (errno != ENOENT) {
        return failure("topology.cli.output_invalid", "destination inspection failed",
                       ExitCode::filesystem);
    }

    std::string temporary;
    FileDescriptor file;
    for (std::uint32_t attempt = 0; attempt < 128U; ++attempt) {
        temporary = ".epycbench-topology-" +
                    std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
                    std::to_string(attempt) + ".tmp";
        const int descriptor =
            ::openat(parent->directory.get(), temporary.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (descriptor >= 0) {
            file = FileDescriptor{descriptor};
            break;
        }
        if (errno != EEXIST) {
            return failure("topology.cli.temporary_file_failed", "temporary output creation failed",
                           ExitCode::filesystem);
        }
    }
    if (!file.valid()) {
        return failure("topology.cli.temporary_file_failed", "temporary namespace exhausted",
                       ExitCode::filesystem);
    }

    const auto cleanup = [&] { (void)::unlinkat(parent->directory.get(), temporary.c_str(), 0); };
    if (!write_all(file.get(), bytes)) {
        cleanup();
        return failure("topology.cli.write_failed", "output write failed", ExitCode::filesystem);
    }
    if (::fsync(file.get()) < 0) {
        cleanup();
        return failure("topology.cli.sync_failed", "output synchronization failed",
                       ExitCode::filesystem);
    }
    const int raw = file.release();
    if (::close(raw) < 0) {
        cleanup();
        return failure("topology.cli.write_failed", "output close failed", ExitCode::filesystem);
    }

    const int renamed = force ? ::renameat(parent->directory.get(), temporary.c_str(),
                                           parent->directory.get(), parent->filename.c_str())
                              : rename_no_replace(parent->directory.get(), temporary.c_str(),
                                                  parent->filename.c_str());
    if (renamed < 0) {
        const int saved = errno;
        cleanup();
        if (!force && saved == EEXIST) {
            return failure("topology.cli.output_exists", "destination already exists",
                           ExitCode::filesystem);
        }
        return failure("topology.cli.publish_failed", "atomic publication failed",
                       ExitCode::filesystem);
    }
    if (::fsync(parent->directory.get()) < 0) {
        return failure("topology.cli.sync_failed", "published destination may already exist",
                       ExitCode::filesystem);
    }
    return {};
}

auto executable_directory() -> std::expected<std::filesystem::path, CliFailure> {
    std::array<char, 4096> buffer{};
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size() - 1U) {
        return failure("topology.cli.reference_fixture_unavailable",
                       "reference fixture resource unavailable", ExitCode::filesystem);
    }
    return std::filesystem::path{std::string{buffer.data(), static_cast<std::size_t>(length)}}
        .parent_path();
}

void print_failure(std::ostream& stream, std::string_view command, const CliFailure& value) {
    stream << "topology " << command << ": failed\n"
           << "code: " << value.code << "\n"
           << "classification: " << value.classification << "\n";
}

auto topology_failure(const core::Error& error, std::string classification,
                      ExitCode exit = ExitCode::topology) -> CliFailure {
    return {error.code.value(), std::move(classification), exit};
}

auto prepare_snapshot(TopologySnapshot topology, Environment& environment,
                      bool already_validated = false)
    -> std::expected<SerializedTopology, CliFailure> {
    if (!already_validated && !environment.validate(topology).valid()) {
        return failure("topology.fingerprint.invalid_topology", "topology validation failed",
                       ExitCode::topology);
    }
    auto ids = environment.derive_ids(topology);
    if (!ids) {
        return std::unexpected{topology_failure(ids.error(), "stable identity derivation failed")};
    }
    auto fingerprints = environment.derive_fingerprints(topology, *ids);
    if (!fingerprints) {
        return std::unexpected{
            topology_failure(fingerprints.error(), "fingerprint derivation failed")};
    }
    auto serialized = environment.serialize(topology, *ids, *fingerprints);
    if (!serialized) {
        return std::unexpected{topology_failure(serialized.error(), "serialization failed")};
    }
    return std::move(*serialized);
}

auto acquire_checked_snapshot(Environment& environment)
    -> std::expected<TopologySnapshot, CliFailure> {
    const detail::CheckedTopologyOperations operations{
        &environment,
        [](void* context) noexcept -> std::expected<TopologySourceEvidence, core::Error> {
            try {
                return static_cast<Environment*>(context)->discover_hwloc();
            } catch (...) {
                return std::unexpected{core::Error{
                    .category = core::ErrorCategory::topology,
                    .code =
                        core::StableErrorCode{"topology.checked_acquisition.contained_exception"},
                    .message = "Topology discovery failed",
                    .operation = "discover_checked_topology",
                    .os_error = std::nullopt,
                    .context = {{"failure_classification", "hwloc_discovery_exception"}},
                    .severity = core::ErrorSeverity::error,
                    .continuation = core::ContinuationPolicy::abort_command}};
            }
        },
        [](void* context) noexcept -> std::expected<TopologySourceEvidence, core::Error> {
            try {
                return static_cast<Environment*>(context)->discover_linux();
            } catch (...) {
                return std::unexpected{core::Error{
                    .category = core::ErrorCategory::topology,
                    .code =
                        core::StableErrorCode{"topology.checked_acquisition.contained_exception"},
                    .message = "Topology discovery failed",
                    .operation = "discover_checked_topology",
                    .os_error = std::nullopt,
                    .context = {{"failure_classification", "linux_discovery_exception"}},
                    .severity = core::ErrorSeverity::error,
                    .continuation = core::ContinuationPolicy::abort_command}};
            }
        },
        [](void* context, const TopologySourceEvidence& hwloc,
           const TopologySourceEvidence& linux) noexcept
            -> std::expected<ReconciliationResult, core::Error> {
            try {
                return static_cast<Environment*>(context)->reconcile(hwloc, linux);
            } catch (...) {
                return std::unexpected{core::Error{
                    .category = core::ErrorCategory::topology,
                    .code =
                        core::StableErrorCode{"topology.checked_acquisition.contained_exception"},
                    .message = "Topology reconciliation failed",
                    .operation = "discover_checked_topology",
                    .os_error = std::nullopt,
                    .context = {{"failure_classification", "reconciliation_exception"}},
                    .severity = core::ErrorSeverity::error,
                    .continuation = core::ContinuationPolicy::abort_command}};
            }
        },
        [](void* context, const TopologySnapshot& value) {
            return static_cast<Environment*>(context)->validate(value);
        }};
    auto result = detail::discover_checked_topology_with(operations);
    if (result)
        return std::move(*result);
    switch (result.error().stage) {
    case detail::CheckedTopologyStage::hwloc_discovery:
        return std::unexpected{
            topology_failure(result.error().error, "hwloc discovery failed", ExitCode::discovery)};
    case detail::CheckedTopologyStage::linux_discovery:
        return std::unexpected{
            topology_failure(result.error().error, "Linux discovery failed", ExitCode::discovery)};
    case detail::CheckedTopologyStage::reconciliation:
        if (result.error().error.code.value() == "topology.reconciliation.material_conflict")
            return std::unexpected{CliFailure{
                "topology.reconciliation.material_conflict",
                "topology reconciliation did not produce a safe snapshot", ExitCode::discovery}};
        return std::unexpected{topology_failure(
            result.error().error, "topology reconciliation failed", ExitCode::discovery)};
    case detail::CheckedTopologyStage::validation:
        return failure("topology.fingerprint.invalid_topology", "topology validation failed",
                       ExitCode::topology);
    }
    return failure("topology.cli.unexpected", "unexpected controller state", ExitCode::unexpected);
}

} // namespace

auto make_production_environment() -> Environment {
    return Environment{
        .discover_hwloc = [] { return discover_hwloc_topology(); },
        .discover_linux = [] { return discover_linux_topology(); },
        .reconcile = [](const auto& hwloc,
                        const auto& linux) { return reconcile_topology(hwloc, linux); },
        .load_fixture = []() -> std::expected<TopologySnapshot, ReferenceFixtureLoadError> {
            auto directory = executable_directory();
            if (!directory) {
                return std::unexpected{ReferenceFixtureLoadError{
                    .primary = {.category = core::ErrorCategory::topology,
                                .code =
                                    core::StableErrorCode{
                                        "topology.cli.reference_fixture_unavailable"},
                                .message = "Reference fixture resource unavailable",
                                .operation = "topology.cli.reference_fixture",
                                .os_error = std::nullopt,
                                .context = nlohmann::json::object(),
                                .severity = core::ErrorSeverity::error,
                                .continuation = core::ContinuationPolicy::abort_command},
                    .invariant_diagnostics = {}}};
            }
            return load_reference_fixture({*directory / fixture_relative_path});
        },
        .validate = validate_topology,
        .derive_ids = derive_stable_topology_ids,
        .derive_fingerprints = derive_topology_fingerprints,
        .serialize = serialize_topology,
        .deserialize = deserialize_topology,
        .read_bounded_file = read_regular_file,
        .publish_file = publish_regular_file,
    };
}

auto run(int argc, char* argv[], Environment& environment, std::ostream& output,
         std::ostream& error) -> int {
    CLI::App app{"Topology-linked benchmarking for AMD EPYC systems", "epycbench"};
    app.require_subcommand(0, 1);

    auto* topology = app.add_subcommand(
        "topology", "Export and validate canonical topology for package Scenarios and experiments");
    topology->require_subcommand(1, 1);
    auto* export_command = topology->add_subcommand("export", "Save a canonical topology snapshot");
    bool live = false;
    bool reference = false;
    bool force = false;
    std::filesystem::path output_path;
    export_command->add_flag("--live", live, "Use live read-only discovery");
    export_command->add_flag("--reference-fixture", reference,
                             "Use the staged development reference fixture");
    export_command->add_option("--output", output_path, "Output artifact path")->required();
    export_command->add_flag("--force", force, "Replace an existing regular file");

    auto* validate_command = topology->add_subcommand("validate", "Validate canonical topology");
    std::filesystem::path input_path;
    validate_command->add_option("--input", input_path, "Input artifact path")->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& parse_error) {
        if (parse_error.get_exit_code() == 0) {
            if (argc >= 2 && std::string_view{argv[1]} == "topology") {
                output << topology->help();
            } else {
                output << app.help();
            }
            return static_cast<int>(ExitCode::success);
        }
        print_failure(error, "command",
                      {"topology.cli.usage", "invalid command usage", ExitCode::usage});
        return static_cast<int>(ExitCode::usage);
    }

    if (*export_command) {
        if (live == reference) {
            print_failure(
                error, "export",
                {"topology.cli.usage", "exactly one source is required", ExitCode::usage});
            return static_cast<int>(ExitCode::usage);
        }

        std::expected<SerializedTopology, CliFailure> prepared =
            failure("topology.cli.unexpected", "unexpected controller state", ExitCode::unexpected);
        if (reference) {
            auto fixture = environment.load_fixture();
            if (!fixture) {
                const auto& primary = fixture.error().primary;
                const auto code = primary.code.value().starts_with("topology.cli.")
                                      ? primary.code.value()
                                      : "topology.cli.reference_fixture_unavailable";
                prepared = std::unexpected{
                    CliFailure{code, "reference fixture unavailable", ExitCode::filesystem}};
            } else {
                prepared = prepare_snapshot(std::move(*fixture), environment);
            }
        } else {
            auto checked = acquire_checked_snapshot(environment);
            if (!checked) {
                prepared = std::unexpected{std::move(checked.error())};
            } else {
                prepared = prepare_snapshot(std::move(*checked), environment, true);
            }
        }
        if (!prepared) {
            print_failure(error, "export", prepared.error());
            return static_cast<int>(prepared.error().exit_code);
        }
        auto published = environment.publish_file(output_path, prepared->canonical_json, force);
        if (!published) {
            print_failure(error, "export", published.error());
            return static_cast<int>(published.error().exit_code);
        }
        output << "topology export: passed\n"
               << "source: " << (live ? "live" : "reference-fixture") << "\n"
               << "output: written\n"
               << "physical fingerprint: " << prepared->fingerprints.physical.to_string() << "\n"
               << "effective fingerprint: " << prepared->fingerprints.effective.to_string() << "\n";
        return static_cast<int>(ExitCode::success);
    }

    if (*validate_command) {
        auto bytes = environment.read_file(input_path);
        if (!bytes) {
            print_failure(error, "validation", bytes.error());
            return static_cast<int>(bytes.error().exit_code);
        }
        auto loaded = environment.deserialize(*bytes, TopologyJsonInputPolicy::require_canonical);
        if (!loaded) {
            const auto value = topology_failure(loaded.error(), "topology artifact invalid");
            print_failure(error, "validation", value);
            return static_cast<int>(value.exit_code);
        }
        output << "topology validation: passed\n"
               << "schema: epycbench.topology.v1\n"
               << "physical fingerprint: " << loaded->fingerprints.physical.to_string() << "\n"
               << "effective fingerprint: " << loaded->fingerprints.effective.to_string() << "\n";
        return static_cast<int>(ExitCode::success);
    }

    output << app.help();
    return static_cast<int>(ExitCode::success);
}

} // namespace epycbench::topology::cli
