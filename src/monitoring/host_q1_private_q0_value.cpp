#include "monitoring/host_q1_private_q0_value.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto rejected(std::string classification) -> MonitoringQ1GitCommandFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{classification.starts_with("contained_")
                                               ? "monitoring.q1_plan_authority.contained_failure"
                                               : "monitoring.q1_plan_authority.q0_rejected"},
             .message = "Private Q0 snapshot rejected",
             .operation = "authenticate_monitoring_q1_plan",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto cleanup_failure(int native = 0) -> core::Error {
    auto result =
        core::Error{.category = core::ErrorCategory::monitoring,
                    .code = core::StableErrorCode{"monitoring.q1_plan_authority.cleanup_failed"},
                    .message = "Private Q0 snapshot cleanup failed",
                    .operation = "authenticate_monitoring_q1_plan",
                    .os_error = std::nullopt,
                    .context = {{"classification", "cleanup_failure"}},
                    .severity = core::ErrorSeverity::error,
                    .continuation = core::ContinuationPolicy::abort_command};
    if (native != 0)
        result.os_error = core::OsError::capture(std::error_code{native, std::generic_category()});
    return result;
}
auto authority_changed(int native = 0) -> MonitoringQ1GitCommandFailure {
    auto result = rejected("q0_snapshot");
    result.primary.code = core::StableErrorCode{"monitoring.q1_plan_authority.authority_changed"};
    result.primary.message = "Private Q0 authority changed";
    result.primary.context = {{"classification", "authority_changed"}};
    if (native != 0)
        result.primary.os_error =
            core::OsError::capture(std::error_code{native, std::generic_category()});
    return result;
}
auto native_open(void*, int parent, const char* name, int flags) noexcept -> int {
    return ::openat(parent, name, flags);
}
auto native_duplicate(void*, int fd) noexcept -> int { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }
auto native_fstat(void*, int fd, struct stat* value) noexcept -> int { return ::fstat(fd, value); }
auto native_stat(void*, int parent, const char* name, struct stat* value, int flags) noexcept
    -> int {
    return ::fstatat(parent, name, value, flags);
}
auto native_read(void*, int fd, void* data, std::size_t size, off_t offset) noexcept -> ssize_t {
    return ::pread(fd, data, size, offset);
}
struct DirectoryListFailure final {
    bool standard{};
    int cleanup_errno{};
};
auto native_list(void*, int fd, std::vector<std::string>* names)
    -> MonitoringQ1PrivateQ0ListResult {
    const int copy = ::dup(fd);
    if (copy < 0)
        return {-1, errno, 0};
    DIR* directory = ::fdopendir(copy);
    if (!directory) {
        const int primary = errno;
        const int close_result = ::close(copy);
        return {-1, primary, close_result == 0 ? 0 : errno};
    }
    std::unique_ptr<DIR, int (*)(DIR*)> owned{directory, ::closedir};
    try {
        ::rewinddir(directory);
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            std::string_view name{entry->d_name};
            if (name != "." && name != "..")
                names->emplace_back(name);
            errno = 0;
        }
    } catch (const std::exception&) {
        const int close_result = ::closedir(owned.release());
        throw DirectoryListFailure{true, close_result == 0 ? 0 : errno};
    } catch (...) {
        const int close_result = ::closedir(owned.release());
        throw DirectoryListFailure{false, close_result == 0 ? 0 : errno};
    }
    const int primary = errno;
    const int close_result = ::closedir(owned.release());
    const int cleanup = close_result == 0 ? 0 : errno;
    if (primary != 0)
        return {-1, primary, cleanup};
    if (cleanup != 0)
        return {-1, cleanup, 0};
    std::ranges::sort(*names);
    return {0, 0, 0};
}
auto native_close(void*, int fd) noexcept -> int { return ::close(fd); }
auto native_euid(void*) noexcept -> unsigned int { return ::geteuid(); }
void native_fault(void*, unsigned int) {}
void native_projection(void*) {}
auto identity(const struct stat& value) noexcept -> MonitoringQ1PrivateQ0Identity {
    return {static_cast<std::uint64_t>(value.st_dev),  static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size), static_cast<std::uint32_t>(value.st_mode),
            static_cast<std::uint32_t>(value.st_uid),  static_cast<std::uint32_t>(value.st_nlink)};
}
auto close_all(std::array<int, 8>& fds,
               const MonitoringQ1PrivateQ0Value::Operations& operations) noexcept {
    std::array<int, 8> failures{};
    std::size_t count{};
    for (auto it = fds.rbegin(); it != fds.rend(); ++it) {
        const int fd = std::exchange(*it, -1);
        if (fd >= 0 && operations.close_fd(operations.context, fd) != 0)
            failures[count++] = errno;
    }
    return std::pair{failures, count};
}
auto read_exact(int fd, std::uint64_t size,
                const MonitoringQ1PrivateQ0Value::Operations& operations)
    -> std::expected<std::string, int> {
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        auto got = operations.read_at(operations.context, fd, bytes.data() + offset,
                                      bytes.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return std::unexpected{got < 0 ? errno : EIO};
        offset += static_cast<std::size_t>(got);
    }
    char extra{};
    ssize_t got{};
    do {
        got = operations.read_at(operations.context, fd, &extra, 1, static_cast<off_t>(offset));
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return std::unexpected{errno};
    if (got != 0)
        return std::unexpected{EFBIG};
    return bytes;
}
class BoundedSax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t, const string_t&) override { return value(); }
    bool string(string_t& text) override { return text.size() <= (1U << 20U) && value(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return value() && ++depth_ <= 24U; }
    bool key(string_t& text) override { return text.size() <= 256U && ++members_ <= 262144U; }
    bool end_object() override {
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override { return value() && ++depth_ <= 24U; }
    bool end_array() override {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    bool value() noexcept { return ++values_ <= 1048576U; }
    std::size_t depth_{}, members_{}, values_{};
};
auto parse(std::string_view raw, std::size_t limit) -> std::optional<nlohmann::json> {
    if (raw.empty() || raw.size() > limit || raw.back() != '\n' ||
        raw.find('\n') != raw.size() - 1U)
        return std::nullopt;
    const auto body = raw.substr(0, raw.size() - 1U);
    BoundedSax sax;
    if (!nlohmann::json::sax_parse(body.begin(), body.end(), &sax))
        return std::nullopt;
    auto value = nlohmann::json::parse(body);
    auto canonical = core::canonicalize_json(value);
    return canonical && *canonical == body ? std::optional{std::move(value)} : std::nullopt;
}
auto exact(const nlohmann::json& value, std::initializer_list<std::string_view> names) -> bool {
    return value.is_object() && value.size() == names.size() &&
           std::ranges::all_of(names, [&](auto name) { return value.contains(name); });
}
auto u32(const nlohmann::json& value) -> bool {
    return value.is_number_unsigned() && value.get<std::uint64_t>() <= 0xffffffffULL;
}
auto u64(const nlohmann::json& value, bool positive = false) -> bool {
    return value.is_number_unsigned() && (!positive || value.get<std::uint64_t>() != 0U);
}
auto nonempty_string(const nlohmann::json& value) -> bool {
    return value.is_string() && !value.get_ref<const std::string&>().empty();
}
auto string_array(const nlohmann::json& value, std::size_t minimum,
                  std::optional<std::size_t> maximum = std::nullopt) -> bool {
    if (!value.is_array() || value.size() < minimum || (maximum && value.size() > *maximum))
        return false;
    std::set<std::string> unique;
    for (const auto& item : value) {
        if (!nonempty_string(item) || !unique.insert(item.get<std::string>()).second)
            return false;
    }
    return true;
}
auto u32_array(const nlohmann::json& value, std::size_t minimum) -> bool {
    if (!value.is_array() || value.size() < minimum)
        return false;
    std::set<std::uint32_t> unique;
    for (const auto& item : value) {
        if (!u32(item) || !unique.insert(item.get<std::uint32_t>()).second)
            return false;
    }
    return true;
}
auto bare_digest(const nlohmann::json& value) -> bool {
    return value.is_string() &&
           core::Sha256Digest::parse(value.get_ref<const std::string&>()).has_value();
}
auto strong_digest(const nlohmann::json& value) -> bool {
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.starts_with("sha256:") && core::Sha256Digest::parse(text.substr(7)).has_value();
}
auto commit_id(const nlohmann::json& value) -> bool {
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    return (text.size() == 40U || text.size() == 64U) &&
           std::ranges::all_of(text, [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}
auto timestamp(const nlohmann::json& value) -> bool {
    if (!value.is_string())
        return false;
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() != 20U || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || s[19] != 'Z')
        return false;
    const auto number = [&](std::size_t at, std::size_t count) -> std::optional<unsigned> {
        unsigned result{};
        for (std::size_t i{}; i < count; ++i) {
            const auto c = static_cast<unsigned char>(s[at + i]);
            if (std::isdigit(c) == 0)
                return std::nullopt;
            result = result * 10U + static_cast<unsigned>(c - '0');
        }
        return result;
    };
    const auto year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2),
               minute = number(14, 2), second = number(17, 2);
    if (!year || *year == 0U || !month || !day || !hour || !minute || !second || *hour > 23U ||
        *minute > 59U || *second > 59U)
        return false;
    using namespace std::chrono;
    return year_month_day{std::chrono::year{static_cast<int>(*year)}, std::chrono::month{*month},
                          std::chrono::day{*day}}
        .ok();
}
auto build_valid(const nlohmann::json& build, std::string_view commit,
                 const core::Sha256Digest& identity) -> bool {
    if (!exact(build, {"project_version", "git", "compiler", "linker", "preset", "enabled_features",
                       "dependencies", "build_identity"}) ||
        !nonempty_string(build.at("project_version")) ||
        build.at("git") != nlohmann::json{{"revision", commit}, {"dirty", false}} ||
        !exact(build.at("compiler"), {"id", "version", "target", "cxx_standard", "flags"}) ||
        !nonempty_string(build.at("compiler").at("id")) ||
        !nonempty_string(build.at("compiler").at("version")) ||
        !nonempty_string(build.at("compiler").at("target")) ||
        build.at("compiler").at("cxx_standard") != "C++23" ||
        !build.at("compiler").at("flags").is_string() ||
        !exact(build.at("linker"), {"id", "version"}) ||
        !nonempty_string(build.at("linker").at("id")) ||
        !nonempty_string(build.at("linker").at("version")) ||
        !nonempty_string(build.at("preset")) || !string_array(build.at("enabled_features"), 0U) ||
        !exact(build.at("dependencies"),
               {"hwloc", "libnuma", "libfyaml", "CLI11", "nlohmann-json", "fmt", "Catch2"}) ||
        !bare_digest(build.at("build_identity")) ||
        build.at("build_identity").get<std::string>() != identity.to_hex())
        return false;
    if (!std::ranges::all_of(build.at("dependencies").items(),
                             [](const auto& item) { return nonempty_string(item.value()); }))
        return false;
    std::vector<std::string> features;
    for (const auto& item : build.at("enabled_features"))
        features.push_back(item.get<std::string>());
    std::vector<std::string> expected{"linux", "cxx23"};
    for (const auto feature : {"testing", "asan_ubsan", "tsan", "native_reference"})
        if (std::ranges::find(features, feature) != features.end())
            expected.emplace_back(feature);
    if (features != expected)
        return false;
    std::vector<std::string> tuple{build.at("project_version").get<std::string>(),
                                   std::string{commit},
                                   "false",
                                   build.at("compiler").at("id").get<std::string>(),
                                   build.at("compiler").at("version").get<std::string>(),
                                   build.at("compiler").at("target").get<std::string>(),
                                   build.at("compiler").at("flags").get<std::string>(),
                                   build.at("linker").at("id").get<std::string>(),
                                   build.at("linker").at("version").get<std::string>(),
                                   build.at("preset").get<std::string>()};
    for (const auto feature : {"testing", "asan_ubsan", "tsan", "native_reference"})
        tuple.emplace_back(std::ranges::find(features, feature) != features.end() ? "true"
                                                                                  : "false");
    for (const auto dependency :
         {"hwloc", "libnuma", "libfyaml", "CLI11", "nlohmann-json", "fmt", "Catch2"})
        tuple.push_back(build.at("dependencies").at(dependency).get<std::string>());
    std::string joined;
    for (std::size_t index{}; index < tuple.size(); ++index) {
        if (index != 0U)
            joined.push_back('|');
        joined += tuple[index];
    }
    return core::sha256(joined) == identity;
}
auto private_safe(const nlohmann::json& value) -> bool {
    static constexpr std::array forbidden{"hostname",
                                          "username",
                                          "home",
                                          "project_path",
                                          "machine_id",
                                          "boot_id",
                                          "pid",
                                          "tid",
                                          "linux_tid",
                                          "perf_fd",
                                          "fd",
                                          "proc_path",
                                          "sysfs_path",
                                          "mmap_address",
                                          "raw_pointer",
                                          "serial",
                                          "serial_number",
                                          "mac",
                                          "mac_address",
                                          "gpu_uuid",
                                          "storage_serial",
                                          "process_listing"};
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            auto normalized = key;
            std::ranges::transform(normalized, normalized.begin(), [](unsigned char c) {
                return c == '-' ? '_' : static_cast<char>(std::tolower(c));
            });
            if (std::ranges::find(forbidden, normalized) != forbidden.end() || !private_safe(child))
                return false;
        }
    } else if (value.is_array()) {
        for (const auto& child : value)
            if (!private_safe(child))
                return false;
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.starts_with("/proc/") || text.starts_with("/sys/") || text.starts_with("/home/") ||
            text.contains('\\'))
            return false;
        std::string token;
        for (char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (std::isalnum(c) != 0 || c == '_' || c == '-')
                token += c == '-' ? '_' : static_cast<char>(std::tolower(c));
            else {
                if (std::ranges::find(forbidden, token) != forbidden.end())
                    return false;
                token.clear();
            }
        }
        if (std::ranges::find(forbidden, token) != forbidden.end())
            return false;
    }
    return true;
}
auto environment_valid(const nlohmann::json& env, const MonitoringQ1ValidatedPublicValues& a)
    -> bool {
    if (!exact(env, {"bandwidth_workers", "capabilities", "latency_workers", "platform",
                     "policy_identity", "privacy_validation", "schema", "selection_policy",
                     "service_reservation", "topology"}) ||
        env.value("schema", "") != a.q0().environment_schema ||
        env.value("policy_identity", "") != a.policy_identity() ||
        env.value("privacy_validation", "") != "passed")
        return false;
    if (env.at("selection_policy") !=
            nlohmann::json{{"id", "epycbench.monitoring-host-selection"}, {"version", 1}} ||
        !exact(env.at("platform"), {"architecture", "cpu_model", "cpu_vendor", "os"}) ||
        !nonempty_string(env.at("platform").at("cpu_model")) ||
        env.at("platform").at("architecture") != "x86_64" ||
        env.at("platform").at("cpu_vendor") != "AuthenticAMD" ||
        env.at("platform").at("os") != "Linux")
        return false;
    const auto& capabilities = env.at("capabilities");
    constexpr std::array capability_names{
        "authentic_amd", "bandwidth_kernel",       "base_page_size",
        "calibration",   "eligible_numa_nonempty", "epyc_model",
        "fingerprints",  "latency_kernel",         "linux",
        "stable_ids",    "topology_valid",         "two_service_cores",
        "x86_64"};
    if (!capabilities.is_object() || capabilities.size() != capability_names.size() ||
        !std::ranges::all_of(capability_names, [&](std::string_view name) {
            return capabilities.contains(name) && capabilities.at(name).is_boolean() &&
                   capabilities.at(name).get<bool>();
        }))
        return false;
    const auto& topology = env.at("topology");
    if (!exact(topology, {"base_page_size_bytes", "effective_fingerprint", "eligible_nodes",
                          "physical_fingerprint"}) ||
        !u64(topology.at("base_page_size_bytes"), true) ||
        !strong_digest(topology.at("effective_fingerprint")) ||
        !strong_digest(topology.at("physical_fingerprint")) ||
        !topology.at("eligible_nodes").is_array() || topology.at("eligible_nodes").empty())
        return false;
    std::vector<std::string> eligible;
    for (const auto& node : topology.at("eligible_nodes")) {
        if (!exact(node, {"installed_memory_bytes", "stable_numa_node_id"}) ||
            !u64(node.at("installed_memory_bytes"), true) ||
            !nonempty_string(node.at("stable_numa_node_id")))
            return false;
        eligible.push_back(node.at("stable_numa_node_id").get<std::string>());
    }
    if (std::set<std::string>{eligible.begin(), eligible.end()}.size() != eligible.size())
        return false;
    const auto& bandwidth = env.at("bandwidth_workers");
    if (!bandwidth.is_array() || bandwidth.empty())
        return false;
    std::set<std::string> nodes, pus, cores;
    std::set<std::uint32_t> cpus;
    for (std::size_t index{}; index < bandwidth.size(); ++index) {
        const auto& worker = bandwidth.at(index);
        if (!exact(worker, {"linux_cpu", "stable_core_id", "stable_numa_node_id", "stable_pu_id",
                            "worker_id"}) ||
            !u32(worker.at("worker_id")) || !u32(worker.at("linux_cpu")) ||
            !nonempty_string(worker.at("stable_core_id")) ||
            !nonempty_string(worker.at("stable_numa_node_id")) ||
            !nonempty_string(worker.at("stable_pu_id")) ||
            worker.at("worker_id").get<std::uint64_t>() != index || index >= eligible.size() ||
            worker.at("stable_numa_node_id") != eligible[index])
            return false;
        nodes.insert(worker.at("stable_numa_node_id").get<std::string>());
        pus.insert(worker.at("stable_pu_id").get<std::string>());
        cores.insert(worker.at("stable_core_id").get<std::string>());
        cpus.insert(worker.at("linux_cpu").get<std::uint32_t>());
    }
    if (nodes.size() != bandwidth.size() || pus.size() != bandwidth.size() ||
        cores.size() != bandwidth.size() || cpus.size() != bandwidth.size())
        return false;
    if (bandwidth.size() != eligible.size())
        return false;
    const auto& latency = env.at("latency_workers");
    const auto& reservation = env.at("service_reservation");
    if (!latency.is_array() || latency.size() != 1U || latency.at(0) != bandwidth.at(0) ||
        !exact(reservation, {"cores"}) || !reservation.at("cores").is_array() ||
        reservation.at("cores").size() != 2U)
        return false;
    std::set<std::string> service_cores, service_pus;
    std::set<std::uint32_t> service_cpus;
    for (std::size_t index{}; index < 2U; ++index) {
        const auto& service = reservation.at("cores").at(index);
        if (!exact(service, {"execution_linux_cpu", "execution_stable_pu_id", "linux_cpus", "role",
                             "stable_core_id", "stable_pu_ids"}) ||
            service.at("role") != (index == 0U ? "control" : "measurement") ||
            !u32(service.at("execution_linux_cpu")) ||
            !nonempty_string(service.at("execution_stable_pu_id")) ||
            !nonempty_string(service.at("stable_core_id")) ||
            !string_array(service.at("stable_pu_ids"), 1U) ||
            !u32_array(service.at("linux_cpus"), 1U) ||
            service.at("stable_pu_ids").size() != service.at("linux_cpus").size())
            return false;
        const auto& execution = service.at("execution_stable_pu_id");
        const auto found = std::ranges::find(service.at("stable_pu_ids"), execution);
        if (found == service.at("stable_pu_ids").end())
            return false;
        const auto offset = static_cast<std::size_t>(found - service.at("stable_pu_ids").begin());
        if (service.at("linux_cpus").at(offset) != service.at("execution_linux_cpu") ||
            !service_cores.insert(service.at("stable_core_id").get<std::string>()).second)
            return false;
        for (const auto& item : service.at("stable_pu_ids"))
            if (!service_pus.insert(item.get<std::string>()).second)
                return false;
        for (const auto& item : service.at("linux_cpus"))
            if (!service_cpus.insert(item.get<std::uint32_t>()).second)
                return false;
    }
    for (const auto& worker : bandwidth)
        if (service_cores.contains(worker.at("stable_core_id").get<std::string>()) ||
            service_pus.contains(worker.at("stable_pu_id").get<std::string>()) ||
            service_cpus.contains(worker.at("linux_cpu").get<std::uint32_t>()))
            return false;
    return true;
}
auto interval_valid(const nlohmann::json& interval, bool candidate) -> bool {
    if (!exact(interval,
               candidate
                   ? std::initializer_list<std::string_view>{"candidate", "start_ns", "end_ns",
                                                             "elapsed_ns"}
                   : std::initializer_list<std::string_view>{"start_ns", "end_ns", "elapsed_ns"}) ||
        (candidate && !u64(interval.at("candidate"), true)) || !u64(interval.at("start_ns")) ||
        !u64(interval.at("end_ns")) || !u64(interval.at("elapsed_ns")))
        return false;
    const auto start = interval.at("start_ns").get<std::uint64_t>();
    const auto end = interval.at("end_ns").get<std::uint64_t>();
    return end >= start && interval.at("elapsed_ns").get<std::uint64_t>() == end - start;
}
auto calibration_valid(const nlohmann::json& calibration, std::uint64_t selected) -> bool {
    if (!exact(calibration,
               {"path", "duration_policy", "empty_harness_trials", "timer_overhead_estimate_ns",
                "candidate_trials", "disposition", "selected_candidate"}) ||
        calibration.at("path") != "RT-009 production first-variant calibration" ||
        calibration.at("disposition") != "selected" ||
        !u64(calibration.at("timer_overhead_estimate_ns")) ||
        !u64(calibration.at("selected_candidate"), true) ||
        calibration.at("selected_candidate").get<std::uint64_t>() != selected)
        return false;
    const auto& duration = calibration.at("duration_policy");
    if (!exact(duration, {"minimum_duration_ns", "target_duration_ns", "maximum_duration_ns"}) ||
        !u64(duration.at("minimum_duration_ns"), true) ||
        !u64(duration.at("target_duration_ns"), true) ||
        !u64(duration.at("maximum_duration_ns"), true))
        return false;
    const auto minimum = duration.at("minimum_duration_ns").get<std::uint64_t>();
    const auto target = duration.at("target_duration_ns").get<std::uint64_t>();
    const auto maximum = duration.at("maximum_duration_ns").get<std::uint64_t>();
    if (minimum > target || target > maximum)
        return false;
    const auto& empty = calibration.at("empty_harness_trials");
    const auto& candidates = calibration.at("candidate_trials");
    if (!empty.is_array() || empty.empty() || !candidates.is_array() || candidates.empty() ||
        !std::ranges::all_of(empty, [](const auto& item) { return interval_valid(item, false); }) ||
        !std::ranges::all_of(candidates,
                             [](const auto& item) { return interval_valid(item, true); }))
        return false;
    for (const auto& item : candidates)
        if (item.at("candidate").get<std::uint64_t>() == selected)
            return true;
    return false;
}
auto evidence_valid(const nlohmann::json& e, const MonitoringQ1ValidatedPublicValues& a,
                    std::string_view environment_digest, const nlohmann::json& environment)
    -> bool {
    if (!exact(e, {"schema", "qualification_id", "policy", "q0b_production_inputs",
                   "host_environment", "host_qualification_plan", "q0b_base_environment", "tool",
                   "repository", "build", "collected_at", "service_plan", "monitoring", "families",
                   "provenance", "pass_fail_eligible", "checksums", "privacy_validation"}) ||
        e.value("schema", "") != a.q0().characterization_schema ||
        e.value("qualification_id", "") != a.qualification_id() ||
        e.value("provenance", "") != "preliminary_characterization" ||
        e.value("pass_fail_eligible", true) || e.value("privacy_validation", "") != "passed")
        return false;
    if (e.at("policy") !=
            nlohmann::json{{"identity", a.policy_identity()}, {"sha256", a.policy_sha256()}} ||
        e.at("host_environment") != nlohmann::json{{"schema", a.q0().environment_schema},
                                                   {"digest", std::string{environment_digest}}} ||
        e.at("host_qualification_plan") !=
            nlohmann::json{{"schema", a.q0().qualification_plan_schema},
                           {"digest", a.q0().qualification_plan_digest}} ||
        e.at("q0b_production_inputs") !=
            nlohmann::json{{"schema", a.q0().production_inputs_schema},
                           {"sha256", a.q0().production_inputs_sha256.to_hex()}} ||
        e.at("q0b_base_environment") !=
            nlohmann::json{{"schema", a.q0().base_environment_schema},
                           {"sha256", a.q0().base_environment_sha256.to_hex()}})
        return false;
    if (!exact(e.at("repository"), {"commit", "dirty"}) ||
        !commit_id(e.at("repository").at("commit")) || e.at("repository").value("dirty", true) ||
        e.at("repository").value("commit", "") != a.q0().repository_commit ||
        e.at("build").at("git") !=
            nlohmann::json{{"revision", a.q0().repository_commit}, {"dirty", false}} ||
        !build_valid(e.at("build"), a.q0().repository_commit, a.q0().build_identity) ||
        !timestamp(e.at("collected_at")) ||
        e.at("tool") != nlohmann::json{{"identity", "epycbench.monitoring-qualification-tool/v1"},
                                       {"version", "1.0.0"}} ||
        e.at("monitoring") != nlohmann::json{{"configuration", "C0"},
                                             {"optional", nlohmann::json::array()},
                                             {"required", nlohmann::json::array()},
                                             {"sampling",
                                              {{"after_observation", false},
                                               {"before_observation", false},
                                               {"during_observation", {{"enabled", false}}}}},
                                             {"unavailable_optional_source", "fail"}})
        return false;
    const auto& families = e.at("families");
    if (!families.is_array() || families.size() != 2U)
        return false;
    for (std::size_t index{}; index < 2U; ++index) {
        const auto& f = families.at(index);
        const auto& expected = a.q0().family_admissions[index];
        if (!exact(f, {"family", "production_admission", "selected_work", "calibration",
                       "finalization"}) ||
            f.value("family", "") != expected.family ||
            f.at("production_admission") !=
                nlohmann::json{
                    {"schema", expected.schema},
                    {"sha256", expected.sha256.to_hex()},
                    {"variant_role", expected.variant_role},
                    {"base_environment_sha256", expected.base_environment_sha256.to_hex()}} ||
            !u64(f.at("selected_work"), true) ||
            f.at("selected_work").get<std::uint64_t>() != a.q0().selected_work[index] ||
            !calibration_valid(f.at("calibration"), f.at("selected_work").get<std::uint64_t>()) ||
            f.at("finalization") !=
                nlohmann::json{{"operation", "CalibrationOnlyProductionRun::finalize"},
                               {"disposition", "succeeded"}})
            return false;
    }
    if (families.at(0).at("calibration").at("duration_policy") !=
        families.at(1).at("calibration").at("duration_policy"))
        return false;
    const auto& service = e.at("service_plan");
    if (!exact(service, {"active_roles", "control_assignment", "reserved_stable_core_ids",
                         "reserved_stable_pu_ids", "reserved_linux_cpus"}) ||
        service.at("active_roles") != nlohmann::json::array({"control"}) ||
        !exact(service.at("control_assignment"), {"stable_core_id", "stable_pu_id", "linux_cpu"}) ||
        !nonempty_string(service.at("control_assignment").at("stable_core_id")) ||
        !nonempty_string(service.at("control_assignment").at("stable_pu_id")) ||
        !u32(service.at("control_assignment").at("linux_cpu")) ||
        !string_array(service.at("reserved_stable_core_ids"), 2U, 2U) ||
        !string_array(service.at("reserved_stable_pu_ids"), 2U) ||
        !u32_array(service.at("reserved_linux_cpus"), 2U))
        return false;
    const auto& services = environment.at("service_reservation").at("cores");
    std::vector<std::string> cores, processing_units;
    std::vector<std::uint32_t> cpus;
    for (const auto& item : services) {
        cores.push_back(item.at("stable_core_id").get<std::string>());
        for (const auto& pu : item.at("stable_pu_ids"))
            processing_units.push_back(pu.get<std::string>());
        for (const auto& cpu : item.at("linux_cpus"))
            cpus.push_back(cpu.get<std::uint32_t>());
    }
    std::ranges::sort(cores);
    std::ranges::sort(processing_units);
    std::ranges::sort(cpus);
    const auto& control = services.at(0);
    if (service.at("reserved_stable_core_ids") != cores ||
        service.at("reserved_stable_pu_ids") != processing_units ||
        service.at("reserved_linux_cpus") != cpus ||
        service.at("control_assignment") !=
            nlohmann::json{{"stable_core_id", control.at("stable_core_id")},
                           {"stable_pu_id", control.at("execution_stable_pu_id")},
                           {"linux_cpu", control.at("execution_linux_cpu")}})
        return false;
    return e.at("checksums") == nlohmann::json{{"artifact", "q0b/evidence.json"},
                                               {"host_environment", "host-environment.json"},
                                               {"manifest", "manifest.json"},
                                               {"checksums", "checksums.sha256"}};
}
enum class SnapshotDisposition : unsigned char {
    accepted,
    rejected,
    standard_exception,
    unknown_exception
};
auto validate_snapshot(const std::array<std::string, 4>& bytes,
                       const MonitoringQ1ValidatedPublicValues& authority) noexcept
    -> SnapshotDisposition {
    try {
        constexpr std::array limits{1U << 20U, 8U << 20U, 1U << 20U};
        std::array<nlohmann::json, 3> values;
        for (std::size_t index{}; index < values.size(); ++index) {
            auto value = parse(bytes[index], limits[index]);
            if (!value || !private_safe(*value))
                return SnapshotDisposition::rejected;
            values[index] = std::move(*value);
        }
        const auto& q0 = authority.q0();
        const auto environment_digest = "sha256:" + core::sha256(bytes[0]).to_hex();
        if (!environment_valid(values[0], authority) ||
            environment_digest != q0.environment_digest ||
            !evidence_valid(values[1], authority, environment_digest, values[0]))
            return SnapshotDisposition::rejected;
        const auto& manifest = values[2];
        if (!exact(manifest, {"artifacts", "environment_digest", "profile", "qualification_id",
                              "schema", "tool"}) ||
            manifest.value("schema", "") != "epycbench.monitoring-qualification-manifest/v2" ||
            manifest.value("qualification_id", "") != authority.qualification_id() ||
            manifest.value("environment_digest", "") != environment_digest ||
            manifest.at("tool") !=
                nlohmann::json{{"identity", "epycbench.monitoring-qualification-tool/v1"},
                               {"version", "1.0.0"}} ||
            manifest.at("profile") != nlohmann::json{{"identity", authority.policy_identity()},
                                                     {"stage", "precharacterization_policy"},
                                                     {"sha256", authority.policy_sha256()}} ||
            !manifest.at("artifacts").is_array() || manifest.at("artifacts").size() != 2U)
            return SnapshotDisposition::rejected;
        const auto entry = [&](std::size_t index, std::string_view path,
                               const core::Sha256Digest& hash) {
            const auto& item = manifest.at("artifacts").at(index);
            return exact(item, {"path", "sha256"}) && item.value("path", "") == path &&
                   item.value("sha256", "") == hash.to_hex();
        };
        if (!entry(0, "host-environment.json", q0.environment_artifact_sha256) ||
            !entry(1, "q0b/evidence.json", q0.characterization_artifact_sha256))
            return SnapshotDisposition::rejected;
        if (bytes[3].size() > (1U << 20U) ||
            core::sha256(bytes[0]) != q0.environment_artifact_sha256 ||
            core::sha256(bytes[1]) != q0.characterization_artifact_sha256 ||
            core::sha256(bytes[2]) != q0.manifest_sha256 ||
            core::sha256(bytes[3]) != q0.checksums_sha256)
            return SnapshotDisposition::rejected;
        const auto line = [](const core::Sha256Digest& hash, std::string_view path) {
            return hash.to_hex() + "  " + std::string{path} + "\n";
        };
        if (bytes[3] != line(q0.manifest_sha256, "manifest.json") +
                            line(q0.environment_artifact_sha256, "host-environment.json") +
                            line(q0.characterization_artifact_sha256, "q0b/evidence.json"))
            return SnapshotDisposition::rejected;
        return SnapshotDisposition::accepted;
    } catch (const nlohmann::json::exception&) {
        return SnapshotDisposition::rejected;
    } catch (const std::invalid_argument&) {
        return SnapshotDisposition::rejected;
    } catch (const std::out_of_range&) {
        return SnapshotDisposition::rejected;
    } catch (const std::exception&) {
        return SnapshotDisposition::standard_exception;
    } catch (...) {
        return SnapshotDisposition::unknown_exception;
    }
}
} // namespace

auto MonitoringQ1PrivateQ0Value::validate(const std::array<std::string, 4>& bytes,
                                          const MonitoringQ1ValidatedPublicValues& authority)
    -> std::expected<void, MonitoringQ1GitCommandFailure> {
    switch (validate_snapshot(bytes, authority)) {
    case SnapshotDisposition::accepted:
        return {};
    case SnapshotDisposition::rejected:
        return std::unexpected{rejected("q0_snapshot")};
    case SnapshotDisposition::standard_exception:
        return std::unexpected{rejected("contained_standard_exception")};
    case SnapshotDisposition::unknown_exception:
        return std::unexpected{rejected("contained_unknown_exception")};
    }
    std::terminate();
}
auto MonitoringQ1PrivateQ0Value::validate_successor_subset(
    std::string_view environment, std::string_view evidence,
    const MonitoringQ1ValidatedPublicValues& authority)
    -> std::expected<void, MonitoringQ1GitCommandFailure> {
    if (environment.empty() || environment.size() > (1U << 20U) ||
        evidence.empty() || evidence.size() > (8U << 20U))
        return std::unexpected{rejected("q0_snapshot")};
    try {
        // These scalar identities came from the closed public-value validator. The
        // reconstructed manifest uses the original Core sorted UTF-8/LF convention.
        const auto environment_hash = core::sha256(environment).to_hex();
        const auto evidence_hash = core::sha256(evidence).to_hex();
        const auto manifest = std::string{
            "{\"artifacts\":[{\"path\":\"host-environment.json\",\"sha256\":\""} +
            environment_hash + "\"},{\"path\":\"q0b/evidence.json\",\"sha256\":\"" +
            evidence_hash + "\"}],\"environment_digest\":\"sha256:" + environment_hash +
            "\",\"profile\":{\"identity\":\"" + std::string{authority.policy_identity()} +
            "\",\"sha256\":\"" + std::string{authority.policy_sha256()} +
            "\",\"stage\":\"precharacterization_policy\"},\"qualification_id\":\"" +
            std::string{authority.qualification_id()} +
            "\",\"schema\":\"epycbench.monitoring-qualification-manifest/v2\",\"tool\":{"
            "\"identity\":\"epycbench.monitoring-qualification-tool/v1\",\"version\":\"1.0.0\"}}\n";
        const auto checksums = core::sha256(manifest).to_hex() + "  manifest.json\n" +
            environment_hash + "  host-environment.json\n" +
            evidence_hash + "  q0b/evidence.json\n";
        return validate({std::string{environment}, std::string{evidence}, manifest, checksums},
                        authority);
    } catch (const std::exception&) {
        return std::unexpected{rejected("contained_standard_exception")};
    } catch (...) {
        return std::unexpected{rejected("contained_unknown_exception")};
    }
}
auto MonitoringQ1PrivateQ0Value::validate_document_for_test(
    std::size_t index, const std::array<std::string, 4>& bytes,
    const MonitoringQ1ValidatedPublicValues& authority) -> bool {
    try {
        auto environment = parse(bytes[0], 1U << 20U);
        if (!environment)
            return false;
        if (index == 0U)
            return private_safe(*environment) && environment_valid(*environment, authority);
        auto value = parse(bytes[index], index == 1U ? 8U << 20U : 1U << 20U);
        if (!value || !private_safe(*value))
            return false;
        const auto digest = "sha256:" + core::sha256(bytes[0]).to_hex();
        if (index == 1U)
            return evidence_valid(*value, authority, digest, *environment);
        if (index != 2U ||
            !exact(*value, {"artifacts", "environment_digest", "profile", "qualification_id",
                            "schema", "tool"}) ||
            value->value("schema", "") != "epycbench.monitoring-qualification-manifest/v2" ||
            value->value("qualification_id", "") != authority.qualification_id() ||
            value->value("environment_digest", "") != digest ||
            value->at("tool") !=
                nlohmann::json{{"identity", "epycbench.monitoring-qualification-tool/v1"},
                               {"version", "1.0.0"}} ||
            value->at("profile") != nlohmann::json{{"identity", authority.policy_identity()},
                                                   {"stage", "precharacterization_policy"},
                                                   {"sha256", authority.policy_sha256()}} ||
            !value->at("artifacts").is_array() || value->at("artifacts").size() != 2U)
            return false;
        const auto entry = [&](std::size_t at, std::string_view path,
                               const core::Sha256Digest& hash) {
            const auto& item = value->at("artifacts").at(at);
            return exact(item, {"path", "sha256"}) && item.value("path", "") == path &&
                   item.value("sha256", "") == hash.to_hex();
        };
        return entry(0, "host-environment.json", authority.q0().environment_artifact_sha256) &&
               entry(1, "q0b/evidence.json", authority.q0().characterization_artifact_sha256);
    } catch (...) {
        return false;
    }
}

MonitoringQ1PrivateQ0Value::MonitoringQ1PrivateQ0Value(
    int root, std::array<int, 8> descriptors,
    std::array<MonitoringQ1PrivateQ0Identity, 8> identities, std::array<std::string, 4> bytes,
    Operations operations, core::Error recheck_standard, core::Error recheck_unknown,
    core::Error retire_standard, core::Error retire_unknown) noexcept
    : descriptors_{descriptors}, root_descriptor_{root}, identities_{identities},
      bytes_{std::move(bytes)}, operations_{operations},
      recheck_standard_{std::move(recheck_standard)}, recheck_unknown_{std::move(recheck_unknown)},
      retire_standard_{std::move(retire_standard)}, retire_unknown_{std::move(retire_unknown)} {}
MonitoringQ1PrivateQ0Value::MonitoringQ1PrivateQ0Value(MonitoringQ1PrivateQ0Value&& other) noexcept
    : descriptors_{std::exchange(other.descriptors_, {-1, -1, -1, -1, -1, -1, -1, -1})},
      root_descriptor_{std::exchange(other.root_descriptor_, -1)}, identities_{other.identities_},
      bytes_{std::move(other.bytes_)}, operations_{other.operations_},
      recheck_standard_{std::move(other.recheck_standard_)},
      recheck_unknown_{std::move(other.recheck_unknown_)},
      retire_standard_{std::move(other.retire_standard_)},
      retire_unknown_{std::move(other.retire_unknown_)} {}
MonitoringQ1PrivateQ0Value::~MonitoringQ1PrivateQ0Value() noexcept {
    static_cast<void>(close_all(descriptors_, operations_));
    if (root_descriptor_ >= 0)
        static_cast<void>(
            operations_.close_fd(operations_.context, std::exchange(root_descriptor_, -1)));
}
auto MonitoringQ1PrivateQ0Value::retain(
    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
    const MonitoringQ1ValidatedPublicValues& authority)
    -> std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure> {
    return retain_with(root.descriptor(), authority,
                       {nullptr, native_duplicate, native_open, native_fstat, native_stat,
                        native_read, native_list, native_close, native_euid, native_fault,
                        native_projection, native_projection});
}
auto MonitoringQ1PrivateQ0Value::retain_with(int root,
                                             const MonitoringQ1ValidatedPublicValues& authority,
                                             Operations operations)
    -> std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure> {
    std::array<int, 8> fds{-1, -1, -1, -1, -1, -1, -1, -1};
    int owned_root{-1};
    auto contained_standard = rejected("contained_standard_exception").primary;
    auto contained_unknown = rejected("contained_unknown_exception").primary;
    auto recheck_standard = contained_standard;
    auto recheck_unknown = contained_unknown;
    auto retire_standard = contained_standard;
    auto retire_unknown = contained_unknown;
    MonitoringQ1GitCommandFailure projection_standard{contained_standard, {}};
    MonitoringQ1GitCommandFailure projection_unknown{contained_unknown, {}};
    auto fail = [&](std::string_view classification, int native = 0, int internal_cleanup = 0) {
        const auto closes = close_all(fds, operations);
        int root_close{};
        int root_native{};
        if (owned_root >= 0) {
            root_close = operations.close_fd(operations.context, std::exchange(owned_root, -1));
            if (root_close != 0)
                root_native = errno;
        }
        try {
            auto result = rejected(std::string{classification});
            result.cleanup.reserve(9U);
            if (native != 0)
                result.primary.os_error =
                    core::OsError::capture(std::error_code{native, std::generic_category()});
            if (internal_cleanup != 0)
                result.cleanup.push_back(cleanup_failure(internal_cleanup));
            for (std::size_t i{}; i < closes.second; ++i)
                result.cleanup.push_back(cleanup_failure(closes.first[i]));
            if (root_close != 0)
                result.cleanup.push_back(cleanup_failure(root_native));
            return std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure>{
                std::unexpected{std::move(result)}};
        } catch (const std::exception&) {
            return std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure>{
                std::unexpected{std::move(projection_standard)}};
        } catch (...) {
            return std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure>{
                std::unexpected{std::move(projection_unknown)}};
        }
    };
    try {
        if (!operations.duplicate_fd || !operations.open_at || !operations.stat_fd ||
            !operations.stat_at || !operations.read_at || !operations.list_dir ||
            !operations.close_fd || !operations.effective_user || !operations.fault ||
            !operations.before_recheck_projection || !operations.before_retire_projection)
            return fail("q0_snapshot");
        owned_root = operations.duplicate_fd(operations.context, root);
        if (owned_root < 0)
            return fail("q0_snapshot", errno);
        if (authority.q0().private_root != ".local/monitoring-qualification")
            return fail("q0_snapshot");
        const std::array<std::string, 4> directory_names{
            authority.q0().private_root.substr(0, 6), authority.q0().private_root.substr(7),
            std::string{authority.qualification_id()}, "q0b"};
        const auto euid = operations.effective_user(operations.context);
        std::array<MonitoringQ1PrivateQ0Identity, 8> identities{};
        int parent = owned_root;
        for (std::size_t i{}; i < 4U; ++i) {
            operations.fault(operations.context, static_cast<unsigned int>(i));
            fds[i] = operations.open_at(operations.context, parent, directory_names[i].c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fds[i] < 0)
                return fail("q0_snapshot", errno);
            struct stat value{};
            if (operations.stat_fd(operations.context, fds[i], &value) != 0)
                return fail("q0_snapshot", errno);
            if (!S_ISDIR(value.st_mode) || value.st_uid != euid ||
                (i == 0U ? (value.st_mode & 0022) != 0
                         : (static_cast<unsigned int>(value.st_mode) & 07777U) != 0700U))
                return fail("q0_snapshot");
            identities[i] = identity(value);
            parent = fds[i];
        }
        std::vector<std::string> root_names, q0b_names;
        const auto root_list = operations.list_dir(operations.context, fds[2], &root_names);
        if (root_list.result != 0)
            return fail("q0_snapshot", root_list.primary_errno, root_list.cleanup_errno);
        const auto q0b_list = operations.list_dir(operations.context, fds[3], &q0b_names);
        if (q0b_list.result != 0)
            return fail("q0_snapshot", q0b_list.primary_errno, q0b_list.cleanup_errno);
        if (root_names != std::vector<std::string>{"checksums.sha256", "host-environment.json",
                                                   "manifest.json", "q0b"} ||
            q0b_names != std::vector<std::string>{"evidence.json"})
            return fail("q0_snapshot");
        constexpr std::array names{"host-environment.json", "evidence.json", "manifest.json",
                                   "checksums.sha256"};
        constexpr std::array limits{1U << 20U, 8U << 20U, 1U << 20U, 1U << 20U};
        std::array<std::string, 4> bytes;
        for (std::size_t i{}; i < 4U; ++i) {
            const int file_parent = i == 1U ? fds[3] : fds[2];
            fds[i + 4U] = operations.open_at(operations.context, file_parent, names[i],
                                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fds[i + 4U] < 0)
                return fail("q0_snapshot", errno);
            struct stat before{}, after{};
            if (operations.stat_fd(operations.context, fds[i + 4U], &before) != 0)
                return fail("q0_snapshot", errno);
            if (!S_ISREG(before.st_mode) || before.st_uid != euid || before.st_nlink != 1 ||
                (static_cast<unsigned int>(before.st_mode) & 07777U) != 0600U ||
                before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > limits[i])
                return fail("q0_snapshot");
            auto read =
                read_exact(fds[i + 4U], static_cast<std::uint64_t>(before.st_size), operations);
            if (!read)
                return fail("q0_snapshot", read.error());
            if (operations.stat_fd(operations.context, fds[i + 4U], &after) != 0)
                return fail("q0_snapshot", errno);
            if (identity(before) != identity(after))
                return fail("q0_snapshot");
            identities[i + 4U] = identity(before);
            bytes[i] = std::move(*read);
        }
        const auto validation = validate_snapshot(bytes, authority);
        if (validation != SnapshotDisposition::accepted) {
            const auto closes = close_all(fds, operations);
            const int root_close =
                operations.close_fd(operations.context, std::exchange(owned_root, -1));
            const int root_native = root_close == 0 ? 0 : errno;
            const auto classification = validation == SnapshotDisposition::rejected ? "q0_snapshot"
                                        : validation == SnapshotDisposition::standard_exception
                                            ? "contained_standard_exception"
                                            : "contained_unknown_exception";
            try {
                auto result = rejected(classification);
                result.cleanup.reserve(9U);
                for (std::size_t i{}; i < closes.second; ++i)
                    result.cleanup.push_back(cleanup_failure(closes.first[i]));
                if (root_close != 0)
                    result.cleanup.push_back(cleanup_failure(root_native));
                return std::unexpected{std::move(result)};
            } catch (const std::exception&) {
                return std::unexpected{std::move(projection_standard)};
            } catch (...) {
                return std::unexpected{std::move(projection_unknown)};
            }
        }
        return MonitoringQ1PrivateQ0Value{std::exchange(owned_root, -1),
                                          fds,
                                          identities,
                                          std::move(bytes),
                                          operations,
                                          std::move(recheck_standard),
                                          std::move(recheck_unknown),
                                          std::move(retire_standard),
                                          std::move(retire_unknown)};
    } catch (const DirectoryListFailure& failure) {
        return fail(failure.standard ? "contained_standard_exception"
                                     : "contained_unknown_exception",
                    0, failure.cleanup_errno);
    } catch (const std::exception&) {
        return fail("contained_standard_exception");
    } catch (...) {
        return fail("contained_unknown_exception");
    }
}
auto MonitoringQ1PrivateQ0Value::recheck(const MonitoringQ1ValidatedPublicValues& authority) const
    -> std::expected<bool, MonitoringQ1GitCommandFailure> {
    const auto changed =
        [&](int native = 0, int cleanup = 0) -> std::expected<bool, MonitoringQ1GitCommandFailure> {
        try {
            operations_.before_recheck_projection(operations_.context);
            auto result = authority_changed(native);
            if (cleanup != 0)
                result.cleanup.push_back(cleanup_failure(cleanup));
            return std::unexpected{std::move(result)};
        } catch (const std::exception&) {
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_standard_), {}}};
        } catch (...) {
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_unknown_), {}}};
        }
    };
    try {
        constexpr std::array names{".local",        "monitoring-qualification", "",
                                   "q0b",           "host-environment.json",    "evidence.json",
                                   "manifest.json", "checksums.sha256"};
        for (std::size_t i{}; i < descriptors_.size(); ++i) {
            struct stat held{}, named{};
            const int parent = i == 0U ? root_descriptor_
                                       : (i < 4U ? descriptors_[i - 1U]
                                                 : (i == 5U ? descriptors_[3] : descriptors_[2]));
            const std::string dynamic =
                i == 2U ? std::string{authority.qualification_id()} : names[i];
            if (operations_.stat_fd(operations_.context, descriptors_[i], &held) != 0)
                return changed(errno);
            if (operations_.stat_at(operations_.context, parent, dynamic.c_str(), &named,
                                    AT_SYMLINK_NOFOLLOW) != 0)
                return changed(errno);
            if (identity(held) != identities_[i] || identity(named) != identities_[i])
                return changed();
            if (i >= 4U) {
                auto current = read_exact(descriptors_[i], identities_[i].size, operations_);
                struct stat after{};
                if (!current)
                    return changed(current.error());
                if (operations_.stat_fd(operations_.context, descriptors_[i], &after) != 0)
                    return changed(errno);
                if (identity(after) != identities_[i] || *current != bytes_[i - 4U])
                    return changed();
            }
        }
        std::vector<std::string> root_names, q0b_names;
        const auto root_list =
            operations_.list_dir(operations_.context, descriptors_[2], &root_names);
        if (root_list.result != 0)
            return changed(root_list.primary_errno, root_list.cleanup_errno);
        const auto q0b_list =
            operations_.list_dir(operations_.context, descriptors_[3], &q0b_names);
        if (q0b_list.result != 0)
            return changed(q0b_list.primary_errno, q0b_list.cleanup_errno);
        if (root_names != std::vector<std::string>{"checksums.sha256", "host-environment.json",
                                                   "manifest.json", "q0b"} ||
            q0b_names != std::vector<std::string>{"evidence.json"})
            return changed();
        switch (validate_snapshot(bytes_, authority)) {
        case SnapshotDisposition::accepted:
            break;
        case SnapshotDisposition::rejected:
            return changed();
        case SnapshotDisposition::standard_exception:
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_standard_), {}}};
        case SnapshotDisposition::unknown_exception:
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_unknown_), {}}};
        }
        return true;
    } catch (const DirectoryListFailure& failure) {
        try {
            operations_.before_recheck_projection(operations_.context);
            std::vector<core::Error> cleanup;
            cleanup.reserve(1U);
            if (failure.cleanup_errno != 0)
                cleanup.push_back(cleanup_failure(failure.cleanup_errno));
            MonitoringQ1GitCommandFailure result{failure.standard ? std::move(recheck_standard_)
                                                                  : std::move(recheck_unknown_),
                                                 std::move(cleanup)};
            return std::unexpected{std::move(result)};
        } catch (const std::exception&) {
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_standard_), {}}};
        } catch (...) {
            return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_unknown_), {}}};
        }
    } catch (const std::exception&) {
        return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_standard_), {}}};
    } catch (...) {
        return std::unexpected{MonitoringQ1GitCommandFailure{std::move(recheck_unknown_), {}}};
    }
}
auto MonitoringQ1PrivateQ0Value::retire() -> std::expected<void, MonitoringQ1GitCommandFailure> {
    const auto closes = close_all(descriptors_, operations_);
    const int root_close =
        root_descriptor_ < 0
            ? 0
            : operations_.close_fd(operations_.context, std::exchange(root_descriptor_, -1));
    const int root_native = root_close == 0 ? 0 : errno;
    if (closes.second == 0U && root_close == 0) {
        bytes_ = {};
        identities_ = {};
        root_descriptor_ = -1;
        return {};
    }
    bytes_ = {};
    identities_ = {};
    root_descriptor_ = -1;
    try {
        operations_.before_retire_projection(operations_.context);
        MonitoringQ1GitCommandFailure result{
            cleanup_failure(closes.second == 0U ? root_native : closes.first[0]), {}};
        for (std::size_t i = 1U; i < closes.second; ++i)
            result.cleanup.push_back(cleanup_failure(closes.first[i]));
        if (root_close != 0 && closes.second != 0U)
            result.cleanup.push_back(cleanup_failure(root_native));
        return std::unexpected{std::move(result)};
    } catch (const std::exception&) {
        return std::unexpected{MonitoringQ1GitCommandFailure{std::move(retire_standard_), {}}};
    } catch (...) {
        return std::unexpected{MonitoringQ1GitCommandFailure{std::move(retire_unknown_), {}}};
    }
}
} // namespace epycbench::monitoring::qualification::detail
