#include "cli/run_energy_qualification.hpp"

#include "epycbench/core/digest.hpp"
#include "platform_linux/experiment_operations.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace epycbench::cli::run_energy {
namespace {
using Json = nlohmann::json;
using Snapshot = platform_linux::PackageEnergySourceSnapshot;
using Timestamp = platform_linux::RawMonotonicTimestamp;
using Microjoules = platform_linux::Microjoules;

// These exact active Linux modules implement the audited AMD MSR low-word
// mask and integer-nanojoule conversion. Other builds retain their identities.
constexpr std::string_view audited_common = "47b8b2fefb6c2a7cfaa1aa3c8098f1ca032c7dcc";
constexpr std::string_view audited_msr = "238442b432a763655ca5acc142ce371dc56b1544";
constexpr std::string_view bound_profile = "epycbench.package-increment-bound/v1";
constexpr std::string_view counter_model = "nonnegative_accumulation_or_reset_to_zero";
constexpr std::uint64_t mask32 = 0xffff'ffffULL;

auto hex(std::string_view value, std::size_t maximum) -> bool {
    return !value.empty() && value.size() <= maximum && value.size() % 2U == 0 &&
        std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
auto digest(std::string_view value) -> bool { return value.size() == 64U && hex(value, 64U); }
auto narrow(std::uint64_t value) -> std::uint32_t {
    if (value > mask32) throw std::invalid_argument{"integer outside 32-bit range"};
    return static_cast<std::uint32_t>(value);
}

template <class T>
auto maybe(const std::optional<T>& value, auto encode) -> Json {
    return value ? Json(encode(*value)) : Json(nullptr);
}
auto optional_text(const Json& value, bool hash) -> std::optional<std::string> {
    if (value.is_null()) return {};
    auto text = value.get<std::string>();
    if (hash ? !digest(text) : !hex(text, 128U)) throw std::invalid_argument{"invalid source identity"};
    return text;
}
auto number(const Json& value) -> std::optional<std::uint64_t> {
    return value.is_null() ? std::nullopt : std::optional{decimal(value)};
}
auto source_json(const Snapshot& source) -> Json {
    const auto exact = [](auto value) { return std::to_string(value); };
    const auto stamp = [](Timestamp value) { return std::to_string(value.nanoseconds()); };
    const auto identity = [](const std::string& value) { return value; };
    return {{"started_raw_ns", maybe(source.started, stamp)}, {"finished_raw_ns", maybe(source.finished, stamp)},
        {"common_build_id", maybe(source.common_build_id, identity)}, {"msr_build_id", maybe(source.msr_build_id, identity)},
        {"boot_sha256", maybe(source.boot_sha256, identity)}, {"amd_family_model", maybe(source.amd_family_model, exact)},
        {"suspend_success", maybe(source.suspend_success, exact)}, {"suspend_fail", maybe(source.suspend_fail, exact)},
        {"boottime_ns", maybe(source.boottime_ns, exact)}, {"attribute_device", maybe(source.attribute_device, exact)},
        {"attribute_inode", maybe(source.attribute_inode, exact)},
        {"current_maximum_microjoules", maybe(source.current_maximum, [](Microjoules v) { return std::to_string(v.value()); })}};
}
auto parse_source(const Json& value) -> std::optional<Snapshot> {
    if (value.is_null()) return {};
    Snapshot source{};
    if (auto start = number(value.at("started_raw_ns"))) source.started = Timestamp::from_nanoseconds(*start);
    if (auto end = number(value.at("finished_raw_ns"))) source.finished = Timestamp::from_nanoseconds(*end);
    source.common_build_id = optional_text(value.at("common_build_id"), false);
    source.msr_build_id = optional_text(value.at("msr_build_id"), false);
    source.boot_sha256 = optional_text(value.at("boot_sha256"), true);
    if (auto sig = number(value.at("amd_family_model"))) source.amd_family_model = narrow(*sig);
    source.suspend_success = number(value.at("suspend_success"));
    source.suspend_fail = number(value.at("suspend_fail"));
    source.boottime_ns = number(value.at("boottime_ns"));
    source.attribute_device = number(value.at("attribute_device"));
    source.attribute_inode = number(value.at("attribute_inode"));
    if (auto maximum = number(value.at("current_maximum_microjoules"))) source.current_maximum = Microjoules::from_base_units(*maximum);
    return source;
}

auto bound_json(const IncrementBound& b) -> Json {
    return {{"profile", bound_profile}, {"counter_model", counter_model}, {"physical_topology", b.physical_topology},
        {"linux_package_number", std::to_string(b.linux_package_number)}, {"boot_sha256", b.boot_sha256},
        {"common_build_id", b.common_build_id}, {"msr_build_id", b.msr_build_id}, {"unit_exponent", std::to_string(b.unit_exponent)},
        {"valid_from_boottime_ns", std::to_string(b.valid_from_boottime_ns)}, {"valid_until_boottime_ns", std::to_string(b.valid_until_boottime_ns)},
        {"maximum_interval_ns", std::to_string(b.maximum_interval_ns)}, {"maximum_increment_microjoules", std::to_string(b.maximum_increment.value())},
        {"source_sha256", b.source_sha256}, {"review_sha256", b.review_sha256}};
}
auto parse_bound(const Json& value) -> IncrementBound {
    IncrementBound b{value.at("physical_topology").get<std::string>(), narrow(decimal(value.at("linux_package_number"))),
        value.at("boot_sha256").get<std::string>(), value.at("common_build_id").get<std::string>(),
        value.at("msr_build_id").get<std::string>(), narrow(decimal(value.at("unit_exponent"))),
        decimal(value.at("valid_from_boottime_ns")), decimal(value.at("valid_until_boottime_ns")), decimal(value.at("maximum_interval_ns")),
        Microjoules::from_base_units(decimal(value.at("maximum_increment_microjoules"))),
        value.at("source_sha256").get<std::string>(), value.at("review_sha256").get<std::string>()};
    if (!b.physical_topology.starts_with("sha256:") || !digest(std::string_view{b.physical_topology}.substr(7U)) ||
        !digest(b.boot_sha256) || !hex(b.common_build_id, 128) || !hex(b.msr_build_id, 128) || b.unit_exponent > 31U ||
        b.valid_from_boottime_ns >= b.valid_until_boottime_ns || !b.maximum_interval_ns ||
        !digest(b.source_sha256) || !digest(b.review_sha256) || b.source_sha256 == b.review_sha256 || bound_json(b) != value)
        throw std::invalid_argument{"invalid package increment bound"};
    return b;
}

auto audited_unit(const Snapshot& first, const Snapshot& last, Microjoules maximum) -> std::optional<unsigned> {
    if (first.common_build_id != audited_common || last.common_build_id != audited_common ||
        first.msr_build_id != audited_msr || last.msr_build_id != audited_msr ||
        !first.amd_family_model || first.amd_family_model != last.amd_family_model ||
        *first.amd_family_model != 0x0019'0011U ||
        !first.current_maximum || first.current_maximum != maximum || last.current_maximum != maximum) return {};
    std::optional<unsigned> match;
    for (unsigned exponent = 0; exponent < 32U; ++exponent) {
        const auto scale = 1'000'000'000ULL >> exponent;
        if (scale && mask32 * scale / 1'000ULL == maximum.value()) {
            if (match) return {};
            match = exponent;
        }
    }
    return match;
}
auto encoded_tick(Microjoules raw, std::uint64_t scale) -> bool {
    const auto ticks = (raw.value() * 1'000ULL + scale - 1U) / scale;
    return ticks <= mask32 && ticks * scale / 1'000ULL == raw.value();
}

struct CheckedSource {
    const Snapshot& first;
    const Snapshot& last;
    unsigned exponent;
};

auto check_source(const Report& report, Json& result) -> std::expected<CheckedSource, std::string_view> {
    if (report.unavailable || !report.before || !report.after || !report.package || !report.run_started || !report.run_finished)
        return std::unexpected{"raw_bracket_unavailable"};
    if (!report.qualification) return std::unexpected{"legacy_source_snapshots_unavailable"};
    const auto& q = *report.qualification;
    if (!q.before || !q.after) return std::unexpected{"source_snapshots_unavailable"};
    const auto& first = *q.before;
    const auto& last = *q.after;
    const auto exponent = audited_unit(first, last, report.before->advertised_maximum);
    if (!exponent) return std::unexpected{"provider_or_unit_unqualified"};
    const auto scale = 1'000'000'000ULL >> *exponent;
    result["unit_exponent"] = std::to_string(*exponent);
    result["driver_nanojoules_per_count"] = std::to_string(scale);
    if (!encoded_tick(report.before->raw_energy, scale) || !encoded_tick(report.after->raw_energy, scale))
        return std::unexpected{"outside_driver_encoding"};
    if (!first.started || !first.finished || !last.started || !last.finished ||
        !(*first.started <= *first.finished && *first.finished <= report.before->read_started &&
          report.after->read_finished <= *last.started && *last.started <= *last.finished) ||
        !first.boottime_ns || !last.boottime_ns || *last.boottime_ns <= *first.boottime_ns)
        return std::unexpected{"source_clock_bracket_unavailable"};
    if (!first.attribute_device || first.attribute_device != last.attribute_device ||
        !first.attribute_inode || first.attribute_inode != last.attribute_inode ||
        !first.boot_sha256 || first.boot_sha256 != last.boot_sha256 ||
        !first.suspend_success || first.suspend_success != last.suspend_success ||
        !first.suspend_fail || first.suspend_fail != last.suspend_fail)
        return std::unexpected{"source_continuity_unavailable"};
    return CheckedSource{first, last, *exponent};
}

auto read_bracket(const Report& report) -> std::expected<monitoring::PackageEnergyEvidence, core::Error> {
    return monitoring::PackageEnergyEvidence::create(*report.package, report.linux_package_number,
        *report.before, *report.after, *report.run_started, *report.run_finished);
}

auto assess(const Report& report, Json& result) -> std::string_view {
    const auto source = check_source(report, result);
    if (!source) return source.error();
    const auto& [first, last, exponent] = *source;
    const auto& q = *report.qualification;
    if (!q.bound) return "independent_increment_bound_unavailable";
    const auto& b = *q.bound;
    if (b.physical_topology != report.physical_topology || b.linux_package_number != report.linux_package_number ||
        b.common_build_id != first.common_build_id || b.msr_build_id != first.msr_build_id ||
        b.boot_sha256 != first.boot_sha256 || b.unit_exponent != exponent ||
        *first.boottime_ns < b.valid_from_boottime_ns || *last.boottime_ns > b.valid_until_boottime_ns ||
        *last.boottime_ns - *first.boottime_ns > b.maximum_interval_ns) return "increment_bound_inapplicable";
    // After a reset to zero in the bracket, the final encoded value could be
    // at most B. This covers a hidden reset even when the endpoints increase.
    if (report.after->raw_energy <= b.maximum_increment) return "reset_cannot_be_excluded";
    const auto evidence = read_bracket(report);
    if (!evidence) return "raw_bracket_unavailable";
    const auto delta = evidence->unwrapped_delta(b.maximum_increment);
    if (!delta) return "increment_or_wrap_guard_failed";
    result["delta_microjoules"] = std::to_string(delta->energy.value());
    result["minimum_elapsed_ns"] = std::to_string(delta->minimum_elapsed.value());
    result["maximum_elapsed_ns"] = std::to_string(delta->maximum_elapsed.value());
    return "conditional_on_supplied_bound";
}

auto ceiling_energy(std::uint64_t milliwatts, std::uint64_t nanoseconds) -> std::optional<Microjoules> {
    // mW times ns gives pJ. Split out the whole and fractional microjoules
    // before multiplying to avoid overflowing an intermediate pJ product.
    constexpr std::uint64_t picojoules_per_microjoule = 1'000'000U;
    const auto whole = nanoseconds / picojoules_per_microjoule;
    const auto remainder = nanoseconds % picojoules_per_microjoule;
    const auto picojoules = remainder * (milliwatts % picojoules_per_microjoule);
    const auto fraction = remainder * (milliwatts / picojoules_per_microjoule) +
        picojoules / picojoules_per_microjoule + (picojoules % picojoules_per_microjoule != 0U ? 1U : 0U);
    if (milliwatts != 0U && whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / milliwatts)
        return {};
    return Microjoules::from_base_units(whole * milliwatts + fraction);
}

auto assess_estimate(const Report& report, const EstimateOptions& options, Json& result) -> std::string_view {
    const auto source = check_source(report, result);
    if (!source) return source.error();
    const auto elapsed = *source->last.boottime_ns - *source->first.boottime_ns;
    result["outer_boottime_bracket"]["elapsed_ns"] = std::to_string(elapsed);
    if (options.reset_model != ResetModel::accumulation_or_reset_to_zero) return "reset_model_unknown";

    const auto delay = core::Duration::from_base_units(options.assumed_max_counter_delay_ns);
    const auto bounding_time = core::Duration::from_base_units(elapsed).checked_add(delay);
    if (!bounding_time) return "assumption_arithmetic_overflow";
    const auto power = options.assumed_max_package_power_milliwatts;
    const auto maximum_increment = ceiling_energy(power, bounding_time->value());
    if (!maximum_increment) return "assumption_arithmetic_overflow";
    result["assumed_maximum_increment_microjoules"] = std::to_string(maximum_increment->value());

    if (report.after->raw_energy <= *maximum_increment) return "reset_cannot_be_excluded";
    const auto evidence = read_bracket(report);
    if (!evidence) return "raw_bracket_unavailable";
    const auto delta = evidence->unwrapped_delta(*maximum_increment);
    if (!delta) {
        if (evidence->after().raw_energy < evidence->before().raw_energy) return "counter_decreased";
        const auto headroom = evidence->before().advertised_maximum.value() - evidence->before().raw_energy.value();
        if (maximum_increment->value() >= headroom) return "rollover_cannot_be_excluded";
        return "increment_exceeds_assumed_bound";
    }

    const auto timing_error = ceiling_energy(power, delay.value());
    if (!timing_error) return "assumption_arithmetic_overflow";
    result["estimated_microjoules"] = std::to_string(delta->energy.value());
    result["conditional_absolute_timing_error_microjoules"] = std::to_string(timing_error->value());
    if (delta->energy.value() != 0U)
        result["conditional_relative_timing_error_percent"] =
            100.0 * static_cast<double>(timing_error->value()) / static_cast<double>(delta->energy.value());
    return {};
}
} // namespace

auto qualification_json(const Report& report) -> Json {
    const auto& q = *report.qualification;
    Json result{{"before", maybe(q.before, source_json)}, {"after", maybe(q.after, source_json)},
        {"supplied_bound", maybe(q.bound, bound_json)}, {"unit_exponent", nullptr}, {"driver_nanojoules_per_count", nullptr},
        {"delta_microjoules", nullptr}, {"minimum_elapsed_ns", nullptr}, {"maximum_elapsed_ns", nullptr}};
    result["status"] = assess(report, result);
    return result;
}

auto parse_qualification(const Json& value) -> Qualification {
    Qualification q{parse_source(value.at("before")), parse_source(value.at("after")), {}};
    if (!value.at("supplied_bound").is_null()) q.bound = parse_bound(value.at("supplied_bound"));
    return q;
}

auto estimate_json(const Report& report, const EstimateOptions& options) -> Json {
    const auto boottime = [](const std::optional<Snapshot>& source) -> Json {
        return source ? maybe(source->boottime_ns, [](auto ns) { return std::to_string(ns); }) : Json(nullptr);
    };
    const auto& q = report.qualification;
    Json result{{"scope", "whole_cpu_package_between_counter_reads"},
        {"assumptions", {{"counter_accuracy", "assumed"},
                         {"maximum_package_power_milliwatts", std::to_string(options.assumed_max_package_power_milliwatts)},
                         {"maximum_counter_delay_ns", std::to_string(options.assumed_max_counter_delay_ns)},
                         {"counter_reset_model", options.reset_model == ResetModel::accumulation_or_reset_to_zero ?
                             counter_model : "unknown"}}},
        {"outer_boottime_bracket", {{"first_ns", q ? boottime(q->before) : Json(nullptr)},
                                   {"last_ns", q ? boottime(q->after) : Json(nullptr)}, {"elapsed_ns", nullptr}}},
        {"unit_exponent", nullptr}, {"driver_nanojoules_per_count", nullptr},
        {"assumed_maximum_increment_microjoules", nullptr}, {"estimated_microjoules", nullptr},
        {"conditional_absolute_timing_error_microjoules", nullptr}, {"conditional_relative_timing_error_percent", nullptr}};
    const auto reason = assess_estimate(report, options, result);
    result["status"] = reason.empty() ? "assumption_based" : "unavailable";
    result["unavailable_reason"] = reason.empty() ? Json(nullptr) : Json(reason);
    return result;
}

auto read_increment_bound(const std::filesystem::path& path) -> std::expected<IncrementBound, core::Error> {
    const auto failure = [] {
        return std::unexpected{core::Error{.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.package_energy.increment_bound"},
            .message = "Invalid package increment bound or missing matching source and review evidence",
            .operation = "read_package_increment_bound", .os_error = {}, .context = {}}};
    };
    auto directory = platform_linux::experiment_operations::Directory::acquire(std::filesystem::absolute(path).parent_path());
    if (!directory) return std::unexpected{directory.error()};
    const auto bytes = directory->read(path.filename().string(), 4096U);
    if (!bytes) return std::unexpected{bytes.error()};
    try {
        std::set<std::string> keys;
        const auto b = parse_bound(Json::parse(*bytes, [&](int depth, Json::parse_event_t event, Json& token) {
            if (depth > 1 || (event == Json::parse_event_t::key && !keys.insert(token.get<std::string>()).second))
                throw std::invalid_argument{"increment bound must have distinct flat fields"};
            return true;
        }));
        const auto source = directory->read("increment-source.evidence", 16U * 1024U * 1024U);
        const auto review = directory->read("increment-review.evidence", 1024U * 1024U);
        if (!source || source->empty() || !review || review->empty() ||
            core::sha256(*source).to_hex() != b.source_sha256 || core::sha256(*review).to_hex() != b.review_sha256)
            return failure();
        return b;
    } catch (const Json::exception&) { return failure(); }
      catch (const std::invalid_argument&) { return failure(); }
}
} // namespace epycbench::cli::run_energy
