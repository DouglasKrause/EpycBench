#include "runtime/scenario_monitoring_policy_projection.hpp"
#include "core/json_ownership.hpp"

namespace epycbench::runtime::detail {
namespace {
using Json = nlohmann::ordered_json;
auto projection_error(std::string code, std::string classification) -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        nlohmann::json value(std::move(classification));
        core::detail::append_object(object, "classification", value);
    });
    core::detail::JsonCleanup context_cleanup{context};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Placement request projection failed",
            .operation = "project_placement_request",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto incoherent() -> core::Error {
    return projection_error("runtime.placement_request_projection.projection_incoherent",
                            "projection_incoherent");
}

auto monitoring_channel(std::string_view value) -> std::optional<ipc::PlacementMonitoringChannel> {
    if (value == "page_faults")
        return ipc::PlacementMonitoringChannel::page_faults;
    if (value == "context_switches")
        return ipc::PlacementMonitoringChannel::context_switches;
    if (value == "temperature")
        return ipc::PlacementMonitoringChannel::temperature;
    if (value == "frequency")
        return ipc::PlacementMonitoringChannel::frequency;
    return std::nullopt;
}

auto unavailable_policy(std::string_view value)
    -> std::optional<ipc::PlacementOptionalUnavailableSourcePolicy> {
    if (value == "fail")
        return ipc::PlacementOptionalUnavailableSourcePolicy::fail;
    if (value == "warn")
        return ipc::PlacementOptionalUnavailableSourcePolicy::warn;
    if (value == "disable")
        return ipc::PlacementOptionalUnavailableSourcePolicy::disable;
    return std::nullopt;
}

} // namespace

auto project_scenario_monitoring_inputs(const Json& root)
    -> std::expected<std::optional<ipc::detail::MonitoringValues>, core::Error> {
    if (!root.contains("monitoring"))
        return std::optional<ipc::detail::MonitoringValues>{};
    const auto& value = root.at("monitoring");
    ipc::detail::MonitoringValues result;
    for (const auto& source : value.at("required")) {
        auto channel = monitoring_channel(source.get<std::string_view>());
        if (!channel)
            return std::unexpected{incoherent()};
        result.required.push_back(*channel);
    }
    for (const auto& source : value.at("optional")) {
        auto channel = monitoring_channel(source.get<std::string_view>());
        if (!channel)
            return std::unexpected{incoherent()};
        result.optional.push_back(*channel);
    }
    const auto& sampling = value.at("sampling");
    result.before = sampling.at("before_observation").get<bool>();
    result.after = sampling.at("after_observation").get<bool>();
    const auto& during = sampling.at("during_observation");
    result.during = during.at("enabled").get<bool>();
    if (result.during)
        result.interval =
            core::Duration::from_base_units(during.at("interval").at("value").get<std::uint64_t>());
    auto unavailable =
        unavailable_policy(value.at("unavailable_optional_source").get<std::string_view>());
    if (!unavailable)
        return std::unexpected{incoherent()};
    result.unavailable = *unavailable;
    return std::optional<ipc::detail::MonitoringValues>{std::move(result)};
}
} // namespace epycbench::runtime::detail
