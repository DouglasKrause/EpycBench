#include "epycbench/topology/reference_fixture.hpp"

#include "epycbench/core/digest.hpp"
#include "epycbench/topology/invariants.hpp"
#include "topology/page_size_projection.hpp"
#include "topology/reference_fixture_sha256.hpp"

#include <hwloc.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace epycbench::topology {
namespace {

using Json = nlohmann::json;

[[nodiscard]] auto failure(std::string code, std::string message, Json context = Json::object())
    -> ReferenceFixtureLoadError {
    return {.primary = {.category = core::ErrorCategory::topology,
                        .code = core::StableErrorCode{std::move(code)},
                        .message = std::move(message),
                        .operation = "load_reference_fixture",
                        .os_error = std::nullopt,
                        .context = std::move(context),
                        .severity = core::ErrorSeverity::error,
                        .continuation = core::ContinuationPolicy::abort_command},
            .invariant_diagnostics = {}};
}

[[nodiscard]] auto safe_relative(const std::filesystem::path& path) -> bool {
    if (path.empty() || path.is_absolute())
        return false;
    for (const auto& component : path) {
        if (component == "." || component == ".." || component.empty())
            return false;
    }
    return true;
}

[[nodiscard]] auto has_component(const std::filesystem::path& path, std::string_view value)
    -> bool {
    return std::ranges::any_of(path, [value](const auto& part) { return part == value; });
}

[[nodiscard]] auto read_json(const std::filesystem::path& path)
    -> std::expected<Json, std::string> {
    std::ifstream input{path};
    if (!input)
        return std::unexpected{"unreadable"};
    try {
        return Json::parse(input, nullptr, true, true);
    } catch (const Json::exception&) {
        return std::unexpected{"malformed"};
    }
}

[[nodiscard]] auto read_text(const std::filesystem::path& path)
    -> std::expected<std::string, std::string> {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::unexpected{"unreadable"};
    return std::string{std::istreambuf_iterator<char>{input}, {}};
}

[[nodiscard]] auto without_spaces(std::string text) -> std::string {
    std::erase_if(text, [](unsigned char value) { return std::isspace(value) != 0; });
    return text;
}

[[nodiscard]] auto parse_u32(std::string_view text, int base = 10) -> std::optional<std::uint32_t> {
    std::uint32_t value{};
    const auto* begin = text.data();
    if (base == 16 && text.starts_with("0x"))
        begin += 2;
    const auto [end, error] = std::from_chars(begin, text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

template <typename Id> [[nodiscard]] auto id_set(std::string_view text) -> core::SparseIdSet<Id> {
    auto parsed = core::SparseIdSet<Id>::parse(text);
    if (!parsed)
        throw std::runtime_error{"invalid ID list"};
    return *parsed;
}

[[nodiscard]] auto byte_size(std::string_view text) -> std::optional<core::ByteCount> {
    if (text.empty())
        return std::nullopt;
    const char suffix = text.back();
    const bool unit = suffix == 'K' || suffix == 'M' || suffix == 'G';
    const auto number = parse_u32(unit ? text.substr(0, text.size() - 1) : text);
    if (!number)
        return std::nullopt;
    std::uint64_t multiplier = 1;
    if (suffix == 'K')
        multiplier = 1024;
    if (suffix == 'M')
        multiplier = 1024 * 1024;
    if (suffix == 'G')
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    return core::ByteCount::from_base_units(static_cast<std::uint64_t>(*number) * multiplier);
}

enum class MemoryParseError { malformed, overflow };

[[nodiscard]] auto numa_memory_bytes(std::string_view text)
    -> std::expected<std::uint64_t, MemoryParseError> {
    std::uint64_t kib{};
    const auto separator = text.find(' ');
    if (separator == std::string_view::npos || text.substr(separator) != " kB")
        return std::unexpected{MemoryParseError::malformed};
    const auto number = text.substr(0, separator);
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), kib);
    if (error == std::errc::result_out_of_range)
        return std::unexpected{MemoryParseError::overflow};
    if (error != std::errc{} || end != number.data() + number.size())
        return std::unexpected{MemoryParseError::malformed};
    if (kib > std::numeric_limits<std::uint64_t>::max() / 1024U)
        return std::unexpected{MemoryParseError::overflow};
    return kib * 1024U;
}

[[nodiscard]] auto validate_page_type_values(std::string_view xml) -> bool {
    constexpr std::string_view element = "<page_type ";
    constexpr std::string_view attribute = "size=\"";
    std::size_t cursor{};
    while ((cursor = xml.find(element, cursor)) != std::string_view::npos) {
        const auto end = xml.find('>', cursor + element.size());
        if (end == std::string_view::npos)
            return false;
        const auto size_start = xml.find(attribute, cursor + element.size());
        if (size_start == std::string_view::npos || size_start >= end)
            return false;
        const auto value_start = size_start + attribute.size();
        const auto value_end = xml.find('"', value_start);
        if (value_end == std::string_view::npos || value_end > end)
            return false;
        std::uint64_t value{};
        const auto text = xml.substr(value_start, value_end - value_start);
        const auto [parsed_end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || parsed_end != text.data() + text.size() || value == 0U)
            return false;
        cursor = end + 1U;
    }
    return true;
}

[[nodiscard]] auto evidence(std::string path, ProvenanceSource source = ProvenanceSource::fixture)
    -> std::vector<Evidence> {
    return {{.source = source, .locator = std::move(path), .confidence = Confidence::reported}};
}

struct HwlocHandle {
    hwloc_topology_t value{};
    ~HwlocHandle() {
        if (value != nullptr)
            hwloc_topology_destroy(value);
    }
};

struct HwlocEvidence {
    std::array<unsigned, 8> counts{};
    std::optional<std::uint64_t> total_memory;
    std::vector<std::vector<unsigned>> group_cpus;
    std::map<unsigned, std::size_t> die_group;
    std::set<unsigned> processing_units;
    std::map<unsigned, std::vector<core::ByteCount>> numa_page_sizes;
};

[[nodiscard]] auto validate_hwloc(const std::filesystem::path& path)
    -> std::expected<HwlocEvidence, std::string> {
    HwlocHandle topology;
    if (hwloc_topology_init(&topology.value) != 0 ||
        hwloc_topology_set_all_types_filter(topology.value, HWLOC_TYPE_FILTER_KEEP_ALL) != 0 ||
        hwloc_topology_set_xml(topology.value, path.c_str()) != 0 ||
        hwloc_topology_load(topology.value) != 0)
        return std::unexpected{"malformed XML"};
    HwlocEvidence result;
    if (const auto* root = hwloc_get_root_obj(topology.value); root != nullptr)
        result.total_memory = root->total_memory;
    const std::array types{HWLOC_OBJ_PACKAGE, HWLOC_OBJ_GROUP, HWLOC_OBJ_DIE,
                           HWLOC_OBJ_CORE,    HWLOC_OBJ_PU,    HWLOC_OBJ_NUMANODE};
    for (std::size_t i = 0; i < types.size(); ++i) {
        hwloc_obj_t object = nullptr;
        while ((object = hwloc_get_next_obj_by_type(topology.value, types[i], object)) != nullptr) {
            ++result.counts[i];
        }
    }
    for (hwloc_obj_t group = nullptr;
         (group = hwloc_get_next_obj_by_type(topology.value, HWLOC_OBJ_GROUP, group)) != nullptr;) {
        std::vector<unsigned> cpus;
        int cpu = -1;
        while ((cpu = hwloc_bitmap_next(group->cpuset, cpu)) >= 0)
            cpus.push_back(static_cast<unsigned>(cpu));
        result.group_cpus.push_back(std::move(cpus));
    }
    std::ranges::sort(result.group_cpus);
    for (hwloc_obj_t die = nullptr;
         (die = hwloc_get_next_obj_by_type(topology.value, HWLOC_OBJ_DIE, die)) != nullptr;) {
        auto* parent = die->parent;
        while (parent != nullptr && parent->type != HWLOC_OBJ_GROUP)
            parent = parent->parent;
        if (parent == nullptr)
            return std::unexpected{"die lacks group"};
        std::vector<unsigned> cpus;
        int cpu = -1;
        while ((cpu = hwloc_bitmap_next(parent->cpuset, cpu)) >= 0)
            cpus.push_back(static_cast<unsigned>(cpu));
        const auto position = std::ranges::find(result.group_cpus, cpus);
        if (position == result.group_cpus.end())
            return std::unexpected{"unknown group"};
        result.die_group.emplace(die->os_index,
                                 static_cast<std::size_t>(position - result.group_cpus.begin()));
    }
    for (hwloc_obj_t unit = nullptr;
         (unit = hwloc_get_next_obj_by_type(topology.value, HWLOC_OBJ_PU, unit)) != nullptr;) {
        result.processing_units.insert(unit->os_index);
    }
    for (hwloc_obj_t node = nullptr;
         (node = hwloc_get_next_obj_by_type(topology.value, HWLOC_OBJ_NUMANODE, node)) != nullptr;) {
        if (node->os_index == HWLOC_UNKNOWN_INDEX || node->attr == nullptr)
            return std::unexpected{"NUMA node lacks page-type identity"};
        std::vector<core::ByteCount> page_sizes;
        page_sizes.reserve(node->attr->numanode.page_types_len);
        for (unsigned index = 0; index < node->attr->numanode.page_types_len; ++index) {
            const auto value = node->attr->numanode.page_types[index].size;
            if (value == 0U)
                return std::unexpected{"NUMA page type has zero size"};
            page_sizes.push_back(core::ByteCount::from_base_units(value));
        }
        if (!result.numa_page_sizes.emplace(node->os_index, std::move(page_sizes)).second)
            return std::unexpected{"duplicate NUMA page-type identity"};
    }
    for (const auto type :
         {HWLOC_OBJ_L1CACHE, HWLOC_OBJ_L1ICACHE, HWLOC_OBJ_L2CACHE, HWLOC_OBJ_L3CACHE}) {
        for (hwloc_obj_t object = nullptr;
             (object = hwloc_get_next_obj_by_type(topology.value, type, object)) != nullptr;) {
            ++result.counts[6];
        }
    }
    for (hwloc_obj_t object = nullptr;
         (object = hwloc_get_next_obj_by_type(topology.value, HWLOC_OBJ_PCI_DEVICE, object)) !=
         nullptr;) {
        ++result.counts[7];
    }
    return result;
}

} // namespace

auto load_reference_fixture_authority(const ReferenceFixtureLoadOptions& options)
    -> std::expected<ReferenceFixtureAuthority, ReferenceFixtureLoadError> {
    const auto& root = options.fixture_root;
    if (!root.is_absolute())
        return std::unexpected(failure("fixture.path.relative", "Fixture root must be absolute"));
    if (has_component(root, ".local"))
        return std::unexpected(
            failure("fixture.path.local_forbidden", "Private fixture paths are forbidden"));
    std::error_code ec;
    const auto root_status = std::filesystem::symlink_status(root, ec);
    if (ec || !std::filesystem::exists(root_status))
        return std::unexpected(failure("fixture.path.root_missing", "Fixture root is missing"));
    if (std::filesystem::is_symlink(root_status))
        return std::unexpected(failure("fixture.path.root_symlink", "Fixture root is a symlink"));
    if (!std::filesystem::is_directory(root_status))
        return std::unexpected(
            failure("fixture.path.root_not_directory", "Fixture root is not a directory"));

    std::set<std::string> actual_files;
    for (std::filesystem::recursive_directory_iterator
             it{root, std::filesystem::directory_options::none, ec},
         end;
         it != end && !ec; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec)
            break;
        const auto relative = std::filesystem::relative(it->path(), root, ec);
        if (ec || !safe_relative(relative))
            return std::unexpected(failure("fixture.path.unsafe_path", "Unsafe fixture path"));
        if (std::filesystem::is_symlink(status))
            return std::unexpected(failure("fixture.path.symlink_payload",
                                           "Fixture contains a symlink",
                                           {{"path", relative.generic_string()}}));
        if (std::filesystem::is_regular_file(status))
            actual_files.insert(relative.generic_string());
        else if (!std::filesystem::is_directory(status))
            return std::unexpected(failure("fixture.path.special_file",
                                           "Fixture contains a special file",
                                           {{"path", relative.generic_string()}}));
    }
    if (ec)
        return std::unexpected(failure("fixture.path.scan_failed", "Fixture tree scan failed"));

    const auto checksum_text = read_text(root / "checksums.sha256");
    if (!checksum_text)
        return std::unexpected(failure("fixture.payload.missing", "Checksum manifest is missing"));
    std::istringstream checksum_input{*checksum_text};
    std::map<std::string, std::string> checksums;
    std::string line;
    while (std::getline(checksum_input, line)) {
        if (line.size() < 67 || line[64] != ' ' || line[65] != ' ')
            return std::unexpected(
                failure("fixture.checksum.malformed", "Malformed checksum line"));
        const auto digest = line.substr(0, 64);
        const auto path = line.substr(66);
        if (!std::ranges::all_of(
                digest,
                [](unsigned char c) { return std::isxdigit(c) != 0 && !std::isupper(c); }) ||
            !safe_relative(path))
            return std::unexpected(
                failure("fixture.checksum.malformed", "Malformed checksum entry"));
        if (!checksums.emplace(path, digest).second)
            return std::unexpected(failure("fixture.checksum.duplicate_path",
                                           "Duplicate checksum path", {{"path", path}}));
    }
    if (!checksums.contains("manifest.json"))
        return std::unexpected(
            failure("fixture.checksum.manifest_missing", "Manifest lacks checksum coverage"));
    for (const auto& [path, expected] : checksums) {
        const auto digest = detail::sha256_file(root / path);
        if (!digest || *digest != expected)
            return std::unexpected(failure("fixture.checksum.digest_mismatch",
                                           "Fixture digest mismatch", {{"path", path}}));
    }

    const auto manifest_text = read_text(root / "manifest.json");
    if (!manifest_text)
        return std::unexpected(failure("fixture.manifest.malformed", "Manifest JSON is malformed"));
    const auto manifest_result = [&]() -> std::expected<Json, std::string> {
        try {
            return Json::parse(*manifest_text, nullptr, true, true);
        } catch (const Json::exception&) {
            return std::unexpected{"malformed"};
        }
    }();
    if (!manifest_result)
        return std::unexpected(failure("fixture.manifest.malformed", "Manifest JSON is malformed"));
    const auto& manifest = *manifest_result;
    try {
        if (manifest.at("schema_version") != "epycbench.reference-fixture.v1")
            return std::unexpected(
                failure("fixture.manifest.unsupported_schema", "Unsupported fixture schema"));
        if (manifest.at("fixture_version") != "1")
            return std::unexpected(
                failure("fixture.manifest.unsupported_version", "Unsupported fixture version"));
        if (manifest.at("fixture_id") != "reference-epyc")
            return std::unexpected(
                failure("fixture.manifest.invalid_contract", "Unexpected fixture identity"));
        if (!manifest.at("files").is_array() || !manifest.at("integrity_scope").is_object())
            throw Json::type_error::create(302, "invalid manifest types", &manifest);
    } catch (const Json::exception&) {
        return std::unexpected(
            failure("fixture.manifest.invalid_contract", "Manifest contract is incomplete"));
    }
    std::set<std::string> payloads;
    try {
        for (const auto& file : manifest.at("files")) {
            const auto path = file.at("path").get<std::string>();
            const auto digest = file.at("sha256").get<std::string>();
            if (!safe_relative(path) || digest.size() != 64)
                return std::unexpected(
                    failure("fixture.manifest.unsafe_path", "Unsafe manifest path"));
            if (!payloads.insert(path).second)
                return std::unexpected(
                    failure("fixture.manifest.duplicate_path", "Duplicate manifest file entry"));
            if (!checksums.contains(path) || checksums.at(path) != digest)
                return std::unexpected(failure("fixture.manifest.missing_accounting",
                                               "Manifest/checksum accounting mismatch",
                                               {{"path", path}}));
        }
    } catch (const Json::exception&) {
        return std::unexpected(
            failure("fixture.manifest.invalid_contract", "Invalid manifest file entry"));
    }
    auto expected_actual = payloads;
    expected_actual.insert("manifest.json");
    expected_actual.insert("checksums.sha256");
    auto expected_checksums = payloads;
    expected_checksums.insert("manifest.json");
    if (actual_files != expected_actual) {
        return std::unexpected(
            failure("fixture.manifest.missing_accounting", "Fixture file accounting mismatch"));
    }
    std::set<std::string> checksum_paths;
    for (const auto& [path, unused] : checksums) {
        (void)unused;
        checksum_paths.insert(path);
    }
    if (checksum_paths != expected_checksums)
        return std::unexpected(
            failure("fixture.checksum.scope_mismatch", "Checksum scope mismatch"));

    const auto source_files = read_json(root / "sanitized/source-files.json");
    if (!source_files)
        return std::unexpected(
            failure("fixture.payload.malformed_json", "Source accounting JSON is malformed"));
    try {
        if (!source_files->at("included").is_array() || !source_files->at("exclusions").is_array())
            throw std::runtime_error{"types"};
        std::set<std::string> accounted_destinations;
        for (const auto& entry : source_files->at("included")) {
            const auto source_path = entry.at("source_path").get<std::string>();
            const auto destination = entry.at("destination").get<std::string>();
            const auto transformation = entry.at("transformation").get<std::string>();
            const auto reason = entry.at("reason").get<std::string>();
            const auto digest = entry.at("source_sha256").get<std::string>();
            if (!safe_relative(source_path) || !safe_relative(destination) ||
                transformation.empty() || reason.empty() || digest.size() != 64 ||
                !payloads.contains(destination))
                throw std::runtime_error{"entry"};
            accounted_destinations.insert(destination);
        }
        for (const auto& entry : source_files->at("exclusions")) {
            if (entry.at("category").get<std::string>().empty() ||
                entry.at("rule").get<std::string>().empty() ||
                entry.at("reason").get<std::string>().empty() ||
                entry.at("file_count").get<std::uint64_t>() == 0)
                throw std::runtime_error{"exclusion"};
        }
        if (accounted_destinations.empty())
            throw std::runtime_error{"empty"};
    } catch (const std::exception&) {
        return std::unexpected(failure("fixture.manifest.invalid_source_accounting",
                                       "Source accounting contract is invalid"));
    }

    const auto privacy = read_json(root / "privacy-report.json");
    const auto source = read_json(root / "sanitized/source-capture-status.json");
    const auto completeness = read_json(root / "sanitized/validation/source-completeness.json");
    if (!privacy || !source || !completeness)
        return std::unexpected(
            failure("fixture.payload.malformed_json", "Status payload is malformed"));
    if (privacy->value("status", "") != "passed")
        return std::unexpected(
            failure("fixture.privacy.not_passed", "Fixture privacy status is not passed"));
    if (privacy->value("unresolved_finding_count", -1) != 0)
        return std::unexpected(failure("fixture.privacy.unresolved_findings",
                                       "Fixture has unresolved privacy findings"));
    if (source->value("overall_status", "") != "complete" ||
        source->value("privacy_status", "") != "passed")
        return std::unexpected(
            failure("fixture.source.not_complete", "Source capture is not complete"));
    if (source->value("mandatory_failures", -1) != 0)
        return std::unexpected(
            failure("fixture.source.mandatory_failure", "Source capture has mandatory failures"));
    if (!std::ranges::all_of(
            *completeness, [](const Json& item) { return item.value("status", "") == "complete"; }))
        return std::unexpected(
            failure("fixture.source.incomplete_evidence", "Source completeness is not complete"));

    const auto cpu = read_json(root / "sanitized/linux/cpu-topology.json");
    const auto cores = read_json(root / "derived/core-smt.json");
    const auto caches = read_json(root / "sanitized/linux/cache-topology.json");
    const auto derived_caches = read_json(root / "derived/cache-sharing.json");
    const auto numa = read_json(root / "sanitized/linux/numa-topology.json");
    const auto derived_numa = read_json(root / "derived/numa-nodes.json");
    const auto distances = read_json(root / "derived/numa-distances.json");
    const auto pci = read_json(root / "sanitized/linux/pci-locality.json");
    const auto derived_pci = read_json(root / "derived/pci-locality.json");
    const auto cpu_sets = read_json(root / "sanitized/linux/cpu-sets.json");
    const auto node_sets = read_json(root / "sanitized/linux/node-sets.json");
    const auto lscpu = read_json(root / "sanitized/lscpu/extended.json");
    const auto summary = read_json(root / "sanitized/lscpu/summary.json");
    const auto hwloc_console = read_text(root / "sanitized/hwloc/console.txt");
    const auto hwloc_distances = read_text(root / "sanitized/hwloc/distances.txt");
    const auto hwloc_support = read_text(root / "sanitized/hwloc/support.txt");
    const auto numactl = read_text(root / "sanitized/numa/numactl-hardware.txt");
    if (!cpu || !cores || !caches || !derived_caches || !numa || !derived_numa || !distances ||
        !pci || !derived_pci || !cpu_sets || !node_sets || !lscpu || !summary)
        return std::unexpected(
            failure("fixture.payload.malformed_json", "Required topology JSON is malformed"));
    if (!hwloc_console || !hwloc_distances || !hwloc_support || !numactl ||
        hwloc_console->empty() || hwloc_support->empty())
        return std::unexpected(failure("fixture.payload.malformed_text",
                                       "Required topology text is missing or empty"));
    if (!numa->is_array() || !std::ranges::all_of(*numa, [](const Json& item) {
            return item.is_object() && item.contains("mem_total") &&
                   item.at("mem_total").is_string();
        }))
        return std::unexpected(failure("fixture.payload.invalid_value",
                                       "Fixture contains invalid NUMA memory evidence",
                                       {{"path", "sanitized/linux/numa-topology.json"},
                                        {"field", "mem_total"},
                                        {"classification", "missing_or_wrong_type"}}));
    if (*caches != derived_caches->at("cache_domains"))
        return std::unexpected(
            failure("fixture.contradiction.cache", "Cache evidence contradicts"));
    for (const auto& item : derived_pci->at("devices")) {
        const auto found = std::ranges::find_if(
            *pci, [&](const Json& source_item) { return source_item.at("bdf") == item.at("bdf"); });
        if (found == pci->end())
            return std::unexpected(
                failure("fixture.contradiction.pci", "PCI evidence contradicts"));
        for (const auto* field : {"class", "device", "local_cpulist", "numa_node", "vendor"}) {
            if (found->at(field) != item.at(field))
                return std::unexpected(
                    failure("fixture.contradiction.pci", "PCI evidence contradicts"));
        }
    }
    for (const auto& item : derived_numa->at("nodes")) {
        const auto found = std::ranges::find_if(
            *numa, [&](const Json& n) { return n.at("node") == item.at("node"); });
        if (found == numa->end() || found->at("cpulist") != item.at("cpulist") ||
            found->at("mem_total") != item.at("mem_total"))
            return std::unexpected(
                failure("fixture.contradiction.numa", "NUMA evidence contradicts"));
    }
    for (const auto& row : distances->at("rows")) {
        const auto found = std::ranges::find_if(
            *numa, [&](const Json& n) { return n.at("node") == row.at("node"); });
        if (found == numa->end() || found->at("distance") != row.at("distance"))
            return std::unexpected(
                failure("fixture.contradiction.distance", "Distance evidence contradicts"));
        const auto node = std::to_string(row.at("node").get<unsigned>());
        const auto compact_distance = without_spaces(row.at("distance").get<std::string>());
        if (without_spaces(*hwloc_distances).find(node + compact_distance) == std::string::npos ||
            without_spaces(*numactl).find(node + ":" + compact_distance) == std::string::npos)
            return std::unexpected(
                failure("fixture.contradiction.distance", "Text distance evidence contradicts"));
    }
    const auto compact_numactl = without_spaces(*numactl);
    for (const auto& item : *numa) {
        const auto expected = "node" + std::to_string(item.at("node").get<unsigned>()) +
                              "cpus:" + without_spaces(item.at("cpulist").get<std::string>());
        if (compact_numactl.find(expected) == std::string::npos)
            return std::unexpected(
                failure("fixture.contradiction.numa", "numactl membership contradicts"));
    }
    std::map<unsigned, Json> cpu_by_id;
    for (const auto& item : *cpu)
        cpu_by_id.emplace(item.at("cpu").get<unsigned>(), item);
    std::set<unsigned> linux_cpu_ids;
    for (const auto& [number, unused] : cpu_by_id) {
        (void)unused;
        linux_cpu_ids.insert(number);
    }
    for (const auto& item : lscpu->at("cpus")) {
        const auto number = item.at("cpu").get<unsigned>();
        if (!cpu_by_id.contains(number) ||
            parse_u32(cpu_by_id.at(number).at("package_id").get<std::string>()) !=
                item.at("socket").get<unsigned>())
            return std::unexpected(failure("fixture.contradiction.lscpu",
                                           "lscpu evidence contradicts Linux topology"));
    }
    for (const auto& record : cores->at("cores")) {
        for (const auto& number : record.at("cpus")) {
            const auto value = number.get<unsigned>();
            if (!cpu_by_id.contains(value) ||
                cpu_by_id.at(value).at("core_id") != record.at("core_id") ||
                cpu_by_id.at(value).at("package_id") != record.at("package_id"))
                return std::unexpected(
                    failure("fixture.contradiction.core", "Core evidence contradicts"));
        }
    }
    const auto hwloc_xml = read_text(root / "sanitized/hwloc/topology.xml");
    if (!hwloc_xml || !validate_page_type_values(*hwloc_xml))
        return std::unexpected(
            failure("fixture.payload.malformed_xml", "Sanitized hwloc XML cannot be loaded"));
    const auto hwloc_counts = validate_hwloc(root / "sanitized/hwloc/topology.xml");
    if (!hwloc_counts)
        return std::unexpected(
            failure("fixture.payload.malformed_xml", "Sanitized hwloc XML cannot be loaded"));
    if (!hwloc_counts->total_memory || *hwloc_counts->total_memory == 0U)
        return std::unexpected(failure("fixture.payload.machine_memory_missing",
                                       "Sanitized hwloc XML does not provide machine total memory",
                                       {{"path", "sanitized/hwloc/topology.xml"},
                                        {"field", "Machine.total_memory"},
                                        {"classification", "missing_or_zero"}}));
    if (hwloc_counts->counts[0] != 1 || hwloc_counts->counts[2] != numa->size() ||
        hwloc_counts->counts[3] != cores->at("cores").size() ||
        hwloc_counts->counts[4] != cpu->size() || hwloc_counts->counts[5] != numa->size() ||
        hwloc_counts->counts[6] != caches->size() || hwloc_counts->counts[7] > pci->size() ||
        hwloc_counts->processing_units != linux_cpu_ids)
        return std::unexpected(failure("fixture.contradiction.hwloc",
                                       "hwloc evidence contradicts Linux evidence",
                                       {{"counts", hwloc_counts->counts}}));

    try {
        std::map<unsigned, core::ByteCount> numa_memory;
        std::uint64_t numa_memory_total{};
        for (const auto& item : *numa) {
            const auto node = item.at("node").get<unsigned>();
            const auto parsed = numa_memory_bytes(item.at("mem_total").get<std::string>());
            if (!parsed) {
                if (parsed.error() == MemoryParseError::overflow)
                    return std::unexpected(
                        failure("fixture.payload.machine_memory_overflow",
                                "Linux NUMA memory evidence exceeds the supported range",
                                {{"path", "sanitized/linux/numa-topology.json"},
                                 {"field", "mem_total"},
                                 {"classification", "conversion_overflow"},
                                 {"node", node}}));
                throw std::runtime_error{"bad NUMA memory"};
            }
            if (*parsed > std::numeric_limits<std::uint64_t>::max() - numa_memory_total)
                return std::unexpected(
                    failure("fixture.payload.machine_memory_overflow",
                            "Linux NUMA memory aggregate exceeds the supported range",
                            {{"path", "sanitized/linux/numa-topology.json"},
                             {"field", "mem_total"},
                             {"classification", "aggregate_overflow"},
                             {"node", node}}));
            numa_memory_total += *parsed;
            if (!numa_memory.emplace(node, core::ByteCount::from_base_units(*parsed)).second)
                throw std::runtime_error{"duplicate NUMA memory"};
        }
        if (numa_memory_total != *hwloc_counts->total_memory)
            return std::unexpected(
                failure("fixture.contradiction.memory",
                        "Hwloc machine memory contradicts Linux NUMA memory evidence",
                        {{"path", "sanitized/hwloc/topology.xml"},
                         {"field", "Machine.total_memory"},
                         {"classification", "source_disagreement"},
                         {"hwloc_total_bytes", *hwloc_counts->total_memory},
                         {"linux_numa_total_bytes", numa_memory_total}}));

        std::map<std::string, std::string> summary_values;
        for (const auto& entry : summary->at("lscpu"))
            summary_values.emplace(entry.at("field").get<std::string>(),
                                   entry.at("data").get<std::string>());
        TopologySnapshotData data{
            .machine =
                Machine{
                    .id = core::TopologyMachineId{0},
                    .architecture = summary_values.at("Architecture:"),
                    .cpu_vendor = Attribute<std::string>::available(
                        summary_values.at("Vendor ID:"), evidence("sanitized/lscpu/summary.json")),
                    .cpu_model = Attribute<std::string>::available(
                        summary_values.at("Model name:"), evidence("sanitized/lscpu/summary.json")),
                    .total_memory = Attribute<core::ByteCount>::available(
                        core::ByteCount::from_base_units(*hwloc_counts->total_memory),
                        evidence("sanitized/hwloc/topology.xml")),
                    .page_sizes = {},
                    .present_cpus =
                        id_set<core::LinuxCpuId>(cpu_sets->at("present").get<std::string>()),
                    .online_cpus =
                        id_set<core::LinuxCpuId>(cpu_sets->at("online").get<std::string>()),
                    .allowed_cpus = Attribute<core::CpuSet>::available(
                        id_set<core::LinuxCpuId>(
                            cpu_sets->at("process_affinity").get<std::string>()),
                        evidence("sanitized/linux/cpu-sets.json")),
                    .effective_cpus = Attribute<core::CpuSet>::available(
                        id_set<core::LinuxCpuId>(
                            cpu_sets->at("cpuset_cpus_effective").get<std::string>()),
                        evidence("sanitized/linux/cpu-sets.json")),
                    .present_nodes =
                        id_set<core::NumaNodeId>(node_sets->at("possible").get<std::string>()),
                    .online_nodes =
                        id_set<core::NumaNodeId>(node_sets->at("online").get<std::string>()),
                    .allowed_nodes =
                        Attribute<core::NodeSet>::available(
                            id_set<core::NumaNodeId>(
                                node_sets->at("cpuset_mems_effective").get<std::string>()),
                            evidence("sanitized/linux/node-sets.json")),
                    .effective_nodes =
                        Attribute<core::NodeSet>::available(
                            id_set<core::NumaNodeId>(
                                node_sets->at("cpuset_mems_effective").get<std::string>()),
                            evidence("sanitized/linux/node-sets.json"))},
            .packages = {},
            .dies = {},
            .architecture_groups = {},
            .cores = {},
            .processing_units = {},
            .caches = {},
            .numa_nodes = {},
            .numa_distances = {},
            .pci_devices = {}};
        std::set<unsigned> package_ids, die_ids, core_ids;
        for (const auto& item : *cpu) {
            const auto cpu_id = item.at("cpu").get<unsigned>();
            const auto package = *parse_u32(item.at("package_id").get<std::string>());
            const auto die = *parse_u32(item.at("die_id").get<std::string>());
            const auto core_id = *parse_u32(item.at("core_id").get<std::string>());
            package_ids.insert(package);
            die_ids.insert(die);
            core_ids.insert(core_id);
            data.processing_units.push_back({core::ProcessingUnitId{cpu_id},
                                             core::LinuxCpuId{cpu_id}, core::CoreId{core_id},
                                             Attribute<bool>::unknown()});
        }
        for (auto id : package_ids)
            data.packages.push_back(
                {core::PackageId{id}, Attribute<std::uint32_t>::available(
                                          id, evidence("sanitized/linux/cpu-topology.json",
                                                       ProvenanceSource::linux_sysfs))});
        unsigned group_id{};
        for (const auto& group_cpus : hwloc_counts->group_cpus) {
            std::ostringstream list;
            for (std::size_t index = 0; index < group_cpus.size(); ++index) {
                if (index != 0)
                    list << ',';
                list << group_cpus[index];
            }
            auto members = id_set<core::ProcessingUnitId>(list.str());
            data.architecture_groups.push_back(
                {core::ArchitectureGroupId{group_id}, core::PackageId{0}, std::nullopt,
                 "hwloc-group", std::nullopt, std::move(members),
                 evidence("sanitized/hwloc/topology.xml", ProvenanceSource::hwloc)});
            ++group_id;
        }
        for (auto id : die_ids)
            data.dies.push_back(
                {core::DieId{id}, core::PackageId{0},
                 Attribute<std::uint32_t>::available(
                     id,
                     evidence("sanitized/linux/cpu-topology.json", ProvenanceSource::linux_sysfs)),
                 core::ArchitectureGroupId{static_cast<unsigned>(hwloc_counts->die_group.at(id))}});
        for (auto id : core_ids) {
            const auto found = std::ranges::find_if(*cpu, [id](const Json& item) {
                return parse_u32(item.at("core_id").get<std::string>()) == id;
            });
            data.cores.push_back(
                {core::CoreId{id},
                 core::PackageId{*parse_u32(found->at("package_id").get<std::string>())},
                 core::DieId{*parse_u32(found->at("die_id").get<std::string>())},
                 Attribute<std::uint32_t>::available(id,
                                                     evidence("sanitized/linux/cpu-topology.json",
                                                              ProvenanceSource::linux_sysfs))});
        }
        unsigned cache_id{};
        for (const auto& item : *caches) {
            const auto size = byte_size(item.at("size").get<std::string>());
            const auto line_size = parse_u32(item.at("line_size").get<std::string>());
            const auto ways = parse_u32(item.at("ways_of_associativity").get<std::string>());
            if (!size || !line_size || !ways)
                throw std::runtime_error{"bad cache"};
            const auto type = item.at("type").get<std::string>();
            data.caches.push_back(
                {core::CacheId{cache_id++}, *parse_u32(item.at("level").get<std::string>()),
                 type == "Data"          ? CacheKind::data
                 : type == "Instruction" ? CacheKind::instruction
                                         : CacheKind::unified,
                 *size, core::ByteCount::from_base_units(*line_size),
                 Attribute<std::uint32_t>::available(
                     *ways, evidence("sanitized/linux/cache-topology.json")),
                 id_set<core::ProcessingUnitId>(item.at("shared_cpu_list").get<std::string>()),
                 evidence("sanitized/linux/cache-topology.json", ProvenanceSource::linux_sysfs)});
        }
        for (const auto& item : *numa) {
            const auto node = item.at("node").get<unsigned>();
            const auto page_sizes = hwloc_counts->numa_page_sizes.find(node);
            if (page_sizes == hwloc_counts->numa_page_sizes.end())
                throw std::runtime_error{"missing NUMA page types"};
            data.numa_nodes.push_back(
                {core::NumaNodeId{node},
                 Attribute<std::uint32_t>::available(
                     node, evidence("sanitized/linux/numa-topology.json")),
                 Attribute<core::ByteCount>::available(
                     numa_memory.at(node), evidence("sanitized/linux/numa-topology.json")),
                 Attribute<core::ByteCount>::unknown(),
                 id_set<core::ProcessingUnitId>(item.at("cpulist").get<std::string>()),
                 page_sizes->second});
            data.numa_distances.nodes.emplace_back(node);
            std::istringstream row{item.at("distance").get<std::string>()};
            unsigned distance{};
            unsigned to{};
            while (row >> distance)
                data.numa_distances.directed_distances.push_back(
                    {core::NumaNodeId{node}, core::NumaNodeId{to++}, distance});
        }
        for (const auto& item : *pci) {
            const auto address = PciAddress::parse(item.at("bdf").get<std::string>());
            const auto vendor = parse_u32(item.at("vendor").get<std::string>(), 16);
            const auto device = parse_u32(item.at("device").get<std::string>(), 16);
            const auto class_code = parse_u32(item.at("class").get<std::string>(), 16);
            const auto node = parse_u32(item.at("numa_node").get<std::string>());
            if (!address || !vendor || !device || !class_code || !node)
                throw std::runtime_error{"bad PCI"};
            data.pci_devices.push_back(
                {*address, static_cast<std::uint16_t>(*vendor), static_cast<std::uint16_t>(*device),
                 *class_code,
                 Attribute<core::NumaNodeId>::available(
                     core::NumaNodeId{*node}, evidence("sanitized/linux/pci-locality.json")),
                 Attribute<ProcessingUnitSet>::available(
                     id_set<core::ProcessingUnitId>(item.at("local_cpulist").get<std::string>()),
                     evidence("sanitized/linux/pci-locality.json"))});
        }
        const auto machine_page_sizes = detail::derive_machine_page_sizes(data.numa_nodes);
        if (!machine_page_sizes)
            return std::unexpected(failure("fixture.payload.invalid_page_size",
                                           "Fixture NUMA page-size evidence is invalid"));
        data.machine.page_sizes = *machine_page_sizes;
        auto snapshot = TopologySnapshot::create(std::move(data));
        if (!snapshot)
            return std::unexpected(failure("fixture.model.construction_failed",
                                           "Topology model construction failed",
                                           {{"identifier", snapshot.error().identifier}}));
        const auto report = validate_topology(*snapshot);
        if (!report.valid()) {
            auto error = failure("fixture.invariant.failed",
                                 "Fixture projection violates topology invariants");
            error.invariant_diagnostics = report.diagnostics;
            return std::unexpected(std::move(error));
        }
        return ReferenceFixtureAuthority{
            std::move(*snapshot), manifest.at("fixture_id").get<std::string>(),
            manifest.at("fixture_version").get<std::string>(), core::sha256(*manifest_text),
            core::sha256(*checksum_text)};
    } catch (const std::exception&) {
        return std::unexpected(
            failure("fixture.payload.invalid_value", "Fixture contains an invalid topology value"));
    }
}

auto load_reference_fixture(const ReferenceFixtureLoadOptions& options)
    -> std::expected<TopologySnapshot, ReferenceFixtureLoadError> {
    auto authority = load_reference_fixture_authority(options);
    if (!authority)
        return std::unexpected(std::move(authority.error()));
    return std::move(authority->topology_);
}

} // namespace epycbench::topology
