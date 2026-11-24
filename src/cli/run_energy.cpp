#include "cli/run_energy.hpp"
#include "cli/run_energy_qualification.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "platform_linux/experiment_operations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace epycbench::cli::run_energy {
namespace {
using Json = nlohmann::json;
using Error = core::Error;
using Timestamp = platform_linux::RawMonotonicTimestamp;
using Reading = platform_linux::PackageEnergyReading;
using Microjoules = platform_linux::Microjoules;
constexpr std::string_view profile = "epycbench.run-package-energy/v1";
constexpr std::array reason_names{
    std::pair{Unavailable::package, std::string_view{"package_unavailable"}},
    std::pair{Unavailable::device, std::string_view{"device_unavailable"}},
    std::pair{Unavailable::access, std::string_view{"device_access_denied"}},
    std::pair{Unavailable::ambiguous_device, std::string_view{"multiple_package_devices"}},
    std::pair{Unavailable::read, std::string_view{"reading_failed"}},
    std::pair{Unavailable::clock, std::string_view{"clock_unavailable"}},
    std::pair{Unavailable::bracket, std::string_view{"invalid_bracket"}},
    std::pair{Unavailable::supplied_workers, std::string_view{"supplied_workers"}}};

auto failure(std::string_view classification) -> std::unexpected<Error> {
    return std::unexpected{Error{
        .category = core::ErrorCategory::results,
        .code = core::StableErrorCode{"result.package_energy." + std::string{classification}},
        .message = "Cannot retain or validate the package-energy evidence",
        .operation = "run_package_energy", .os_error = {}, .context = {}}};
}

} // namespace

auto decimal(const nlohmann::json& value) -> std::uint64_t {
    const auto text = value.get<std::string_view>();
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || (text.size() > 1U && text.front() == '0') ||
        parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument{"package energy decimal"};
    return result;
}

namespace {
auto unsigned_id(const Json& value) -> std::uint32_t {
    const auto number = decimal(value);
    if (number > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument{"package energy package ID"};
    return static_cast<std::uint32_t>(number);
}

auto timestamp(const Json& value) -> std::optional<Timestamp> {
    if (value.is_null()) return {};
    return Timestamp::from_nanoseconds(decimal(value));
}

auto reading(const Json& value, std::uint32_t number) -> std::optional<Reading> {
    if (value.is_null()) return {};
    return Reading{number, Microjoules::from_base_units(decimal(value.at("raw_microjoules"))),
        Microjoules::from_base_units(decimal(value.at("advertised_maximum_microjoules"))),
        Timestamp::from_nanoseconds(decimal(value.at("read_started_ns"))),
        Timestamp::from_nanoseconds(decimal(value.at("read_finished_ns")))};
}

auto reading(const std::optional<Reading>& value) -> Json {
    if (!value) return nullptr;
    return {{"raw_microjoules", std::to_string(value->raw_energy.value())},
            {"advertised_maximum_microjoules", std::to_string(value->advertised_maximum.value())},
            {"read_started_ns", std::to_string(value->read_started.nanoseconds())},
            {"read_finished_ns", std::to_string(value->read_finished.nanoseconds())}};
}

auto timestamp(const std::optional<Timestamp>& value) -> Json {
    return value ? Json(std::to_string(value->nanoseconds())) : Json(nullptr);
}

auto valid_zone(std::string_view locator) -> bool {
    constexpr std::string_view prefix = "intel-rapl/intel-rapl:";
    if (!locator.starts_with(prefix)) return false;
    const auto index = locator.substr(prefix.size());
    return !index.empty() && index.size() <= 8U && index.find_first_not_of("0123456789abcdef") == index.npos;
}

struct ResultBinding {
    std::string_view run_id;
    const core::Sha256Digest& digest;
    std::string physical_topology;
    const Json* linux_packages{};
    bool supplied_workers{};

    auto unique_package(std::uint32_t number) const -> bool {
        return std::ranges::count(*linux_packages, Json(number)) == 1;
    }
};

auto bind(const results::PackageResult& result) -> std::expected<ResultBinding, Error> {
    const auto& plan = result.document.at("plan");
    const auto packages = plan.find("linux_packages");
    if (packages == plan.end() || !packages->is_array()) return failure("topology_association");
    return ResultBinding{result.run_id, result.digest, result.physical_topology, &*packages,
                         result.document.at("evidence_source") == "supplied"};
}

auto encoded(const Report& value, const ResultBinding& result) -> std::expected<std::string, Error> {
    if (result.physical_topology != value.physical_topology)
        return failure("topology_association");
    const bool available_package = result.unique_package(value.linux_package_number);
    if (value.package.has_value() != available_package || value.stable_package.has_value() != available_package ||
        (value.stable_package && *value.stable_package != "package/" + std::to_string(value.linux_package_number)))
        return failure("package_identity");
    if ((!result.supplied_workers && value.unavailable == Unavailable::supplied_workers) ||
        (result.supplied_workers && value.unavailable !=
            (available_package ? Unavailable::supplied_workers : Unavailable::package)) ||
        (result.supplied_workers && value.qualification && (value.qualification->before || value.qualification->after)))
        return failure("supplied_scope");
    const bool discovered = !value.unavailable || *value.unavailable == Unavailable::read ||
        *value.unavailable == Unavailable::clock || *value.unavailable == Unavailable::bracket;
    if (value.powercap_zone.has_value() != discovered ||
        (!discovered && (value.before || value.after || value.run_started || value.run_finished)) ||
        (value.powercap_zone && (!value.package || !valid_zone(*value.powercap_zone))))
        return failure("powercap_zone");
    const auto raw_reading = [&](const auto& item) {
        return !item || (value.package && item->linux_package_number == value.linux_package_number &&
                         item->advertised_maximum.value() != 0U && item->raw_energy <= item->advertised_maximum &&
                         item->read_started <= item->read_finished);
    };
    if (!raw_reading(value.before) || !raw_reading(value.after)) return failure("reading_shape");
    if (!value.unavailable) {
        if (!value.package || !value.before || !value.after || !value.run_started || !value.run_finished ||
            !monitoring::PackageEnergyEvidence::create(*value.package, value.linux_package_number,
                *value.before, *value.after, *value.run_started, *value.run_finished))
            return failure("measurement_bracket");
    } else if ((*value.unavailable == Unavailable::package) != !value.package ||
        (*value.unavailable == Unavailable::package && (value.before || value.after ||
            value.run_started || value.run_finished))) {
        return failure("package_availability");
    }

    auto report = Json{{"profile", value.qualification ? "epycbench.run-package-energy/v2" : profile}, {"scope", "whole_run_cpu_package"},
        {"physical_topology", value.physical_topology},
        {"result", {{"run_id", result.run_id}, {"content_sha256", result.digest.to_hex()}}},
        {"package", {{"linux_package_number", std::to_string(value.linux_package_number)},
                     {"capture_local_id", value.package ? Json(std::to_string(value.package->value())) : Json(nullptr)},
                     {"stable_id", value.stable_package ? Json(*value.stable_package) : Json(nullptr)}}},
        {"powercap_zone", value.powercap_zone ? Json(*value.powercap_zone) : Json(nullptr)},
        {"controller_started_ns", timestamp(value.run_started)}, {"controller_finished_ns", timestamp(value.run_finished)},
        {"before", reading(value.before)}, {"after", reading(value.after)},
        {"unavailable", value.unavailable ? Json(core::detail::enum_name(*value.unavailable, reason_names)) : Json(nullptr)},
        {"counter_resolution_microjoules", nullptr}, {"task_attribution", "not_established"},
        {"content_sha256", nullptr}};
    if (value.qualification) {
        report["qualification"] = qualification_json(value);
        report.erase("counter_resolution_microjoules");
    }
    auto digest = core::hash_canonical_json(report);
    if (!digest) return std::unexpected{digest.error()};
    report["content_sha256"] = digest->to_hex();
    return core::canonicalize_json(report);
}
auto sidecar_leaf(std::string_view run) -> std::string {
    return std::string{run} + ".energy.json";
}
} // namespace

auto Capture::begin(const topology::LoadedTopology& topology, std::uint32_t number,
                    std::optional<IncrementBound> bound, bool supplied) -> Capture {
    return begin(topology.topology, topology.stable_ids, topology.fingerprints, number, std::move(bound), supplied);
}

auto Capture::begin(const topology::TopologySnapshot& topology, const topology::StableTopologyIds& stable_ids,
                    const topology::TopologyFingerprints& fingerprints, std::uint32_t number,
                    std::optional<IncrementBound> bound, bool supplied) -> Capture {
    Capture state{number};
    state.report_.qualification.emplace();
    state.report_.qualification->bound = std::move(bound);
    state.report_.physical_topology = fingerprints.physical.to_string();
    const topology::Package* chosen{};
    for (const auto& package : topology.packages()) {
        if (package.os_id.value() != number) continue;
        if (chosen) return state;
        chosen = &package;
    }
    if (!chosen) return state;
    const auto stable = stable_ids.package_id(chosen->id);
    if (!stable) return state;
    state.report_.package = chosen->id;
    state.report_.stable_package = stable->get().value();
    if (supplied) {
        state.report_.unavailable = Unavailable::supplied_workers;
        return state;
    }
    auto zone = platform_linux::PackageEnergyZone::discover(number);
    if (!zone) {
        const auto& reason = zone.error().code.value();
        state.report_.unavailable = reason == "linux.package_energy.access_denied" ? Unavailable::access :
            reason == "linux.package_energy.ambiguous" ? Unavailable::ambiguous_device : Unavailable::device;
        return state;
    }
    state.report_.powercap_zone = zone->zone_locator();
    state.zone_.emplace(std::move(*zone));
    state.report_.qualification->before = state.zone_->source_snapshot();
    const auto sample = state.zone_->sample();
    if (!sample) {
        state.report_.unavailable = Unavailable::read;
        state.zone_.reset();
        return state;
    }
    state.report_.before = *sample;
    const auto started = platform_linux::raw_monotonic_now();
    if (!started) {
        state.report_.unavailable = Unavailable::clock;
        state.zone_.reset();
        return state;
    }
    state.report_.run_started = *started;
    state.report_.unavailable.reset();
    return state;
}

void Capture::finish() {
    if (!zone_ || !report_.before || !report_.run_started || report_.run_finished) return;
    const auto finished = platform_linux::raw_monotonic_now();
    if (finished) report_.run_finished = *finished;
    const auto after = zone_->sample();
    if (after) report_.after = *after;
    report_.qualification->after = zone_->source_snapshot();
    zone_.reset();
    if (!finished) {
        report_.unavailable = Unavailable::clock;
    } else if (!after) {
        report_.unavailable = Unavailable::read;
    } else if (!monitoring::PackageEnergyEvidence::create(*report_.package, report_.linux_package_number,
                    *report_.before, *after, *report_.run_started, *finished)) {
        report_.unavailable = Unavailable::bracket;
    }
}

namespace {
auto save_sidecar(const Report& report, const ResultBinding& result, const std::filesystem::path& result_path)
    -> std::expected<std::filesystem::path, Error> {
    auto data = encoded(report, result);
    if (!data) return std::unexpected{data.error()};
    const auto leaf = sidecar_leaf(result.run_id);
    auto directory = platform_linux::experiment_operations::Directory::acquire(result_path.parent_path());
    if (!directory) return std::unexpected{directory.error()};
    const auto written = directory->write_new(leaf, *data, maximum_bytes);
    if (!written) return std::unexpected{written.error()};
    return directory->path() / leaf;
}

struct DecodedSidecar {
    Report report;
    Json original;
};

auto decode(std::string_view bytes, const ResultBinding& result)
    -> std::expected<DecodedSidecar, Error> {
    if (bytes.size() > maximum_bytes) return failure("capacity");
    try {
        auto data = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) throw std::invalid_argument{"package energy depth"};
            return true;
        });
        const auto& package = data.at("package");
        const auto number = unsigned_id(package.at("linux_package_number"));
        const auto& logical = package.at("capture_local_id");
        const auto& stable = package.at("stable_id");
        const auto& zone = data.at("powercap_zone");
        const auto& unavailable = data.at("unavailable");
        Report report{number, data.at("physical_topology").get<std::string>(),
            logical.is_null() ? std::nullopt : std::optional{core::PackageId{unsigned_id(logical)}},
            stable.is_null() ? std::nullopt : std::optional{stable.get<std::string>()},
            zone.is_null() ? std::nullopt : std::optional{zone.get<std::string>()},
            timestamp(data.at("controller_started_ns")), timestamp(data.at("controller_finished_ns")),
            reading(data.at("before"), number), reading(data.at("after"), number),
            unavailable.is_null() ? std::nullopt :
                std::optional{core::detail::parse_enum(unavailable.get<std::string_view>(), reason_names)}};
        if (data.at("profile") == "epycbench.run-package-energy/v2")
            report.qualification = parse_qualification(data.at("qualification"));
        const auto reencoded = encoded(report, result);
        if (!reencoded) return std::unexpected{reencoded.error()};
        if (*reencoded != bytes) return failure("encoding_binding_or_digest");
        return DecodedSidecar{std::move(report), std::move(data)};
    } catch (const Json::exception&) {
        return failure("shape");
    } catch (const std::invalid_argument&) {
        return failure("value");
    }
}

auto read_sidecar(const std::filesystem::path& result_path, const ResultBinding& result)
    -> std::expected<std::string, Error> {
    auto directory = platform_linux::experiment_operations::Directory::acquire(
        std::filesystem::absolute(result_path).parent_path());
    if (!directory) return std::unexpected{directory.error()};
    return directory->read(sidecar_leaf(result.run_id), maximum_bytes);
}

auto load_sidecar(const std::filesystem::path& result_path, const ResultBinding& result)
    -> std::expected<std::string, Error> {
    auto bytes = read_sidecar(result_path, result);
    if (!bytes) return std::unexpected{bytes.error()};
    const auto valid = decode(*bytes, result);
    if (!valid) return std::unexpected{valid.error()};
    return bytes;
}

auto estimate_sidecar(const std::filesystem::path& result_path, const ResultBinding& result,
    const EstimateOptions& options) -> std::expected<std::string, Error> {
    const auto bytes = read_sidecar(result_path, result);
    if (!bytes) return std::unexpected{bytes.error()};
    auto decoded = decode(*bytes, result);
    if (!decoded) return std::unexpected{decoded.error()};
    Json output{{"profile", "epycbench.run-package-energy-estimate/v1"},
        {"original", std::move(decoded->original)}, {"estimate", estimate_json(decoded->report, options)}};
    return core::canonicalize_json_jcs(output, {2U * maximum_bytes, 12U, maximum_bytes});
}
} // namespace

auto save(const Report& report, const results::PackageResult& result, const std::filesystem::path& result_path)
    -> std::expected<std::filesystem::path, Error> {
    const auto binding = bind(result);
    if (!binding) return std::unexpected{binding.error()};
    return save_sidecar(report, *binding, result_path);
}

auto load(const std::filesystem::path& result_path, const results::PackageResult& result)
    -> std::expected<std::string, Error> {
    const auto binding = bind(result);
    if (!binding) return std::unexpected{binding.error()};
    return load_sidecar(result_path, *binding);
}

auto load_estimate(const std::filesystem::path& result_path, const results::PackageResult& result,
    const EstimateOptions& options) -> std::expected<std::string, Error> {
    const auto binding = bind(result);
    if (!binding) return std::unexpected{binding.error()};
    return estimate_sidecar(result_path, *binding, options);
}

} // namespace epycbench::cli::run_energy
