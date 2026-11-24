#include "epycbench/platform_linux/package_energy.hpp"

#include "epycbench/core/digest.hpp"
#include "epycbench/platform_linux/file.hpp"
#include "platform_linux/package_energy_access.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace epycbench::platform_linux {
namespace {
auto failure(std::string_view classification, int native = 0) -> std::unexpected<core::Error> {
    std::optional<core::OsError> os;
    if (native) os = core::OsError::capture({native, std::generic_category()});
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{"linux.package_energy." + std::string{classification}},
        .message = "Cannot obtain powercap package energy evidence",
        .operation = "package_energy", .os_error = std::move(os), .context = {}}};
}

auto owner(int descriptor) -> std::expected<FileDescriptor, core::Error> {
    if (descriptor >= 0) return FileDescriptor::adopt(descriptor);
    const auto error = errno;
    switch (error) {
    case EACCES:
    case EPERM: return failure("access_denied", error);
    case ENOENT:
    case ENODEV:
    case EOPNOTSUPP: return failure("unavailable", error);
    default: return failure("open_failed", error);
    }
}

constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
constexpr std::string_view virtual_powercap_root = "/sys/devices/virtual/powercap";
constexpr char msr_control[] = "intel-rapl";
constexpr std::size_t zone_entries = 4096;

auto open_directory_path(const std::filesystem::path& directory)
    -> std::expected<FileDescriptor, core::Error> {
    const auto& path = directory.native();
    if (!directory.is_absolute() || path.size() > 4095 || path.find('\0') != path.npos)
        return failure("invalid_zone_path");
    auto current = owner(::open("/", directory_flags));
    if (!current) return current;
    for (const auto& component : directory.relative_path()) {
        const auto& name = component.native();
        if (name.empty()) continue;
        if (name == "." || name == ".." || name.size() > 255)
            return failure("invalid_zone_path");
        auto next = owner(::openat(current->native_handle(), name.c_str(), directory_flags));
        if (!next) return next;
        current = std::move(next);
    }
    return current;
}

auto scan_directories(const FileDescriptor& directory, std::size_t& remaining, auto visit)
    -> std::expected<void, core::Error> {
    const int fd = ::openat(directory.native_handle(), ".", directory_flags);
    if (fd < 0) return failure("enumeration_failed", errno);
    std::unique_ptr<DIR, int (*)(DIR*)> stream{::fdopendir(fd), ::closedir};
    if (!stream) {
        const int error = errno;
        static_cast<void>(::close(fd));
        return failure("enumeration_failed", error);
    }
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (entry == nullptr) {
            if (errno != 0) return failure("enumeration_failed", errno);
            break;
        }
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..") continue;
        if (remaining == 0) return failure("discovery_limit");
        --remaining;
        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;
        if (auto inspected = visit(directory.native_handle(), entry->d_name); !inspected)
            return inspected;
    }
    if (::closedir(stream.release()) != 0) return failure("enumeration_failed", errno);
    return {};
}

auto top_level_zone(std::string_view control, std::string_view name) noexcept -> bool {
    if (!name.starts_with(control) || name.size() <= control.size() || name[control.size()] != ':')
        return false;
    const auto id = name.substr(control.size() + 1);
    // The powercap device name uses the parent's name and a hexadecimal ID.
    return !id.empty() && id.size() <= 8 && id.find_first_not_of("0123456789abcdef") == id.npos;
}

auto may_be_non_directory(const core::Error& error) noexcept -> bool {
    return error.os_error && (error.os_error->value == ENOENT || error.os_error->value == ENODEV ||
                             error.os_error->value == ENOTDIR || error.os_error->value == ELOOP);
}

auto attribute(const FileDescriptor& directory, const char* name)
    -> std::expected<FileDescriptor, core::Error> {
    auto file = owner(::openat(directory.native_handle(), name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) return file;
    struct stat status {};
    if (::fstat(file->native_handle(), &status) != 0) return failure("attribute_identity", errno);
    if (!S_ISREG(status.st_mode)) return failure("attribute_identity");
    return file;
}

struct AttributeText {
    std::array<char, 32> bytes;
    std::size_t count;
    [[nodiscard]] auto text() const noexcept -> std::string_view { return {bytes.data(), count}; }
};

auto read_attribute(const FileDescriptor& file) -> std::expected<AttributeText, core::Error> {
    AttributeText value{};
    ssize_t count;
    do {
        count = ::pread(file.native_handle(), value.bytes.data(), value.bytes.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        const int error = errno;
        if (error == EACCES || error == EPERM) return failure("access_denied", error);
        return failure("read_failed", error);
    }
    if (count == 0 || static_cast<std::size_t>(count) >= value.bytes.size())
        return failure("attribute_size");
    value.count = static_cast<std::size_t>(count);
    return value;
}

auto energy_value(const AttributeText& sample)
    -> std::expected<Microjoules, core::Error> {
    auto raw = parse_integer<std::uint64_t>(sample.text(), "powercap_energy");
    if (!raw) return failure("invalid_counter_value");
    return Microjoules::from_base_units(*raw);
}

struct ZoneSource {
    FileDescriptor energy;
    Microjoules maximum;
};

auto small_file(const std::filesystem::path& path) -> std::optional<std::string> {
    auto file = owner(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file) return {};
    std::array<char, 128> bytes{};
    ssize_t count;
    do {
        count = ::read(file->native_handle(), bytes.data(), bytes.size());
    } while (count < 0 && errno == EINTR);
    if (count <= 0 || static_cast<std::size_t>(count) == bytes.size()) return {};
    return std::string{bytes.data(), static_cast<std::size_t>(count)};
}

// Linux exposes the complete ELF note through the module's sysfs directory.
auto module_build_id(const std::filesystem::path& path) -> std::optional<std::string> {
    const auto note = small_file(path / "notes/.note.gnu.build-id");
    if (!note || note->size() < 16U) return {};
    std::array<std::uint32_t, 3> header{};
    std::memcpy(header.data(), note->data(), sizeof(header));
    const auto length = header[1];
    if (header[0] != 4U || header[2] != 3U || length == 0U || length > 64U ||
        note->size() != 16U + ((length + 3U) & ~3U) || note->compare(12, 4, "GNU\0", 4) != 0)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (std::uint32_t i = 0; i < length; ++i) {
        const auto byte = static_cast<unsigned char>((*note)[16U + i]);
        hex.push_back(digits[byte >> 4U]);
        hex.push_back(digits[byte & 15U]);
    }
    return hex;
}

auto package_cpu_model(const std::filesystem::path& cpuinfo, std::uint32_t package) -> std::optional<std::uint32_t> {
    // Use a CPU explicitly assigned to the powercap package. The Controller
    // may execute on another socket while it captures this metadata.
    struct CpuRecord {
        std::optional<std::uint32_t> package, family, model;
        bool amd{}, flags_without_hypervisor{};
    } cpu;
    const auto model = [&]() -> std::optional<std::uint32_t> {
        if (cpu.package == package && cpu.amd && cpu.flags_without_hypervisor && cpu.family && cpu.model &&
            *cpu.family <= 0xffffU && *cpu.model <= 0xffffU) return (*cpu.family << 16U) | *cpu.model;
        return {};
    };
    std::ifstream in{cpuinfo};
    std::array<char, 16384> line{};
    std::size_t total{};
    while (in.getline(line.data(), line.size()) && (total += static_cast<std::size_t>(in.gcount())) <= 8U * 1024U * 1024U) {
        const std::string_view field{line.data()};
        if (field.empty()) {
            if (cpu.package == package) return model();
            cpu = {};
            continue;
        }
        const auto separator = field.find(':');
        if (separator == field.npos) continue;
        const auto name = detail::trim_ascii_space(field.substr(0, separator));
        const auto value = detail::trim_ascii_space(field.substr(separator + 1U));
        if (name == "vendor_id") cpu.amd = value == "AuthenticAMD";
        else if (name == "flags") {
            std::string flags{" "};
            flags.append(value).push_back(' ');
            cpu.flags_without_hypervisor = !value.empty() && !flags.contains(" hypervisor ");
        } else if (name == "physical id" || name == "cpu family" || name == "model") {
            const auto number = parse_integer<std::uint32_t>(value, "cpuinfo_field");
            if (!number) continue;
            if (name == "physical id") cpu.package = *number;
            else if (name == "cpu family") cpu.family = *number;
            else cpu.model = *number;
        }
    }
    if (in.eof()) return model();
    return {};
}

auto matches_package(const FileDescriptor& zone, std::string_view package_name)
    -> std::expected<bool, core::Error> {
    const auto name_file = attribute(zone, "name");
    if (!name_file) return std::unexpected{name_file.error()};
    const auto name = read_attribute(*name_file);
    if (!name) return std::unexpected{name.error()};
    auto label = name->text();
    if (label.ends_with('\n')) label.remove_suffix(1);
    return label == package_name;
}

auto open_zone_source(const FileDescriptor& zone) -> std::expected<ZoneSource, core::Error> {
    const auto range_file = attribute(zone, "max_energy_range_uj");
    if (!range_file) return std::unexpected{range_file.error()};
    const auto range = read_attribute(*range_file);
    if (!range) return std::unexpected{range.error()};
    const auto maximum = energy_value(*range);
    if (!maximum || maximum->value() == 0U) return failure("invalid_range");

    auto energy = attribute(zone, "energy_uj");
    if (!energy) return std::unexpected{energy.error()};
    return ZoneSource{std::move(*energy), *maximum};
}
} // namespace

auto PackageEnergyZone::open(const std::filesystem::path& directory, std::uint32_t linux_package_number)
    -> std::expected<PackageEnergyZone, core::Error> {
    auto zone = open_directory_path(directory);
    if (!zone) return std::unexpected{zone.error()};
    const auto matches = matches_package(*zone, "package-" + std::to_string(linux_package_number));
    if (!matches) return std::unexpected{matches.error()};
    if (!*matches) return failure("package_identity");
    auto source = open_zone_source(*zone);
    if (!source) return std::unexpected{std::move(source.error())};
    return PackageEnergyZone{std::move(source->energy), std::move(*zone), linux_package_number, source->maximum};
}

auto PackageEnergyZone::discover(std::uint32_t linux_package_number)
    -> std::expected<PackageEnergyZone, core::Error> {
    return detail::PackageEnergyZoneAccess::discover(virtual_powercap_root, linux_package_number);
}

auto detail::PackageEnergyZoneAccess::discover(const std::filesystem::path& root,
                                             std::uint32_t linux_package_number)
    -> std::expected<PackageEnergyZone, core::Error> {
    auto directory = open_directory_path(root);
    if (!directory) return std::unexpected{std::move(directory.error())};
    auto control = owner(::openat(directory->native_handle(), msr_control, directory_flags));
    if (!control) return std::unexpected{std::move(control.error())};
    const auto package_name = "package-" + std::to_string(linux_package_number);
    std::optional<FileDescriptor> selected;
    std::string locator;
    auto remaining_zones = zone_entries;
    auto scanned = scan_directories(*control, remaining_zones,
        [&](int control_fd, const char* zone_name) -> std::expected<void, core::Error> {
            if (!top_level_zone(msr_control, zone_name)) return {};
            auto zone = owner(::openat(control_fd, zone_name, directory_flags));
            if (!zone) {
                if (may_be_non_directory(zone.error())) return {};
                return std::unexpected{std::move(zone.error())};
            }
            const auto matches = matches_package(*zone, package_name);
            if (!matches) return std::unexpected{matches.error()};
            if (!*matches) return {};
            if (selected) return failure("ambiguous");
            selected.emplace(std::move(*zone));
            locator = std::string{msr_control} + '/' + zone_name;
            return {};
        });
    if (!scanned) return std::unexpected{std::move(scanned.error())};
    if (!selected) return failure("unavailable");
    auto source = open_zone_source(*selected);
    if (!source) return std::unexpected{std::move(source.error())};
    return PackageEnergyZone{std::move(source->energy), std::move(*selected), linux_package_number, source->maximum,
                             std::move(locator)};
}

auto detail::PackageEnergyZoneAccess::snapshot(const PackageEnergyZone& zone,
    const std::filesystem::path& proc, const std::filesystem::path& sys) -> PackageEnergySourceSnapshot {
    PackageEnergySourceSnapshot state{};
    if (const auto start = raw_monotonic_now()) state.started = *start;
    state.common_build_id = module_build_id(sys / "module/intel_rapl_common");
    state.msr_build_id = module_build_id(sys / "module/intel_rapl_msr");
    state.amd_family_model = package_cpu_model(proc / "cpuinfo", zone.package_number_);
    const auto boot = small_file(proc / "sys/kernel/random/boot_id");
    if (boot && boot->size() == 37U && boot->back() == '\n') {
        const auto uuid = std::string_view{*boot}.substr(0, 36);
        bool valid = true;
        for (std::size_t i = 0; i < uuid.size(); ++i) {
            const auto c = uuid[i];
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            valid &= dash ? c == '-' : (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (valid) state.boot_sha256 = core::sha256(*boot).to_hex();
    }
    const auto statistic = [&](const char* name) -> std::optional<std::uint64_t> {
        if (const auto raw = small_file(sys / "power/suspend_stats" / name)) {
            if (const auto number = parse_integer<std::uint64_t>(*raw, "suspend_stat")) return *number;
        }
        return {};
    };
    state.suspend_success = statistic("success");
    state.suspend_fail = statistic("fail");
    struct stat held {}, linked {};
    const auto same_package = matches_package(zone.directory_, "package-" + std::to_string(zone.package_number_));
    if (same_package && *same_package && ::fstat(zone.energy_.native_handle(), &held) == 0 &&
        ::fstatat(zone.directory_.native_handle(), "energy_uj", &linked, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(linked.st_mode) && held.st_dev == linked.st_dev && held.st_ino == linked.st_ino) {
        state.attribute_device = held.st_dev;
        state.attribute_inode = held.st_ino;
        if (const auto range = attribute(zone.directory_, "max_energy_range_uj"))
            if (const auto raw = read_attribute(*range))
                if (const auto maximum = energy_value(*raw); maximum && maximum->value())
                    state.current_maximum = *maximum;
    }
    timespec now{};
    if (::clock_gettime(CLOCK_BOOTTIME, &now) == 0 && now.tv_sec >= 0)
        state.boottime_ns = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U +
                            static_cast<std::uint64_t>(now.tv_nsec);
    if (const auto end = raw_monotonic_now()) state.finished = *end;
    return state;
}

auto PackageEnergyZone::source_snapshot() const -> PackageEnergySourceSnapshot {
    return detail::PackageEnergyZoneAccess::snapshot(*this, "/proc", "/sys");
}

auto PackageEnergyZone::sample() const -> std::expected<PackageEnergyReading, core::Error> {
    if (!energy_.active()) return failure("invalid_zone_owner");
    const auto started = raw_monotonic_now();
    if (!started) return std::unexpected{started.error()};
    const auto text = read_attribute(energy_);
    if (!text) return std::unexpected{text.error()};
    const auto finished = raw_monotonic_now();
    if (!finished) return std::unexpected{finished.error()};
    if (*finished < *started) return failure("invalid_clock_bracket");
    const auto raw = energy_value(*text);
    if (!raw || *raw > maximum_) return failure("invalid_counter_value");
    return PackageEnergyReading{package_number_, *raw, maximum_, *started, *finished};
}

} // namespace epycbench::platform_linux
