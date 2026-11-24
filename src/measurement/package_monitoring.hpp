#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace epycbench::measurement::package_monitoring {

enum class Channel { cpu_migration, page_faults, context_switches, temperature, frequency };
inline constexpr std::array<std::string_view, 5> channel_names{
    "cpu_migration", "page_faults", "context_switches", "temperature", "frequency"};
inline constexpr std::array<std::string_view, 5> source_names{
    "linux.sched_getcpu", "linux.getrusage.thread", "linux.perf.context_switch",
    "linux.hwmon.cpu_control", "linux.cpufreq.policy"};

inline auto channel_name(Channel channel) -> std::string_view {
    return channel_names.at(static_cast<std::size_t>(channel));
}

enum class Requirement { disabled, optional, required };
enum class OptionalUnsupported { fail, warn, disable };

struct Policy {
    std::array<Requirement, channel_names.size()> requirements{};
    OptionalUnsupported unsupported{OptionalUnsupported::fail};
    bool before{}, after{}, during{};
    std::optional<std::uint32_t> service_cpu;
    std::uint64_t interval_ns{};

    [[nodiscard]] auto requests(Channel channel) const noexcept -> bool {
        return requirements[static_cast<std::size_t>(channel)] != Requirement::disabled;
    }
};

inline auto read(const nlohmann::json& settings) -> Policy {
    Policy result;
    for (std::size_t i = 0; i < channel_names.size(); ++i) {
        for (const auto& name : settings.at("required"))
            if (name == channel_names[i])
                result.requirements[i] = Requirement::required;
        for (const auto& name : settings.at("optional"))
            if (name == channel_names[i])
                result.requirements[i] = Requirement::optional;
    }
    const auto& unsupported = settings.at("unavailable_optional_source");
    if (unsupported == "warn") result.unsupported = OptionalUnsupported::warn;
    if (unsupported == "disable") result.unsupported = OptionalUnsupported::disable;
    const auto& sampling = settings.at("sampling");
    result.before = sampling.at("before_observation").get<bool>();
    result.after = sampling.at("after_observation").get<bool>();
    result.during = sampling.at("during_observation").at("enabled").get<bool>();
    if (result.during)
        result.interval_ns = sampling.at("during_observation").at("interval_ns").get<std::uint64_t>();
    if (settings.contains("service_cpu"))
        result.service_cpu = settings.at("service_cpu").get<std::uint32_t>();
    return result;
}

inline auto unsupported_sampling(Channel channel, const Policy& policy) noexcept -> std::string_view {
    switch (channel) {
    case Channel::cpu_migration:
    case Channel::page_faults:
        if (!policy.before || !policy.after)
            return "requires_before_and_after_observation";
        break;
    case Channel::context_switches:
        break;
    case Channel::temperature:
    case Channel::frequency:
        if (policy.during && !policy.service_cpu)
            return "during_observation_service_cpu_not_requested";
        if (!policy.before && !policy.after && !policy.during)
            return "no_observation_samples_requested";
        break;
    }
    return {};
}

} // namespace epycbench::measurement::package_monitoring
