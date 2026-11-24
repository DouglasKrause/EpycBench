#include "runtime/hardware_counter_wire.hpp"

#include <array>
#include <charconv>
#include <nlohmann/json.hpp>

namespace epycbench::runtime::hardware_counter_wire {
namespace {
using Evidence = monitoring::WorkerHardwareCounterEvidence;
using Unavailable = monitoring::HardwareCounterUnavailable;
using Coverage = platform_linux::HardwareCounterCoverage;
using Timestamp = platform_linux::RawMonotonicTimestamp;
using Window = platform_linux::HardwareCounterWindow;
using Json = nlohmann::json;
using namespace std::literals::string_view_literals;

constexpr auto scope = "user_mode_worker_bracket"sv;
constexpr std::array coverage_names{
    std::pair{Coverage::complete, "complete"sv},
    std::pair{Coverage::multiplexed, "multiplexed"sv},
    std::pair{Coverage::not_scheduled, "not_scheduled"sv},
};
constexpr std::array unavailable_names{
    std::pair{Unavailable::unsupported_event, "unsupported_event"sv},
    std::pair{Unavailable::access_denied, "access_denied"sv},
    std::pair{Unavailable::worker_retired, "worker_retired"sv},
    std::pair{Unavailable::attachment_failed, "attachment_failed"sv},
    std::pair{Unavailable::begin_failed, "begin_failed"sv},
    std::pair{Unavailable::finish_failed, "finish_failed"sv},
    std::pair{Unavailable::invalid_window, "invalid_window"sv},
};

auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"runtime.hardware_counters.invalid_evidence"},
        .message = "Invalid worker hardware counter evidence",
        .operation = "hardware_counter_wire",
        .os_error = {},
        .context = {{"classification", classification}},
    }};
}

struct InvalidValue {};

auto unsigned_decimal(const Json& value) -> std::uint64_t {
    const auto text = value.get<std::string_view>();
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
        throw InvalidValue{};
    std::uint64_t number{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, number, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) throw InvalidValue{};
    return number;
}

auto read_window(const Json& value) -> Window {
    if (!value.is_object() || value.size() != 8U) throw InvalidValue{};
    const auto time = [&value](const char* key) {
        return Timestamp::from_nanoseconds(unsigned_decimal(value.at(key)));
    };
    const auto duration = [&value](const char* key) {
        return core::Duration::from_base_units(unsigned_decimal(value.at(key)));
    };
    return {{time("enable_before_ns"), time("enable_after_ns")},
            {time("disable_before_ns"), time("disable_after_ns")},
            {unsigned_decimal(value.at("cpu_cycles")), unsigned_decimal(value.at("retired_instructions")),
             duration("time_enabled_ns"), duration("time_running_ns")}};
}
} // namespace

auto project(const Evidence& evidence) -> Json {
    Json result{{"worker_id", std::to_string(evidence.worker_id().value())}, {"scope", scope},
                {"coverage", "unavailable"}, {"window", nullptr}, {"unavailable_reason", nullptr}};
    if (const auto* window = evidence.window()) {
        result["coverage"] = core::detail::enum_name(window->coverage(), coverage_names);
        result["window"] = {
            {"enable_before_ns", std::to_string(window->enable.before.nanoseconds())},
            {"enable_after_ns", std::to_string(window->enable.after.nanoseconds())},
            {"disable_before_ns", std::to_string(window->disable.before.nanoseconds())},
            {"disable_after_ns", std::to_string(window->disable.after.nanoseconds())},
            {"cpu_cycles", std::to_string(window->counts.cpu_cycles)},
            {"retired_instructions", std::to_string(window->counts.retired_instructions)},
            {"time_enabled_ns", std::to_string(window->counts.enabled.value())},
            {"time_running_ns", std::to_string(window->counts.running.value())},
        };
    } else {
        result["unavailable_reason"] = core::detail::enum_name(*evidence.unavailable_reason(),
                                                               unavailable_names);
    }
    return result;
}

auto decode(const Json& record, core::WorkerId expected_worker, Timestamp enclosing_start,
            Timestamp enclosing_end) -> std::expected<Evidence, core::Error> {
    if (enclosing_end < enclosing_start) return failure("interval_order");
    try {
        if (!record.is_object() || record.size() != 5U || record.at("scope") != scope)
            return failure("record_shape");
        if (unsigned_decimal(record.at("worker_id")) != expected_worker.value())
            return failure("worker_identity");
        const auto& raw = record.at("window");
        if (raw.is_null()) {
            const auto reason = record.at("unavailable_reason").get<std::string_view>();
            for (const auto& [typed, token] : unavailable_names) {
                if (reason != token) continue;
                const auto evidence = Evidence::unavailable(expected_worker, typed);
                if (project(evidence) != record) return failure("record_shape");
                return evidence;
            }
            return failure("unavailable_reason");
        }
        auto evidence = Evidence::create(expected_worker, read_window(raw), enclosing_start, enclosing_end);
        if (!evidence) return evidence;
        if (project(*evidence) != record) return failure("record_shape_or_coverage");
        return evidence;
    } catch (const Json::exception&) {
        return failure("record_shape");
    } catch (const InvalidValue&) {
        return failure("record_value");
    }
}

} // namespace epycbench::runtime::hardware_counter_wire
