#include "runtime/finalized_evidence_wire_memory_projection.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace epycbench::runtime::wire_v1::detail {
namespace {

template <class Json = nlohmann::json>
auto ratio_projection(const memory::ExactRatio& value) -> Json {
    return component_projection::object<Json>([&](auto& object_0) {
        component_projection::set(object_0, "numerator", projection_decimal(value.numerator()));
        component_projection::set(object_0, "denominator", projection_decimal(value.denominator()));
    });
}

auto qualification_token(memory::PlacementQualificationState value) -> std::string_view {
    switch (value) {
    case memory::PlacementQualificationState::skipped: return "skipped";
    case memory::PlacementQualificationState::qualified: return "qualified";
    case memory::PlacementQualificationState::not_qualified: return "not_qualified";
    }
    std::unreachable();
}

template <class Json = nlohmann::json>
auto observation_projection(const memory::RegionPlacementObservation& value) -> Json {
    auto counts = Json::array();
    component_projection::Cleanup counts_cleanup{counts};
    for (const auto& count : value.resident_page_counts()) {
        component_projection::append(
            counts, component_projection::object<Json>([&](auto& object_1) {
                component_projection::set(object_1, "linux_node", projection_decimal(count.linux_node().value()));
                component_projection::set(object_1, "page_count", projection_decimal(count.page_count()));
            }));
    }
    return component_projection::object<Json>([&](auto& object_2) {
        component_projection::set(object_2, "total_pages", projection_decimal(value.total_pages()));
        component_projection::set(object_2, "resident_pages", projection_decimal(value.resident_pages()));
        component_projection::set(object_2, "unresolved_pages", projection_decimal(value.unresolved_pages()));
        component_projection::set(object_2, "resident_page_counts", std::move(counts));
    });
}

template <class Json = nlohmann::json>
auto evaluation_projection(const memory::RegionPlacementEvaluation& value) -> Json {
    return component_projection::object<Json>([&](auto& object_3) {
        component_projection::set(object_3, "requested_node_pages",
                               projection_decimal(value.requested_node_pages()));
        component_projection::set(object_3, "unexpected_node_pages",
                               projection_decimal(value.unexpected_node_pages()));
        component_projection::set(object_3, "unresolved_pages", projection_decimal(value.unresolved_pages()));
        component_projection::set(object_3, "observed_ratio",
                               ratio_projection<Json>(value.observed_ratio()));
    });
}

template <class Json, class Value, class Projection>
auto optional_projection(const std::optional<Value>& value, Projection projection)
    -> Json {
    return value ? projection(*value) : Json(nullptr);
}

template <class Json = nlohmann::json>
auto pre_run_projection(const memory::PreRunPlacementDiagnostic& value) -> Json {
    return component_projection::object<Json>([&](auto& object_4) {
        component_projection::set(
            object_4, "state",
            value.state() == memory::PreRunDiagnosticState::skipped ? "skipped" : "observed");
        component_projection::set(object_4, "observation",
                               optional_projection<Json>(value.observation(), observation_projection<Json>));
        component_projection::set(object_4, "evaluation",
                               optional_projection<Json>(value.evaluation(), evaluation_projection<Json>));
        component_projection::set(
            object_4, "qualification_state",
            value.qualification_state()
                ? Json(qualification_token(*value.qualification_state()))
                : Json(nullptr));
    });
}

template <class Json = nlohmann::json>
auto post_run_projection(const memory::PostRunPlacementEvidence& value) -> Json {
    std::string_view state;
    switch (value.state()) {
    case memory::PostRunVerificationState::not_applicable: state = "not_applicable"; break;
    case memory::PostRunVerificationState::skipped: state = "skipped"; break;
    case memory::PostRunVerificationState::observed: state = "observed"; break;
    }
    return component_projection::object<Json>([&](auto& object_5) {
        component_projection::set(object_5, "state", state);
        component_projection::set(object_5, "observation",
                               optional_projection<Json>(value.observation(), observation_projection<Json>));
        component_projection::set(object_5, "evaluation",
                               optional_projection<Json>(value.evaluation(), evaluation_projection<Json>));
        component_projection::set(
            object_5, "qualification_state",
            value.qualification_state()
                ? Json(qualification_token(*value.qualification_state()))
                : Json(nullptr));
    });
}

template <class Json = nlohmann::json>
auto region_projection(const memory::RegionFinalizationDiagnostic& value) -> Json {
    if (const auto* unavailable = value.unavailable()) {
        const auto reason = unavailable->reason() ==
                                    memory::RegionFinalizationUnavailableReason::contained_exception
                                ? 0U : 1U;
        return component_projection::object<Json>([&](auto& object_6) {
            component_projection::set(object_6, "kind", "unavailable");
            component_projection::set(object_6, "unavailable_reason", projection_decimal(reason));
        });
    }

    const auto& available = *value.available();
    const auto& request = available.request();
    auto targets = Json::array();
    component_projection::Cleanup targets_cleanup{targets};
    for (const auto& target : request.numa_targets()) {
        component_projection::append(
            targets, component_projection::object<Json>([&](auto& object_7) {
                component_projection::set(object_7, "stable_node", target.stable_node.value());
                component_projection::set(object_7, "linux_node", projection_decimal(target.linux_node.value()));
                component_projection::set(object_7, "topology_node",
                                       projection_decimal(target.topology_node.value()));
            }));
    }
    auto protection = Json(nullptr);
    component_projection::Cleanup protection_cleanup{protection};
    if (available.protection_evidence()) {
        const auto& evidence = *available.protection_evidence();
        protection = component_projection::object<Json>([&](auto& object_8) {
            component_projection::set(object_8, "protection_kind", "read_only");
            component_projection::set(object_8, "range", "full_mapping");
            component_projection::set(object_8, "protection_accepted", evidence.protection_accepted());
            component_projection::set(object_8, "writable", evidence.writable());
            component_projection::set(object_8, "executable", evidence.executable());
        });
    }
    auto post_run = available.post_run()
                        ? post_run_projection<Json>(*available.post_run())
                        : Json(nullptr);
    component_projection::Cleanup post_run_cleanup{post_run};
    std::string_view finalization_path;
    switch (available.finalization_path()) {
    case memory::RegionFinalizationPath::protected_execution:
        finalization_path = "protected_execution"; break;
    case memory::RegionFinalizationPath::unprotected_execution_ineligible:
        finalization_path = "unprotected_execution_ineligible"; break;
    case memory::RegionFinalizationPath::unprotected_protection_failed:
        finalization_path = "unprotected_protection_failed"; break;
    }
    const auto& binding = available.binding_evidence();
    const auto& initialization = available.initialization_evidence();
    const auto& cleanup = available.cleanup_evidence();
    return component_projection::object<Json>([&](auto& object_9) {
        component_projection::set(object_9, "kind", "available");
        component_projection::set(
            object_9, "request", component_projection::object<Json>([&](auto& object_10) {
                component_projection::set(object_10, "worker_id",
                                       projection_decimal(request.worker_id().value()));
                component_projection::set(object_10, "requested_bytes",
                                       projection_decimal(request.requested_bytes().value()));
                component_projection::set(object_10, "page_size",
                                       projection_decimal(request.page_size().value()));
                component_projection::set(object_10, "page_mode", "base");
                component_projection::set(object_10, "placement_policy", "bind");
                component_projection::set(object_10, "numa_targets", std::move(targets));
                component_projection::set(object_10, "placement_threshold",
                                       ratio_projection<Json>(request.placement_threshold()));
                component_projection::set(object_10, "prefault", request.prefault());
                component_projection::set(object_10, "verify_before", request.verify_before());
                component_projection::set(object_10, "verify_after", request.verify_after());
            }));
        component_projection::set(
            object_9, "binding", component_projection::object<Json>([&](auto& object_11) {
                component_projection::set(object_11, "policy", "bind");
                component_projection::set(object_11, "range", "full_mapping");
                component_projection::set(object_11, "policy_call_accepted",
                                       binding.policy_call_accepted());
                component_projection::set(object_11, "migration_requested",
                                       binding.migration_requested());
                component_projection::set(object_11, "strict_verification_requested",
                                       binding.strict_verification_requested());
            }));
        component_projection::set(object_9, "initialization",
                               component_projection::object<Json>([&](auto& object_12) {
                                   component_projection::set(object_12, "range", "logical_requested_region");
                                   component_projection::set(object_12, "prefault_requested",
                                                          initialization.prefault_requested());
                                   component_projection::set(object_12, "prefault_pass_performed",
                                                          initialization.prefault_pass_performed());
                                   component_projection::set(
                                       object_12, "initialization_completed",
                                       initialization.initialization_completed());
                                   component_projection::set(object_12, "padding_touched",
                                                          initialization.padding_touched());
                               }));
        component_projection::set(object_9, "pre_run", pre_run_projection<Json>(available.pre_run()));
        component_projection::set(object_9, "protection", std::move(protection));
        component_projection::set(object_9, "post_run", std::move(post_run));
        component_projection::set(
            object_9, "cleanup", component_projection::object<Json>([&](auto& object_13) {
                component_projection::set(object_13, "release_attempted", cleanup.release_attempted());
                component_projection::set(object_13, "release_succeeded", cleanup.release_succeeded());
                component_projection::set(object_13, "range", "full_mapping");
                component_projection::set(object_13, "mapping_active_after",
                                       cleanup.mapping_active_after());
            }));
        component_projection::set(object_9, "finalization_path", finalization_path);
        component_projection::set(object_9, "disposition",
                               available.disposition() ==
                                       memory::RegionFinalizationDisposition::succeeded
                                   ? "succeeded"
                                   : "failed");
    });
}

template <class Json = nlohmann::json>
auto variant_projection(const FinalizedVariantEvidence& value) -> Json {
    auto workers = Json::array();
    component_projection::Cleanup workers_cleanup{workers};
    for (const auto& worker : value.workers) {
        component_projection::append(
            workers, component_projection::object<Json>([&](auto& object_14) {
                component_projection::set(object_14, "worker_id", projection_decimal(worker.worker_id.value()));
                component_projection::set(object_14, "region", region_projection<Json>(worker.region));
                component_projection::set(
                    object_14, "primary_error_ordinal",
                    worker.primary_error_ordinal
                        ? Json(projection_decimal(*worker.primary_error_ordinal))
                        : Json(nullptr));
                component_projection::set(
                    object_14, "cleanup_error_ordinal",
                    worker.cleanup_error_ordinal
                        ? Json(projection_decimal(*worker.cleanup_error_ordinal))
                        : Json(nullptr));
            }));
    }
    return component_projection::object<Json>([&](auto& object_15) {
        component_projection::set(object_15, "slot",
                               value.slot == measurement::ComparisonVariantSlot::first ? "first"
                                                                                       : "second");
        component_projection::set(object_15, "workers", std::move(workers));
    });
}

} // namespace

template <class Json>
auto finalization_projection(const PairwiseFinalizationEvidence& value)
    -> Json {
    return component_projection::object<Json>([&](auto& object_16) {
        component_projection::set(object_16, "first", variant_projection<Json>(value.first));
        component_projection::set(object_16, "second", variant_projection<Json>(value.second));
    });
}

auto project_pairwise_finalization_evidence(const PairwiseFinalizationEvidence& value) -> nlohmann::json {
    return finalization_projection<nlohmann::json>(value);
}
auto project_region_finalization_diagnostic(const memory::RegionFinalizationDiagnostic& value)
    -> nlohmann::json {
    return region_projection<nlohmann::json>(value);
}
auto count_region_finalization_diagnostic(const memory::RegionFinalizationDiagnostic& value)
    -> ComponentProjectionCount {
    return region_projection<ComponentProjectionCount>(value);
}
auto count_pairwise_finalization_evidence(const PairwiseFinalizationEvidence& value) -> ComponentProjectionCount {
    return finalization_projection<ComponentProjectionCount>(value);
}

} // namespace epycbench::runtime::wire_v1::detail
