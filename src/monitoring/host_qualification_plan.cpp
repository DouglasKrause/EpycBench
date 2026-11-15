#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_frozen_source_composition.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "monitoring/privacy_validation.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <ranges>

namespace epycbench::monitoring::qualification {
namespace {
using Json=nlohmann::json;
auto fail(std::string code,std::string classification)->core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Monitoring host qualification plan resolution failed",
            .operation = "resolve_monitoring_host_qualification_plan",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& json_value_19) {
                core::detail::set_json(json_value_19, "classification", std::move(classification));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto contained(bool unknown=false) { return fail("monitoring.host_environment.contained_failure",
    unknown?"contained_unknown_failure":"contained_exception"); }
auto prerequisite_mismatch()->core::Error {
    return fail("monitoring.host_environment.plan_rejected","prerequisite_binding_mismatch");
}
auto worker_json(const HostEnvironmentWorker& w)->Json {
    return core::detail::json_object<Json>([&](auto& json_value_10003) {
        core::detail::set_json(json_value_10003, "linux_cpu", Json(w.linux_cpu.value()));
        core::detail::set_json(json_value_10003, "stable_core_id", Json(w.stable_core_id.value()));
        core::detail::set_json(json_value_10003, "stable_numa_node_id",
                               Json(w.stable_numa_node_id.value()));
        core::detail::set_json(json_value_10003, "stable_pu_id", Json(w.stable_pu_id.value()));
        core::detail::set_json(json_value_10003, "worker_id", Json(w.worker_id));
    });
}
auto service_json(const HostEnvironmentServiceCore& s)->Json {
    Json pus = Json::array();
    core::detail::JsonCleanup pus_cleanup{pus};
    Json cpus = Json::array();
    core::detail::JsonCleanup cpus_cleanup{cpus};
    for(const auto& pu:s.stable_pu_ids) pus.push_back(pu.value());
    for(auto cpu:s.linux_cpus) cpus.push_back(cpu.value());
    return core::detail::json_object<Json>([&](auto& json_value_10004) {
        core::detail::set_json(json_value_10004, "execution_linux_cpu",
                               Json(s.execution_linux_cpu.value()));
        core::detail::set_json(json_value_10004, "execution_stable_pu_id",
                               Json(s.execution_stable_pu_id.value()));
        core::detail::set_json(json_value_10004, "linux_cpus", std::move(cpus));
        core::detail::set_json(json_value_10004, "stable_core_id", Json(s.stable_core_id.value()));
        core::detail::set_json(json_value_10004, "stable_pu_ids", std::move(pus));
    });
}
auto duration_json(core::Duration value)->Json {
    return core::detail::json_object<Json>([&](auto& json_value_10005) {
        core::detail::set_json(json_value_10005, "unit", Json("ns"));
        core::detail::set_json(json_value_10005, "value", Json(value.value()));
    });
}
auto ratio_json(std::uint64_t numerator,std::uint64_t denominator)->Json {
    return core::detail::json_object<Json>([&](auto& json_value_10006) {
        core::detail::set_json(json_value_10006, "denominator", Json(denominator));
        core::detail::set_json(json_value_10006, "numerator", Json(numerator));
        core::detail::set_json(json_value_10006, "unit", Json("ratio"));
    });
}
auto family_json(std::string family,std::string kernel,core::ByteCount working_set,
                 std::span<const HostEnvironmentWorker> workers)->Json {
    Json projected = Json::array();
    core::detail::JsonCleanup projected_cleanup{projected};
    for (const auto& w : workers)
        core::detail::append_json(projected, worker_json(w));
    Json memory = core::detail::json_object<Json>([&](auto& json_value_20) {
        core::detail::set_json(
            json_value_20, "nodes", core::detail::json_object<Json>([&](auto& json_value_21) {
                core::detail::set_json(json_value_21, "local-to-each-worker", Json(true));
            }));
        core::detail::set_json(json_value_20, "policy", Json("bind"));
    });
    core::detail::JsonCleanup memory_cleanup{memory};
    return core::detail::json_object<Json>([&](auto& json_value_10007) {
        core::detail::set_json(json_value_10007, "family", std::move(family));
        core::detail::set_json(json_value_10007, "kernel", std::move(kernel));
        core::detail::set_json(
            json_value_10007, "variants", ([&]() {
                auto json_value_12 = Json::array();
                core::detail::JsonCleanup json_value_12_cleanup{json_value_12};
                core::detail::append_json(
                    json_value_12, core::detail::json_object<Json>([&](auto& json_value_13) {
                        core::detail::set_json(json_value_13, "id", Json("first"));
                        core::detail::set_json(json_value_13, "memory",
                                               core::detail::clone_json_as<Json>(memory));
                    }));
                core::detail::append_json(
                    json_value_12, core::detail::json_object<Json>([&](auto& json_value_14) {
                        core::detail::set_json(json_value_14, "id", Json("second"));
                        core::detail::set_json(json_value_14, "memory",
                                               core::detail::clone_json_as<Json>(memory));
                    }));
                return Json(std::move(json_value_12));
            }()));
        core::detail::set_json(json_value_10007, "workers", std::move(projected));
        core::detail::set_json(json_value_10007, "workload",
                               core::detail::json_object<Json>([&](auto& json_value_10008) {
                                   core::detail::set_json(json_value_10008, "working_set_bytes",
                                                          Json(working_set.value()));
                               }));
    });
}
auto linux_node(const runtime::RunnerContext& context,const topology::StableNumaNodeId& stable)
    ->std::optional<core::LinuxNumaNodeId> {
    for(const auto& node:context.topology().numa_nodes()) {
        auto id=context.stable_ids().numa_node_id(node.id);
        if(id && id->get()==stable && node.linux_node_id.value())
            return core::LinuxNumaNodeId{*node.linux_node_id.value()};
    }
    return std::nullopt;
}
}

class detail_MonitoringHostQualificationPlanFactory {
  public:
    static auto resolve(const RunnerContextAcquisitionSession& session,
                        const HostEnvironmentAuthority& environment) noexcept
        -> std::expected<MonitoringHostQualificationPlan, core::Error> {
        try {
            return resolve_fallible(session, environment, nullptr, nullptr);
        } catch (const std::exception&) {
            return std::unexpected{contained(false)};
        } catch (...) {
            return std::unexpected{contained(true)};
        }
    }
    static auto resolve_fallible(const RunnerContextAcquisitionSession& session,
                                 const HostEnvironmentAuthority& environment,
                                 std::optional<core::Error>* raw_failure,
                                 detail::HostFrozenCompositionStage* stage)
        -> std::expected<MonitoringHostQualificationPlan, core::Error> {
        auto session_binding = detail_HostQualificationPlanAccess::binding_fallible(session);
        core::detail::ExpectedErrorCleanup session_binding_cleanup{session_binding};
        if (!session_binding) {
            if (raw_failure && !*raw_failure)
                raw_failure->emplace(std::move(session_binding.error()));
            return std::unexpected{fail("monitoring.host_qualification_plan.binding_rejected",
                                        "environment_session_binding_mismatch")};
        }
        if (session_binding->get() !=
            detail_HostQualificationPlanAccess::binding(environment).get())
            return std::unexpected{fail("monitoring.host_qualification_plan.binding_rejected",
                                        "environment_session_binding_mismatch")};
        auto templ = detail_RunnerContextAcquisitionAccess::input_template_fallible(session);
        core::detail::ExpectedErrorCleanup templ_cleanup{templ};
        if (!templ) {
            if (raw_failure && !*raw_failure)
                raw_failure->emplace(std::move(templ.error()));
            return std::unexpected{
                fail("monitoring.host_qualification_plan.binding_rejected", "invalid_session")};
        }
        auto prereq = detail_RunnerContextAcquisitionAccess::prerequisites_fallible(session);
        core::detail::ExpectedErrorCleanup prereq_cleanup{prereq};
        if (!prereq) {
            if (raw_failure && !*raw_failure)
                raw_failure->emplace(std::move(prereq.error()));
            return std::unexpected{
                fail("monitoring.host_qualification_plan.binding_rejected", "invalid_session")};
        }
        auto context = detail_RunnerContextAcquisitionAccess::runner_context_fallible(session);
        core::detail::ExpectedErrorCleanup context_cleanup{context};
        if (!context) {
            if (raw_failure && !*raw_failure)
                raw_failure->emplace(std::move(context.error()));
            return std::unexpected{
                fail("monitoring.host_qualification_plan.binding_rejected", "invalid_session")};
        }
        if(!templ||!prereq||!context)
            return std::unexpected{fail("monitoring.host_qualification_plan.binding_rejected",
                                        "invalid_session")};
        if(environment.physical_fingerprint()!=context->get().fingerprints().physical ||
           environment.effective_fingerprint()!=context->get().fingerprints().effective)
            return std::unexpected{fail("monitoring.host_qualification_plan.binding_rejected",
                                        "environment_context_mismatch")};
        if(prereq->get().bandwidth_descriptor().kernel_id()!=templ->get().bandwidth_kernel() ||
           prereq->get().latency_descriptor().kernel_id()!=templ->get().latency_kernel() ||
           prereq->get().calibration_policy().minimum_duration()!=templ->get().minimum_duration() ||
           prereq->get().calibration_policy().target_duration()!=templ->get().target_duration() ||
           prereq->get().calibration_policy().maximum_duration()!=templ->get().maximum_duration())
            return std::unexpected{prerequisite_mismatch()};

        Json service = core::detail::json_object<Json>([&](auto& json_value_22) {
            core::detail::set_json(json_value_22, "active_roles", ([&]() {
                                       auto json_value_15 = Json::array();
                                       core::detail::JsonCleanup json_value_15_cleanup{
                                           json_value_15};
                                       core::detail::append_json(json_value_15, Json("control"));
                                       return Json(std::move(json_value_15));
                                   }()));
        });
        core::detail::JsonCleanup service_cleanup{service};
        for (const auto& s : environment.service_cores())
            core::detail::set_json(service, s.role, service_json(s));
        Json document = core::detail::json_object<Json>([&](auto& json_value_23) {
            core::detail::set_json(
                json_value_23, "bandwidth",
                Json(family_json("sequential-read-bandwidth", "memory.bandwidth.read/v1",
                                 templ->get().working_set_bytes(),
                                 environment.bandwidth_workers())));
            core::detail::set_json(json_value_23, "effective_fingerprint",
                                   Json(environment.effective_fingerprint().to_string()));
            core::detail::set_json(json_value_23, "environment_digest",
                                   Json(environment.environment_digest()));
            core::detail::set_json(
                json_value_23, "latency",
                Json(family_json("dependent-load-latency", "memory.latency.dependent-load/v1",
                                 templ->get().working_set_bytes(), environment.latency_workers())));
            core::detail::set_json(
                json_value_23, "measurement",
                core::detail::json_object<Json>([&](auto& json_value_24) {
                    core::detail::set_json(
                        json_value_24, "calibration",
                        core::detail::json_object<Json>([&](auto& json_value_25) {
                            core::detail::set_json(json_value_25, "enabled", Json(true));
                            core::detail::set_json(json_value_25, "strategy",
                                                   Json("geometric-refine"));
                        }));
                    core::detail::set_json(
                        json_value_24, "observation",
                        core::detail::json_object<Json>([&](auto& json_value_26) {
                            core::detail::set_json(
                                json_value_26, "maximum_duration",
                                Json(duration_json(templ->get().maximum_duration())));
                            core::detail::set_json(json_value_26, "maximum_harness_overhead",
                                                   Json(ratio_json(1, 100)));
                            core::detail::set_json(
                                json_value_26, "minimum_duration",
                                Json(duration_json(templ->get().minimum_duration())));
                            core::detail::set_json(
                                json_value_26, "target_duration",
                                Json(duration_json(templ->get().target_duration())));
                        }));
                    core::detail::set_json(
                        json_value_24, "repetition",
                        core::detail::json_object<Json>([&](auto& json_value_27) {
                            core::detail::set_json(json_value_27, "maximum_blocks", Json(20));
                            core::detail::set_json(json_value_27, "minimum_blocks", Json(8));
                            core::detail::set_json(
                                json_value_27, "minimum_valid_observations_per_variant", Json(16));
                        }));
                    core::detail::set_json(
                        json_value_24, "stopping",
                        core::detail::json_object<Json>([&](auto& json_value_28) {
                            core::detail::set_json(json_value_28, "confidence",
                                                   Json(ratio_json(19, 20)));
                            core::detail::set_json(json_value_28, "enabled", Json(false));
                            core::detail::set_json(json_value_28,
                                                   "ratio_interval_relative_half_width",
                                                   Json(ratio_json(1, 50)));
                        }));
                    core::detail::set_json(
                        json_value_24, "warmup",
                        core::detail::json_object<Json>([&](auto& json_value_29) {
                            core::detail::set_json(json_value_29, "maximum", Json(10));
                            core::detail::set_json(json_value_29, "minimum", Json(3));
                            core::detail::set_json(json_value_29, "stability_threshold",
                                                   Json(ratio_json(3, 100)));
                            core::detail::set_json(json_value_29, "stability_window", Json(3));
                        }));
                }));
            core::detail::set_json(
                json_value_23, "memory", core::detail::json_object<Json>([&](auto& json_value_30) {
                    core::detail::set_json(
                        json_value_30, "nodes",
                        core::detail::json_object<Json>([&](auto& json_value_31) {
                            core::detail::set_json(json_value_31, "local-to-each-worker",
                                                   Json(true));
                        }));
                    core::detail::set_json(json_value_30, "policy", Json("bind"));
                }));
            core::detail::set_json(
                json_value_23, "monitoring",
                core::detail::json_object<Json>([&](auto& json_value_32) {
                    core::detail::set_json(
                        json_value_32, "active_roles", ([&]() {
                            auto json_value_16 = Json::array();
                            core::detail::JsonCleanup json_value_16_cleanup{json_value_16};
                            core::detail::append_json(json_value_16, Json("control"));
                            return Json(std::move(json_value_16));
                        }()));
                    core::detail::set_json(json_value_32, "configuration", Json("C0"));
                    core::detail::set_json(json_value_32, "optional", Json::array());
                    core::detail::set_json(json_value_32, "required", Json::array());
                    core::detail::set_json(
                        json_value_32, "sampling",
                        core::detail::json_object<Json>([&](auto& json_value_33) {
                            core::detail::set_json(json_value_33, "after_observation", Json(false));
                            core::detail::set_json(json_value_33, "before_observation",
                                                   Json(false));
                            core::detail::set_json(
                                json_value_33, "during_observation",
                                core::detail::json_object<Json>([&](auto& json_value_34) {
                                    core::detail::set_json(json_value_34, "enabled", Json(false));
                                }));
                        }));
                    core::detail::set_json(json_value_32, "unavailable_optional_source",
                                           Json("fail"));
                }));
            core::detail::set_json(json_value_23, "physical_fingerprint",
                                   Json(environment.physical_fingerprint().to_string()));
            core::detail::set_json(json_value_23, "policy_identity",
                                   Json(std::string{templ->get().policy_identity()}));
            core::detail::set_json(json_value_23, "privacy_validation", Json("passed"));
            core::detail::set_json(json_value_23, "schema",
                                   Json("epycbench.monitoring-host-qualification-plan/v2"));
            core::detail::set_json(json_value_23, "service_reservation", std::move(service));
            core::detail::set_json(json_value_23, "template_sha256",
                                   Json(templ->get().template_sha256().to_hex()));
        });
        core::detail::JsonCleanup document_cleanup{document};
        if(!detail::privacy_safe(document))
            return std::unexpected{fail("monitoring.host_qualification_plan.resolution_rejected",
                                        "privacy_rejected")};
        auto canonical = core::canonicalize_json(document);
        core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
        if (!canonical)
            return std::unexpected{std::move(canonical.error())};
        canonical->push_back('\n'); auto digest=core::sha256(*canonical);

        const auto make_memory=[&](std::span<const HostEnvironmentWorker> workers)
          ->std::expected<std::vector<HostPlanWorkerMemoryBinding>,core::Error>{
          std::vector<HostPlanWorkerMemoryBinding> bindings;
          for(const auto& w:workers){auto node=linux_node(context->get(),w.stable_numa_node_id);
            if(!node)return std::unexpected{fail("monitoring.host_qualification_plan.resolution_rejected",
                                                 "worker_node_binding_mismatch")};
            bindings.push_back({HostPlanVariantId::first,w.worker_id,w.linux_cpu,*node});
            bindings.push_back({HostPlanVariantId::second,w.worker_id,w.linux_cpu,*node});}
          return bindings;
        };
        auto bandwidth_memory=make_memory(environment.bandwidth_workers());
        core::detail::ExpectedErrorCleanup bandwidth_memory_cleanup{bandwidth_memory};
        if (!bandwidth_memory)
            return std::unexpected{std::move(bandwidth_memory.error())};
        auto latency_memory=make_memory(environment.latency_workers());
        core::detail::ExpectedErrorCleanup latency_memory_cleanup{latency_memory};
        if (!latency_memory)
            return std::unexpected{std::move(latency_memory.error())};

        MonitoringHostQualificationPlan plan{
          prereq->get().bandwidth_descriptor(),prereq->get().latency_descriptor(),
          prereq->get().calibration_policy(),
          {environment.bandwidth_workers().begin(),environment.bandwidth_workers().end()},
          {environment.latency_workers().begin(),environment.latency_workers().end()},
          {environment.service_cores().begin(),environment.service_cores().end()},
          templ->get().working_set_bytes(),environment.base_page_size(),
          std::move(*bandwidth_memory),std::move(*latency_memory),
          std::string{templ->get().policy_identity()},templ->get().template_sha256(),
          environment.environment_digest(),
          environment.physical_fingerprint(),environment.effective_fingerprint(),
          *session_binding,
          std::move(*canonical),digest};
        detail_HostSelectedResourceRequirements requirements{
          *session_binding,plan.environment_digest(),plan.physical_fingerprint(),
          plan.effective_fingerprint(),{}, {}};
        const auto append=[&](std::string family,std::span<const HostEnvironmentWorker> workers,
                              std::span<const HostPlanWorkerMemoryBinding> bindings) {
          for(const auto& worker:workers) {
            auto binding=std::ranges::find_if(bindings,[&](const auto& candidate){
              return candidate.variant==HostPlanVariantId::first &&
                     candidate.worker_id==worker.worker_id;
            });
            if(binding==bindings.end()) return false;
            requirements.workers.push_back({family,worker.worker_id,worker.stable_numa_node_id,
              worker.stable_core_id,worker.stable_pu_id,worker.linux_cpu,binding->linux_node});
          }
          return true;
        };
        if(!append("bandwidth",plan.bandwidth_workers(),plan.bandwidth_memory_bindings()) ||
           !append("latency",plan.latency_workers(),plan.latency_memory_bindings()))
          return std::unexpected{fail("monitoring.host_qualification_plan.resolution_rejected",
                                      "worker_node_binding_mismatch")};
        for(const auto& selected_service:plan.service_cores())
          requirements.service.push_back({selected_service.role,selected_service.stable_core_id,
                                           selected_service.stable_pu_ids,
                                           selected_service.linux_cpus});
        if (stage)
            *stage = detail::HostFrozenCompositionStage::registration;
        auto registered = detail_HostQualificationPlanAccess::register_requirements_fallible(
            session, std::move(requirements), raw_failure);
        core::detail::ExpectedErrorCleanup registered_cleanup{registered};
        if (!registered)
            return std::unexpected{std::move(registered.error())};
        return MonitoringHostQualificationPlan(std::move(plan));
    }
};

auto resolve_monitoring_host_qualification_plan(const RunnerContextAcquisitionSession& session,
 const HostEnvironmentAuthority& environment) noexcept
 ->std::expected<MonitoringHostQualificationPlan,core::Error>{
 return detail_MonitoringHostQualificationPlanFactory::resolve(session,environment);
}

auto detail_HostQualificationPlanAccess::resolve_fallible(
    const RunnerContextAcquisitionSession& session, const HostEnvironmentAuthority& environment,
    std::optional<core::Error>* raw_failure, detail::HostFrozenCompositionStage* stage)
    -> std::expected<MonitoringHostQualificationPlan, core::Error> {
    return detail_MonitoringHostQualificationPlanFactory::resolve_fallible(session, environment,
                                                                           raw_failure, stage);
}

auto detail_HostQualificationPlanAccess::canonical_projection(
 const MonitoringHostQualificationPlan& plan)->std::expected<std::string,core::Error> {
    Json service = core::detail::json_object<Json>([&](auto& json_value_35) {
        core::detail::set_json(json_value_35, "active_roles", ([&]() {
                                   auto json_value_17 = Json::array();
                                   core::detail::JsonCleanup json_value_17_cleanup{json_value_17};
                                   core::detail::append_json(json_value_17, Json("control"));
                                   return Json(std::move(json_value_17));
                               }()));
    });
    core::detail::JsonCleanup service_cleanup{service};
    for (const auto& s : plan.service_cores())
        core::detail::set_json(service, s.role, service_json(s));
    Json document = core::detail::json_object<Json>([&](auto& json_value_36) {
        core::detail::set_json(
            json_value_36, "bandwidth",
            Json(family_json("sequential-read-bandwidth", "memory.bandwidth.read/v1",
                             plan.working_set_bytes(), plan.bandwidth_workers())));
        core::detail::set_json(json_value_36, "effective_fingerprint",
                               Json(plan.effective_fingerprint().to_string()));
        core::detail::set_json(json_value_36, "environment_digest",
                               Json(plan.environment_digest()));
        core::detail::set_json(
            json_value_36, "latency",
            Json(family_json("dependent-load-latency", "memory.latency.dependent-load/v1",
                             plan.working_set_bytes(), plan.latency_workers())));
        core::detail::set_json(
            json_value_36, "measurement", core::detail::json_object<Json>([&](auto& json_value_37) {
                core::detail::set_json(
                    json_value_37, "calibration",
                    core::detail::json_object<Json>([&](auto& json_value_38) {
                        core::detail::set_json(json_value_38, "enabled", Json(true));
                        core::detail::set_json(json_value_38, "strategy", Json("geometric-refine"));
                    }));
                core::detail::set_json(
                    json_value_37, "observation",
                    core::detail::json_object<Json>([&](auto& json_value_39) {
                        core::detail::set_json(
                            json_value_39, "maximum_duration",
                            Json(duration_json(plan.calibration_policy().maximum_duration())));
                        core::detail::set_json(json_value_39, "maximum_harness_overhead",
                                               Json(ratio_json(1, 100)));
                        core::detail::set_json(
                            json_value_39, "minimum_duration",
                            Json(duration_json(plan.calibration_policy().minimum_duration())));
                        core::detail::set_json(
                            json_value_39, "target_duration",
                            Json(duration_json(plan.calibration_policy().target_duration())));
                    }));
                core::detail::set_json(
                    json_value_37, "repetition",
                    core::detail::json_object<Json>([&](auto& json_value_40) {
                        core::detail::set_json(json_value_40, "maximum_blocks", Json(20));
                        core::detail::set_json(json_value_40, "minimum_blocks", Json(8));
                        core::detail::set_json(json_value_40,
                                               "minimum_valid_observations_per_variant", Json(16));
                    }));
                core::detail::set_json(
                    json_value_37, "stopping",
                    core::detail::json_object<Json>([&](auto& json_value_41) {
                        core::detail::set_json(json_value_41, "confidence",
                                               Json(ratio_json(19, 20)));
                        core::detail::set_json(json_value_41, "enabled", Json(false));
                        core::detail::set_json(json_value_41, "ratio_interval_relative_half_width",
                                               Json(ratio_json(1, 50)));
                    }));
                core::detail::set_json(
                    json_value_37, "warmup",
                    core::detail::json_object<Json>([&](auto& json_value_42) {
                        core::detail::set_json(json_value_42, "maximum", Json(10));
                        core::detail::set_json(json_value_42, "minimum", Json(3));
                        core::detail::set_json(json_value_42, "stability_threshold",
                                               Json(ratio_json(3, 100)));
                        core::detail::set_json(json_value_42, "stability_window", Json(3));
                    }));
            }));
        core::detail::set_json(
            json_value_36, "memory", core::detail::json_object<Json>([&](auto& json_value_43) {
                core::detail::set_json(json_value_43, "nodes",
                                       core::detail::json_object<Json>([&](auto& json_value_44) {
                                           core::detail::set_json(
                                               json_value_44, "local-to-each-worker", Json(true));
                                       }));
                core::detail::set_json(json_value_43, "policy", Json("bind"));
            }));
        core::detail::set_json(
            json_value_36, "monitoring", core::detail::json_object<Json>([&](auto& json_value_45) {
                core::detail::set_json(
                    json_value_45, "active_roles", ([&]() {
                        auto json_value_18 = Json::array();
                        core::detail::JsonCleanup json_value_18_cleanup{json_value_18};
                        core::detail::append_json(json_value_18, Json("control"));
                        return Json(std::move(json_value_18));
                    }()));
                core::detail::set_json(json_value_45, "configuration", Json("C0"));
                core::detail::set_json(json_value_45, "optional", Json::array());
                core::detail::set_json(json_value_45, "required", Json::array());
                core::detail::set_json(
                    json_value_45, "sampling",
                    core::detail::json_object<Json>([&](auto& json_value_46) {
                        core::detail::set_json(json_value_46, "after_observation", Json(false));
                        core::detail::set_json(json_value_46, "before_observation", Json(false));
                        core::detail::set_json(
                            json_value_46, "during_observation",
                            core::detail::json_object<Json>([&](auto& json_value_47) {
                                core::detail::set_json(json_value_47, "enabled", Json(false));
                            }));
                    }));
                core::detail::set_json(json_value_45, "unavailable_optional_source", Json("fail"));
            }));
        core::detail::set_json(json_value_36, "physical_fingerprint",
                               Json(plan.physical_fingerprint().to_string()));
        core::detail::set_json(json_value_36, "policy_identity", Json(plan.policy_identity_));
        core::detail::set_json(json_value_36, "privacy_validation", Json("passed"));
        core::detail::set_json(json_value_36, "schema",
                               Json("epycbench.monitoring-host-qualification-plan/v2"));
        core::detail::set_json(json_value_36, "service_reservation", std::move(service));
        core::detail::set_json(json_value_36, "template_sha256",
                               Json(plan.template_sha256().to_hex()));
    });
    core::detail::JsonCleanup document_cleanup{document};
    auto canonical = core::canonicalize_json(document);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical)
        return std::unexpected{std::move(canonical.error())};
    canonical->push_back('\n');
    return canonical;
}
auto detail_HostQualificationPlanAccess::policy_identity(
    const MonitoringHostQualificationPlan& plan) noexcept -> const std::string& {
    return plan.policy_identity_;
}
auto detail_HostQualificationPlanAccess::plan_binding(
 const MonitoringHostQualificationPlan& plan) noexcept
 -> const std::shared_ptr<const detail_HostSessionBinding>& { return plan.binding_; }
} // namespace epycbench::monitoring::qualification
