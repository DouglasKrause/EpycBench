#include "monitoring/host_frozen_evidence.hpp"

#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/runtime/finalized_evidence_error.hpp"
#include "runtime/finalized_evidence_wire_component.hpp"
#include "runtime/finalized_evidence_wire_memory_projection.hpp"
#include "runtime/finalized_evidence_wire_monitoring_projection.hpp"
#include "runtime/worker_team_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::json;
using Failure = HostFrozenEvidenceFailure;
namespace wire = runtime::wire_v1::detail;
using namespace runtime;
using wire::projection_decimal;
using wire::projection_binary64;
using core::detail::JsonCleanup;
using core::detail::set_json;
using core::detail::append_json;
template<class F> auto object(F&& fill) -> Json {
    return core::detail::json_object<Json>(std::forward<F>(fill));
}
template<class E, std::size_t N> auto name(E value, const std::array<std::string_view,N>& names) -> std::string_view {
    return names.at(static_cast<std::size_t>(value));
}
auto phase(HostFrozenPhase value) noexcept -> std::string_view {
    return value == HostFrozenPhase::q5 ? "Q5" : "Q6";
}
auto domain(HostFrozenDomain value) noexcept -> std::string_view {
    return value == HostFrozenDomain::source ? "source" : "synthetic";
}
auto reason(HostFrozenCollectionReason value) -> std::string_view {
    constexpr std::array<std::string_view,20> names{"invalid_input","unavailable","allocation","contained_exception",
        "admission","capacity","headroom","clock","deadline","cancelled","activation","observation",
        "retirement","metadata","invalid_observation","statistics","outstanding","association","abandoned","output"};
    return name(value,names);
}
auto canonical(const Json& value) -> std::string {
    // Sorted nlohmann object keys and ASCII escaping match Python for this closed
    // integer/string/bool/null grammar. Python additionally escapes raw DEL.
    const auto body = value.dump(-1, ' ', true);
    std::string bytes;
    bytes.reserve(body.size() + 1U);
    for (const auto byte : body) {
        if (static_cast<unsigned char>(byte) == 127U) bytes.append("\\u007f");
        else bytes.push_back(byte);
    }
    bytes.push_back('\n'); return bytes;
}
auto envelope(std::string_view schema, Json& payload) -> std::string {
    const auto bytes = canonical(payload);
    const auto sha = core::sha256(bytes);
    auto value = object([&](auto& parent) {
        set_json(parent,"schema",Json(schema));
        auto child = core::detail::clone_json(payload); JsonCleanup cleanup{child};
        set_json(parent,"payload",std::move(child));
        set_json(parent,"sha256",Json(sha.to_hex()));
    });
    JsonCleanup cleanup{value}; return canonical(value);
}
auto authority_json(const HostProductionAuthorityView& view) -> Json {
    const auto stock = wire::count_component_wire_shape(view.canonical_json,
                                                       view.canonical_json.size());
    if (!stock) throw std::invalid_argument{"authority shape"};
    auto parsed = wire::parse_wire_json(view.canonical_json,stock->depth);
    if (!parsed) throw std::invalid_argument{"authority JSON"};
    return core::detail::clone_json(parsed->value);
}
void assign_authority(Json& destination, std::string& common,
                      const HostProductionAuthorityView& view,
                      void (*after_clone)() noexcept = nullptr) {
    auto child=authority_json(view); JsonCleanup guard{child};
    if(after_clone) after_clone();
    common=view.canonical_common;
    destination=std::move(child);
}
auto interval(const measurement::MeasurementInterval& value) -> Json {
    return object([&](auto& parent) {
        set_json(parent,"start",projection_decimal(value.start().nanoseconds()));
        set_json(parent,"end",projection_decimal(value.end().nanoseconds()));
        set_json(parent,"elapsed_nanoseconds",projection_decimal(value.elapsed().value()));
    });
}
auto ratio(const measurement::MaximumHarnessOverhead& value) -> Json {
    return object([&](auto& parent) {
        set_json(parent,"numerator",projection_decimal(value.numerator()));
        set_json(parent,"denominator",projection_decimal(value.denominator()));
    });
}
auto validity(const measurement::ObservationValidityEvidence& value) -> Json {
    auto checks = Json::array(); JsonCleanup checks_cleanup{checks};
    constexpr std::array<std::string_view,5> outcomes{"pass","fail","warning","unavailable","not_applicable"};
    for (const auto& check : value.checks()) append_json(checks,object([&](auto& parent) {
        set_json(parent,"id",projection_decimal(static_cast<unsigned>(check.id())));
        set_json(parent,"outcome",Json(name(check.outcome(),outcomes)));
    }));
    const auto timer = value.timer_overhead();
    return object([&](auto& parent) {
        set_json(parent,"observation_id",projection_decimal(value.observation_id().value()));
        set_json(parent,"disposition",Json(value.disposition()==measurement::ObservationValidityDisposition::valid ? "valid":"invalid"));
        set_json(parent,"checks",std::move(checks));
        set_json(parent,"policy",object([&](auto& policy) { set_json(policy,"maximum_harness_overhead",ratio(value.policy().maximum_harness_overhead())); }));
        set_json(parent,"timer_overhead",object([&](auto& output) {
            set_json(output,"estimate",projection_decimal(timer.estimate().duration().value()));
            set_json(output,"threshold",ratio(timer.threshold()));
            auto fraction = Json(nullptr); JsonCleanup cleanup{fraction};
            if (timer.fraction()) fraction = object([&](auto& f) {
                set_json(f,"numerator_nanoseconds",projection_decimal(timer.fraction()->numerator_nanoseconds()));
                set_json(f,"denominator_nanoseconds",projection_decimal(timer.fraction()->denominator_nanoseconds()));
            });
            set_json(output,"fraction",std::move(fraction));
        }));
    });
}
auto measurement_json(const measurement::MeasurementObservation& observation) -> Json {
    return std::visit([](const auto& value) -> Json {
        constexpr bool bw = std::same_as<std::decay_t<decltype(value)>,measurement::SequentialReadBandwidthObservation>;
        auto workers = Json::array(); JsonCleanup workers_cleanup{workers};
        for (const auto& worker : value.workers()) append_json(workers,object([&](auto& parent) {
            set_json(parent,"worker_id",projection_decimal(worker.worker_id().value()));
            set_json(parent,"interval",interval(worker.interval()));
            if constexpr (bw) set_json(parent,"work",object([&](auto& w) {
                set_json(w,"logical_bytes_read",projection_decimal(worker.work().logical_bytes_read().value()));
            }));
            else {
                set_json(parent,"schedule",object([&](auto& w) {
                    set_json(w,"start_index",projection_decimal(worker.schedule().start_index()));
                    set_json(w,"step_count",projection_decimal(worker.schedule().step_count().value()));
                }));
                set_json(parent,"completed_work",projection_decimal(worker.completed_work().value()));
            }
            set_json(parent,"verification",object([&](auto& verification) {
                set_json(verification,"expected",projection_decimal(worker.verification().expected().value()));
                set_json(verification,"actual",projection_decimal(worker.verification().actual().value()));
                set_json(verification,"matched",Json(worker.verification().matched()));
            }));
        }));
        return object([&](auto& parent) {
            set_json(parent,"kind",Json(bw ? "bandwidth":"latency"));
            set_json(parent,"observation_id",projection_decimal(value.observation_id().value()));
            set_json(parent,"experiment_id",projection_decimal(value.experiment_id().value()));
            set_json(parent,"block_id",projection_decimal(value.block_id().value()));
            set_json(parent,"implementation_id",Json(value.implementation_id()==kernel::KernelImplementationId::scalar_u64_sequential_v1 ? "scalar_u64_sequential_v1":"scalar_u64_random_cycle_v1"));
            set_json(parent,"configuration",object([&](auto& config) {
                if constexpr (bw) set_json(config,"working_set_per_worker",projection_decimal(value.configuration().working_set_per_worker().value()));
                else {
                    set_json(config,"working_set",projection_decimal(value.configuration().working_set().value()));
                    const auto& seed = value.configuration().pointer_cycle_seed();
                    set_json(config,"pointer_cycle_seed",object([&](auto& s) {
                        set_json(s,"mode",Json(seed.mode()==kernel::PointerCycleSeedMode::explicit_value ? "explicit_value":"derive_per_experiment"));
                        set_json(s,"value",seed.value() ? Json(projection_decimal(*seed.value())):Json(nullptr));
                    }));
                }
            }));
            if constexpr (bw) set_json(parent,"pass_count",projection_decimal(value.pass_count().value()));
            set_json(parent,"workers",std::move(workers));
            set_json(parent,"enclosing_interval",interval(value.enclosing_interval()));
            set_json(parent,"aggregate_work",projection_decimal(value.aggregate_work().value()));
        });
    },observation);
}
auto placements(const runtime::detail::FrozenObservation& value) -> Json {
    constexpr std::array<std::string_view,4> states{"qualified","not_qualified","unavailable","not_applicable"};
    auto result=Json::array(); JsonCleanup cleanup{result};
    for(const auto& entry:value.placements) append_json(result,object([&](auto& parent) {
        set_json(parent,"worker_id",projection_decimal(entry.worker_id().value()));
        set_json(parent,"state",Json(name(entry.state(),states)));
    }));
    return Json(std::move(result));
}
auto migrations(const runtime::detail::FrozenObservation& value) -> Json {
    constexpr std::array<std::string_view,4> states{"not_migrated","migrated","unavailable","not_applicable"};
    auto result=Json::array(); JsonCleanup cleanup{result};
    for(const auto& entry:value.migrations) append_json(result,object([&](auto& parent) {
        set_json(parent,"worker_id",projection_decimal(entry.worker_id().value()));
        set_json(parent,"state",Json(name(entry.state(),states)));
    }));
    return Json(std::move(result));
}
#define EPYCBENCH_ENUM_TOKEN_CASE(Enum, name)                                                      \
    case Enum::name:                                                                               \
        return #name

[[nodiscard]] constexpr auto token(PairwiseLifecyclePhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, active);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, finalization);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, cleanup);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, create);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, service_exception);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, begin_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, abort_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, complete_capture);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, stop_and_join);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, prepare);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, page_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, context_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, temperature_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, frequency_read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_mapping);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_bound);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, retained_region_release_initialized);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase,
                                  retained_region_release_placement_observed);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, execution_input_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, execution_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, start_state_schedule);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, terminal_verification_preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_launch);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_startup);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_callback);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_command);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_execute);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_generation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_projection);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_result_publication);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseLifecyclePhase, worker_join);
    case PairwiseLifecyclePhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseRole value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRole, first);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRole, second);
    case PairwiseRole::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelFamily value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelFamily, bandwidth);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelFamily, latency);
    case PairwiseKernelFamily::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelImplementation value) noexcept
    -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelImplementation, scalar_u64_sequential_v1);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelImplementation, scalar_u64_random_cycle_v1);
    case PairwiseKernelImplementation::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseKernelPhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, none);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, pass_count_preparation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, work_accounting);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, execution_input_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, execution_validation);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, start_state_schedule);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseKernelPhase, terminal_verification_preparation);
    case PairwiseKernelPhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseVerificationPhase value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseVerificationPhase, before);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseVerificationPhase, after);
    case PairwiseVerificationPhase::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMonitoringChannel value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, page_faults);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, context_switches);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, temperature);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringChannel, frequency);
    case PairwiseMonitoringChannel::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMissingOperation value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, now);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, bind_service_cpu);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, support);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, read);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMissingOperation, clock_now);
    case PairwiseMissingOperation::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseProtectionKind value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseProtectionKind, read_only);
    case PairwiseProtectionKind::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseRangeKind value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseRangeKind, full_mapping);
    case PairwiseRangeKind::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseQualificationState value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, qualified);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, skipped);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseQualificationState, not_qualified);
    case PairwiseQualificationState::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMappingProtection value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, unspecified);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, read_write);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingProtection, read_only);
    case PairwiseMappingProtection::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwiseMappingAdvice value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, none);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, base_pages);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, huge_pages);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMappingAdvice, no_huge_pages);
    case PairwiseMappingAdvice::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(
    PairwiseMonitoringCapacityAcquisitionLeg value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringCapacityAcquisitionLeg,
                                  first_controller);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwiseMonitoringCapacityAcquisitionLeg,
                                  second_runner);
    case PairwiseMonitoringCapacityAcquisitionLeg::count:
        break;
    }
    std::terminate();
}

[[nodiscard]] constexpr auto token(PairwisePerfLossType value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfLossType, lost);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfLossType, lost_samples);
    case PairwisePerfLossType::count: break;
    }
    std::terminate();
}
[[nodiscard]] constexpr auto token(PairwisePerfDrainRole value) noexcept -> std::string_view {
    switch (value) {
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfDrainRole, unscoped);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfDrainRole, initial);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfDrainRole, scheduled);
        EPYCBENCH_ENUM_TOKEN_CASE(PairwisePerfDrainRole, final);
    case PairwisePerfDrainRole::count: break;
    }
    std::terminate();
}

#undef EPYCBENCH_ENUM_TOKEN_CASE

[[nodiscard]] constexpr auto os_domain_token(PairwiseOsDomain domain) noexcept -> std::string_view {
    switch (domain) {
#define OS_DOMAIN(name, spelling)                                                                  \
    case PairwiseOsDomain::name:                                                                   \
        return spelling;
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef OS_DOMAIN
    case PairwiseOsDomain::count:
        break;
    }
    std::terminate();
}



auto code_token(PairwiseFailureCode code) noexcept -> std::string_view {
    switch(code) {
#define OPERATION(...)
#define REASON(...)
#define CONTEXT_CONSTRAINT(...)
#define TUPLE(...)
#define FAILURE(code, subsystem, spelling, renderer) case PairwiseFailureCode::code: return #code;
#include "epycbench/runtime/pairwise_finalized_error_registry.def"
#undef FAILURE
#undef TUPLE
#undef CONTEXT_CONSTRAINT
#undef REASON
#undef OPERATION
    case PairwiseFailureCode::count: break;
    }
    std::terminate();
}
template<class Context> auto context_json(const Context& context) -> Json;
auto nested_json(const PairwiseNestedFailureValue& value) -> Json {
    return object([&](auto& p) {
        set_json(p,"code",Json(code_token(value.code)));
        set_json(p,"operation",Json(pairwise_operation_token(value.operation)));
        set_json(p,"reason",Json(pairwise_reason_token(value.reason)));
        set_json(p,"context",context_json(value.context));
        set_json(p,"posix_errno",value.posix_errno ? Json(projection_decimal(*value.posix_errno)):Json(nullptr));
    });
}
template<class Context> auto context_json(const Context& context) -> Json {
    return std::visit([&](const auto& value) {
        using T=std::decay_t<decltype(value)>;
        return object([&](auto& p) {
            set_json(p,"kind",Json(pairwise_context_token(context_kind(context))));
            if constexpr(std::same_as<T,LifecyclePhaseContext>) set_json(p,"phase",Json(token(value.phase)));
            else if constexpr(std::same_as<T,ExpectedObservedCountContext> || std::same_as<T,ExpectedObservedOrdinalContext>) {
                set_json(p,"expected",projection_decimal(value.expected.value)); set_json(p,"observed",projection_decimal(value.observed.value));
            } else if constexpr(std::same_as<T,WorkerAssociationContext>) {
                set_json(p,"role",Json(token(value.role))); set_json(p,"worker_ordinal",projection_decimal(value.worker_ordinal.value));
            } else if constexpr(std::same_as<T,KernelIdentityContext>) {
                set_json(p,"family",Json(token(value.family))); set_json(p,"implementation",Json(token(value.implementation))); set_json(p,"kernel_phase",Json(token(value.phase)));
            } else if constexpr(std::same_as<T,VerificationPhaseContext>) set_json(p,"verification_phase",Json(token(value.phase)));
            else if constexpr(std::same_as<T,MonitoringChannelContext>) set_json(p,"channel",Json(token(value.channel)));
            else if constexpr(std::same_as<T,MissingOperationContext>) set_json(p,"missing_operation",Json(token(value.operation)));
            else if constexpr(std::same_as<T,MemoryProtectionStateContext>) {
                set_json(p,"protection",Json(token(value.protection))); set_json(p,"range",Json(token(value.range)));
                set_json(p,"qualification",Json(token(value.qualification))); set_json(p,"reason",Json(pairwise_reason_token(value.reason)));
            } else if constexpr(std::same_as<T,RequestedProtectionAdviceContext>) {
                set_json(p,"protection",Json(token(value.protection))); set_json(p,"advice",Json(token(value.advice)));
            } else if constexpr(std::same_as<T,PairwiseMonitoringCapacityAcquisitionContext>) {
                set_json(p,"leg",Json(token(value.leg))); set_json(p,"uncertain",Json(value.uncertain));
            } else if constexpr(std::same_as<T,PairwisePerfLossContext>) {
                const auto optional_count=[](const auto& count) {
                    return count ? Json(projection_decimal(count->value)):Json(nullptr);
                };
                set_json(p,"record",object([&](auto& record) {
                    set_json(record,"loss_type",Json(token(value.record.loss_type)));
                    set_json(record,"header_size",projection_decimal(value.record.header_size.value));
                    set_json(record,"lost_count",optional_count(value.record.lost_count));
                }));
                set_json(p,"ring",object([&](auto& ring) {
                    set_json(ring,"captured_head",projection_decimal(value.ring.captured_head.value));
                    set_json(ring,"captured_tail",projection_decimal(value.ring.captured_tail.value));
                    set_json(ring,"ring_bytes",projection_decimal(value.ring.ring_bytes.value));
                }));
                set_json(p,"drain",object([&](auto& drain) {
                    set_json(drain,"role",Json(token(value.drain.role)));
                    set_json(drain,"ordinal",optional_count(value.drain.ordinal));
                    set_json(drain,"started_ns",optional_count(value.drain.started_ns));
                }));
                set_json(p,"last_successful_drain_completed_ns",
                    optional_count(value.last_successful_drain_completed_ns));
            } else if constexpr(std::same_as<T,NestedFailureContext>) {
                set_json(p,"failure",nested_json(value.failure));
                set_json(p,"nested_phase",value.phase ? Json(token(*value.phase)):Json(nullptr));
                auto nested=Json(nullptr); JsonCleanup guard{nested};
                if(value.nested_platform_failure) nested=nested_json(*value.nested_platform_failure);
                set_json(p,"nested_platform_failure",std::move(nested));
            }
        });
    },context);
}
auto error_json(const PairwiseEvidenceFailure& failure) -> Json {
    const auto value=FinalizedEvidenceError::from_failure(failure);
    const auto& profile=pairwise_finalized_error_profiles()[static_cast<std::size_t>(value.code())];
    constexpr std::array<std::string_view,8> subsystems{"platform","memory","monitoring","kernel","measurement","statistics","worker","runtime"};
    constexpr std::array<std::string_view,15> categories{"core","build","platform_linux","topology","scenario","runtime","memory","kernel","measurement","monitoring","statistics","results","reporting","validation","usage"};
    constexpr std::array<std::string_view,4> severities{"information","warning","error","fatal"};
    constexpr std::array<std::string_view,5> continuations{"continue_operation","skip_item","invalidate_observation","abort_experiment","abort_command"};
    return object([&](auto& p) {
        set_json(p,"subsystem",Json(name(value.subsystem(),subsystems)));
        set_json(p,"category",Json(name(value.category(),categories)));
        set_json(p,"code",Json(code_token(value.code()))); set_json(p,"stable_code",Json(value.stable_code()));
        set_json(p,"operation",Json(value.operation_token())); set_json(p,"reason",Json(value.reason_token()));
        set_json(p,"severity",Json(name(value.severity(),severities)));
        set_json(p,"continuation",Json(name(value.continuation(),continuations)));
        set_json(p,"renderer",Json(profile.renderer_token)); set_json(p,"context",context_json(value.context()));
        auto native=Json(nullptr); JsonCleanup guard{native};
        if(value.os_error()) native=object([&](auto& n) {
            set_json(n,"domain",Json(os_domain_token(value.os_error()->domain()))); set_json(n,"value",projection_decimal(value.os_error()->value()));
        });
        set_json(p,"os_error",std::move(native));
    });
}
auto cleanup_json(const memory::RegionCleanupEvidence& value) -> Json {
    return object([&](auto& p) {
        set_json(p,"release_attempted",Json(value.release_attempted()));
        set_json(p,"release_succeeded",Json(value.release_succeeded()));
        set_json(p,"range",Json("full_mapping")); set_json(p,"mapping_active_after",Json(value.mapping_active_after()));
    });
}
auto terminal_json(const runtime::detail::FrozenObservationTerminalFacts& value) -> Json {
    auto preparation=Json(nullptr); JsonCleanup pg{preparation};
    if(value.preparation) preparation=object([&](auto& p) {
        const auto& f=*value.preparation;
        set_json(p,"domain",Json(f.domain==runtime::detail::MonitoringPreparationFactDomain::source ? "source":"synthetic"));
        set_json(p,"workers",projection_decimal(f.workers)); set_json(p,"frequency_domains",projection_decimal(f.frequency_domains));
        auto channels=Json::array(); JsonCleanup cg{channels};
        constexpr std::array<std::string_view,3> supports{"not_requested","supported","unsupported"};
        constexpr std::array<std::string_view,3> effective{"active","unsupported_warning","disabled"};
        for(const auto& ch:f.channels) append_json(channels,object([&](auto& c) {
            set_json(c,"support",Json(name(ch.support,supports))); set_json(c,"effective",Json(name(ch.effective,effective)));
        }));
        set_json(p,"channels",std::move(channels));
        auto service=Json(nullptr); JsonCleanup sg{service};
        if(f.service) service=object([&](auto& c) {
            set_json(c,"assigned",projection_decimal(f.service->assigned.value()));
            set_json(c,"verified_singleton",projection_decimal(f.service->verified_singleton.value()));
            set_json(c,"observed",projection_decimal(f.service->observed.value()));
        });
        set_json(p,"service",std::move(service));
        auto workers=Json::array(); JsonCleanup wg{workers};
        for(const auto& w:f.worker_associations) append_json(workers,object([&](auto& c) {
            set_json(c,"worker_id",projection_decimal(w.worker_id.value())); set_json(c,"cpu",projection_decimal(w.cpu.value()));
        }));
        set_json(p,"worker_associations",std::move(workers));
    });
    auto workers=Json::array(); JsonCleanup wg{workers};
    for(const auto& w:value.workers) append_json(workers,object([&](auto& p) {
        set_json(p,"worker_id",projection_decimal(w.worker_id.value())); set_json(p,"timed_entered",Json(w.timed_entered));
        auto diagnostic=Json(nullptr); JsonCleanup dg{diagnostic};
        if(w.diagnostic) diagnostic=wire::project_region_finalization_diagnostic(*w.diagnostic);
        set_json(p,"diagnostic",std::move(diagnostic));
        auto partial=Json(nullptr); JsonCleanup cg{partial};
        if(w.partial_cleanup) partial=cleanup_json(*w.partial_cleanup);
        set_json(p,"partial_cleanup",std::move(partial)); set_json(p,"ownership_remains",Json(w.ownership_remains));
    }));
    auto failures=Json::array(); JsonCleanup fg{failures};
    for(const auto& f:value.failures) append_json(failures,error_json(f));
    auto mf=Json::array(); JsonCleanup mg{mf};
    for(std::size_t i{};i<value.monitoring_force_count;++i) append_json(mf,object([&](auto& p) {
        const auto& f=value.monitoring_force[i]; set_json(p,"attempt",projection_decimal(f.attempt));
        set_json(p,"outcome",projection_decimal(f.outcome)); set_json(p,"ownership_remains",Json(f.ownership_remains));
    }));
    auto wf=Json::array(); JsonCleanup wfg{wf};
    constexpr std::array<std::string_view,4> kinds{"join_failed","contained_exception","contained_unknown_failure","capacity_exhausted"};
    for(const auto& f:value.worker_force) append_json(wf,object([&](auto& p) {
        set_json(p,"kind",Json(name(f.kind,kinds))); set_json(p,"sweep",projection_decimal(f.sweep)); set_json(p,"worker_index",projection_decimal(f.worker_index));
    }));
    constexpr std::array<std::string_view,4> metadata{"not_requested","no_completed_capture","matched","mismatch"};
    return object([&](auto& p) {
        set_json(p,"preparation",std::move(preparation)); set_json(p,"workers",std::move(workers));
        set_json(p,"failures",std::move(failures)); set_json(p,"metadata",Json(name(value.metadata,metadata)));
        set_json(p,"monitoring_force",std::move(mf)); set_json(p,"worker_force",std::move(wf));
    });
}
auto confirmed(const runtime::detail::FrozenObservationTerminalFacts& value) noexcept -> bool {
    if(value.monitoring_force_count>value.monitoring_force.size()) return false;
    for(const auto& f:value.workers) if(f.ownership_remains) return false;
    // Force entries preserve historical attempts. A detached actual cell has
    // already proved current native destruction; earlier attempt flags are facts.
    return true;
}
auto observation_json(const runtime::detail::FrozenObservation& value) -> Json {
    const auto stock=wire::count_monitoring_projection(value.monitoring);
    if(stock.invalid || stock.overflow) throw std::length_error{"monitoring stock"};
    return object([&](auto& p) {
        set_json(p,"measurement",measurement_json(value.observation)); set_json(p,"validity",validity(value.validity));
        set_json(p,"placements",placements(value)); set_json(p,"migrations",migrations(value));
        set_json(p,"monitoring",wire::project_monitoring(value.monitoring));
    });
}
auto metric(double value, bool bandwidth) -> Json {
    if(!std::isfinite(value) || value<=0.0) throw std::invalid_argument{"metric"};
    return object([&](auto& p) {
        set_json(p,"unit",Json(bandwidth ? "bytes_per_second":"seconds_per_completed_load"));
        set_json(p,"binary64",projection_binary64(value));
    });
}
auto primary(const measurement::MeasurementObservation& value) -> Json {
    return std::visit([](const auto& observation) {
        if(!observation.aggregate_work().value() || !observation.enclosing_interval().elapsed().value()) throw std::invalid_argument{"zero metric"};
        if constexpr(std::same_as<std::decay_t<decltype(observation)>,measurement::SequentialReadBandwidthObservation>) {
            const auto output=statistics::Bandwidth::create(observation.aggregate_work(),observation.enclosing_interval().elapsed());
            if(!output) throw std::invalid_argument{"bandwidth"};
            return metric(output->bytes_per_second(),true);
        } else {
            const auto output=statistics::DependentLoadLatency::create(observation.enclosing_interval().elapsed(),observation.aggregate_work());
            if(!output) throw std::invalid_argument{"latency"};
            return metric(output->seconds_per_completed_load(),false);
        }
    },value);
}
auto representatives(const HostFrozenBlockRepresentatives* value) -> Json {
    if(!value) return nullptr;
    return std::visit([](const auto& reps) {
        constexpr bool bw=std::same_as<std::decay_t<decltype(reps)>,statistics::BlockRepresentatives<statistics::Bandwidth>>;
        return object([&](auto& p) {
            if constexpr(bw) { set_json(p,"disabled",metric(reps.first().bytes_per_second(),true)); set_json(p,"enabled",metric(reps.second().bytes_per_second(),true)); }
            else { set_json(p,"disabled",metric(reps.first().seconds_per_completed_load(),false)); set_json(p,"enabled",metric(reps.second().seconds_per_completed_load(),false)); }
        });
    },*value);
}
} // namespace
struct HostFrozenCellEvidence::State { std::string bytes, common; };
auto HostFrozenCellEvidence::authority_component(const HostProductionAuthority& owner,
    void (*after_clone)() noexcept) noexcept -> std::expected<std::string,Failure> {
    const auto view=owner.view();
    if(!view) return std::unexpected{Failure::unavailable};
    try {
        std::string common;
        auto child=Json(nullptr); JsonCleanup guard{child};
        assign_authority(child,common,*view,after_clone);
        auto parent=object([&](auto& value) { set_json(value,"authority",std::move(child)); });
        JsonCleanup parent_guard{parent};
        return canonical(parent);
    } catch(const std::bad_alloc&) { return std::unexpected{Failure::allocation}; }
      catch(const std::length_error&) { return std::unexpected{Failure::capacity}; }
      catch(...) { return std::unexpected{Failure::contained_exception}; }
}
HostFrozenCellEvidence::HostFrozenCellEvidence(std::unique_ptr<State> value) noexcept:state_{std::move(value)} {}
HostFrozenCellEvidence::HostFrozenCellEvidence(HostFrozenCellEvidence&&) noexcept=default;
HostFrozenCellEvidence::~HostFrozenCellEvidence() noexcept=default;
auto HostFrozenCellEvidence::view() const noexcept -> std::optional<HostFrozenEvidenceView> {
    if(!state_) return std::nullopt;
    return HostFrozenEvidenceView{state_->bytes,state_->common};
}
auto HostFrozenCellEvidence::create(const HostFrozenCell& cell, const MonitoringQ1ValidatedPublicValues& values,
    const MonitoringRunningBuildAuthority& build, std::size_t allowance) noexcept -> std::expected<HostFrozenCellEvidence,Failure> {
    return project(cell,&values,&build,false,allowance);
}
auto HostFrozenCellEvidence::project(const HostFrozenCell& cell,const MonitoringQ1ValidatedPublicValues* values,
    const MonitoringRunningBuildAuthority* build,bool synthetic,std::size_t allowance) noexcept -> std::expected<HostFrozenCellEvidence,Failure> {
    const auto id=cell.identity(); const auto* observed=cell.observation(); const auto* terminal=cell.terminal_facts();
    if(!id || !observed || !terminal || !cell.placement() || !cell.timer_estimate()) return std::unexpected{Failure::unavailable};
    if((cell.domain()==HostFrozenDomain::synthetic)!=synthetic || (synthetic && cell.admission())) return std::unexpected{Failure::domain};
    if(!confirmed(*terminal)) return std::unexpected{Failure::uncertain_cleanup};
    if(!synthetic && terminal->preparation && terminal->preparation->domain!=runtime::detail::MonitoringPreparationFactDomain::source) return std::unexpected{Failure::association};
    if(allowance>hard_limit) return std::unexpected{Failure::capacity};
    if(observed->observation_id.value()!=id->ordinal+1U || observed->experiment_id.value()!=id->comparison+1U || cell.empty_trials().size()!=5U) return std::unexpected{Failure::association};
    try {
        auto state=std::make_unique<State>();
        auto authority=Json(nullptr); JsonCleanup ag{authority};
        if(!synthetic) {
            if(!cell.admission() || !values || !build || !terminal->preparation) return std::unexpected{Failure::association};
            auto owner=HostProductionAuthority::create(*cell.admission(),*values,*build);
            if(!owner || !owner->view()) return std::unexpected{Failure::association};
            assign_authority(authority,state->common,*owner->view());
        }
        auto timer=object([&](auto& p) {
            auto trials=Json::array(); JsonCleanup tg{trials};
            for(const auto& trial:cell.empty_trials()) append_json(trials,interval(trial.interval()));
            set_json(p,"trials",std::move(trials)); set_json(p,"estimate",projection_decimal(cell.timer_estimate()->duration().value()));
        }); JsonCleanup tg{timer};
        auto obs=observation_json(*observed); JsonCleanup og{obs};
        constexpr std::array configs{"C0","C1","C2","C3","C4"};
        auto payload=object([&](auto& p) {
            set_json(p,"domain",Json(domain(cell.domain())));
            set_json(p,"identity",object([&](auto& i) {
                set_json(i,"phase",Json(phase(id->phase))); set_json(i,"family",Json(id->family==HostQ0bFamily::bandwidth ? "bandwidth":"latency"));
                set_json(i,"configuration",Json(configs.at(static_cast<std::size_t>(id->configuration))));
                set_json(i,"ordinal",projection_decimal(id->ordinal)); set_json(i,"comparison",projection_decimal(id->comparison));
                set_json(i,"block",projection_decimal(id->block)); set_json(i,"slot",projection_decimal(id->slot));
                set_json(i,"treatment",Json(id->treatment==measurement::ComparisonVariantSlot::first ? "disabled":"enabled"));
            }));
            set_json(p,"authority",std::move(authority)); set_json(p,"timer",std::move(timer));
            for(const auto& key:{"measurement","validity","placements","migrations","monitoring"}) {
                auto child=core::detail::clone_json(obs.at(key)); JsonCleanup cg{child}; set_json(p,key,std::move(child));
            }
            set_json(p,"terminal",terminal_json(*terminal));
            set_json(p,"primary",observed->validity.disposition()==measurement::ObservationValidityDisposition::invalid
                ? Json(nullptr):primary(observed->observation));
            set_json(p,"representatives",representatives(cell.representatives()));
        }); JsonCleanup pg{payload};
        state->bytes=envelope("epycbench.monitoring-host-frozen-cell/v1",payload);
        if(state->bytes.size()>allowance) return std::unexpected{Failure::capacity};
        return HostFrozenCellEvidence{std::move(state)};
    } catch(const std::bad_alloc&) { return std::unexpected{Failure::allocation}; }
      catch(const std::length_error&) { return std::unexpected{Failure::capacity}; }
      catch(...) { return std::unexpected{Failure::contained_exception}; }
}
struct HostFrozenFailureEvidence::State { std::string bytes,common; };
HostFrozenFailureEvidence::HostFrozenFailureEvidence(std::unique_ptr<State> value) noexcept:state_{std::move(value)} {}
HostFrozenFailureEvidence::HostFrozenFailureEvidence(HostFrozenFailureEvidence&&) noexcept=default;
HostFrozenFailureEvidence::~HostFrozenFailureEvidence() noexcept=default;
auto HostFrozenFailureEvidence::view() const noexcept -> std::optional<HostFrozenEvidenceView> {
    if(!state_) return std::nullopt;
    return HostFrozenEvidenceView{state_->bytes,state_->common};
}
auto HostFrozenFailureEvidence::create(const HostFrozenCollection& origin,const HostFrozenCollectionFailure& failure,
    const MonitoringQ1ValidatedPublicValues& values,const MonitoringRunningBuildAuthority& build,std::size_t allowance) noexcept -> std::expected<HostFrozenFailureEvidence,Failure> {
    return project(origin,failure,&values,&build,false,allowance);
}
auto HostFrozenFailureEvidence::project(const HostFrozenCollection& origin,const HostFrozenCollectionFailure& failure,
    const MonitoringQ1ValidatedPublicValues* values,const MonitoringRunningBuildAuthority* build,bool synthetic,std::size_t allowance) noexcept -> std::expected<HostFrozenFailureEvidence,Failure> {
    const auto snapshot=origin.snapshot();
    if(!origin.owns_failure(failure) || !snapshot || !snapshot->terminal || snapshot->completed) return std::unexpected{Failure::association};
    if((snapshot->domain==HostFrozenDomain::synthetic)!=synthetic) return std::unexpected{Failure::domain};
    if(failure.retained_retirement() || failure.charge_retained()) return std::unexpected{Failure::uncertain_cleanup};
    if(failure.capacity_failure() || failure.headroom_failure() || failure.lower_error() || failure.binding_failure() || failure.projection_failure()) return std::unexpected{Failure::unrepresented_failure};
    const auto* terminal=failure.terminal_facts();
    if(!terminal) return std::unexpected{Failure::unavailable};
    if(!confirmed(*terminal)) return std::unexpected{Failure::uncertain_cleanup};
    if(!synthetic && terminal->preparation && terminal->preparation->domain!=runtime::detail::MonitoringPreparationFactDomain::source) return std::unexpected{Failure::association};
    if(allowance>HostFrozenCellEvidence::hard_limit) return std::unexpected{Failure::capacity};
    try {
        auto state=std::make_unique<State>(); auto authority=Json(nullptr); JsonCleanup ag{authority};
        if(!synthetic && failure.admission()) {
            if(!values || !build) return std::unexpected{Failure::association};
            auto owner=HostProductionAuthority::create(*failure.admission(),*values,*build);
            if(!owner || !owner->view()) return std::unexpected{Failure::association};
            assign_authority(authority,state->common,*owner->view());
        }
        auto observed=Json(nullptr); JsonCleanup og{observed};
        if(failure.observation()) observed=observation_json(*failure.observation());
        auto payload=object([&](auto& p) {
            set_json(p,"domain",Json(domain(snapshot->domain))); set_json(p,"phase",Json(phase(snapshot->phase)));
            set_json(p,"reason",Json(reason(failure.reason()))); set_json(p,"authority",std::move(authority));
            set_json(p,"observation",std::move(observed)); set_json(p,"terminal",terminal_json(*terminal));
        }); JsonCleanup pg{payload};
        state->bytes=envelope("epycbench.monitoring-host-frozen-failure/v1",payload);
        if(state->bytes.size()>allowance) return std::unexpected{Failure::capacity};
        return HostFrozenFailureEvidence{std::move(state)};
    } catch(const std::bad_alloc&) { return std::unexpected{Failure::allocation}; }
      catch(const std::length_error&) { return std::unexpected{Failure::capacity}; }
      catch(...) { return std::unexpected{Failure::contained_exception}; }
}

struct HostFrozenTerminationEvidence::State { std::string bytes, common; };
HostFrozenTerminationEvidence::HostFrozenTerminationEvidence(std::unique_ptr<State> value) noexcept
    : state_{std::move(value)} {}
HostFrozenTerminationEvidence::HostFrozenTerminationEvidence(HostFrozenTerminationEvidence&&) noexcept = default;
HostFrozenTerminationEvidence::~HostFrozenTerminationEvidence() noexcept = default;
auto HostFrozenTerminationEvidence::view() const noexcept -> std::optional<HostFrozenEvidenceView> {
    if (!state_) return std::nullopt;
    return HostFrozenEvidenceView{state_->bytes, state_->common};
}
auto HostFrozenTerminationEvidence::create(const HostFrozenCollection& origin,
    const HostFrozenCollectionFailure* failure, const MonitoringQ1ValidatedPublicValues& values,
    const MonitoringRunningBuildAuthority& build, std::size_t allowance) noexcept
    -> std::expected<HostFrozenTerminationEvidence, Failure> {
    return project(origin, failure, &values, &build, false, allowance);
}
auto HostFrozenTerminationEvidence::project(const HostFrozenCollection& origin,
    const HostFrozenCollectionFailure* failure, const MonitoringQ1ValidatedPublicValues* values,
    const MonitoringRunningBuildAuthority* build, bool synthetic, std::size_t allowance) noexcept
    -> std::expected<HostFrozenTerminationEvidence, Failure> {
    const auto snapshot = origin.snapshot();
    if (!snapshot) return std::unexpected{Failure::unavailable};
    if ((snapshot->domain == HostFrozenDomain::synthetic) != synthetic)
        return std::unexpected{Failure::domain};
    if (allowance > HostFrozenCellEvidence::hard_limit) return std::unexpected{Failure::capacity};
    if (!snapshot->terminal || snapshot->outstanding || snapshot->consumed > snapshot->expected)
        return std::unexpected{Failure::association};
    if (snapshot->completed) {
        const bool q5 = snapshot->phase == HostFrozenPhase::q5;
        if (failure || snapshot->failure || snapshot->consumed != (q5 ? 8U : 384U) ||
            snapshot->expected != snapshot->consumed ||
            snapshot->representatives != (q5 ? 0U : 192U) || snapshot->pairs != (q5 ? 0U : 96U))
            return std::unexpected{Failure::association};
    } else {
        if (!snapshot->failure) return std::unexpected{Failure::association};
        if (failure) {
            if (!origin.owns_failure(*failure) || failure->reason() != *snapshot->failure)
                return std::unexpected{Failure::association};
        } else if (*snapshot->failure != HostFrozenCollectionReason::invalid_observation) {
            // Only a consumed invalid Cell has a confirmed terminal without an
            // owning Failure. Never omit a retained/uncertain Failure to finish.
            return std::unexpected{Failure::unrepresented_failure};
        }
    }
    try {
        auto state = std::make_unique<State>();
        auto confirmed_failure = Json(nullptr); JsonCleanup failure_cleanup{confirmed_failure};
        if (failure) {
            auto projected = HostFrozenFailureEvidence::project(origin, *failure, values, build,
                                                               synthetic, allowance);
            if (!projected) return std::unexpected{projected.error()};
            const auto view = projected->view();
            if (!view) return std::unexpected{Failure::unavailable};
            const auto stock = wire::count_component_wire_shape(view->canonical_json,
                                                                view->canonical_json.size());
            if (!stock) return std::unexpected{Failure::capacity};
            auto parsed = wire::parse_wire_json(view->canonical_json, stock->depth);
            if (!parsed) return std::unexpected{Failure::contained_exception};
            confirmed_failure = core::detail::clone_json(parsed->value);
            state->common = view->canonical_common;
        }
        auto payload = object([&](auto& value) {
            set_json(value, "kind", Json(snapshot->completed ? "completed" : "failed"));
            set_json(value, "consumed", projection_decimal(snapshot->consumed));
            if (!snapshot->completed) {
                set_json(value, "reason", Json(reason(*snapshot->failure)));
                set_json(value, "confirmed_failure", std::move(confirmed_failure));
            }
        }); JsonCleanup payload_cleanup{payload};
        state->bytes = canonical(payload);
        // The final newline and the full nested failure belong to this cap.
        if (state->bytes.size() > allowance) return std::unexpected{Failure::capacity};
        return HostFrozenTerminationEvidence{std::move(state)};
    } catch (const std::bad_alloc&) { return std::unexpected{Failure::allocation}; }
      catch (const std::length_error&) { return std::unexpected{Failure::capacity}; }
      catch (...) { return std::unexpected{Failure::contained_exception}; }
}
} // namespace epycbench::monitoring::qualification::detail
