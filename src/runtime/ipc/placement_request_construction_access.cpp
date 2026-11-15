#include "runtime/ipc/placement_request_construction_access.hpp"
#include "core/json_ownership.hpp"
#include "runtime/placement_provenance_access.hpp"

#include <algorithm>
#include <numeric>
#include <ranges>
#include <set>

namespace epycbench::runtime::ipc::detail {
PlacementRequestConstructionValues::PlacementRequestConstructionValues(
 scenario::ScenarioDigest scenario_digest, scenario::ResolvedScenarioPlanDigest resolved_digest,
 topology::PhysicalTopologyFingerprint physical_value, topology::EffectiveTopologyFingerprint effective_value,
 std::uint64_t experiment_index_value, scenario::ScenarioIdentifier experiment_id_value,
 scenario::ScenarioIdentifier workload_id_value, kernel::KernelId kernel_id_value,
 KernelValues kernel_value, MeasurementValues measurement_value,
 std::optional<MonitoringValues> monitoring_value, std::vector<core::LinuxCpuId> service_cpus_value,
 std::vector<topology::StableCoreId> service_cores_value, std::vector<WorkerValues> workers_value)
 : PlacementRequestConstructionValues{
      monitoring_value ? PlacementRequestTransportVersion::v3 : PlacementRequestTransportVersion::v2,
      runtime::detail::PlacementProvenanceConstructionAccess::scenario(std::move(scenario_digest),std::move(resolved_digest)),
      std::move(physical_value),std::move(effective_value),experiment_index_value,
      std::move(experiment_id_value),std::move(workload_id_value),kernel_id_value,
      std::move(kernel_value),std::move(measurement_value),std::move(monitoring_value),
      std::move(service_cpus_value),std::move(service_cores_value),std::move(workers_value)} {}
namespace {
using Json = nlohmann::ordered_json;
auto invalid(std::string classification, std::string field = {}) -> core::Error {
    nlohmann::json context = core::detail::json_object<nlohmann::json>([&](auto& json_value_68) {
        core::detail::set_json(json_value_68, "classification", std::move(classification));
    });
    core::detail::JsonCleanup context_cleanup{context};
    if (!field.empty())
        context["field"] = std::move(field);
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.ipc.invalid_construction"},
            .message = "Typed placement request construction is invalid",
            .operation = "construct_placement_request",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto decimal(std::uint64_t value) { return std::to_string(value); }
auto ratio_json(std::uint64_t n, std::uint64_t d) -> Json {
    return core::detail::json_object<Json>([&](auto& json_value_10009) {
        core::detail::set_json(json_value_10009, "numerator", Json(decimal(n)));
        core::detail::set_json(json_value_10009, "denominator", Json(decimal(d)));
    });
}
auto ratio_json(const RatioValues& ratio) -> Json {
    return ratio_json(ratio.numerator, ratio.denominator);
}
auto channel_token(PlacementMonitoringChannel value) -> std::string_view {
    switch (value) {
    case PlacementMonitoringChannel::page_faults:
        return "page_faults";
    case PlacementMonitoringChannel::context_switches:
        return "context_switches";
    case PlacementMonitoringChannel::temperature:
        return "temperature";
    case PlacementMonitoringChannel::frequency:
        return "frequency";
    }
    return {};
}
auto order_token(scenario::MatrixComparisonOrder value) -> std::string_view {
    switch (value) {
    case scenario::MatrixComparisonOrder::randomized_abba:
        return "randomized-abba";
    case scenario::MatrixComparisonOrder::randomized_baab:
        return "randomized-baab";
    case scenario::MatrixComparisonOrder::randomized_balanced:
        return "randomized-balanced";
    }
    return {};
}
template <class Enum> auto closed(Enum value, std::initializer_list<Enum> values) -> bool {
    return std::ranges::find(values, value) != values.end();
}
auto ratio_valid(const RatioValues& ratio) -> bool {
    return ratio.denominator != 0 && ratio.numerator <= ratio.denominator &&
           std::gcd(ratio.numerator, ratio.denominator) == 1;
}
} // namespace

auto validate_placement_request_local(const PlacementRequestConstructionValues& v)
    -> std::expected<void, core::Error> {
    if (v.version != PlacementRequestTransportVersion::v2 &&
        v.version != PlacementRequestTransportVersion::v3 &&
        v.version != PlacementRequestTransportVersion::v4)
        return std::unexpected{invalid("provenance_invalid")};
    if (!ratio_valid(v.measurement.overhead))
        return std::unexpected{invalid("ratio_invalid", "measurement.overhead")};
    if (!ratio_valid(v.measurement.warmup_threshold))
        return std::unexpected{invalid("ratio_invalid", "measurement.warmup_threshold")};
    if (!ratio_valid(v.measurement.stopping_confidence))
        return std::unexpected{invalid("ratio_invalid", "measurement.stopping_confidence")};
    if (!ratio_valid(v.measurement.stopping_precision))
        return std::unexpected{invalid("ratio_invalid", "measurement.stopping_precision")};
    if (std::ranges::any_of(
            v.workers, [](const auto& worker) { return !ratio_valid(worker.region.threshold); }))
        return std::unexpected{invalid("ratio_invalid", "worker.region.threshold")};
    if (!v.measurement.target.value() || !v.measurement.minimum.value() ||
        !v.measurement.maximum.value())
        return std::unexpected{invalid("quantity_not_positive", "measurement.duration")};
    if (v.measurement.minimum > v.measurement.target ||
        v.measurement.target > v.measurement.maximum)
        return std::unexpected{invalid("measurement_order_invalid", "measurement.observation")};
    if (v.measurement.warmup_minimum > v.measurement.warmup_maximum)
        return std::unexpected{invalid("measurement_order_invalid", "measurement.warmup")};
    if (v.measurement.repetition_minimum > v.measurement.repetition_maximum)
        return std::unexpected{invalid("measurement_order_invalid", "measurement.repetition")};
    if (!closed(v.measurement.calibration_strategy,
                {PlacementCalibrationStrategy::geometric_refine}))
        return std::unexpected{invalid("enum_invalid", "measurement.calibration_strategy")};
    if (!closed(v.measurement.comparison_order,
                {scenario::MatrixComparisonOrder::randomized_abba,
                 scenario::MatrixComparisonOrder::randomized_baab,
                 scenario::MatrixComparisonOrder::randomized_balanced}))
        return std::unexpected{invalid("enum_invalid", "measurement.comparison_order")};
    if (v.monitoring) {
        const auto& m = *v.monitoring;
        if (!std::ranges::is_sorted(m.required) ||
            std::ranges::adjacent_find(m.required) != m.required.end() ||
            !std::ranges::is_sorted(m.optional) ||
            std::ranges::adjacent_find(m.optional) != m.optional.end())
            return std::unexpected{invalid("monitoring_invalid", "monitoring.channel_order")};
        if (std::ranges::any_of(m.required, [&](auto channel) {
                return std::ranges::find(m.optional, channel) != m.optional.end();
            }))
            return std::unexpected{invalid("monitoring_invalid", "monitoring.channel_overlap")};
        if (m.during != m.interval.has_value())
            return std::unexpected{invalid("monitoring_invalid", "monitoring.interval_presence")};
        if (m.interval && !m.interval->value())
            return std::unexpected{invalid("quantity_not_positive", "monitoring.interval")};
        for (auto c : m.required)
            if (channel_token(c).empty())
                return std::unexpected{invalid("enum_invalid", "monitoring.channel")};
        for (auto c : m.optional)
            if (channel_token(c).empty())
                return std::unexpected{invalid("enum_invalid", "monitoring.channel")};
        if (!closed(m.unavailable, {PlacementOptionalUnavailableSourcePolicy::fail,
                                    PlacementOptionalUnavailableSourcePolicy::warn,
                                    PlacementOptionalUnavailableSourcePolicy::disable}))
            return std::unexpected{invalid("enum_invalid", "monitoring.unavailable_policy")};
    }
    const bool scenario = std::holds_alternative<runtime::ScenarioPlacementProvenance>(v.provenance);
    if ((v.version == PlacementRequestTransportVersion::v2 && v.monitoring) ||
        (v.version != PlacementRequestTransportVersion::v2 && !v.monitoring) ||
        (v.version == PlacementRequestTransportVersion::v3 && !scenario))
        return std::unexpected{invalid("provenance_invalid")};
    const bool bw = v.kernel_id.family() == kernel::KernelFamily::bandwidth;
    if (bw != std::holds_alternative<BandwidthValues>(v.kernel))
        return std::unexpected{invalid("kernel_alternative_inconsistent", "execution.kernel")};
    if (const auto* bandwidth = std::get_if<BandwidthValues>(&v.kernel);
        bandwidth && (bandwidth->forbidden_seed || bandwidth->forbidden_origin))
        return std::unexpected{invalid("bandwidth_seed_present", "execution.kernel")};
    if (const auto* latency = std::get_if<LatencyValues>(&v.kernel)) {
        const auto& intent = latency->configuration.pointer_cycle_seed();
        const bool explicit_intent = intent.mode() == kernel::PointerCycleSeedMode::explicit_value;
        if ((explicit_intent !=
             (latency->origin == scenario::PointerCycleSeedOrigin::explicit_value)) ||
            (explicit_intent && intent.value() != latency->seed))
            return std::unexpected{invalid("latency_seed_inconsistent", "execution.kernel")};
    }
    if (v.workers.empty())
        return std::unexpected{invalid("worker_or_numa_collection_invalid", "workers.empty")};
    std::set<std::string> pus;
    std::set<std::uint32_t> cpus;
    for (const auto& worker : v.workers) {
        const auto& r = worker.region;
        if (!r.bytes.value())
            return std::unexpected{invalid("quantity_not_positive", "worker.region.bytes")};
        if (!r.page_size.value())
            return std::unexpected{invalid("quantity_not_positive", "worker.region.page_size")};
        if (r.nodes.empty())
            return std::unexpected{
                invalid("worker_or_numa_collection_invalid", "worker.region.nodes")};
        if (!pus.insert(std::string{worker.stable_pu.value()}).second ||
            !cpus.insert(worker.linux_cpu.value()).second)
            return std::unexpected{invalid("worker_or_numa_collection_invalid", "worker.identity")};
        std::set<std::string> stable;
        std::set<std::uint32_t> linux;
        for (const auto& node : r.nodes)
            if (!stable.insert(std::string{node.stable.value()}).second ||
                !linux.insert(node.linux.value()).second)
                return std::unexpected{
                    invalid("worker_or_numa_collection_invalid", "worker.region.node_identity")};
        if (r.bytes.value() % r.page_size.value())
            return std::unexpected{invalid("region_divisibility_invalid", "worker.region")};
        if (!closed(r.policy, {PlacementPolicy::bind}) ||
            !closed(r.page_mode, {PlacementPageMode::base}))
            return std::unexpected{invalid("enum_invalid", "worker.region")};
    }
    if (!std::ranges::is_sorted(v.service_cpus) ||
        std::ranges::adjacent_find(v.service_cpus) != v.service_cpus.end())
        return std::unexpected{invalid("service_order_invalid", "service.linux_cpus")};
    if (!std::ranges::is_sorted(v.service_cores, {},
                                [](const auto& value) { return value.value(); }) ||
        std::ranges::adjacent_find(v.service_cores) != v.service_cores.end())
        return std::unexpected{invalid("service_order_invalid", "service.stable_cores")};
    return {};
}

auto placement_request_wire(const PlacementRequestConstructionValues& v) -> Json {
    Json kernel_json;
    core::detail::JsonCleanup kernel_json_cleanup{kernel_json};
    if (const auto* b = std::get_if<BandwidthValues>(&v.kernel)) {
        kernel_json = core::detail::json_object<Json>([&](auto& json_value_30001) {
            core::detail::set_json(json_value_30001, "family", Json("sequential-read-bandwidth"));
            core::detail::set_json(
                json_value_30001, "configuration",
                core::detail::json_object<Json>([&](auto& json_value_30002) {
                    core::detail::set_json(
                        json_value_30002, "working_set_per_worker_bytes",
                        Json(decimal(b->configuration.working_set_per_worker().value())));
                }));
        });
    } else {
        const auto& l = std::get<LatencyValues>(v.kernel);
        Json intent = core::detail::json_object<Json>([&](auto& json_value_69) {
            core::detail::set_json(json_value_69, "mode",
                                   Json(l.configuration.pointer_cycle_seed().mode() ==
                                                kernel::PointerCycleSeedMode::explicit_value
                                            ? "explicit-value"
                                            : "derive-per-experiment"));
        });
        core::detail::JsonCleanup intent_cleanup{intent};
        if (l.configuration.pointer_cycle_seed().value())
            intent["value"] = decimal(*l.configuration.pointer_cycle_seed().value());
        kernel_json = core::detail::json_object<Json>([&](auto& json_value_30003) {
            core::detail::set_json(json_value_30003, "family", Json("dependent-load-latency"));
            core::detail::set_json(
                json_value_30003, "configuration",
                core::detail::json_object<Json>([&](auto& json_value_30004) {
                    core::detail::set_json(json_value_30004, "working_set_bytes",
                                           Json(decimal(l.configuration.working_set().value())));
                    core::detail::set_json(json_value_30004, "pointer_cycle_seed_intent",
                                           std::move(intent));
                }));
            core::detail::set_json(json_value_30003, "resolved_permutation_seed",
                                   Json(decimal(l.seed)));
            core::detail::set_json(json_value_30003, "seed_origin",
                                   Json(l.origin == scenario::PointerCycleSeedOrigin::explicit_value
                                            ? "explicit-value"
                                            : "derived-value"));
        });
    }
    const auto& m = v.measurement;
    Json measurement = core::detail::json_object<Json>([&](auto& json_value_70) {
        core::detail::set_json(json_value_70, "observation",
                               core::detail::json_object<Json>([&](auto& json_value_71) {
                                   core::detail::set_json(json_value_71, "target_duration_ns",
                                                          Json(decimal(m.target.value())));
                                   core::detail::set_json(json_value_71, "minimum_duration_ns",
                                                          Json(decimal(m.minimum.value())));
                                   core::detail::set_json(json_value_71, "maximum_duration_ns",
                                                          Json(decimal(m.maximum.value())));
                                   core::detail::set_json(json_value_71, "maximum_harness_overhead",
                                                          Json(ratio_json(m.overhead)));
                               }));
        core::detail::set_json(
            json_value_70, "calibration", core::detail::json_object<Json>([&](auto& json_value_72) {
                core::detail::set_json(json_value_72, "enabled", Json(m.calibration_enabled));
                core::detail::set_json(json_value_72, "strategy", Json("geometric-refine"));
            }));
        core::detail::set_json(
            json_value_70, "warmup", core::detail::json_object<Json>([&](auto& json_value_73) {
                core::detail::set_json(json_value_73, "minimum", Json(decimal(m.warmup_minimum)));
                core::detail::set_json(json_value_73, "maximum", Json(decimal(m.warmup_maximum)));
                core::detail::set_json(json_value_73, "stability_window",
                                       Json(decimal(m.warmup_window)));
                core::detail::set_json(json_value_73, "stability_threshold",
                                       Json(ratio_json(m.warmup_threshold)));
            }));
        core::detail::set_json(
            json_value_70, "repetition", core::detail::json_object<Json>([&](auto& json_value_74) {
                core::detail::set_json(json_value_74, "minimum_blocks",
                                       Json(decimal(m.repetition_minimum)));
                core::detail::set_json(json_value_74, "maximum_blocks",
                                       Json(decimal(m.repetition_maximum)));
                core::detail::set_json(json_value_74, "minimum_valid_observations_per_variant",
                                       Json(decimal(m.minimum_valid)));
            }));
        core::detail::set_json(
            json_value_70, "stopping", core::detail::json_object<Json>([&](auto& json_value_75) {
                core::detail::set_json(json_value_75, "enabled", Json(m.stopping_enabled));
                core::detail::set_json(json_value_75, "confidence",
                                       Json(ratio_json(m.stopping_confidence)));
                core::detail::set_json(json_value_75, "ratio_interval_relative_half_width",
                                       Json(ratio_json(m.stopping_precision)));
            }));
        core::detail::set_json(json_value_70, "comparison_order",
                               Json(order_token(m.comparison_order)));
        core::detail::set_json(json_value_70, "root_seed", Json(decimal(m.root_seed)));
    });
    core::detail::JsonCleanup measurement_cleanup{measurement};
    Json execution = core::detail::json_object<Json>([&](auto& json_value_76) {
        core::detail::set_json(json_value_76, "kernel", std::move(kernel_json));
        core::detail::set_json(json_value_76, "measurement", std::move(measurement));
    });
    core::detail::JsonCleanup execution_cleanup{execution};
    if (v.monitoring) {
        const auto& x = *v.monitoring;
        Json required = Json::array();
        core::detail::JsonCleanup required_cleanup{required};
        Json optional = Json::array();
        core::detail::JsonCleanup optional_cleanup{optional};
        for (auto c : x.required)
            required.push_back(channel_token(c));
        for (auto c : x.optional)
            optional.push_back(channel_token(c));
        Json during = core::detail::json_object<Json>([&](auto& json_value_77) {
            core::detail::set_json(json_value_77, "enabled", Json(x.during));
        });
        core::detail::JsonCleanup during_cleanup{during};
        if (x.interval)
            during["interval"] = decimal(x.interval->value());
        std::string_view unavailable =
            x.unavailable == PlacementOptionalUnavailableSourcePolicy::fail   ? "fail"
            : x.unavailable == PlacementOptionalUnavailableSourcePolicy::warn ? "warn"
                                                                              : "disable";
        core::detail::set_json(
            execution, "monitoring", core::detail::json_object<Json>([&](auto& json_value_30005) {
                core::detail::set_json(json_value_30005, "required", std::move(required));
                core::detail::set_json(json_value_30005, "optional", std::move(optional));
                core::detail::set_json(
                    json_value_30005, "sampling",
                    core::detail::json_object<Json>([&](auto& json_value_30006) {
                        core::detail::set_json(json_value_30006, "before_observation",
                                               Json(x.before));
                        core::detail::set_json(json_value_30006, "after_observation",
                                               Json(x.after));
                        core::detail::set_json(json_value_30006, "during_observation",
                                               std::move(during));
                    }));
                core::detail::set_json(json_value_30005, "unavailable_optional_source",
                                       Json(unavailable));
            }));
    }
    Json service_cpus = Json::array();
    core::detail::JsonCleanup service_cpus_cleanup{service_cpus};
    Json service_cores = Json::array();
    core::detail::JsonCleanup service_cores_cleanup{service_cores};
    for (auto x : v.service_cpus)
        service_cpus.push_back(x.value());
    for (const auto& x : v.service_cores)
        service_cores.push_back(x.value());
    Json workers = Json::array();
    core::detail::JsonCleanup workers_cleanup{workers};
    for (const auto& w : v.workers) {
        Json nodes = Json::array();
        core::detail::JsonCleanup nodes_cleanup{nodes};
        for (const auto& n : w.region.nodes)
            core::detail::append_json(
                nodes, core::detail::json_object<Json>([&](auto& json_value_30010) {
                    core::detail::set_json(json_value_30010, "stable_numa_node_id",
                                           Json(n.stable.value()));
                    core::detail::set_json(json_value_30010, "linux_numa_node",
                                           Json(n.linux.value()));
                }));
        core::detail::append_json(
            workers, core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_78) {
                core::detail::set_json(
                    json_value_78, "cpu",
                    core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_79) {
                        core::detail::set_json(json_value_79, "stable_pu_id",
                                               nlohmann::ordered_json(w.stable_pu.value()));
                        core::detail::set_json(json_value_79, "stable_core_id",
                                               nlohmann::ordered_json(w.stable_core.value()));
                        core::detail::set_json(json_value_79, "linux_cpu",
                                               nlohmann::ordered_json(w.linux_cpu.value()));
                    }));
                core::detail::set_json(
                    json_value_78, "region",
                    core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_80) {
                        core::detail::set_json(
                            json_value_80, "bytes",
                            nlohmann::ordered_json(decimal(w.region.bytes.value())));
                        core::detail::set_json(json_value_80, "policy",
                                               nlohmann::ordered_json("bind"));
                        core::detail::set_json(json_value_80, "nodes", std::move(nodes));
                        core::detail::set_json(json_value_80, "page_mode",
                                               nlohmann::ordered_json("base"));
                        core::detail::set_json(
                            json_value_80, "page_size_bytes",
                            nlohmann::ordered_json(decimal(w.region.page_size.value())));
                        core::detail::set_json(
                            json_value_80, "placement_threshold",
                            nlohmann::ordered_json(ratio_json(w.region.threshold.numerator,
                                                              w.region.threshold.denominator)));
                        core::detail::set_json(json_value_80, "prefault",
                                               nlohmann::ordered_json(w.region.prefault));
                        core::detail::set_json(json_value_80, "verify_before",
                                               nlohmann::ordered_json(w.region.verify_before));
                        core::detail::set_json(json_value_80, "verify_after",
                                               nlohmann::ordered_json(w.region.verify_after));
                    }));
            }));
    }
    Json result = Json::object();
    core::detail::JsonCleanup result_cleanup{result};
    if (v.version == PlacementRequestTransportVersion::v4)
        core::detail::set_json(result, "provenance",
                               runtime::detail::placement_provenance_json(v.provenance));
    else {
        const auto& scenario = std::get<runtime::ScenarioPlacementProvenance>(v.provenance);
        result["scenario_digest"] = scenario.scenario_digest().text();
        result["resolved_scenario_plan_digest"] = scenario.resolved_scenario_plan_digest().text();
    }
    core::detail::set_json(result, "topology",
                           core::detail::json_object<Json>([&](auto& json_value_30007) {
                               core::detail::set_json(json_value_30007, "physical_fingerprint",
                                                      Json(v.physical.to_string()));
                               core::detail::set_json(json_value_30007, "effective_fingerprint",
                                                      Json(v.effective.to_string()));
                           }));
    core::detail::set_json(
        result, "experiment", core::detail::json_object<Json>([&](auto& json_value_30008) {
            core::detail::set_json(json_value_30008, "index", Json(decimal(v.experiment_index)));
            core::detail::set_json(json_value_30008, "id", Json(v.experiment_id.value()));
            core::detail::set_json(json_value_30008, "workload_id", Json(v.workload_id.value()));
            core::detail::set_json(json_value_30008, "kernel_id",
                                   Json(kernel::kernel_id_token(v.kernel_id)));
        }));
    result["execution"] = std::move(execution);
    core::detail::set_json(
        result, "reserved_service", core::detail::json_object<Json>([&](auto& json_value_30009) {
            core::detail::set_json(json_value_30009, "linux_cpus", std::move(service_cpus));
            core::detail::set_json(json_value_30009, "stable_cores", std::move(service_cores));
        }));
    result["workers"] = std::move(workers);
    return Json(std::move(result));
}

auto PlacementRequestConstructionAccess::construct(PlacementRequestConstructionValues v)
    -> std::expected<PlacementRequest, core::Error> {
    auto valid = validate_placement_request_local(v);
    core::detail::ExpectedErrorCleanup valid_cleanup{valid};
    if (!valid)
        return std::unexpected{std::move(valid.error())};
    auto wire = placement_request_wire(v);
    core::detail::JsonCleanup wire_cleanup{wire};
    PlacementKernelExecutionInputs kernel_inputs =
        std::holds_alternative<BandwidthValues>(v.kernel)
            ? PlacementKernelExecutionInputs{PlacementBandwidthExecutionInputs{
                  std::move(std::get<BandwidthValues>(v.kernel).configuration)}}
            : PlacementKernelExecutionInputs{PlacementLatencyExecutionInputs{
                  std::move(std::get<LatencyValues>(v.kernel).configuration),
                  std::get<LatencyValues>(v.kernel).seed,
                  std::get<LatencyValues>(v.kernel).origin}};
    auto overhead = scenario::ExactRatio::create(v.measurement.overhead.numerator,
                                                 v.measurement.overhead.denominator);
    core::detail::ExpectedErrorCleanup overhead_cleanup{overhead};
    auto warmup_threshold = scenario::ExactRatio::create(
        v.measurement.warmup_threshold.numerator, v.measurement.warmup_threshold.denominator);
    core::detail::ExpectedErrorCleanup warmup_threshold_cleanup{warmup_threshold};
    auto stopping_confidence = scenario::ExactRatio::create(
        v.measurement.stopping_confidence.numerator, v.measurement.stopping_confidence.denominator);
    core::detail::ExpectedErrorCleanup stopping_confidence_cleanup{stopping_confidence};
    auto stopping_precision = scenario::ExactRatio::create(
        v.measurement.stopping_precision.numerator, v.measurement.stopping_precision.denominator);
    core::detail::ExpectedErrorCleanup stopping_precision_cleanup{stopping_precision};
    if (!overhead || !warmup_threshold || !stopping_confidence || !stopping_precision)
        return std::unexpected{invalid("ratio_invalid")};
    auto measurement = PlacementMeasurementInputs{v.measurement.target,
                                                  v.measurement.minimum,
                                                  v.measurement.maximum,
                                                  std::move(*overhead),
                                                  v.measurement.calibration_enabled,
                                                  v.measurement.calibration_strategy,
                                                  v.measurement.warmup_minimum,
                                                  v.measurement.warmup_maximum,
                                                  v.measurement.warmup_window,
                                                  std::move(*warmup_threshold),
                                                  v.measurement.repetition_minimum,
                                                  v.measurement.repetition_maximum,
                                                  v.measurement.minimum_valid,
                                                  v.measurement.stopping_enabled,
                                                  std::move(*stopping_confidence),
                                                  std::move(*stopping_precision),
                                                  v.measurement.comparison_order,
                                                  v.measurement.root_seed};
    std::optional<PlacementMonitoringInputs> monitoring;
    if (v.monitoring) {
        auto& m = *v.monitoring;
        monitoring = PlacementMonitoringInputs{
            std::move(m.required), std::move(m.optional), m.before, m.after, m.during, m.interval,
            m.unavailable};
    }
    std::vector<PlacementWorkerRequest> workers;
    workers.reserve(v.workers.size());
    for (auto& w : v.workers) {
        std::vector<PlacementNumaTarget> nodes;
        for (auto& n : w.region.nodes)
            nodes.push_back(PlacementNumaTarget{std::move(n.stable), n.linux});
        auto ratio = PlacementRatio{w.region.threshold.numerator, w.region.threshold.denominator};
        auto region = PlacementRegionRequest{
            w.region.bytes,     w.region.policy,        std::move(nodes),
            w.region.page_mode, w.region.page_size,     ratio,
            w.region.prefault,  w.region.verify_before, w.region.verify_after};
        workers.push_back(PlacementWorkerRequest{
            PlacementCpuAssignment{std::move(w.stable_pu), std::move(w.stable_core), w.linux_cpu},
            std::move(region)});
    }
    return PlacementRequest{
        v.version, std::move(v.provenance),
        PlacementTopologyRequest{std::move(v.physical), std::move(v.effective)},
        PlacementExperimentRequest{v.experiment_index, std::move(v.experiment_id),
                                   std::move(v.workload_id), v.kernel_id},
        PlacementExecutionInputs{std::move(kernel_inputs), std::move(measurement)},
        std::move(monitoring),
        PlacementServiceReservation{std::move(v.service_cpus), std::move(v.service_cores)},
        std::move(workers),
        std::move(wire)};
}
void PlacementRequestConstructionAccess::retire_wire(PlacementRequest& request) noexcept {
    core::detail::retire_json(request.wire_);
}

} // namespace epycbench::runtime::ipc::detail
