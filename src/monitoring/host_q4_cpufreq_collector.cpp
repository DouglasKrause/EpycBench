#include "monitoring/host_q4_cpufreq_collector.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace epycbench::monitoring::qualification::detail { namespace {
using Reason = MonitoringQ4CapacityReason;
auto local_error(std::string code,std::string classification) -> core::Error {
    return {.category=core::ErrorCategory::monitoring,
            .code=core::StableErrorCode{std::move(code)},
            .message="Q4 CPU frequency collection timing failed",
            .operation="collect_monitoring_q4_cpu_frequency",
            .os_error=std::nullopt,
            .context={{"classification",std::move(classification)}},
            .severity=core::ErrorSeverity::error,
            .continuation=core::ContinuationPolicy::abort_command};
}
auto snapshots_equal(const MonitoringQ4CpuFreqSemanticSnapshot& a,
                     const MonitoringQ4CpuFreqSemanticSnapshot& b) noexcept -> bool {
    if (a.policy_count()!=b.policy_count() || !std::ranges::equal(a.cpus(),b.cpus()) ||
        a.domains().size()!=b.domains().size()) return false;
    for (std::size_t i{};i<a.domains().size();++i) {
        const auto& x=a.domains()[i]; const auto& y=b.domains()[i];
        if (!std::ranges::equal(x.cpus,y.cpus) || x.source_kind!=y.source_kind ||
            x.driver!=y.driver || x.governor!=y.governor || x.minimum_hz!=y.minimum_hz ||
            x.maximum_hz!=y.maximum_hz) return false;
    }
    return true;
}
auto production_now(void*) noexcept -> std::expected<std::uint64_t,core::Error> {
    auto value=platform_linux::raw_monotonic_now();
    if(!value) return std::unexpected{std::move(value.error())};
    return value->nanoseconds();
}
}

auto MonitoringQ4CpuFreqCollector::collect(const MonitoringQ4SourceReady& ready) noexcept
 -> std::expected<MonitoringQ4CpuFreqCollection,MonitoringQ4CpuFreqCollectionFailure> {
    return collect_with(ready,{nullptr,production_now,
        MonitoringQ4CpuFreqInventoryAuthority::native_operations(),nullptr,nullptr,false});
}

auto MonitoringQ4CpuFreqCollector::classify_cleanup_ledger_failure(
    MonitoringQ4CpuFreqCycleFailureKind kind) noexcept
    -> MonitoringQ4CpuFreqCollectionFailureKind {
    using Cycle = MonitoringQ4CpuFreqCycleFailureKind;
    using Collection = MonitoringQ4CpuFreqCollectionFailureKind;
    switch (kind) {
    case Cycle::allocation:
        return Collection::allocation;
    case Cycle::capacity:
        return Collection::capacity;
    case Cycle::contained:
        return Collection::contained;
    default:
        return Collection::contained;
    }
}

auto MonitoringQ4CpuFreqCollector::collect_with(const MonitoringQ4SourceReady& ready,Operations o) noexcept
 -> std::expected<MonitoringQ4CpuFreqCollection,MonitoringQ4CpuFreqCollectionFailure> {
    std::optional<MonitoringQ4CpuFreqCleanupLedger> ledger;
    std::array<std::optional<MonitoringQ4CpuFreqCycleAuthority>,3> authorities;
    std::vector<MonitoringQ4CpuFreqCycleResult> cycles;
    std::vector<MonitoringQ4CpuFreqInterval> discoveries;
    std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCyclePreparationFact>> preparations;
    std::vector<MonitoringQ4CpuFreqTimedFact<MonitoringQ4CpuFreqCycleReadFact>> raw,typed;
    std::optional<MonitoringQ4CpuFreqCollectionFailure> failure;
    std::optional<core::Error> reversal_error, deadline_error, missing_clock_error;
    auto fail=[&](MonitoringQ4CpuFreqCollectionFailureKind kind,std::uint64_t ordinal,
                  std::optional<core::Error> clock={},
                  std::optional<MonitoringQ4CpuFreqCycleFailure> cycle={}, Reason reason=Reason::none) {
        if(!failure) failure.emplace(MonitoringQ4CpuFreqCollectionFailure{kind,ordinal,
            std::move(clock),std::move(cycle),{}, {}, reason});
    };
    auto abort_all=[&] {
        for(auto& authority:authorities) if(authority) {
            auto cleanup=authority->abort();
            if(cleanup && failure && !failure->cleanup) failure->cleanup.emplace(std::move(*cleanup));
        }
    };
    auto reject=[&]() -> std::expected<MonitoringQ4CpuFreqCollection,MonitoringQ4CpuFreqCollectionFailure> {
        abort_all();
        if(ledger && failure) failure->ledger.emplace(std::move(*ledger));
        if(!failure) fail(MonitoringQ4CpuFreqCollectionFailureKind::contained,0);
        return std::unexpected{std::move(*failure)};
    };
    auto allocation_reason = Reason::collector_ledger_allocation;
    try {
        const auto admitted=ready.inputs().admitted_cpus().size();
        const auto rounds=ready.authority().governed().plan().agreement_rounds_per_cycle().value;
        if (o.force_capacity_failure) {
            fail(MonitoringQ4CpuFreqCollectionFailureKind::capacity, 0, {}, {}, Reason::collector_test_capacity); return reject();
        }
        if (rounds != 16) {
            fail(MonitoringQ4CpuFreqCollectionFailureKind::capacity, 0, {}, {}, Reason::collector_rounds); return reject();
        }
        if (admitted > std::numeric_limits<std::size_t>::max()/48U) {
            fail(MonitoringQ4CpuFreqCollectionFailureKind::capacity, 0, {}, {}, Reason::collector_read_count); return reject();
        }
        auto created_ledger=MonitoringQ4CpuFreqCleanupLedger::create(ready);
        if(!created_ledger) { fail(classify_cleanup_ledger_failure(created_ledger.error().kind),0, {}, {}, created_ledger.error().reason);
            return reject(); }
        ledger.emplace(std::move(*created_ledger));
        allocation_reason = Reason::collector_cycles_allocation;
        if(o.fault) o.fault(o.fault_context,1);
        cycles.reserve(3);
        allocation_reason = Reason::collector_discoveries_allocation;
        if(o.fault) o.fault(o.fault_context,2);
        discoveries.reserve(6);
        allocation_reason = Reason::collector_preparations_allocation;
        if(o.fault) o.fault(o.fault_context,3);
        preparations.reserve(3U*admitted);
        allocation_reason = Reason::collector_raw_allocation;
        if(o.fault) o.fault(o.fault_context,4);
        raw.reserve(48U*admitted);
        allocation_reason = Reason::collector_typed_allocation;
        if(o.fault) o.fault(o.fault_context,5);
        typed.reserve(48U*admitted);
        allocation_reason = Reason::collector_clock_errors_allocation;
        if(o.fault) o.fault(o.fault_context,6);
        reversal_error.emplace(local_error("monitoring.q4.clock_failed","clock_reversal"));
        deadline_error.emplace(local_error("monitoring.q4.deadline","deadline"));
        missing_clock_error.emplace(local_error("monitoring.q4.operations_invalid","missing_clock"));
        allocation_reason = Reason::collector_cycle_call_allocation;
        for(std::size_t index{};index<authorities.size();++index) {
            auto& authority=authorities[index];
            auto value=MonitoringQ4CpuFreqCycleAuthority::create_with(ready,*ledger,o.source);
            if(!value) { fail(MonitoringQ4CpuFreqCollectionFailureKind::cycle,0,{},std::move(value.error())); return reject(); }
            authority.emplace(std::move(*value));
            if(o.fault) o.fault(o.fault_context,static_cast<unsigned>(7U+index));
        }
        allocation_reason = Reason::collector_active_allocation;
        if(!o.now) { fail(MonitoringQ4CpuFreqCollectionFailureKind::operations,0,std::move(*missing_clock_error)); return reject(); }
        auto origin=o.now(o.clock_context);
        if(!origin) { fail(MonitoringQ4CpuFreqCollectionFailureKind::clock,0,std::move(origin.error())); return reject(); }
        std::uint64_t last=*origin;
        const auto seconds=ready.deadline().value;
        if(seconds>std::numeric_limits<std::uint64_t>::max()/1'000'000'000ULL) {
            fail(MonitoringQ4CpuFreqCollectionFailureKind::capacity,0, {}, {}, Reason::collector_deadline_ns); return reject();
        }
        const auto budget=seconds*1'000'000'000ULL;
        auto stamp=[&](std::uint64_t ordinal)->std::optional<std::uint64_t> {
            auto value=o.now(o.clock_context);
            if(!value) { fail(MonitoringQ4CpuFreqCollectionFailureKind::clock,ordinal,std::move(value.error())); return {}; }
            if(*value<last || *value<*origin) { fail(MonitoringQ4CpuFreqCollectionFailureKind::clock,ordinal,std::move(*reversal_error)); return {}; }
            last=*value;
            if(*value-*origin>budget) { fail(MonitoringQ4CpuFreqCollectionFailureKind::deadline,ordinal,std::move(*deadline_error)); return {}; }
            return *value;
        };
        auto bracket=[&](std::uint64_t ordinal,auto&& operation,MonitoringQ4CpuFreqInterval* output)->bool {
            auto before=stamp(ordinal); if(!before) return false;
            if(!operation()) return false;
            auto after=stamp(ordinal); if(!after) return false;
            if(output) *output={*before,*after};
            return true;
        };
        for(std::size_t ci{};ci<3;++ci) {
            const auto ordinal=ci+1U; auto& authority=*authorities[ci];
            MonitoringQ4CpuFreqInterval interval;
            if(!bracket(ordinal,[&]{auto value=authority.initial_discovery(); if(!value){fail(
                MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}return true;},&interval)) return reject();
            discoveries.push_back(interval);
            if(ci && !snapshots_equal(cycles.front().initial(),*authority.initial_snapshot())) {
                fail(MonitoringQ4CpuFreqCollectionFailureKind::semantic_drift,ordinal); return reject();
            }
            for(std::size_t ai{};ai<admitted;++ai) {
                MonitoringQ4CpuFreqCyclePreparationFact fact;
                if(!bracket(ordinal,[&]{auto value=authority.prepare_next();if(!value){fail(
                    MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                    fact=std::move(*value);return true;},&interval)) return reject();
                preparations.push_back({ordinal,interval,std::move(fact)});
            }
            bool unsupported{};
            if(!bracket(ordinal,[&]{auto value=authority.finish_preparations();if(!value){fail(
                MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                unsupported=*value;return true;},nullptr)) return reject();
            if(!unsupported) {
                for(std::size_t n{};n<rounds*authority.initial_snapshot()->domains().size();++n) {
                    MonitoringQ4CpuFreqCycleReadFact fact;
                    if(!bracket(ordinal,[&]{auto value=authority.raw_next();if(!value){fail(
                        MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                        fact=*value;return true;},&interval)) return reject();
                    raw.push_back({ordinal,interval,fact});
                    if(!bracket(ordinal,[&]{auto value=authority.typed_next();if(!value){fail(
                        MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                        fact=*value;return true;},&interval)) return reject();
                    typed.push_back({ordinal,interval,fact});
                }
                if(!bracket(ordinal,[&]{auto value=authority.final_discovery_and_compare();if(!value){fail(
                    MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}return true;},&interval)) return reject();
                discoveries.push_back(interval);
                bool retired{};
                do { if(!bracket(ordinal,[&]{auto value=authority.retire_next();if(!value){fail(
                    MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                    retired=*value;return true;},nullptr)) return reject(); } while(!retired);
            }
            std::optional<MonitoringQ4CpuFreqCycleResult> result;
            if(!bracket(ordinal,[&]{auto value=authority.take_result();if(!value){fail(
                MonitoringQ4CpuFreqCollectionFailureKind::cycle,ordinal,{},std::move(value.error()));return false;}
                result.emplace(std::move(*value));return true;},nullptr)) return reject();
            cycles.push_back(std::move(*result));
            if(unsupported) return MonitoringQ4CpuFreqCollection{ready.binding_,true,std::move(cycles),std::move(discoveries),
                std::move(preparations),std::move(raw),std::move(typed),std::move(*ledger)};
        }
        return MonitoringQ4CpuFreqCollection{ready.binding_,false,std::move(cycles),std::move(discoveries),
            std::move(preparations),std::move(raw),std::move(typed),std::move(*ledger)};
    } catch(const std::bad_alloc&) {
        fail(MonitoringQ4CpuFreqCollectionFailureKind::allocation,0, {}, {}, allocation_reason); return reject();
    } catch(...) {
        fail(MonitoringQ4CpuFreqCollectionFailureKind::contained,0); return reject();
    }
}
}
