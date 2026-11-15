#include "runtime/finalized_evidence_wire_monitoring_projection.hpp"
#include "core/json_ownership.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace epycbench::runtime::wire_v1::detail {
namespace {
template <std::integral T> auto dec(T v) noexcept { return projection_decimal(v); }

auto token(monitoring::MonitoringSignalState v) noexcept -> std::string_view {
    switch (v) {
    case monitoring::MonitoringSignalState::clear: return "clear";
    case monitoring::MonitoringSignalState::detected: return "detected";
    case monitoring::MonitoringSignalState::unavailable: return "unavailable";
    case monitoring::MonitoringSignalState::not_applicable: return "not_applicable";
    }
    return {};
}
auto token(monitoring::MonitoringSamplePhase v) noexcept -> std::string_view {
    switch (v) {
    case monitoring::MonitoringSamplePhase::before_observation: return "before_observation";
    case monitoring::MonitoringSamplePhase::during_observation: return "during_observation";
    case monitoring::MonitoringSamplePhase::after_observation: return "after_observation";
    }
    return {};
}
auto token(monitoring::MonitoringChannel v) noexcept -> std::string_view {
    switch (v) {
    case monitoring::MonitoringChannel::page_faults: return "page_faults";
    case monitoring::MonitoringChannel::context_switches: return "context_switches";
    case monitoring::MonitoringChannel::temperature: return "temperature";
    case monitoring::MonitoringChannel::frequency: return "frequency";
    case monitoring::MonitoringChannel::throttling: return "throttling";
    }
    return {};
}
auto token(monitoring::FrequencySourceKind v) noexcept -> std::string_view {
    switch (v) {
    case monitoring::FrequencySourceKind::hardware_current: return "hardware_current";
    case monitoring::FrequencySourceKind::scaling_current: return "scaling_current";
    }
    return {};
}
template <class T> auto completeness(T v) noexcept -> std::string_view {
    return v == T::complete ? "complete" : "incomplete";
}
template <class Json> auto domain(const monitoring::FrequencyDomainId& v) -> Json {
    auto cpus = Json::array();
    component_projection::Cleanup cpus_cleanup{cpus};
    for (const auto cpu : v.cpus())
        component_projection::append(cpus, dec(cpu.value()));
    return component_projection::object<Json>(
        [&](auto& object_0) { component_projection::set(object_0, "cpus", std::move(cpus)); });
}
template <class Json, class O, class F> auto optional(const O& v, F f) -> Json {
    return v ? Json(f(*v)) : Json(nullptr);
}
template <class Json, class A, class F> auto acquisition(const A& v, F project) -> Json {
    auto payload = Json(nullptr);
    component_projection::Cleanup payload_cleanup{payload};
    if (v.payload()) payload = project(*v.payload());
    return component_projection::object<Json>([&](auto& object_1) {
        component_projection::set(object_1, "channel", token(v.channel()));
        component_projection::set(object_1, "implementation_id", v.implementation_id().value());
        component_projection::set(object_1, "phase", token(v.phase()));
        component_projection::set(object_1, "collection_start",
                               dec(v.collection_start().nanoseconds()));
        component_projection::set(object_1, "collection_end", dec(v.collection_end().nanoseconds()));
        component_projection::set(object_1, "collection_duration",
                               dec(v.collection_duration().value()));
        component_projection::set(object_1, "status",
                               v.status() == monitoring::MonitoringAcquisitionStatus::available
                                   ? "available"
                                   : "unavailable");
        component_projection::set(object_1, "payload", std::move(payload));
    });
}
} // namespace

template <class Json> auto project_monitoring_as(const monitoring::MonitoringCaptureResult& capture) -> Json {
    const auto& evidence = capture.evidence();
    auto page = Json(nullptr);
    component_projection::Cleanup page_cleanup{page};
    if (evidence.page_faults()) {
        auto workers = Json::array();
        component_projection::Cleanup workers_cleanup{workers};
        for (const auto& v : evidence.page_faults()->workers())
            component_projection::append(
                workers, component_projection::object<Json>([&](auto& object_2) {
                    component_projection::set(object_2, "worker_id", dec(v.worker_id().value()));
                    component_projection::set(object_2, "state", token(v.state()));
                    component_projection::set(
                        object_2, "minor_delta",
                        optional<Json>(v.minor_delta(), [](auto x) { return dec(x.value()); }));
                    component_projection::set(
                        object_2, "major_delta",
                        optional<Json>(v.major_delta(), [](auto x) { return dec(x.value()); }));
                }));
        page = component_projection::object<Json>([&](auto& object_3) {
            component_projection::set(object_3, "state", token(evidence.page_faults()->state()));
            component_projection::set(object_3, "workers", std::move(workers));
        });
    }
    auto context = Json(nullptr);
    component_projection::Cleanup context_cleanup{context};
    if (evidence.context_switches()) {
        auto workers = Json::array();
        component_projection::Cleanup workers_cleanup{workers};
        for (const auto& v : evidence.context_switches()->workers())
            component_projection::append(
                workers, component_projection::object<Json>([&](auto& object_4) {
                    component_projection::set(object_4, "worker_id", dec(v.worker_id().value()));
                    component_projection::set(object_4, "state", token(v.state()));
                    component_projection::set(
                        object_4, "voluntary_count",
                        optional<Json>(v.voluntary_count(), [](auto x) { return dec(x.value()); }));
                    component_projection::set(
                        object_4, "involuntary_count",
                        optional<Json>(v.involuntary_count(), [](auto x) { return dec(x.value()); }));
                }));
        context = component_projection::object<Json>([&](auto& object_5) {
            component_projection::set(object_5, "state", token(evidence.context_switches()->state()));
            component_projection::set(object_5, "workers", std::move(workers));
        });
    }
    auto temperature = Json(nullptr);
    component_projection::Cleanup temperature_cleanup{temperature};
    if (evidence.temperature()) {
        const auto& v = *evidence.temperature();
        temperature = component_projection::object<Json>([&](auto& object_6) {
            component_projection::set(object_6, "sensor_kind", "cpu_control");
            component_projection::set(object_6, "state", token(v.state()));
            component_projection::set(
                object_6, "observed_minimum",
                optional<Json>(v.observed_minimum(), [](auto x) { return dec(x.value()); }));
            component_projection::set(
                object_6, "observed_maximum",
                optional<Json>(v.observed_maximum(), [](auto x) { return dec(x.value()); }));
            component_projection::set(
                object_6, "observed_range",
                optional<Json>(v.observed_range(), [](auto x) { return dec(x.value()); }));
            component_projection::set(object_6, "sample_count", dec(v.sample_count()));
            component_projection::set(object_6, "sampling_interval",
                                   dec(v.sampling_interval().value()));
            component_projection::set(object_6, "capture_completeness",
                                   completeness(v.capture_completeness()));
        });
    }
    auto frequency = Json(nullptr);
    component_projection::Cleanup frequency_cleanup{frequency};
    if (evidence.frequency()) {
        const auto& v = *evidence.frequency();
        auto domains = Json::array();
        component_projection::Cleanup domains_cleanup{domains};
        for (const auto& d : v.domains()) {
            auto variation = Json(nullptr);
            component_projection::Cleanup variation_cleanup{variation};
            if (d.observed_variation())
                variation = component_projection::object<Json>([&](auto& object_7) {
                    component_projection::set(object_7, "numerator",
                                           dec(d.observed_variation()->numerator()));
                    component_projection::set(object_7, "denominator",
                                           dec(d.observed_variation()->denominator()));
                });
            component_projection::append(
                domains, component_projection::object<Json>([&](auto& object_8) {
                    component_projection::set(object_8, "domain_id", domain<Json>(d.domain_id()));
                    component_projection::set(object_8, "source_kind", token(d.source_kind()));
                    component_projection::set(object_8, "state", token(d.state()));
                    component_projection::set(
                        object_8, "observed_minimum",
                        optional<Json>(d.observed_minimum(), [](auto x) { return dec(x.value()); }));
                    component_projection::set(
                        object_8, "observed_maximum",
                        optional<Json>(d.observed_maximum(), [](auto x) { return dec(x.value()); }));
                    component_projection::set(
                        object_8, "observed_range",
                        optional<Json>(d.observed_range(), [](auto x) { return dec(x.value()); }));
                    component_projection::set(object_8, "observed_variation", std::move(variation));
                    component_projection::set(object_8, "sample_count", dec(d.sample_count()));
                }));
        }
        frequency = component_projection::object<Json>([&](auto& object_9) {
            component_projection::set(object_9, "state", token(v.state()));
            component_projection::set(object_9, "domains", std::move(domains));
            component_projection::set(object_9, "sampling_interval",
                                   dec(v.sampling_interval().value()));
            component_projection::set(object_9, "capture_completeness",
                                   completeness(v.capture_completeness()));
        });
    }

    const auto& raw = capture.raw();
    const auto page_payload = [](const monitoring::PageFaultSnapshot& v) {
        return component_projection::object<Json>([&](auto& object_10) {
            component_projection::set(object_10, "worker_id", dec(v.worker_id().value()));
            component_projection::set(object_10, "minor_faults", dec(v.minor_faults().value()));
            component_projection::set(object_10, "major_faults", dec(v.major_faults().value()));
        });
    };
    auto before = Json::array();
    component_projection::Cleanup before_cleanup{before};
    for (const auto& v : raw.page_fault_before())
        component_projection::append(before, acquisition<Json>(v, page_payload));
    auto after = Json::array();
    component_projection::Cleanup after_cleanup{after};
    for (const auto& v : raw.page_fault_after())
        component_projection::append(after, acquisition<Json>(v, page_payload));
    auto contexts = Json::array();
    component_projection::Cleanup contexts_cleanup{contexts};
    for (const auto& v : raw.context_switches())
        component_projection::append(
            contexts, acquisition<Json>(v, [](const auto& batch) {
                auto events = Json::array();
                component_projection::Cleanup events_cleanup{events};
                for (const auto& event : batch.events())
                    component_projection::append(
                        events, component_projection::object<Json>([&](auto& object_11) {
                            component_projection::set(object_11, "worker_id",
                                                   dec(event.worker_id().value()));
                            component_projection::set(object_11, "timestamp",
                                                   dec(event.timestamp().nanoseconds()));
                            component_projection::set(object_11, "kind",
                                                   event.kind() ==
                                                           monitoring::ContextSwitchKind::voluntary
                                                       ? "voluntary"
                                                       : "involuntary");
                        }));
                return component_projection::object<Json>([&](auto& object_12) {
                    component_projection::set(object_12, "worker_id", dec(batch.worker_id().value()));
                    component_projection::set(object_12, "completeness",
                                           completeness(batch.completeness()));
                    component_projection::set(object_12, "events", std::move(events));
                });
            }));
    auto temperatures = Json::array();
    component_projection::Cleanup temperatures_cleanup{temperatures};
    for (const auto& v : raw.temperature())
        component_projection::append(
            temperatures, acquisition<Json>(v, [](const auto& item) {
                return component_projection::object<Json>([&](auto& object_13) {
                    component_projection::set(object_13, "sensor_kind", "cpu_control");
                    component_projection::set(object_13, "temperature",
                                           dec(item.temperature().value()));
                });
            }));
    auto frequencies = Json::array();
    component_projection::Cleanup frequencies_cleanup{frequencies};
    for (const auto& group : raw.frequency()) {
        auto samples = Json::array();
        component_projection::Cleanup samples_cleanup{samples};
        for (const auto& v : group.acquisitions)
            component_projection::append(
                samples, acquisition<Json>(v, [](const auto& item) {
                    return component_projection::object<Json>([&](auto& object_14) {
                        component_projection::set(object_14, "domain_id", domain<Json>(item.domain_id()));
                        component_projection::set(object_14, "source_kind", token(item.source_kind()));
                        component_projection::set(object_14, "frequency",
                                               dec(item.frequency().value()));
                    });
                }));
        component_projection::append(
            frequencies, component_projection::object<Json>([&](auto& object_15) {
                component_projection::set(object_15, "domain_id", domain<Json>(group.domain_id));
                component_projection::set(object_15, "acquisitions", std::move(samples));
            }));
    }
    auto metadata = Json::array();
    component_projection::Cleanup metadata_cleanup{metadata};
    for (const auto& v : raw.frequency_metadata())
        component_projection::append(
            metadata, component_projection::object<Json>([&](auto& object_16) {
                component_projection::set(object_16, "domain_id", domain<Json>(v.domain_id()));
                component_projection::set(object_16, "source_kind", token(v.source_kind()));
                component_projection::set(object_16, "scaling_driver", v.scaling_driver().value());
                component_projection::set(object_16, "scaling_governor", v.scaling_governor().value());
                component_projection::set(object_16, "scaling_minimum",
                                       dec(v.scaling_minimum().value()));
                component_projection::set(object_16, "scaling_maximum",
                                       dec(v.scaling_maximum().value()));
            }));

    const auto& activity = capture.activity();
    return component_projection::object<Json>([&](auto& object_17) {
        component_projection::set(
            object_17, "evidence", component_projection::object<Json>([&](auto& object_18) {
                component_projection::set(object_18, "page_faults_state",
                                       token(evidence.page_faults_state()));
                component_projection::set(object_18, "context_switches_state",
                                       token(evidence.context_switches_state()));
                component_projection::set(object_18, "temperature_state",
                                       token(evidence.temperature_state()));
                component_projection::set(object_18, "frequency_state",
                                       token(evidence.frequency_state()));
                component_projection::set(object_18, "throttling_state",
                                       token(evidence.throttling_state()));
                component_projection::set(object_18, "page_faults", std::move(page));
                component_projection::set(object_18, "context_switches", std::move(context));
                component_projection::set(object_18, "temperature", std::move(temperature));
                component_projection::set(object_18, "frequency", std::move(frequency));
            }));
        component_projection::set(
            object_17, "raw", component_projection::object<Json>([&](auto& object_19) {
                component_projection::set(object_19, "page_fault_before", std::move(before));
                component_projection::set(object_19, "page_fault_after", std::move(after));
                component_projection::set(object_19, "context_switches", std::move(contexts));
                component_projection::set(object_19, "temperature", std::move(temperatures));
                component_projection::set(object_19, "frequency", std::move(frequencies));
                component_projection::set(object_19, "frequency_metadata", std::move(metadata));
            }));
        component_projection::set(
            object_17, "activity", component_projection::object<Json>([&](auto& object_20) {
                component_projection::set(object_20, "scheduled_rounds",
                                       dec(activity.scheduled_rounds()));
                component_projection::set(object_20, "completed_rounds",
                                       dec(activity.completed_rounds()));
                component_projection::set(object_20, "missed_rounds", dec(activity.missed_rounds()));
                component_projection::set(object_20, "service_wakeups",
                                       dec(activity.service_wakeups()));
                component_projection::set(object_20, "context_drains", dec(activity.context_drains()));
                component_projection::set(object_20, "temperature_attempts",
                                       dec(activity.temperature_attempts()));
                component_projection::set(object_20, "frequency_attempts",
                                       dec(activity.frequency_attempts()));
            }));
    });
}
auto project_monitoring(const monitoring::MonitoringCaptureResult& value) -> nlohmann::json {
    return project_monitoring_as<nlohmann::json>(value);
}
auto count_monitoring_projection(const monitoring::MonitoringCaptureResult& value)
    -> ComponentProjectionCount {
    return project_monitoring_as<ComponentProjectionCount>(value);
}
} // namespace epycbench::runtime::wire_v1::detail
