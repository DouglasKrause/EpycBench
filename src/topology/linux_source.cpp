#include "epycbench/topology/linux_source.hpp"

#include "epycbench/platform_linux/file.hpp"
#include "epycbench/topology/model.hpp"
#include "topology/linux_source_access.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <variant>

namespace epycbench::topology {
namespace {

using platform_linux::detail::trim_ascii_space;

using SourceIO = platform_linux::detail::PreparedTopologySourceIO;
using SourceRoot = platform_linux::detail::TopologySourceRoot;
using SourceReason = platform_linux::detail::TopologySourceReason;
using SourceCount = platform_linux::detail::TopologySourceCount;
struct ConversionStopped {};
void work(SourceIO* io, std::uint64_t units = 1) {
    if (io && !io->work(units)) throw ConversionStopped{};
}
void count(SourceIO* io, SourceCount axis, std::uint64_t value) {
    if (io && !io->count(axis, value)) throw ConversionStopped{};
}
struct ContextField {
    using Value = std::variant<std::string_view, std::string, std::uint64_t, std::int64_t, bool>;
    std::string_view name;
    Value value;
    ContextField(std::string_view key, std::string text) : name{key}, value{std::move(text)} {}
    ContextField(std::string_view key, std::string_view text) : name{key}, value{text} {}
    ContextField(std::string_view key, const char* text) : name{key}, value{std::string_view{text}} {}
    template<class T> requires std::is_integral_v<T>
    ContextField(std::string_view key, T number) : name{key}, value{numeric(number)} {}
    template<class T> static auto numeric(T number) -> Value {
        if constexpr (std::is_same_v<T, bool>) return number;
        else if constexpr (std::is_signed_v<T>) return static_cast<std::int64_t>(number);
        else return static_cast<std::uint64_t>(number);
    }
};
auto make_error(std::string_view code, std::string_view message, std::string_view operation,
                nlohmann::json context) -> core::Error {
    core::detail::JsonCleanup cleanup{context};
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::string{code}},
            .message = std::string{message}, .operation = std::string{operation},
            .os_error = std::nullopt, .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto make_error(std::string_view code, std::string_view message, std::string_view operation,
                std::initializer_list<ContextField> fields = {}) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup cleanup{context};
    for (const auto& field : fields)
        std::visit([&](const auto& value) { context[std::string{field.name}] = value; }, field.value);
    return make_error(code, message, operation, std::move(context));
}
auto io_error(const platform_linux::detail::TopologySourceFailure& failure,
              std::optional<core::Error>* raw_failure = nullptr) -> core::Error {
    const bool native = failure.reason == SourceReason::native || failure.reason == SourceReason::missing;
    const auto code = native ? (failure.native_error == EACCES || failure.native_error == EPERM
        ? "topology.linux.permission_denied" : "topology.linux.required_file_missing")
        : failure.reason == SourceReason::path || failure.reason == SourceReason::identity
        ? "topology.linux.invalid_root"
        : failure.reason == SourceReason::capacity || failure.reason == SourceReason::arithmetic
        ? "topology.linux.source_capacity" : "topology.linux.source_guard_failed";
    constexpr std::array<std::string_view, 3> names{"sys", "proc", "cgroup"};
    auto result = make_error(code, "Prepared Linux topology acquisition refused", "discover_linux_topology",
        {{"path", std::string{names[static_cast<std::size_t>(failure.root)]} + '/' + failure.relative.data()}});
    core::detail::JsonCleanup cleanup{result.context};
    if (failure.native_error)
        result.os_error = core::OsError::capture(std::error_code{failure.native_error, std::generic_category()});
    if (raw_failure && !*raw_failure) {
        raw_failure->emplace(std::move(result)); // actual cause before the later owning projection
        return io_error(failure);
    }
    return core::Error{std::move(result)};
}
struct EvidenceCleanup {
    TopologySourceEvidence* value;
    ~EvidenceCleanup() noexcept {
        if (value) for (auto& error : value->diagnostics) core::detail::retire_json(error.context);
    }
};

auto logical(std::string_view root_name, const std::filesystem::path& relative) -> std::string {
    return std::string{root_name} + '/' + relative.generic_string();
}

auto source_for(std::string_view root_name) -> TopologyEvidenceSource {
    return root_name == "proc" ? TopologyEvidenceSource::linux_procfs
                               : TopologyEvidenceSource::linux_sysfs;
}

auto provenance(std::string_view root_name, const std::filesystem::path& relative,
                SourceConfidence confidence = SourceConfidence::reported)
    -> std::vector<SourceProvenance> {
    return {{source_for(root_name), logical(root_name, relative), confidence}};
}

template <typename T>
auto observed(T value, std::string_view root_name, const std::filesystem::path& relative,
              SourceConfidence confidence = SourceConfidence::reported) -> SourceObservation<T> {
    return {SourceAvailability::available, std::move(value),
            provenance(root_name, relative, confidence)};
}

template <typename T>
auto unavailable(std::string_view root_name, const std::filesystem::path& relative)
    -> SourceObservation<T> {
    return {SourceAvailability::unavailable, std::nullopt,
            provenance(root_name, relative, SourceConfidence::unknown)};
}

struct SafeRoot {
    std::filesystem::path path;
    std::string name;
    SourceIO* io{};
    SourceRoot source_root{};
    std::optional<core::Error>* raw_failure{};
};

auto validate_root(const std::filesystem::path& path, std::string name)
    -> std::expected<SafeRoot, core::Error> {
    std::error_code status_error;
    if (!path.is_absolute() || path.lexically_normal() != path ||
        !std::filesystem::is_directory(path, status_error) || status_error) {
        return std::unexpected{make_error("topology.linux.invalid_root",
                                          "Linux topology root must be an absolute directory",
                                          "discover_linux_topology", {{"root", name}})};
    }
    const auto canonical = std::filesystem::weakly_canonical(path, status_error);
    if (status_error || canonical != path) {
        return std::unexpected{make_error("topology.linux.invalid_root",
                                          "Linux topology root must not traverse symlinks",
                                          "discover_linux_topology", {{"root", name}})};
    }
    return SafeRoot{canonical, std::move(name)};
}

auto safe_path(const SafeRoot& root, const std::filesystem::path& relative, bool required)
    -> std::expected<std::optional<std::filesystem::path>, core::Error> {
    if (relative.is_absolute() || relative.lexically_normal() != relative ||
        relative.string().starts_with("..")) {
        return std::unexpected{make_error(
            "topology.linux.invalid_root", "Linux topology path escapes its injected root",
            "discover_linux_topology", {{"path", logical(root.name, relative)}})};
    }
    auto current = root.path;
    for (const auto& component : relative) {
        current /= component;
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(current, status_error);
        if (status_error) {
            if (status_error == std::errc::permission_denied) {
                return std::unexpected{make_error(
                    "topology.linux.permission_denied",
                    "Permission denied while inspecting Linux topology evidence",
                    "discover_linux_topology", {{"path", logical(root.name, relative)}})};
            }
            if (!required && status_error == std::errc::no_such_file_or_directory) {
                return std::optional<std::filesystem::path>{};
            }
            return std::unexpected{make_error("topology.linux.required_file_missing",
                                              "Required Linux topology evidence is missing",
                                              "discover_linux_topology",
                                              {{"path", logical(root.name, relative)}})};
        }
        if (std::filesystem::is_symlink(status)) {
            const auto target = std::filesystem::weakly_canonical(current, status_error);
            const auto root_text = root.path.generic_string() + '/';
            if (status_error ||
                (target != root.path && !target.generic_string().starts_with(root_text))) {
                return std::unexpected{make_error(
                    "topology.linux.invalid_root",
                    "Linux topology evidence contains a symlink escaping its injected root",
                    "discover_linux_topology", {{"path", logical(root.name, relative)}})};
            }
        }
    }
    std::error_code status_error;
    const auto status = std::filesystem::status(current, status_error);
    if (status_error || !std::filesystem::exists(status)) {
        if (!required && (!status_error || status_error == std::errc::no_such_file_or_directory)) {
            return std::optional<std::filesystem::path>{};
        }
        return std::unexpected{make_error(
            "topology.linux.required_file_missing", "Required Linux topology evidence is missing",
            "discover_linux_topology", {{"path", logical(root.name, relative)}})};
    }
    if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) {
        return std::unexpected{make_error("topology.linux.required_file_missing",
                                          "Linux topology evidence has an unsupported file type",
                                          "discover_linux_topology",
                                          {{"path", logical(root.name, relative)}})};
    }
    return std::optional<std::filesystem::path>{current};
}

auto read_text(const SafeRoot& root, const std::filesystem::path& relative, bool required)
    -> std::expected<std::optional<std::string>, core::Error> {
    if (root.io) {
        auto text = root.io->read(root.source_root, relative.generic_string(), required);
        if (!text) return std::unexpected{io_error(text.error(), root.raw_failure)};
        return std::move(*text);
    }
    auto path = safe_path(root, relative, required);
    core::detail::ExpectedErrorCleanup path_cleanup{path};
    if (!path) {
        return std::unexpected{std::move(path.error())};
    }
    if (!*path) {
        return std::optional<std::string>{};
    }
    auto text = platform_linux::read_text_file(**path);
    core::detail::ExpectedErrorCleanup text_cleanup{text};
    if (!text) {
        const auto permission = text.error().code.value() == "linux.file.permission_denied";
        return std::unexpected{
            make_error(permission ? "topology.linux.permission_denied"
                                  : "topology.linux.required_file_missing",
                       permission ? "Permission denied while reading Linux topology evidence"
                                  : "Unable to read required Linux topology evidence",
                       "discover_linux_topology", {{"path", logical(root.name, relative)}})};
    }
    return std::optional<std::string>{std::move(*text)};
}

auto parse_cpu_set(std::string_view text, std::string path, SourceIO* io = nullptr)
    -> std::expected<core::CpuSet, core::Error> {
    work(io, text.size());
    const auto trimmed = trim_ascii_space(text);
    if (trimmed.empty()) {
        return core::CpuSet{};
    }
    auto parsed = core::CpuSet::parse(trimmed);
    if (!parsed) {
        return std::unexpected{make_error("topology.linux.malformed_cpu_list",
                                          "Linux CPU-list evidence is malformed",
                                          "discover_linux_topology", {{"path", std::move(path)}})};
    }
    count(io, SourceCount::memberships, parsed->cardinality());
    // Only admitted compressed ranges are expanded to validate numeric indices.
    if (io) for (const auto id : *parsed) { work(io); count(io, SourceCount::cpu_index, id.value()); }
    return std::move(*parsed);
}

auto parse_node_set(std::string_view text, std::string path, SourceIO* io = nullptr)
    -> std::expected<core::NodeSet, core::Error> {
    work(io, text.size());
    const auto trimmed = trim_ascii_space(text);
    if (trimmed.empty()) {
        return core::NodeSet{};
    }
    auto parsed = core::NodeSet::parse(trimmed);
    if (!parsed) {
        return std::unexpected{make_error("topology.linux.malformed_node_list",
                                          "Linux node-list evidence is malformed",
                                          "discover_linux_topology", {{"path", std::move(path)}})};
    }
    count(io, SourceCount::memberships, parsed->cardinality());
    // Only admitted compressed ranges are expanded to validate numeric indices.
    if (io) for (const auto id : *parsed) { work(io); count(io, SourceCount::node_index, id.value()); }
    return std::move(*parsed);
}

template <typename Integer>
auto parse_integer(std::string_view text, std::string path, std::optional<core::Error>* raw_failure = nullptr) -> std::expected<Integer, core::Error> {
    auto parsed = platform_linux::parse_integer<Integer>(text, "discover_linux_topology");
    core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
    if (!parsed) {
        if (raw_failure && !*raw_failure) raw_failure->emplace(std::move(parsed.error()));
        return std::unexpected{make_error("topology.linux.malformed_integer",
                                          "Linux integer topology evidence is malformed",
                                          "discover_linux_topology", {{"path", std::move(path)}})};
    }
    return *parsed;
}

auto parse_hex(std::string_view text, std::string path)
    -> std::expected<std::uint32_t, core::Error> {
    auto trimmed = trim_ascii_space(text);
    if (trimmed.starts_with("0x")) {
        trimmed.remove_prefix(2);
    }
    std::uint32_t value{};
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value, 16);
    if (trimmed.empty() || result.ec != std::errc{} ||
        result.ptr != trimmed.data() + trimmed.size()) {
        return std::unexpected{make_error("topology.linux.malformed_integer",
                                          "Linux hexadecimal topology evidence is malformed",
                                          "discover_linux_topology", {{"path", std::move(path)}})};
    }
    return value;
}

auto parse_size(std::string_view text, std::string path)
    -> std::expected<core::ByteCount, core::Error> {
    auto trimmed = trim_ascii_space(text);
    std::uint64_t multiplier = 1;
    if (!trimmed.empty()) {
        switch (static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed.back())))) {
        case 'K':
            multiplier = 1024;
            trimmed.remove_suffix(1);
            break;
        case 'M':
            multiplier = 1024 * 1024;
            trimmed.remove_suffix(1);
            break;
        case 'G':
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            trimmed.remove_suffix(1);
            break;
        default:
            break;
        }
    }
    std::uint64_t value{};
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (trimmed.empty() || result.ec != std::errc{} ||
        result.ptr != trimmed.data() + trimmed.size() ||
        value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return std::unexpected{make_error("topology.linux.malformed_size",
                                          "Linux size topology evidence is malformed",
                                          "discover_linux_topology", {{"path", std::move(path)}})};
    }
    return core::ByteCount::from_base_units(value * multiplier);
}

auto parse_cpu_map(std::string_view text, std::string path, SourceIO* io = nullptr)
    -> std::expected<core::CpuSet, core::Error> {
    std::vector<std::string_view> groups;
    auto remaining = trim_ascii_space(text);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        work(io); count(io, SourceCount::bitmap_groups, groups.size() + 1U);
        groups.push_back(remaining.substr(0, comma));
        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    if (io && groups.size() > (static_cast<std::uint64_t>(UINT32_MAX) + 1U) / 32U) {
        static_cast<void>(io->reject(SourceReason::arithmetic)); throw ConversionStopped{};
    }
    core::CpuSet result;
    for (std::size_t reverse_index = 0; reverse_index < groups.size(); ++reverse_index) {
        const auto token = groups[groups.size() - reverse_index - 1];
        std::uint32_t word{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), word, 16);
        if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
            return std::unexpected{make_error(
                "topology.linux.malformed_cpu_list", "Linux CPU bitmap evidence is malformed",
                "discover_linux_topology", {{"path", std::move(path)}})};
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            work(io);
            if ((word & (1U << bit)) != 0) {
                const auto cpu = static_cast<std::uint32_t>(reverse_index * 32) + bit;
                count(io, SourceCount::cpu_index, cpu);
                count(io, SourceCount::memberships, result.cardinality() + 1U);
                result = result.set_union(core::CpuSet{core::LinuxCpuId{cpu}});
            }
        }
    }
    return result;
}

auto numeric_directories(const SafeRoot& root, const std::filesystem::path& relative,
                         std::string_view prefix)
    -> std::expected<std::vector<std::pair<std::uint32_t, std::filesystem::path>>, core::Error> {
    if (root.io) {
        auto names = root.io->entries(root.source_root, relative.generic_string());
        if (!names) return std::unexpected{io_error(names.error(), root.raw_failure)};
        std::vector<std::pair<std::uint32_t, std::filesystem::path>> result;
        for (const auto& name : *names) {
            work(root.io);
            if (!name.starts_with(prefix)) continue;
            const auto suffix = std::string_view{name}.substr(prefix.size());
            std::uint32_t id{};
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), id);
            if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) continue;
            count(root.io, prefix == "cpu" ? SourceCount::cpu_index : SourceCount::cache_index, id);
            count(root.io, prefix == "cpu" ? SourceCount::enumerated_cpus : SourceCount::cache_entries_per_cpu, result.size() + 1U);
            auto checked = root.io->directory(root.source_root, (relative / name).generic_string(), true);
            if (!checked) return std::unexpected{io_error(checked.error(), root.raw_failure)};
            result.emplace_back(id, relative / name);
        }
        std::ranges::sort(result, [&](const auto& left, const auto& right) {
            work(root.io); return left.first < right.first;
        });
        return result;
    }
    auto directory = safe_path(root, relative, true);
    core::detail::ExpectedErrorCleanup directory_cleanup{directory};
    if (!directory)
        return std::unexpected{std::move(directory.error())};
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> result;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator{**directory, iteration_error}, end;
         iterator != end && !iteration_error; iterator.increment(iteration_error)) {
        const auto name = iterator->path().filename().string();
        if (!name.starts_with(prefix))
            continue;
        const auto suffix = std::string_view{name}.substr(prefix.size());
        std::uint32_t id{};
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), id);
        if (suffix.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != suffix.data() + suffix.size())
            continue;
        const auto status = iterator->symlink_status(iteration_error);
        if (iteration_error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return std::unexpected{make_error(
                "topology.linux.invalid_root",
                "Linux topology enumeration found an unsafe directory", "discover_linux_topology",
                {{"path", logical(root.name, relative / name)}})};
        }
        result.emplace_back(id, relative / name);
    }
    if (iteration_error) {
        const auto permission = iteration_error == std::errc::permission_denied;
        return std::unexpected{make_error(permission ? "topology.linux.permission_denied"
                                                     : "topology.linux.required_file_missing",
                                          "Unable to enumerate Linux topology evidence",
                                          "discover_linux_topology",
                                          {{"path", logical(root.name, relative)}})};
    }
    std::ranges::sort(result, {}, &decltype(result)::value_type::first);
    return result;
}

template <typename Set>
auto read_set(const SafeRoot& root, const std::filesystem::path& relative, bool required,
              auto parser) -> std::expected<SourceObservation<Set>, core::Error> {
    auto text = read_text(root, relative, required);
    core::detail::ExpectedErrorCleanup text_cleanup{text};
    if (!text)
        return std::unexpected{std::move(text.error())};
    if (!*text)
        return unavailable<Set>(root.name, relative);
    auto set = parser(**text, logical(root.name, relative), root.io);
    core::detail::ExpectedErrorCleanup set_cleanup{set};
    if (!set)
        return std::unexpected{std::move(set.error())};
    return observed(std::move(*set), root.name, relative);
}

struct CpuRecord {
    std::uint32_t cpu{};
    std::uint32_t package{};
    std::uint32_t core{};
    core::CpuSet siblings;
    std::optional<core::CpuSet> core_cpus;
    std::optional<std::uint32_t> die;
    std::optional<core::CpuSet> package_cpus;
    std::optional<core::CpuSet> die_cpus;
    std::optional<std::int64_t> cluster;
    std::optional<core::CpuSet> cluster_cpus;
    std::optional<std::uint32_t> book;
    std::optional<std::uint32_t> drawer;
};

auto read_optional_u32(const SafeRoot& root, const std::filesystem::path& path)
    -> std::expected<std::optional<std::uint32_t>, core::Error> {
    auto text = read_text(root, path, false);
    core::detail::ExpectedErrorCleanup text_cleanup{text};
    if (!text)
        return std::unexpected{std::move(text.error())};
    if (!*text)
        return std::optional<std::uint32_t>{};
    auto value = parse_integer<std::uint32_t>(**text, logical(root.name, path), root.raw_failure);
    core::detail::ExpectedErrorCleanup value_cleanup{value};
    if (!value)
        return std::unexpected{std::move(value.error())};
    return std::optional<std::uint32_t>{*value};
}

auto read_optional_i64(const SafeRoot& root, const std::filesystem::path& path)
    -> std::expected<std::optional<std::int64_t>, core::Error> {
    auto text = read_text(root, path, false);
    core::detail::ExpectedErrorCleanup text_cleanup{text};
    if (!text)
        return std::unexpected{std::move(text.error())};
    if (!*text)
        return std::optional<std::int64_t>{};
    auto value = parse_integer<std::int64_t>(**text, logical(root.name, path), root.raw_failure);
    core::detail::ExpectedErrorCleanup value_cleanup{value};
    if (!value)
        return std::unexpected{std::move(value.error())};
    return std::optional<std::int64_t>{*value};
}

auto read_optional_cpu_set(const SafeRoot& root, const std::filesystem::path& path)
    -> std::expected<std::optional<core::CpuSet>, core::Error> {
    auto text = read_text(root, path, false);
    core::detail::ExpectedErrorCleanup text_cleanup{text};
    if (!text)
        return std::unexpected{std::move(text.error())};
    if (!*text)
        return std::optional<core::CpuSet>{};
    auto value = parse_cpu_set(**text, logical(root.name, path), root.io);
    core::detail::ExpectedErrorCleanup value_cleanup{value};
    if (!value)
        return std::unexpected{std::move(value.error())};
    return std::optional<core::CpuSet>{std::move(*value)};
}

auto cache_kind(std::string_view text) -> SourceCacheKind {
    const auto value = trim_ascii_space(text);
    if (value == "Data")
        return SourceCacheKind::data;
    if (value == "Instruction")
        return SourceCacheKind::instruction;
    if (value == "Unified")
        return SourceCacheKind::unified;
    return SourceCacheKind::unknown;
}

auto parse_status_set(std::string_view status, std::string_view field, bool cpu, SourceIO* io)
    -> std::expected<std::string, core::Error> {
    work(io, status.size());
    std::size_t start = 0;
    while (start < status.size()) {
        work(io);
        const auto end = status.find('\n', start);
        const auto line = status.substr(start, end == std::string_view::npos ? status.size() - start
                                                                             : end - start);
        if (line.starts_with(field)) {
            const auto colon = line.find(':');
            if (colon != std::string_view::npos)
                return std::string{trim_ascii_space(line.substr(colon + 1))};
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return std::unexpected{make_error(
        cpu ? "topology.linux.malformed_cpu_list" : "topology.linux.malformed_node_list",
        "Process allowed-list evidence is missing", "discover_linux_topology", {{"field", field}})};
}

auto source_key(const core::CpuSet& cpus, std::optional<std::uint32_t> os_id) -> SourceObjectKey {
    return {cpus, os_id};
}

auto convert(const SafeRoot& sys, const SafeRoot& proc, const SafeRoot& cgroup)
    -> std::expected<TopologySourceEvidence, core::Error> {
    TopologySourceEvidence result;
    EvidenceCleanup result_cleanup{&result};
    work(sys.io);
    result.source = TopologyEvidenceSource::linux_sysfs;
    result.source_version = "linux-sysfs-procfs-v1";
    const auto cpu_base = std::filesystem::path{"devices/system/cpu"};
    const auto node_base = std::filesystem::path{"devices/system/node"};

    auto possible = read_set<core::CpuSet>(sys, cpu_base / "possible", true, parse_cpu_set);
    core::detail::ExpectedErrorCleanup possible_cleanup{possible};
    if (!possible) return std::unexpected{std::move(possible.error())};
    auto present = read_set<core::CpuSet>(sys, cpu_base / "present", true, parse_cpu_set);
    core::detail::ExpectedErrorCleanup present_cleanup{present};
    if (!present) return std::unexpected{std::move(present.error())};
    auto online_before = read_set<core::CpuSet>(sys, cpu_base / "online", true, parse_cpu_set);
    core::detail::ExpectedErrorCleanup online_before_cleanup{online_before};
    if (!online_before) return std::unexpected{std::move(online_before.error())};
    if (!possible)
        return std::unexpected{std::move(possible.error())};
    if (!present)
        return std::unexpected{std::move(present.error())};
    if (!online_before)
        return std::unexpected{std::move(online_before.error())};
    result.machine.cpu_sets.possible = *possible;
    result.machine.cpu_sets.present = *present;
    result.machine.cpu_sets.online = *online_before;
    for (const auto& [name, target] :
         std::array<std::pair<const char*, SourceObservation<core::CpuSet>*>, 3>{
             {{"offline", &result.machine.cpu_sets.offline},
              {"isolated", &result.machine.cpu_sets.isolated},
              {"nohz_full", &result.machine.cpu_sets.nohz_full}}}) {
        auto value = read_set<core::CpuSet>(sys, cpu_base / name, false, parse_cpu_set);
        core::detail::ExpectedErrorCleanup value_cleanup{value};
        if (!value)
            return std::unexpected{std::move(value.error())};
        *target = std::move(*value);
    }

    auto node_possible = read_set<core::NodeSet>(sys, node_base / "possible", true, parse_node_set);
    core::detail::ExpectedErrorCleanup node_possible_cleanup{node_possible};
    if (!node_possible) return std::unexpected{std::move(node_possible.error())};
    auto node_online_before =
        read_set<core::NodeSet>(sys, node_base / "online", true, parse_node_set);
    core::detail::ExpectedErrorCleanup node_online_before_cleanup{node_online_before};
    if (!node_online_before) return std::unexpected{std::move(node_online_before.error())};
    if (!node_possible)
        return std::unexpected{std::move(node_possible.error())};
    if (!node_online_before)
        return std::unexpected{std::move(node_online_before.error())};
    result.machine.node_sets.possible = *node_possible;
    result.machine.node_sets.online = *node_online_before;
    auto has_cpu = read_set<core::NodeSet>(sys, node_base / "has_cpu", false, parse_node_set);
    core::detail::ExpectedErrorCleanup has_cpu_cleanup{has_cpu};
    if (!has_cpu) return std::unexpected{std::move(has_cpu.error())};
    auto has_memory = read_set<core::NodeSet>(sys, node_base / "has_memory", false, parse_node_set);
    core::detail::ExpectedErrorCleanup has_memory_cleanup{has_memory};
    if (!has_memory) return std::unexpected{std::move(has_memory.error())};
    if (!has_cpu)
        return std::unexpected{std::move(has_cpu.error())};
    if (!has_memory)
        return std::unexpected{std::move(has_memory.error())};
    result.machine.node_sets.has_cpu = *has_cpu;
    result.machine.node_sets.has_memory = *has_memory;

    std::vector<CpuRecord> cpu_records;
    auto cpu_directories = numeric_directories(sys, cpu_base, "cpu");
    core::detail::ExpectedErrorCleanup cpu_directories_cleanup{cpu_directories};
    if (!cpu_directories)
        return std::unexpected{std::move(cpu_directories.error())};
    const std::set<std::uint32_t> enumerated_cpus = [&] {
        std::set<std::uint32_t> values;
        for (const auto& [id, path] : *cpu_directories) {
            work(sys.io);
            static_cast<void>(path);
            values.insert(id);
        }
        return values;
    }();
    count(sys.io, SourceCount::online_cpus, online_before->value->cardinality());
    for (const auto cpu_id : *online_before->value) {
        work(sys.io);
        if (!enumerated_cpus.contains(cpu_id.value()))
            return std::unexpected{make_error(
                "topology.linux.incomplete_cpu_topology", "An online CPU has no cpuN directory",
                "discover_linux_topology", {{"linux_cpu", cpu_id.value()}})};
        const auto base = cpu_base / ("cpu" + std::to_string(cpu_id.value())) / "topology";
        auto package_text = read_text(sys, base / "physical_package_id", true);
        core::detail::ExpectedErrorCleanup package_text_cleanup{package_text};
        if (!package_text) return std::unexpected{make_error("topology.linux.incomplete_cpu_topology", "Required online CPU topology evidence is incomplete", "discover_linux_topology", {{"linux_cpu", cpu_id.value()}})};
        auto core_text = read_text(sys, base / "core_id", true);
        core::detail::ExpectedErrorCleanup core_text_cleanup{core_text};
        if (!core_text) return std::unexpected{make_error("topology.linux.incomplete_cpu_topology", "Required online CPU topology evidence is incomplete", "discover_linux_topology", {{"linux_cpu", cpu_id.value()}})};
        auto siblings_text = read_text(sys, base / "thread_siblings_list", true);
        core::detail::ExpectedErrorCleanup siblings_text_cleanup{siblings_text};
        if (!siblings_text) return std::unexpected{make_error("topology.linux.incomplete_cpu_topology", "Required online CPU topology evidence is incomplete", "discover_linux_topology", {{"linux_cpu", cpu_id.value()}})};
        if (!package_text || !core_text || !siblings_text)
            return std::unexpected{make_error("topology.linux.incomplete_cpu_topology",
                                              "Required online CPU topology evidence is incomplete",
                                              "discover_linux_topology",
                                              {{"linux_cpu", cpu_id.value()}})};
        auto package = parse_integer<std::uint32_t>(
            **package_text, logical(sys.name, base / "physical_package_id"), sys.raw_failure);
        core::detail::ExpectedErrorCleanup package_cleanup{package};
        if (!package) return std::unexpected{std::move(package.error())};
        auto core_id =
            parse_integer<std::uint32_t>(**core_text, logical(sys.name, base / "core_id"), sys.raw_failure);
        core::detail::ExpectedErrorCleanup core_id_cleanup{core_id};
        if (!core_id) return std::unexpected{std::move(core_id.error())};
        auto siblings =
            parse_cpu_set(**siblings_text, logical(sys.name, base / "thread_siblings_list"), sys.io);
        core::detail::ExpectedErrorCleanup siblings_cleanup{siblings};
        if (!siblings) return std::unexpected{std::move(siblings.error())};
        if (!package || !core_id || !siblings) {
            if (!package)
                return std::unexpected{std::move(package.error())};
            if (!core_id)
                return std::unexpected{std::move(core_id.error())};
            return std::unexpected{std::move(siblings.error())};
        }
        CpuRecord record{.cpu = cpu_id.value(),
                         .package = *package,
                         .core = *core_id,
                         .siblings = std::move(*siblings),
                         .core_cpus = std::nullopt,
                         .die = std::nullopt,
                         .package_cpus = std::nullopt,
                         .die_cpus = std::nullopt,
                         .cluster = std::nullopt,
                         .cluster_cpus = std::nullopt,
                         .book = std::nullopt,
                         .drawer = std::nullopt};
        auto die = read_optional_u32(sys, base / "die_id");
        core::detail::ExpectedErrorCleanup die_cleanup{die};
        if (!die) return std::unexpected{std::move(die.error())};
        auto core_cpus = read_optional_cpu_set(sys, base / "core_cpus_list");
        core::detail::ExpectedErrorCleanup core_cpus_cleanup{core_cpus};
        if (!core_cpus) return std::unexpected{std::move(core_cpus.error())};
        auto package_cpus = read_optional_cpu_set(sys, base / "package_cpus_list");
        core::detail::ExpectedErrorCleanup package_cpus_cleanup{package_cpus};
        if (!package_cpus) return std::unexpected{std::move(package_cpus.error())};
        auto die_cpus = read_optional_cpu_set(sys, base / "die_cpus_list");
        core::detail::ExpectedErrorCleanup die_cpus_cleanup{die_cpus};
        if (!die_cpus) return std::unexpected{std::move(die_cpus.error())};
        auto cluster = read_optional_i64(sys, base / "cluster_id");
        core::detail::ExpectedErrorCleanup cluster_cleanup{cluster};
        if (!cluster) return std::unexpected{std::move(cluster.error())};
        auto cluster_cpus = read_optional_cpu_set(sys, base / "cluster_cpus_list");
        core::detail::ExpectedErrorCleanup cluster_cpus_cleanup{cluster_cpus};
        if (!cluster_cpus) return std::unexpected{std::move(cluster_cpus.error())};
        auto book = read_optional_u32(sys, base / "book_id");
        core::detail::ExpectedErrorCleanup book_cleanup{book};
        if (!book) return std::unexpected{std::move(book.error())};
        auto drawer = read_optional_u32(sys, base / "drawer_id");
        core::detail::ExpectedErrorCleanup drawer_cleanup{drawer};
        if (!drawer) return std::unexpected{std::move(drawer.error())};
        if (!die || !core_cpus || !package_cpus || !die_cpus || !cluster || !cluster_cpus ||
            !book || !drawer)
            return std::unexpected{!die            ? std::move(die.error())
                                   : !core_cpus    ? std::move(core_cpus.error())
                                   : !package_cpus ? std::move(package_cpus.error())
                                   : !die_cpus     ? std::move(die_cpus.error())
                                   : !cluster      ? std::move(cluster.error())
                                   : !cluster_cpus ? std::move(cluster_cpus.error())
                                   : !book         ? std::move(book.error())
                                                   : std::move(drawer.error())};
        record.core_cpus = *core_cpus;
        record.die = *die;
        record.package_cpus = *package_cpus;
        record.die_cpus = *die_cpus;
        record.cluster = *cluster;
        record.cluster_cpus = *cluster_cpus;
        record.book = *book;
        record.drawer = *drawer;
        cpu_records.push_back(std::move(record));
    }

    std::map<std::uint32_t, core::CpuSet> package_members;
    std::map<std::pair<std::uint32_t, std::uint32_t>, core::CpuSet> die_members;
    std::map<std::pair<std::uint32_t, std::uint32_t>, core::CpuSet> core_members;
    const auto merge_membership =
        [](core::CpuSet& accumulated, const std::optional<core::CpuSet>& reported,
           core::LinuxCpuId cpu,
           std::string_view relationship) -> std::expected<void, core::Error> {
        if (reported) {
            if (!reported->contains(cpu) || (!accumulated.empty() && accumulated != *reported)) {
                return std::unexpected{make_error(
                    "topology.linux.incomplete_cpu_topology",
                    "Online CPUs disagree about reported topology membership",
                    "discover_linux_topology", {{"relationship", std::string{relationship}}})};
            }
            accumulated = *reported;
        } else {
            accumulated = accumulated.set_union(core::CpuSet{cpu});
        }
        return {};
    };
    for (const auto& record : cpu_records) {
        work(sys.io);
        const auto cpu = core::LinuxCpuId{record.cpu};
        auto& package_set = package_members[record.package];
        if (auto merged = merge_membership(package_set, record.package_cpus, cpu, "package");
            !merged) {
            return std::unexpected{std::move(merged.error())};
        }
        auto& core_set = core_members[{record.package, record.core}];
        const auto reported_core = record.core_cpus ? *record.core_cpus : record.siblings;
        if (!record.siblings.contains(cpu) ||
            (record.core_cpus && *record.core_cpus != record.siblings))
            return std::unexpected{make_error("topology.linux.incomplete_cpu_topology",
                                              "Core and thread-sibling membership disagree",
                                              "discover_linux_topology")};
        if (!core_set.empty() && core_set != reported_core)
            return std::unexpected{make_error("topology.linux.incomplete_cpu_topology",
                                              "Online CPUs disagree about core membership",
                                              "discover_linux_topology")};
        core_set = reported_core;
        if (record.die) {
            auto& die_set = die_members[{record.package, *record.die}];
            if (auto merged = merge_membership(die_set, record.die_cpus, cpu, "die"); !merged) {
                return std::unexpected{std::move(merged.error())};
            }
        }
    }
    using ClusterKey =
        std::tuple<std::uint32_t, std::optional<std::uint32_t>, std::string>;
    struct ClusterRecord {
        core::CpuSet members;
        std::optional<std::uint32_t> usable_id;
    };
    const auto usable_cluster_id = [](const std::optional<std::int64_t>& reported)
        -> std::optional<std::uint32_t> {
        constexpr std::int64_t default_cluster_id = 65535;
        if (!reported || *reported < 0 || *reported == default_cluster_id ||
            static_cast<std::uint64_t>(*reported) >
                std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        return static_cast<std::uint32_t>(*reported);
    };
    const auto cluster_context = [&](const CpuRecord& record, std::string_view field,
                                     std::string_view classification) {
        auto context = nlohmann::json::object();
        core::detail::JsonCleanup cleanup{context};
        context = core::detail::json_object<nlohmann::json>([&](auto& object) {
            object["linux_cpu"] = record.cpu;
            object["field"] = field;
            object["package_id"] = record.package;
            object["path"] = logical("sys", cpu_base / ("cpu" + std::to_string(record.cpu)) / ("topology/" + std::string{field}));
            object["classification"] = classification;
        });
        if (record.die)
            context["die_id"] = *record.die;
        if (record.cluster)
            context["cluster_id"] = *record.cluster;
        // Direct move preserves the object and leaves the guarded local null;
        // braces would select the allocating initializer-list array constructor.
        return nlohmann::json(std::move(context));
    };
    if (sys.io) {
        std::uint64_t maximum_members{};
        for (const auto& record : cpu_records) {
            work(sys.io);
            if (record.cluster_cpus) maximum_members = std::max(maximum_members, record.cluster_cpus->cardinality());
        }
        if (!sys.io->cluster_work_for(cpu_records.size(), maximum_members)) throw ConversionStopped{};
    }
    std::map<ClusterKey, ClusterRecord> clusters;
    std::map<std::uint32_t, ClusterKey> cpu_cluster_keys;
    using GroupKey = std::tuple<std::string, std::uint32_t, std::uint32_t>;
    std::map<GroupKey, core::CpuSet> group_members;
    for (const auto& record : cpu_records) {
        work(sys.io);
        const auto cpu = core::LinuxCpuId{record.cpu};
        if (record.cluster_cpus) {
            if (!record.cluster_cpus->contains(cpu)) {
                return std::unexpected{make_error(
                    "topology.linux.cluster_membership_conflict",
                    "Linux cluster membership omits its reporting CPU",
                    "discover_linux_topology",
                    cluster_context(record, "cluster_cpus_list", "inconsistent membership"))};
            }
            std::optional<std::uint32_t> containing_die = record.die;
            for (const auto member : *record.cluster_cpus) {
                work(sys.io);
                const auto found = std::ranges::find_if(cpu_records, [&](const auto& candidate) {
                    work(sys.io); return candidate.cpu == member.value();
                });
                if (found == cpu_records.end())
                    continue;
                if (found->package != record.package ||
                    (containing_die && found->die && *containing_die != *found->die)) {
                    return std::unexpected{make_error(
                        "topology.linux.cluster_membership_conflict",
                        "Linux cluster membership crosses containment context",
                        "discover_linux_topology",
                        cluster_context(record, "cluster_cpus_list", "inconsistent membership"))};
                }
                if (!containing_die && found->die)
                    containing_die = found->die;
                if (found->cluster_cpus && *found->cluster_cpus != *record.cluster_cpus) {
                    return std::unexpected{make_error(
                        "topology.linux.cluster_membership_conflict",
                        "Linux cluster member CPUs report different memberships",
                        "discover_linux_topology",
                        cluster_context(record, "cluster_cpus_list", "inconsistent membership"))};
                }
            }
            const ClusterKey key{record.package, containing_die,
                                 record.cluster_cpus->to_range_list()};
            const auto reported_id = usable_cluster_id(record.cluster);
            auto [position, inserted] =
                clusters.try_emplace(key, ClusterRecord{*record.cluster_cpus, reported_id});
            if (!inserted && position->second.usable_id && reported_id &&
                position->second.usable_id != reported_id) {
                return std::unexpected{make_error(
                    "topology.linux.cluster_id_conflict",
                    "Linux cluster members report contradictory usable numeric IDs",
                    "discover_linux_topology",
                    cluster_context(record, "cluster_id", "contradictory usable cluster ID"))};
            }
            if (!position->second.usable_id)
                position->second.usable_id = reported_id;
            cpu_cluster_keys.emplace(record.cpu, key);
        }
        if (record.book)
            group_members[{"linux-book", record.package, *record.book}] =
                group_members[{"linux-book", record.package, *record.book}].set_union(
                    core::CpuSet{cpu});
        if (record.drawer)
            group_members[{"linux-drawer", record.package, *record.drawer}] =
                group_members[{"linux-drawer", record.package, *record.drawer}].set_union(
                    core::CpuSet{cpu});
    }
    for (const auto& [key, cluster] : clusters) {
        work(sys.io);
        result.architecture_groups.push_back(
            {source_key(cluster.members, cluster.usable_id), "linux-cluster",
             unavailable<std::string>("sys", cpu_base / "cpu*/topology"), std::nullopt,
             provenance("sys", cpu_base / "cpu*/topology", SourceConfidence::derived)});
    }
    for (const auto& [key, members] : group_members) {
        work(sys.io);
        result.architecture_groups.push_back(
            {source_key(members, std::get<2>(key)), std::get<0>(key),
             unavailable<std::string>("sys", cpu_base / "cpu*/topology"), std::nullopt,
             provenance("sys", cpu_base / "cpu*/topology", SourceConfidence::derived)});
    }
    for (const auto& [id, cpus] : package_members) {
        work(sys.io);
        result.packages.push_back(
            {source_key(cpus, id), provenance("sys", cpu_base / "cpu*/topology/physical_package_id",
                                              SourceConfidence::derived)});
    }
    for (const auto& [ids, cpus] : die_members) {
        work(sys.io);
        result.dies.push_back(
            {source_key(cpus, ids.second), source_key(package_members.at(ids.first), ids.first),
             provenance("sys", cpu_base / "cpu*/topology/die_id", SourceConfidence::derived)});
    }
    for (const auto& [ids, cpus] : core_members) {
        work(sys.io);
        const auto die = std::ranges::find_if(cpu_records, [&](const auto& record) {
                             work(sys.io);
                             return record.package == ids.first && record.core == ids.second;
                         })->die;
        result.cores.push_back(
            {source_key(cpus, ids.second), source_key(package_members.at(ids.first), ids.first),
             die ? std::optional<SourceObjectKey>{source_key(die_members.at({ids.first, *die}),
                                                             *die)}
                 : std::nullopt,
             provenance("sys", cpu_base / "cpu*/topology/core_id", SourceConfidence::derived)});
    }
    for (const auto& record : cpu_records) {
        work(sys.io);
        const auto core_key =
            source_key(core_members.at({record.package, record.core}), record.core);
        const auto package_key = source_key(package_members.at(record.package), record.package);
        std::vector<SourceObjectKey> groups;
        if (const auto cluster = cpu_cluster_keys.find(record.cpu);
            cluster != cpu_cluster_keys.end()) {
            const auto& evidence = clusters.at(cluster->second);
            groups.push_back(source_key(evidence.members, evidence.usable_id));
        }
        if (record.book)
            groups.push_back(source_key(
                group_members.at({"linux-book", record.package, *record.book}), *record.book));
        if (record.drawer)
            groups.push_back(
                source_key(group_members.at({"linux-drawer", record.package, *record.drawer}),
                           *record.drawer));
        result.processing_units.push_back(
            {core::LinuxCpuId{record.cpu},
             observed(true, "sys", cpu_base / ("cpu" + std::to_string(record.cpu))), core_key,
             package_key,
             record.die ? std::optional<SourceObjectKey>{source_key(
                              die_members.at({record.package, *record.die}), *record.die)}
                        : std::nullopt,
             groups, provenance("sys", cpu_base / ("cpu" + std::to_string(record.cpu)))});
    }
    std::ranges::sort(result.processing_units, [&](const auto& left, const auto& right) {
        work(sys.io);
        return left.linux_cpu_id < right.linux_cpu_id;
    });

    std::uint64_t cache_visits{};
    std::map<std::tuple<std::uint32_t, SourceCacheKind, std::string>, CacheSourceEvidence> caches;
    for (const auto& record : cpu_records) {
        work(sys.io);
        const auto cache_root = cpu_base / ("cpu" + std::to_string(record.cpu)) / "cache";
        auto cache_dirs = numeric_directories(sys, cache_root, "index");
        core::detail::ExpectedErrorCleanup cache_dirs_cleanup{cache_dirs};
        if (!cache_dirs)
            return std::unexpected{std::move(cache_dirs.error())};
        count(sys.io, SourceCount::cache_entries_per_cpu, cache_dirs->size());
        if (sys.io && cache_dirs->size() > UINT64_MAX - cache_visits) {
            static_cast<void>(sys.io->reject(SourceReason::arithmetic)); throw ConversionStopped{};
        }
        cache_visits += cache_dirs->size(); count(sys.io, SourceCount::cache_entries, cache_visits);
        for (const auto& [index, base] : *cache_dirs) {
            work(sys.io);
            static_cast<void>(index);
            auto level_text = read_text(sys, base / "level", true);
            core::detail::ExpectedErrorCleanup level_text_cleanup{level_text};
            if (!level_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required cache topology evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
            auto type_text = read_text(sys, base / "type", true);
            core::detail::ExpectedErrorCleanup type_text_cleanup{type_text};
            if (!type_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required cache topology evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
            auto size_text = read_text(sys, base / "size", true);
            core::detail::ExpectedErrorCleanup size_text_cleanup{size_text};
            if (!size_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required cache topology evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
            auto line_text = read_text(sys, base / "coherency_line_size", true);
            core::detail::ExpectedErrorCleanup line_text_cleanup{line_text};
            if (!line_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required cache topology evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
            auto shared_text = read_text(sys, base / "shared_cpu_list", true);
            core::detail::ExpectedErrorCleanup shared_text_cleanup{shared_text};
            if (!shared_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required cache topology evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
            if (!level_text || !type_text || !size_text || !line_text || !shared_text)
                return std::unexpected{make_error("topology.linux.required_file_missing",
                                                  "Required cache topology evidence is missing",
                                                  "discover_linux_topology",
                                                  {{"path", logical(sys.name, base)}})};
            auto level =
                parse_integer<std::uint32_t>(**level_text, logical(sys.name, base / "level"), sys.raw_failure);
            core::detail::ExpectedErrorCleanup level_cleanup{level};
            if (!level) return std::unexpected{std::move(level.error())};
            auto size = parse_size(**size_text, logical(sys.name, base / "size"));
            core::detail::ExpectedErrorCleanup size_cleanup{size};
            if (!size) return std::unexpected{std::move(size.error())};
            auto line = parse_size(**line_text, logical(sys.name, base / "coherency_line_size"));
            core::detail::ExpectedErrorCleanup line_cleanup{line};
            if (!line) return std::unexpected{std::move(line.error())};
            auto shared = parse_cpu_set(**shared_text, logical(sys.name, base / "shared_cpu_list"), sys.io);
            core::detail::ExpectedErrorCleanup shared_cleanup{shared};
            if (!shared) return std::unexpected{std::move(shared.error())};
            if (!level)
                return std::unexpected{std::move(level.error())};
            if (!size)
                return std::unexpected{std::move(size.error())};
            if (!line)
                return std::unexpected{std::move(line.error())};
            if (!shared)
                return std::unexpected{std::move(shared.error())};
            const auto kind = cache_kind(**type_text);
            const auto key = std::tuple{*level, kind, shared->to_range_list()};
            CacheSourceEvidence evidence{
                .level = *level,
                .kind = kind,
                .size = *size,
                .line_size = *line,
                .associativity = unavailable<std::uint32_t>("sys", base / "ways_of_associativity"),
                .source_cache_id = unavailable<std::uint32_t>("sys", base / "id"),
                .number_of_sets = unavailable<std::uint64_t>("sys", base / "number_of_sets"),
                .physical_line_partition =
                    unavailable<std::uint32_t>("sys", base / "physical_line_partition"),
                .allocation_policy = unavailable<std::string>("sys", base / "allocation_policy"),
                .write_policy = unavailable<std::string>("sys", base / "write_policy"),
                .shared_cpus = *shared,
                .provenance = provenance("sys", base)};
            const auto read_optional_integer =
                [&](const char* name, auto& target) -> std::expected<void, core::Error> {
                auto text = read_text(sys, base / name, false);
                core::detail::ExpectedErrorCleanup text_cleanup{text};
                if (!text)
                    return std::unexpected{std::move(text.error())};
                if (*text) {
                    using Value =
                        typename std::remove_reference_t<decltype(target.value)>::value_type;
                    auto value = parse_integer<Value>(**text, logical(sys.name, base / name), sys.raw_failure);
                    core::detail::ExpectedErrorCleanup value_cleanup{value};
                    if (!value)
                        return std::unexpected{std::move(value.error())};
                    target = observed(*value, "sys", base / name);
                }
                return {};
            };
            if (auto r = read_optional_integer("id", evidence.source_cache_id); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = read_optional_integer("ways_of_associativity", evidence.associativity); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = read_optional_integer("number_of_sets", evidence.number_of_sets); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = read_optional_integer("physical_line_partition",
                                               evidence.physical_line_partition);
                !r)
                return std::unexpected{std::move(r.error())};
            for (const auto* name : {"allocation_policy", "write_policy"}) {
                work(sys.io);
                auto text = read_text(sys, base / name, false);
                core::detail::ExpectedErrorCleanup text_cleanup{text};
                if (!text)
                    return std::unexpected{std::move(text.error())};
                if (*text) {
                    auto& target = std::string_view{name} == "allocation_policy"
                                       ? evidence.allocation_policy
                                       : evidence.write_policy;
                    target = observed(std::string{trim_ascii_space(**text)}, "sys", base / name);
                }
            }
            const auto existing = caches.find(key);
            if (existing == caches.end())
                caches.emplace(key, std::move(evidence));
            else {
                const auto same_observation = [](const auto& left, const auto& right) {
                    return left.availability == right.availability && left.value == right.value;
                };
                const auto agrees =
                    evidence.level == existing->second.level &&
                    evidence.kind == existing->second.kind &&
                    evidence.size == existing->second.size &&
                    evidence.line_size == existing->second.line_size &&
                    evidence.shared_cpus == existing->second.shared_cpus &&
                    same_observation(evidence.associativity, existing->second.associativity) &&
                    same_observation(evidence.source_cache_id, existing->second.source_cache_id) &&
                    same_observation(evidence.number_of_sets, existing->second.number_of_sets) &&
                    same_observation(evidence.physical_line_partition,
                                     existing->second.physical_line_partition) &&
                    same_observation(evidence.allocation_policy,
                                     existing->second.allocation_policy) &&
                    same_observation(evidence.write_policy, existing->second.write_policy);
                if (!agrees)
                    return std::unexpected{
                        make_error("topology.linux.incomplete_cpu_topology",
                                   "Cache observations with one semantic key disagree",
                                   "discover_linux_topology")};
                existing->second.provenance.insert(existing->second.provenance.end(),
                                                   evidence.provenance.begin(),
                                                   evidence.provenance.end());
            }
        }
    }
    for (auto& [key, cache] : caches) {
        work(sys.io);
        static_cast<void>(key);
        result.caches.push_back(std::move(cache));
    }

    const auto online_nodes = *node_online_before->value;
    count(sys.io, SourceCount::online_nodes, online_nodes.cardinality());
    if (sys.io && !sys.io->distance_values_for(online_nodes.cardinality())) throw ConversionStopped{};
    NumaDistanceSourceEvidence distances{.name = "LinuxNodeDistance",
                                         .meaning = DistanceMeaning::latency,
                                         .units = "linux-relative-distance",
                                         .linux_nodes = {},
                                         .directed_values = {},
                                         .provenance =
                                             provenance("sys", node_base / "node*/distance")};
    for (const auto node_id : online_nodes) {
        work(sys.io);
        distances.linux_nodes.push_back(node_id.value());
    }
    for (const auto node_id : online_nodes) {
        work(sys.io);
        const auto base = node_base / ("node" + std::to_string(node_id.value()));
        auto cpulist = read_text(sys, base / "cpulist", true);
        core::detail::ExpectedErrorCleanup cpulist_cleanup{cpulist};
        if (!cpulist) return std::unexpected{make_error("topology.linux.incomplete_numa_topology", "Required NUMA topology evidence is incomplete", "discover_linux_topology", {{"linux_node", node_id.value()}})};
        auto distance = read_text(sys, base / "distance", true);
        core::detail::ExpectedErrorCleanup distance_cleanup{distance};
        if (!distance) return std::unexpected{make_error("topology.linux.incomplete_numa_topology", "Required NUMA topology evidence is incomplete", "discover_linux_topology", {{"linux_node", node_id.value()}})};
        auto meminfo = read_text(sys, base / "meminfo", true);
        core::detail::ExpectedErrorCleanup meminfo_cleanup{meminfo};
        if (!meminfo) return std::unexpected{make_error("topology.linux.incomplete_numa_topology", "Required NUMA topology evidence is incomplete", "discover_linux_topology", {{"linux_node", node_id.value()}})};
        if (!cpulist || !distance || !meminfo)
            return std::unexpected{make_error("topology.linux.incomplete_numa_topology",
                                              "Required NUMA topology evidence is incomplete",
                                              "discover_linux_topology",
                                              {{"linux_node", node_id.value()}})};
        auto cpus = parse_cpu_set(**cpulist, logical(sys.name, base / "cpulist"), sys.io);
        core::detail::ExpectedErrorCleanup cpus_cleanup{cpus};
        if (!cpus)
            return std::unexpected{std::move(cpus.error())};
        auto cpumap = read_text(sys, base / "cpumap", false);
        core::detail::ExpectedErrorCleanup cpumap_cleanup{cpumap};
        if (!cpumap)
            return std::unexpected{std::move(cpumap.error())};
        if (*cpumap) {
            auto mapped = parse_cpu_map(**cpumap, logical(sys.name, base / "cpumap"), sys.io);
            core::detail::ExpectedErrorCleanup mapped_cleanup{mapped};
            if (!mapped)
                return std::unexpected{std::move(mapped.error())};
            if (*mapped != *cpus)
                return std::unexpected{make_error(
                    "topology.linux.incomplete_numa_topology", "NUMA cpulist and cpumap disagree",
                    "discover_linux_topology", {{"linux_node", node_id.value()}})};
        }
        const auto marker = std::string{"MemTotal:"};
        work(sys.io, (**meminfo).size());
        const auto marker_pos = (**meminfo).find(marker);
        if (marker_pos == std::string::npos)
            return std::unexpected{make_error(
                "topology.linux.malformed_size", "NUMA MemTotal evidence is missing",
                "discover_linux_topology", {{"path", logical(sys.name, base / "meminfo")}})};
        auto memory_text =
            trim_ascii_space(std::string_view{**meminfo}.substr(marker_pos + marker.size()));
        const auto newline = memory_text.find('\n');
        if (newline != std::string_view::npos)
            memory_text = memory_text.substr(0, newline);
        if (memory_text.ends_with("kB"))
            memory_text.remove_suffix(2);
        auto kb = parse_integer<std::uint64_t>(memory_text, logical(sys.name, base / "meminfo"), sys.raw_failure);
        core::detail::ExpectedErrorCleanup kb_cleanup{kb};
        if (!kb && sys.raw_failure && !*sys.raw_failure) sys.raw_failure->emplace(std::move(kb.error()));
        if (!kb || *kb > std::numeric_limits<std::uint64_t>::max() / 1024)
            return std::unexpected{make_error(
                "topology.linux.malformed_size", "NUMA memory size is malformed",
                "discover_linux_topology", {{"path", logical(sys.name, base / "meminfo")}})};
        result.numa_nodes.push_back(
            {observed(node_id.value(), "sys", base),
             *cpus,
             observed(core::ByteCount::from_base_units(*kb * 1024), "sys", base / "meminfo"),
             {},
             provenance("sys", base)});
        std::vector<std::uint64_t> row;
        auto remaining = trim_ascii_space(**distance);
        while (!remaining.empty()) {
            work(sys.io);
            const auto space = remaining.find_first_of(" \t\n");
            const auto token = remaining.substr(0, space);
            auto value = parse_integer<std::uint64_t>(token, logical(sys.name, base / "distance"), sys.raw_failure);
            core::detail::ExpectedErrorCleanup value_cleanup{value};
            if (!value && sys.raw_failure && !*sys.raw_failure) sys.raw_failure->emplace(std::move(value.error()));
            if (!value)
                return std::unexpected{make_error(
                    "topology.linux.incomplete_numa_topology", "NUMA distance row is malformed",
                    "discover_linux_topology", {{"linux_node", node_id.value()}})};
            count(sys.io, SourceCount::online_nodes, row.size() + 1U);
            row.push_back(*value);
            if (space == std::string_view::npos)
                break;
            remaining = trim_ascii_space(remaining.substr(space + 1));
        }
        if (row.size() != distances.linux_nodes.size())
            return std::unexpected{make_error("topology.linux.incomplete_numa_topology",
                                              "NUMA distance row has the wrong number of entries",
                                              "discover_linux_topology",
                                              {{"linux_node", node_id.value()}})};
        for (std::size_t index = 0; index < row.size(); ++index) {
            work(sys.io);
            distances.directed_values.push_back(
                {node_id.value(), distances.linux_nodes[index], row[index]});
        }
    }
    if (!distances.linux_nodes.empty())
        result.numa_distances.push_back(std::move(distances));

    const auto append_pci = [&](const std::string& name) -> std::expected<void, core::Error> {
        work(sys.io);
        const auto address = PciAddress::parse(name);
        if (!address)
            return std::unexpected{
                make_error("topology.linux.invalid_pci_address", "PCI directory has an invalid BDF",
                           "discover_linux_topology", {{"path", "sys/bus/pci/devices/" + name}})};
        const auto base = std::filesystem::path{"bus/pci/devices"} / name;
        auto vendor_text = read_text(sys, base / "vendor", true);
        core::detail::ExpectedErrorCleanup vendor_text_cleanup{vendor_text};
        if (!vendor_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required PCI identity evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
        auto device_text = read_text(sys, base / "device", true);
        core::detail::ExpectedErrorCleanup device_text_cleanup{device_text};
        if (!device_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required PCI identity evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
        auto class_text = read_text(sys, base / "class", true);
        core::detail::ExpectedErrorCleanup class_text_cleanup{class_text};
        if (!class_text) return std::unexpected{make_error("topology.linux.required_file_missing", "Required PCI identity evidence is missing", "discover_linux_topology", {{"path", logical(sys.name, base)}})};
        if (!vendor_text || !device_text || !class_text)
            return std::unexpected{make_error(
                "topology.linux.required_file_missing", "Required PCI identity evidence is missing",
                "discover_linux_topology", {{"path", logical(sys.name, base)}})};
        auto vendor = parse_hex(**vendor_text, logical(sys.name, base / "vendor"));
        core::detail::ExpectedErrorCleanup vendor_cleanup{vendor};
        if (!vendor) return std::unexpected{std::move(vendor.error())};
        auto device = parse_hex(**device_text, logical(sys.name, base / "device"));
        core::detail::ExpectedErrorCleanup device_cleanup{device};
        if (!device) return std::unexpected{std::move(device.error())};
        auto class_code = parse_hex(**class_text, logical(sys.name, base / "class"));
        core::detail::ExpectedErrorCleanup class_code_cleanup{class_code};
        if (!class_code) return std::unexpected{std::move(class_code.error())};
        if (!vendor)
            return std::unexpected{std::move(vendor.error())};
        if (!device)
            return std::unexpected{std::move(device.error())};
        if (!class_code)
            return std::unexpected{std::move(class_code.error())};
        PciFunctionSourceEvidence pci{
            .bdf = name,
            .vendor_id = static_cast<std::uint16_t>(*vendor),
            .device_id = static_cast<std::uint16_t>(*device),
            .class_code = *class_code,
            .subsystem_vendor_id = unavailable<std::uint16_t>("sys", base / "subsystem_vendor"),
            .subsystem_device_id = unavailable<std::uint16_t>("sys", base / "subsystem_device"),
            .local_cpus = unavailable<core::CpuSet>("sys", base / "local_cpulist"),
            .linux_numa_node = unavailable<std::uint32_t>("sys", base / "numa_node"),
            .provenance = provenance("sys", base)};
        for (const auto* field : {"subsystem_vendor", "subsystem_device"}) {
            work(sys.io);
            auto text = read_text(sys, base / field, false);
            core::detail::ExpectedErrorCleanup text_cleanup{text};
            if (!text)
                return std::unexpected{std::move(text.error())};
            if (*text) {
                auto value = parse_hex(**text, logical(sys.name, base / field));
                core::detail::ExpectedErrorCleanup value_cleanup{value};
                if (!value)
                    return std::unexpected{std::move(value.error())};
                auto& target = std::string_view{field} == "subsystem_vendor"
                                   ? pci.subsystem_vendor_id
                                   : pci.subsystem_device_id;
                target = observed(static_cast<std::uint16_t>(*value), "sys", base / field);
            }
        }
        auto local = read_text(sys, base / "local_cpulist", false);
        core::detail::ExpectedErrorCleanup local_cleanup{local};
        if (!local)
            return std::unexpected{std::move(local.error())};
        if (*local) {
            auto value = parse_cpu_set(**local, logical(sys.name, base / "local_cpulist"), sys.io);
            core::detail::ExpectedErrorCleanup value_cleanup{value};
            if (!value)
                return std::unexpected{std::move(value.error())};
            pci.local_cpus = observed(*value, "sys", base / "local_cpulist");
        }
        auto local_map = read_text(sys, base / "local_cpus", false);
        core::detail::ExpectedErrorCleanup local_map_cleanup{local_map};
        if (!local_map)
            return std::unexpected{std::move(local_map.error())};
        if (*local_map) {
            auto mapped = parse_cpu_map(**local_map, logical(sys.name, base / "local_cpus"), sys.io);
            core::detail::ExpectedErrorCleanup mapped_cleanup{mapped};
            if (!mapped)
                return std::unexpected{std::move(mapped.error())};
            if (pci.local_cpus.value && *mapped != *pci.local_cpus.value)
                return std::unexpected{make_error("topology.linux.incomplete_cpu_topology",
                                                  "PCI local_cpulist and local_cpus disagree",
                                                  "discover_linux_topology", {{"bdf", name}})};
            if (!pci.local_cpus.value)
                pci.local_cpus = observed(*mapped, "sys", base / "local_cpus");
        }
        auto numa = read_text(sys, base / "numa_node", false);
        core::detail::ExpectedErrorCleanup numa_cleanup{numa};
        if (!numa)
            return std::unexpected{std::move(numa.error())};
        if (*numa) {
            auto value = parse_integer<std::int64_t>(**numa, logical(sys.name, base / "numa_node"), sys.raw_failure);
            core::detail::ExpectedErrorCleanup value_cleanup{value};
            if (!value)
                return std::unexpected{std::move(value.error())};
            if (*value >= 0 &&
                static_cast<std::uint64_t>(*value) <= std::numeric_limits<std::uint32_t>::max())
                pci.linux_numa_node =
                    observed(static_cast<std::uint32_t>(*value), "sys", base / "numa_node");
        }
        result.pci_functions.push_back(std::move(pci));
        return {};
    };
    if (sys.io) {
        auto names = sys.io->entries(SourceRoot::sys, "bus/pci/devices");
        if (!names) return std::unexpected{io_error(names.error(), sys.raw_failure)};
        count(sys.io, SourceCount::pci_entries, names->size());
        for (const auto& name : *names) {
            work(sys.io);
            auto directory = sys.io->directory(SourceRoot::sys, "bus/pci/devices/" + name, false);
            if (!directory) return std::unexpected{io_error(directory.error(), sys.raw_failure)};
            auto appended = append_pci(name);
            core::detail::ExpectedErrorCleanup appended_cleanup{appended};
            if (!appended) return std::unexpected{std::move(appended.error())};
        }
    } else {
        auto pci_dirs = safe_path(sys, "bus/pci/devices", true);
        core::detail::ExpectedErrorCleanup pci_dirs_cleanup{pci_dirs};
        if (!pci_dirs)
            return std::unexpected{std::move(pci_dirs.error())};
        std::error_code pci_error;
        for (std::filesystem::directory_iterator iterator{**pci_dirs, pci_error}, end;
             iterator != end && !pci_error; iterator.increment(pci_error)) {
            const auto name = iterator->path().filename().string();
            if (std::filesystem::is_symlink(iterator->symlink_status(pci_error))) {
                const auto target = std::filesystem::weakly_canonical(iterator->path(), pci_error);
                const auto root_text = sys.path.generic_string() + '/';
                if (pci_error || !target.generic_string().starts_with(root_text))
                    return std::unexpected{make_error(
                        "topology.linux.invalid_root", "PCI evidence symlink escapes the sysfs root",
                        "discover_linux_topology", {{"path", "sys/bus/pci/devices/" + name}})};
            }

            auto appended = append_pci(name);
            core::detail::ExpectedErrorCleanup appended_cleanup{appended};
            if (!appended) return std::unexpected{std::move(appended.error())};
        }
        if (pci_error)
            return std::unexpected{make_error("topology.linux.permission_denied",
                                              "Unable to enumerate PCI evidence",
                                              "discover_linux_topology")};
    }
    std::ranges::sort(result.pci_functions, [&](const auto& left, const auto& right) {
        work(sys.io); return left.bdf < right.bdf;
    });

    auto status = read_text(proc, "self/status", true);
    core::detail::ExpectedErrorCleanup status_cleanup{status};
    if (!status)
        return std::unexpected{std::move(status.error())};
    auto cpu_allowed_text = parse_status_set(**status, "Cpus_allowed_list", true, proc.io);
    core::detail::ExpectedErrorCleanup cpu_allowed_text_cleanup{cpu_allowed_text};
    if (!cpu_allowed_text) return std::unexpected{std::move(cpu_allowed_text.error())};
    auto node_allowed_text = parse_status_set(**status, "Mems_allowed_list", false, proc.io);
    core::detail::ExpectedErrorCleanup node_allowed_text_cleanup{node_allowed_text};
    if (!node_allowed_text) return std::unexpected{std::move(node_allowed_text.error())};
    if (!cpu_allowed_text)
        return std::unexpected{std::move(cpu_allowed_text.error())};
    if (!node_allowed_text)
        return std::unexpected{std::move(node_allowed_text.error())};
    auto cpu_allowed = parse_cpu_set(*cpu_allowed_text, "proc/self/status:Cpus_allowed_list", sys.io);
    core::detail::ExpectedErrorCleanup cpu_allowed_cleanup{cpu_allowed};
    if (!cpu_allowed) return std::unexpected{std::move(cpu_allowed.error())};
    auto node_allowed = parse_node_set(*node_allowed_text, "proc/self/status:Mems_allowed_list", sys.io);
    core::detail::ExpectedErrorCleanup node_allowed_cleanup{node_allowed};
    if (!node_allowed) return std::unexpected{std::move(node_allowed.error())};
    if (!cpu_allowed)
        return std::unexpected{std::move(cpu_allowed.error())};
    if (!node_allowed)
        return std::unexpected{std::move(node_allowed.error())};
    result.machine.cpu_sets.process_allowed = observed(*cpu_allowed, "proc", "self/status");
    result.machine.node_sets.process_allowed = observed(*node_allowed, "proc", "self/status");

    result.machine.cpu_sets.cgroup_effective =
        unavailable<core::CpuSet>("sys", "cpuset.cpus.effective");
    result.machine.node_sets.cgroup_effective =
        unavailable<core::NodeSet>("sys", "cpuset.mems.effective");
    auto cgroup_text = read_text(proc, "self/cgroup", false);
    core::detail::ExpectedErrorCleanup cgroup_text_cleanup{cgroup_text};
    if (!cgroup_text)
        return std::unexpected{std::move(cgroup_text.error())};
    std::optional<std::filesystem::path> group_path;
    bool cgroup_v1_cpuset = false;
    if (*cgroup_text) {
        work(proc.io, (**cgroup_text).size());
        std::size_t start = 0;
        while (start < (**cgroup_text).size()) {
            work(proc.io);
            const auto end = (**cgroup_text).find('\n', start);
            const auto line = std::string_view{**cgroup_text}.substr(
                start, end == std::string::npos ? (**cgroup_text).size() - start : end - start);
            const auto first = line.find(':');
            const auto second = first == std::string_view::npos ? first : line.find(':', first + 1);
            if (second != std::string_view::npos) {
                const auto controllers = line.substr(first + 1, second - first - 1);
                if (line.substr(0, first) == "0" ||
                    controllers.find("cpuset") != std::string_view::npos) {
                    auto value = std::filesystem::path{line.substr(second + 1)}
                                     .relative_path()
                                     .lexically_normal();
                    if (value.string().starts_with(".."))
                        return std::unexpected{make_error("topology.linux.invalid_root",
                                                          "Cgroup path escapes its root",
                                                          "discover_linux_topology")};
                    cgroup_v1_cpuset = line.substr(0, first) != "0";
                    group_path = cgroup_v1_cpuset ? std::filesystem::path{"cpuset"} / value : value;
                    break;
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (group_path) {
        auto cgroup_cpus = read_set<core::CpuSet>(cgroup, *group_path / "cpuset.cpus.effective",
                                                  false, parse_cpu_set);
        core::detail::ExpectedErrorCleanup cgroup_cpus_cleanup{cgroup_cpus};
        if (!cgroup_cpus)
            return std::unexpected{std::move(cgroup_cpus.error())};
        auto cgroup_nodes = read_set<core::NodeSet>(cgroup, *group_path / "cpuset.mems.effective",
                                                    false, parse_node_set);
        core::detail::ExpectedErrorCleanup cgroup_nodes_cleanup{cgroup_nodes};
        if (!cgroup_nodes)
            return std::unexpected{std::move(cgroup_nodes.error())};
        result.machine.cpu_sets.cgroup_effective = *cgroup_cpus;
        result.machine.node_sets.cgroup_effective = *cgroup_nodes;
    }
    if (result.machine.cpu_sets.cgroup_effective.value) {
        const auto effective = online_before->value->intersection(*cpu_allowed)
                                   .intersection(*result.machine.cpu_sets.cgroup_effective.value);
        result.machine.cpu_sets.final_effective = SourceObservation<core::CpuSet>{
            SourceAvailability::available,
            effective,
            {{TopologyEvidenceSource::linux_sysfs, "derived/final-effective-cpus",
              SourceConfidence::derived}}};
    }
    if (result.machine.node_sets.cgroup_effective.value) {
        const auto effective = node_online_before->value->intersection(*node_allowed)
                                   .intersection(*result.machine.node_sets.cgroup_effective.value);
        result.machine.node_sets.final_effective = SourceObservation<core::NodeSet>{
            SourceAvailability::available,
            effective,
            {{TopologyEvidenceSource::linux_sysfs, "derived/final-effective-nodes",
              SourceConfidence::derived}}};
    }

    auto online_after = read_set<core::CpuSet>(sys, cpu_base / "online", true, parse_cpu_set);
    core::detail::ExpectedErrorCleanup online_after_cleanup{online_after};
    if (!online_after)
        return std::unexpected{std::move(online_after.error())};
    auto node_online_after =
        read_set<core::NodeSet>(sys, node_base / "online", true, parse_node_set);
    core::detail::ExpectedErrorCleanup node_online_after_cleanup{node_online_after};
    if (!node_online_after)
        return std::unexpected{std::move(node_online_after.error())};
    if (online_after->value != online_before->value ||
        node_online_after->value != node_online_before->value)
        return std::unexpected{make_error("topology.linux.snapshot_changed",
                                          "Linux online resource sets changed during discovery",
                                          "discover_linux_topology")};
    return TopologySourceEvidence{std::move(result)};
}

} // namespace

auto discover_linux_topology(const LinuxTopologyPaths& paths)
    -> std::expected<TopologySourceEvidence, core::Error> {
    auto sys = validate_root(paths.sysfs_root, "sys");
    core::detail::ExpectedErrorCleanup sys_cleanup{sys};
    if (!sys)
        return std::unexpected{std::move(sys.error())};
    auto proc = validate_root(paths.procfs_root, "proc");
    core::detail::ExpectedErrorCleanup proc_cleanup{proc};
    if (!proc)
        return std::unexpected{std::move(proc.error())};
    auto cgroup = validate_root(paths.cgroup_root, "cgroup");
    core::detail::ExpectedErrorCleanup cgroup_cleanup{cgroup};
    if (!cgroup)
        return std::unexpected{std::move(cgroup.error())};
    return convert(*sys, *proc, *cgroup);
}

namespace detail {
auto discover_linux_topology_with_prepared(SourceIO& io, std::uint64_t end,
    const LinuxTopologyPaths& paths, std::optional<core::Error>* raw_failure) -> std::expected<TopologySourceEvidence, core::Error> {
    struct Finish { SourceIO& io; ~Finish() noexcept { static_cast<void>(io.finish()); } } cleanup{io};
    try {
        const auto started = io.begin(end);
        if (!started) { static_cast<void>(io.finish()); return std::unexpected{io_error(started.error(), raw_failure)}; }
        const SafeRoot sys{paths.sysfs_root, "sys", &io, SourceRoot::sys, raw_failure};
        const SafeRoot proc{paths.procfs_root, "proc", &io, SourceRoot::proc, raw_failure};
        const SafeRoot cgroup{paths.cgroup_root, "cgroup", &io, SourceRoot::cgroup, raw_failure};
        auto result = convert(sys, proc, cgroup);
        core::detail::ExpectedErrorCleanup result_error_cleanup{result};
        EvidenceCleanup result_cleanup{result ? &*result : nullptr};
        const bool closed = io.finish();
        if (!result) return std::unexpected{std::move(result.error())};
        if (!closed) return std::unexpected{io_error(*io.failure(), raw_failure)};
        return TopologySourceEvidence{std::move(*result)};
    } catch (const ConversionStopped&) {
        static_cast<void>(io.finish());
        return std::unexpected{io_error(*io.failure(), raw_failure)};
    }
}
} // namespace detail

} // namespace epycbench::topology
