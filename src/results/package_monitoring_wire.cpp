#include "results/package_monitoring_wire.hpp"

#include "measurement/package_monitoring.hpp"
#include "results/package_observation.hpp"

#include <algorithm>
#include <utility>

namespace epycbench::results::package_monitoring_wire {
namespace {
using Json = nlohmann::json;
using U64 = std::uint64_t;
namespace monitor = measurement::package_monitoring;
using monitor::Channel;
using package_observation::invalid;

auto exact(const Json& value, std::initializer_list<const char*> fields) -> bool {
    return value.is_object() && value.size() == fields.size() &&
           std::ranges::all_of(fields, [&](const auto field) { return value.contains(field); });
}

auto number(const Json& value, U64 low = 0, U64 high = UINT64_MAX) -> bool {
    return value.is_number_unsigned() && value.get<U64>() >= low && value.get<U64>() <= high;
}

struct Finding {
    std::string_view status;
    Assessment assessment{Assessment::qualified};
};

auto inspect_channel(const Json& row, Channel channel, const monitor::Policy& policy,
                     U64 begin, U64 end, U64 cpu, bool supplied) -> std::optional<Finding> {
    const auto index = static_cast<std::size_t>(channel);
    if (!exact(row, {"source", "before", "after", "during", "events", "drained_ns", "unavailable_reason",
                     "during_unavailable_reason"}) ||
        row.at("source") != monitor::source_names[index])
        return {};
    const auto& unavailable = row.at("unavailable_reason");
    const auto& before = row.at("before");
    const auto& after = row.at("after");
    const auto& during = row.at("during");
    const auto& events = row.at("events");
    const auto& drained = row.at("drained_ns");
    const auto& during_missing = row.at("during_unavailable_reason");
    const bool sensor = channel == Channel::temperature || channel == Channel::frequency;
    const bool context = channel == Channel::context_switches;
    const auto sampling_issue = monitor::unsupported_sampling(channel, policy);
    if ((!unavailable.is_null() && (!exact(unavailable, {"kind", "code"}) ||
            (unavailable.at("kind") != "unsupported" && unavailable.at("kind") != "capture_failed" &&
             unavailable.at("kind") != "missing") || !unavailable.at("code").is_string() ||
             unavailable.at("code").get_ref<const std::string&>().empty() ||
             unavailable.at("code").get_ref<const std::string&>().size() > 160U ||
             (unavailable.at("code") == "supplied_reading_missing" && !supplied))) ||
        (!sampling_issue.empty() && (unavailable.is_null() ||
            (unavailable.at("kind") != "capture_failed" && unavailable.at("code") != sampling_issue))) ||
        (!before.is_null() && (!policy.before || context || !exact(before, {"time_ns", "value"}) ||
            !number(before.at("time_ns"), 0, begin))) ||
        (!after.is_null() && (!policy.after || context || !exact(after, {"time_ns", "value"}) ||
            !number(after.at("time_ns"), end))) ||
        !during.is_array() || during.size() > 512U ||
        (!during.empty() && (!sensor || !policy.during || !policy.service_cpu)) ||
        (!during_missing.is_null() && (!during.empty() || !sensor || !policy.during || !policy.service_cpu ||
            during_missing != (end - begin < policy.interval_ns
                ? "window_shorter_than_sampling_interval" : "no_in_window_samples"))) ||
        (sensor && policy.during && policy.service_cpu && during.empty() && unavailable.is_null() && during_missing.is_null()) ||
        !events.is_array() || events.size() > 8192U ||
        (!context && (!events.empty() || !drained.is_null())))
        return {};
    Finding state{"clear"};
    if (context) {
        if ((!drained.is_null() && !number(drained, end)) ||
            (drained.is_null() && (!events.empty() || unavailable.is_null())))
            return {};
        U64 previous{};
        for (const auto& event : events) {
            if (!exact(event, {"time_ns", "kind"}) || !number(event.at("time_ns"), previous, drained.get<U64>()))
                return {};
            previous = event.at("time_ns").get<U64>();
            const auto& kind = event.at("kind");
            if (kind != "switch_in" && kind != "voluntary_switch_out" && kind != "involuntary_switch_out")
                return {};
            if (kind == "involuntary_switch_out" && previous >= begin && previous <= end)
                state.status = "warning";
        }
    } else {
        if (unavailable.is_null() &&
            ((policy.before && before.is_null()) || (policy.after && after.is_null())))
            return {};
        if (channel == Channel::cpu_migration) {
            for (const auto* point : {&before, &after})
                if (!point->is_null()) {
                    if (!number(point->at("value"), 0, UINT32_MAX)) return {};
                    if (point->at("value") != cpu) state = {"detected", Assessment::factual_rejection};
                }
        } else if (channel == Channel::page_faults) {
            U64 minor{}, major{};
            for (const auto* point : {&before, &after}) {
                if (point->is_null()) continue;
                const auto& value = point->at("value");
                if (!exact(value, {"minor", "major"}) || !number(value.at("minor"), minor) ||
                    !number(value.at("major"), major)) return {};
                const auto next_major = value.at("major").get<U64>();
                if (point == &after && !before.is_null() && next_major > major)
                    state = {"detected", Assessment::factual_rejection};
                minor = value.at("minor").get<U64>();
                major = next_major;
            }
        } else {
            U64 min = UINT64_MAX, max{};
            std::size_t count{};
            const Json* policy_source = nullptr;
            const auto read = [&](const Json& point) {
                if (point.is_null()) return true;
                const auto& value = point.at("value");
                U64 level;
                if (channel == Channel::temperature) {
                    if (!value.is_number_integer() || (value.is_number_unsigned() && !number(value, 0, INT64_MAX)))
                        return false;
                    level = static_cast<U64>(value.get<std::int64_t>()) ^ (U64{1} << 63U);
                } else {
                    if (!exact(value, {"hz", "policy_cpus", "kind"}) || !number(value.at("hz"), 1) ||
                        (value.at("kind") != "cpuinfo_cur_freq" && value.at("kind") != "scaling_cur_freq"))
                        return false;
                    const auto& cpus = value.at("policy_cpus");
                    if (!cpus.is_array() || cpus.size() > 65536U || !std::ranges::contains(cpus, Json(cpu)) ||
                        !std::ranges::all_of(cpus, [](const auto& id) { return number(id, 0, UINT32_MAX); }) ||
                        !std::ranges::is_sorted(cpus) || std::ranges::adjacent_find(cpus) != cpus.end())
                        return false;
                    if (policy_source && (policy_source->at("policy_cpus") != cpus || policy_source->at("kind") != value.at("kind")))
                        return false;
                    policy_source = &value;
                    level = value.at("hz").get<U64>();
                }
                min = std::min(min, level);
                max = std::max(max, level);
                ++count;
                return true;
            };
            if (!read(before) || !read(after)) return {};
            U64 previous = begin;
            for (const auto& point : during) {
                if (!exact(point, {"read_start_ns", "time_ns", "value"}) || !number(point.at("time_ns"), previous, end) ||
                    !number(point.at("read_start_ns"), previous, point.at("time_ns").get<U64>()) ||
                    !read(point)) return {};
                previous = point.at("time_ns").get<U64>();
            }
            if (count == 0U && unavailable.is_null()) return {};
            if (count == 1U) state.status = "observed";
            if (count >= 2U) {
                const auto spread = max - min;
                if (channel == Channel::temperature ? spread >= 5000U :
                    (spread > max / 20U || (spread == max / 20U && max % 20U == 0U)))
                    state.status = "warning";
            }
        }
    }
    if (!unavailable.is_null()) {
        const bool unsupported = unavailable.at("kind") == "unsupported";
        const bool optional_unavailable = policy.requirements[index] == monitor::Requirement::optional &&
            unavailable.at("kind") != "capture_failed" &&
            (!unsupported || policy.unsupported != monitor::OptionalUnsupported::fail);
        if (!optional_unavailable)
            state.assessment = Assessment::acquisition_failure;
        if (state.status != "detected")
            state.status = unsupported && optional_unavailable && policy.unsupported == monitor::OptionalUnsupported::disable
                ? "not_applicable" : "unavailable";
    }
    return state;
}

void increment(Json& count) {
    count = count.is_null() ? 1U : count.get<U64>() + 1U;
}

auto inspect_window(const monitor::Policy* policy, const Json& record, const Json& window,
                    U64 cpu, bool supplied, Json* summary)
    -> std::expected<Assessment, core::Error> {
    const auto retained = record.find("monitoring");
    if (!policy)
        return retained == record.end()
            ? std::expected<Assessment, core::Error>{Assessment::qualified}
            : std::unexpected{invalid("Unrequested monitoring evidence")};
    if (window.is_null() || !number(window.at("begin_ns")) || !number(window.at("end_ns")) ||
        window.at("begin_ns") >= window.at("end_ns"))
        return Assessment::qualified;
    const auto fail = [] { return std::unexpected{invalid("Malformed or unbound rank monitoring evidence")}; };
    if (retained == record.end() || !retained->is_object()) return fail();
    auto count = retained->size();
    auto assessment = Assessment::qualified;
    for (std::size_t i = 0; i < monitor::channel_names.size(); ++i) {
        const auto channel = static_cast<Channel>(i);
        if (!policy->requests(channel)) continue;
        const auto name = monitor::channel_name(channel);
        const auto raw = retained->find(name);
        if (raw == retained->end()) return fail();
        --count;
        const auto finding = inspect_channel(*raw, channel, *policy, window.at("begin_ns").get<U64>(),
            window.at("end_ns").get<U64>(), cpu, supplied);
        if (!finding) return std::unexpected{invalid("Malformed or unbound monitoring evidence for " + std::string{name})};
        assessment = std::max(assessment, finding->assessment);
        if (summary) {
            auto& aggregate = (*summary)[name];
            increment(aggregate["states"][finding->status]);
            if (finding->assessment != Assessment::qualified) increment(aggregate["disqualifying_callbacks"]);
            const auto& unavailable = raw->at("unavailable_reason");
            if (!unavailable.is_null())
                increment(aggregate["unavailable_reasons"][unavailable.at("code").get_ref<const std::string&>()]);
            if (const auto& during = raw->at("during_unavailable_reason"); !during.is_null())
                increment(aggregate["unavailable_during_reasons"][during.get_ref<const std::string&>()]);
        }
    }
    if (count != 0U) return fail();
    return assessment;
}
} // namespace

auto inspect(const Json& execution, const Json& block, bool calibrating, bool supplied, Json* summary,
             bool include_mixed_alone)
    -> std::expected<Assessment, core::Error> {
    const auto settings = execution.find("monitoring");
    const auto policy = settings != execution.end() && !calibrating
        ? std::optional{monitor::read(*settings)} : std::nullopt;
    auto assessment = Assessment::qualified;
    const auto visit = [&](const Json& ranks) -> std::expected<void, core::Error> {
        for (const auto& rank : ranks) {
            auto assessed = inspect_window(policy ? &*policy : nullptr, rank, rank.at("trial"),
                                           rank.at("placement").at("cpu").get<U64>(), supplied, summary);
            if (!assessed) return std::unexpected{assessed.error()};
            assessment = std::max(assessment, *assessed);
        }
        return {};
    };
    if (auto ranks = visit(block.at("ranks")); !ranks) return std::unexpected{ranks.error()};
    if (block.contains("stream"))
        for (const auto& chunk : block.at("stream").at("chunks"))
            if (auto ranks = visit(chunk.at("ranks")); !ranks) return std::unexpected{ranks.error()};
    if (block.contains("collectives"))
        for (const auto& collective : block.at("collectives")) {
            const auto phase = collective.find("finalization");
            if (phase == collective.end()) continue;
            const auto& ranks = block.at("ranks");
            const auto owner = std::ranges::find_if(ranks, [&](const auto& rank) {
                return rank.at("role") == collective.at("role") && rank.at("rank") == collective.at("owner_rank");
            });
            if (owner == ranks.end())
                return std::unexpected{invalid("Collective finalization monitoring lacks its owner")};
            auto assessed = inspect_window(policy ? &*policy : nullptr, *phase, *phase,
                                           owner->at("placement").at("cpu").get<U64>(), supplied, summary);
            if (!assessed) return std::unexpected{assessed.error()};
            assessment = std::max(assessment, *assessed);
        }
    if (include_mixed_alone && block.contains("mixed")) {
        for (const auto& condition : block.at("mixed").at("alone")) {
            const auto& alone = condition.at("block");
            if (alone.contains("mixed")) return std::unexpected{invalid("Unexpected nested mixed condition")};
            const auto assessed = inspect(execution, alone, calibrating, supplied, summary);
            if (!assessed) return std::unexpected{assessed.error()};
            assessment = std::max(assessment, *assessed);
        }
    }
    return assessment;
}

} // namespace epycbench::results::package_monitoring_wire
