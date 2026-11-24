#include "results/package_worker_counter_wire.hpp"

#include "runtime/hardware_counter_wire.hpp"

#include <charconv>

namespace epycbench::results::package_worker_counter_wire {
namespace {
using Json = nlohmann::json;
using CounterEvidence = platform_linux::package_execution::CounterEvidence;
using Timestamp = platform_linux::RawMonotonicTimestamp;
namespace wire = runtime::hardware_counter_wire;

auto invalid() -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{core::ErrorCategory::results,
        core::StableErrorCode{"result.package.hardware_counter_scope"},
        "Invalid package worker hardware counter scope", "decode_package_worker_counters", {}, Json::object(),
        core::ErrorSeverity::error, core::ContinuationPolicy::abort_command}};
}

auto timestamp(const Json& value) -> std::optional<Timestamp> {
    if (value.is_null()) return std::nullopt;
    const auto text = value.get<std::string_view>();
    std::uint64_t number{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, number);
    if (text.empty() || (text.size() > 1U && text.front() == '0') || parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::invalid_argument{"package counter timestamp"};
    return Timestamp::from_nanoseconds(number);
}
} // namespace

auto project(const CounterEvidence& evidence) -> Json {
    const auto time = [](const auto& value) {
        return value ? Json(std::to_string(value->nanoseconds())) : Json(nullptr);
    };
    const auto supplied = evidence.worker.unavailable_reason() == monitoring::HardwareCounterUnavailable::supplied_workers;
    auto worker = supplied ? Json{{"worker_id", std::to_string(evidence.worker.worker_id().value())},
        {"scope", "user_mode_worker_bracket"}, {"coverage", "unavailable"}, {"window", nullptr},
        {"unavailable_reason", "supplied_workers"}} : wire::project(evidence.worker);
    return {{"enclosing_start_ns", time(evidence.enclosing_start)},
        {"enclosing_end_ns", time(evidence.enclosing_end)}, {"worker", std::move(worker)}};
}

auto decode(const Json& record, core::WorkerId worker, std::optional<std::uint64_t> callback_start,
    std::optional<std::uint64_t> callback_end, bool supplied) -> std::expected<CounterEvidence, core::Error> try {
    if (!record.is_object() || record.size() != 3U) return invalid();
    const auto start = timestamp(record.at("enclosing_start_ns"));
    const auto end = timestamp(record.at("enclosing_end_ns"));
    if (supplied) {
        const auto evidence = CounterEvidence{monitoring::WorkerHardwareCounterEvidence::unavailable(
            worker, monitoring::HardwareCounterUnavailable::supplied_workers), {}, {}};
        if (record != project(evidence)) return invalid();
        return evidence;
    }
    constexpr auto zero = Timestamp::from_nanoseconds(0U);
    auto evidence = wire::decode(record.at("worker"), worker, start.value_or(zero), end.value_or(zero));
    if (!evidence) return std::unexpected{std::move(evidence.error())};
    if (const auto* window = evidence->window()) {
        if (!start || !end || !callback_start || !callback_end || *callback_start >= *callback_end ||
            start->nanoseconds() != *callback_start || end->nanoseconds() != *callback_end ||
            window->enable.after.nanoseconds() > *callback_start ||
            window->disable.before.nanoseconds() < *callback_end) return invalid();
    } else if (start || end) {
        return invalid();
    }
    return CounterEvidence{std::move(*evidence), start, end};
} catch (const Json::exception&) {
    return invalid();
} catch (const std::invalid_argument&) {
    return invalid();
}

} // namespace epycbench::results::package_worker_counter_wire
