#include "results/incremental_event_pair.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "runtime/production_admission_policy.hpp"
#include "epycbench/topology/serialization.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <bit>
#include "memory/pairwise_memory_access.hpp"
#include "measurement/measurement_leaf_access.hpp"
#include "results/pairwise_analysis_serialization.hpp"

namespace epycbench::results::event_profile::detail {
namespace {
using CheckJson=nlohmann::json;
void check_shape(bool value) {if(!value)throw false;}
void check_keys(const CheckJson& value,std::initializer_list<std::string_view> keys) {
    check_shape(value.is_object()&&value.size()==keys.size());
    for(auto key:keys)check_shape(value.contains(key));
}
auto check_text(const CheckJson& value) -> std::string_view {
    check_shape(value.is_string());return value.get_ref<const std::string&>();
}
template<class T> auto check_integer(const CheckJson& value) -> T {
    const auto text=check_text(value);check_shape(!text.empty());
    T result{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    check_shape(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size());
    check_shape(std::to_string(result)==text);return result;
}
void check_utc(const CheckJson& value) {
    if(value.is_null())return;
    auto parsed=journal::Timestamp::parse(check_text(value));core::detail::ExpectedErrorCleanup cleanup{parsed};
    check_shape(parsed.has_value());
}
void check_token(const CheckJson& value,std::string_view tokens) {
    const auto text=check_text(value);check_shape(!text.empty()&&text.find('|')==text.npos);
    while(!tokens.empty()) {const auto stop=tokens.find('|');if(tokens.substr(0,stop)==text)return;
        if(stop==tokens.npos)break;
        tokens.remove_prefix(stop+1);
    }
    throw false;
}
constexpr std::string_view task_kinds="invalid_operations|open_failed|read_failed|close_failed|contained_exception|contained_unknown_failure|task_id|task_identity_mismatch|command_delimiter|truncated_record|minor_faults|minor_faults_out_of_range|major_faults|major_faults_out_of_range";
constexpr std::string_view hwmon_kinds="enumerate|read|value|ambiguous|enumeration_capacity|token_capacity|contained|preparation_rejected";
constexpr std::string_view frequency_kinds="enumerate|enumeration_capacity|token_capacity|membership|metadata|value|ambiguous|read|contained|preparation_rejected";
constexpr std::string_view dispositions="success|native_failure|contained_standard|contained_unknown";
constexpr std::array<std::string_view,4> check_channels{"page_faults","context_switches","temperature","frequency"};
void check_platform_failure(const CheckJson& value,const error_serialization::Limits& limits) {
    check_keys(value,{"stage","error_number","detail"});
    check_token(value.at("stage"),"admission|open|identity|read|close|perf_map|perf_metadata|perf_unmap|clock");
    static_cast<void>(check_integer<int>(value.at("error_number")));
    if(!value.at("detail").is_null()) {
        error_serialization::Usage usage{};
        auto decoded=error_serialization::detail::decode_value(check_text(value.at("detail")),limits,usage);
        check_shape(decoded.has_value());
    }
}
void check_lower_failure(const CheckJson& value,std::string_view kinds) {
    check_keys(value,{"kind","native","disposition"});check_token(value.at("kind"),kinds);
    static_cast<void>(check_integer<int>(value.at("native")));check_token(value.at("disposition"),dispositions);
}
void check_source_arrays(const CheckJson& value,std::string_view kinds,std::size_t maximum) {
    const auto& cleanup=value.at("source_cleanup");const auto& retired=value.at("retirement");
    check_shape(cleanup.is_array()&&cleanup.size()<=maximum&&retired.is_array()&&retired.size()<=maximum);
    for(const auto& failure:cleanup)check_lower_failure(failure,kinds);
    for(const auto& row:retired) {
        check_keys(row,{"attempted","disposition","native"});check_shape(row.at("attempted").is_boolean());
        check_token(row.at("disposition"),dispositions);static_cast<void>(check_integer<int>(row.at("native")));
    }
    const auto& failure=value.at("source_failure");
    if(!failure.is_null()) {check_keys(failure,{"primary","cleanup_count"});check_lower_failure(failure.at("primary"),kinds);
        check_shape(check_integer<std::uint64_t>(failure.at("cleanup_count"))==cleanup.size());}
    else check_shape(cleanup.empty());
}
template<class Function> auto each_check_failure(const CheckJson& snapshot,Function&& function) -> std::size_t {
    std::size_t ordinal{};
    for(const auto& attempt:snapshot.at("attempts")) {
        const auto& outcome=attempt.at("outcome");const auto& value=attempt.at("value");
        const CheckJson absent=nullptr;
        const auto emit=[&](std::string_view stage,const CheckJson& native,const CheckJson& source) {
            function(ordinal++,attempt.at("channel"),stage,native,source);
        };
        const bool owned_retirement=attempt.at("channel")=="temperature"||attempt.at("channel")=="frequency";
        const auto& primary=outcome.at("primary");
        if(!primary.is_null()&&(!owned_retirement||primary.at("stage")!="close"))emit(check_text(primary.at("stage")),primary.at("error_number"),
            value.contains("source_failure")?value.at("source_failure"):absent);
        for(const auto& failure:outcome.at("cleanup"))
            if(!owned_retirement||failure.at("stage")!="close")emit(check_text(failure.at("stage")),failure.at("error_number"),absent);
        if(value.contains("source_cleanup"))for(const auto& failure:value.at("source_cleanup"))
            emit("close",failure.at("native"),failure);
        if(value.contains("retirement"))for(const auto& failure:value.at("retirement"))
            if(failure.at("attempted")==true&&failure.at("disposition")!="success")emit("close",failure.at("native"),failure);
    }
    return ordinal;
}
auto check_failure_at(const core::Error& error,const CheckJson& snapshot,std::optional<std::size_t> selected) -> bool {
    const auto& context=error.context;bool matched{};
    each_check_failure(snapshot,[&](std::size_t ordinal,const CheckJson& channel,std::string_view stage,
        const CheckJson& native,const CheckJson& source) {
        if((!selected||*selected==ordinal)&&context.at("channel")==channel&&context.at("stage")==stage&&
           context.at("error_number")==native&&context.at("source_failure")==source&&context.at("cleanup")==bool(ordinal))matched=true;
    });
    return matched;
}

}
auto controller_check_snapshot_valid(const nlohmann::json& snapshot,const error_serialization::Limits& limits) noexcept -> bool {
    try {
        check_keys(snapshot,{"profile","attempts"});check_shape(check_text(snapshot.at("profile"))=="epycbench.controller-checks/v1");
        const auto& attempts=snapshot.at("attempts");check_shape(attempts.is_array()&&attempts.size()<=4);
        std::size_t next{},source_bytes{};
        for(const auto& attempt:attempts) {
            check_keys(attempt,{"channel","utc","outcome","value"});
            const auto channel=std::ranges::find(check_channels,check_text(attempt.at("channel")));
            check_shape(channel!=check_channels.end());const auto index=static_cast<std::size_t>(channel-check_channels.begin());
            check_shape(index>=next);next=index+1;check_utc(attempt.at("utc"));
            const auto& outcome=attempt.at("outcome");check_keys(outcome,{"primary","cleanup"});
            const auto validate_failure=[&](const CheckJson& failure) {
                check_platform_failure(failure,limits);
                if(index!=1)check_token(failure.at("stage"),"admission|open|identity|read|close|clock");
            };
            if(!outcome.at("primary").is_null())validate_failure(outcome.at("primary"));
            check_shape(outcome.at("cleanup").is_array()&&outcome.at("cleanup").size()<=4);
            for(const auto& failure:outcome.at("cleanup"))validate_failure(failure);
            const auto& value=attempt.at("value");
            if(index==0) {
                check_keys(value,{"counters","source_failure"});const auto& counts=value.at("counters");
                if(!counts.is_null()){check_keys(counts,{"minor","major"});static_cast<void>(check_integer<std::uint64_t>(counts.at("minor")));static_cast<void>(check_integer<std::uint64_t>(counts.at("major")));}
                const auto& failure=value.at("source_failure");
                if(!failure.is_null()) {
                    check_keys(failure,{"primary","cleanup"});
                    for(auto key:{"primary","cleanup"})if(!failure.at(key).is_null()) {
                        const auto& part=failure.at(key);check_keys(part,{"kind","error_number"});
                        check_token(part.at("kind"),task_kinds);static_cast<void>(check_integer<int>(part.at("error_number")));
                    }
                    check_shape(!failure.at("primary").is_null());
                }
            } else if(index==1) {check_keys(value,{"observed"});check_shape(value.at("observed").is_boolean());}
            else {
                if(index==2)check_keys(value,{"evaluated","supported","millicelsius","source_bytes","source_failure","source_cleanup","retirement"});
                else check_keys(value,{"evaluated","supported","discovered_policies","source_bytes","memberships","domains","source_failure","source_cleanup","retirement"});
                check_shape(value.at("evaluated").is_boolean()&&value.at("supported").is_boolean());
                const auto bytes=check_integer<std::uint64_t>(value.at("source_bytes"));check_shape(bytes<=16U*1024U*1024U-source_bytes);source_bytes+=static_cast<std::size_t>(bytes);
                check_source_arrays(value,index==2?hwmon_kinds:frequency_kinds,index==2?3U:1048576U);
                if(value.at("supported")==true)check_shape(value.at("evaluated")==true&&outcome.at("primary").is_null()&&outcome.at("cleanup").empty());
                const auto close_is_retained=[&](const CheckJson& failure) {
                    if(failure.at("stage")!="close")return;
                    check_shape(std::ranges::any_of(value.at("retirement"),[&](const auto& row) {
                        return row.at("attempted")==true&&row.at("disposition")!="success"&&row.at("native")==failure.at("error_number");
                    }));
                };
                if(!outcome.at("primary").is_null())close_is_retained(outcome.at("primary"));
                for(const auto& failure:outcome.at("cleanup"))close_is_retained(failure);
                if(index==2) {if(!value.at("millicelsius").is_null())static_cast<void>(check_integer<std::int64_t>(value.at("millicelsius")));
                    if(value.at("supported")==true)check_shape(!value.at("millicelsius").is_null());}
                else {
                    check_shape(check_integer<std::uint64_t>(value.at("discovered_policies"))<=1024);
                    const auto& members=value.at("memberships");const auto& domains=value.at("domains");
                    check_shape(members.is_array()&&members.size()<=16384&&domains.is_array()&&domains.size()<=1024);
                    for(const auto& cpu:members)check_shape(check_integer<std::uint64_t>(cpu)<=4095);
                    for(const auto& domain:domains) {
                        check_keys(domain,{"membership_offset","membership_count","kind","driver","governor","minimum_hz","maximum_hz","current_hz","utc"});
                        const auto offset=check_integer<std::uint64_t>(domain.at("membership_offset"));const auto count=check_integer<std::uint64_t>(domain.at("membership_count"));
                        check_shape(count&&offset<=members.size()&&count<=members.size()-offset);
                        check_token(domain.at("kind"),"hardware_current|scaling_current");
                        check_shape(check_text(domain.at("driver")).size()<=4096&&check_text(domain.at("governor")).size()<=4096);
                        static_cast<void>(check_integer<std::uint64_t>(domain.at("minimum_hz")));static_cast<void>(check_integer<std::uint64_t>(domain.at("maximum_hz")));
                        if(!domain.at("current_hz").is_null())static_cast<void>(check_integer<std::uint64_t>(domain.at("current_hz")));
                        check_utc(domain.at("utc"));
                    }
                }
            }
        }
        return true;
    } catch(...) {return false;}
}
auto controller_check_failure_batch(const nlohmann::json& snapshot,const core::Error* primary,
    std::span<const core::Error> cleanup) noexcept -> bool {
    try {
        const auto count=each_check_failure(snapshot,[](auto,const auto&,auto,const auto&,const auto&){});
        if(count!=(primary?cleanup.size()+1U:0U)||(!primary&&!cleanup.empty()))return false;
        const auto matches=[&](const core::Error& error,std::size_t ordinal) {
            return preparation_failure_matches(error,"preflight_admission",ordinal!=0)&&
                error.context.at("observations")==snapshot&&check_failure_at(error,snapshot,ordinal);
        };
        if(primary&&!matches(*primary,0))return false;
        for(std::size_t i{};i<cleanup.size();++i)if(!matches(cleanup[i],i+1U))return false;
        return true;
    } catch(...) {return false;}
}
auto controller_check_snapshot_matches(const nlohmann::json& snapshot,const validation_event::Owner& assessment,
    const nlohmann::json& temperatures) noexcept -> bool {
    try {
        const auto checks=assessment.checks();check_shape(checks.size()==4&&temperatures.is_array());
        std::array<bool,4> present{};bool thermal{};
        for(const auto& attempt:snapshot.at("attempts")) {
            const auto found=std::ranges::find(check_channels,check_text(attempt.at("channel")));check_shape(found!=check_channels.end());
            const auto index=static_cast<std::size_t>(found-check_channels.begin());present[index]=true;
            const auto& check=checks[index];const auto& value=attempt.at("value");const auto& outcome=attempt.at("outcome");
            const bool success=outcome.at("primary").is_null()&&outcome.at("cleanup").empty();
            check_shape(check.evaluation==(success?validation_event::Evaluation::evaluated:validation_event::Evaluation::not_evaluated));
            if(success) {
                const bool supported=index==0?!value.at("counters").is_null():index==1?value.at("observed").get<bool>():value.at("supported").get<bool>();
                check_shape(check.support&&(*check.support==monitoring::MonitoringSourceSupport::supported)==supported&&
                    check.checked_at&&attempt.at("utc")==*check.checked_at);
                if(index==3&&!value.at("domains").empty())check_shape(attempt.at("utc")==value.at("domains").back().at("utc"));
            } else check_shape(!check.support&&!check.checked_at);
            if(index==2&&success) {
                thermal=true;check_shape(temperatures.size()==1);const auto& fact=temperatures[0];
                check_shape(fact.at("observed_at")==attempt.at("utc"));
                if(value.at("supported").get<bool>())check_shape(fact.at("state")=="available"&&fact.at("milli_celsius")==value.at("millicelsius"));
                else check_shape(fact.at("state")=="unavailable"&&fact.at("milli_celsius").is_null());
            }
        }
        if(!thermal)check_shape(temperatures.empty());
        for(std::size_t i=0;i<checks.size();++i)if(!present[i])check_shape(checks[i].evaluation!=validation_event::Evaluation::evaluated);
        return true;
    } catch(...) {return false;}
}
auto preparation_failure_matches(const core::Error& error,std::string_view target,
    std::optional<bool> cleanup) noexcept -> bool {
    if(error.category!=core::ErrorCategory::results)return false;
    const auto code=error.code.value();
    if(target=="kernel_evidence") {
        if((error.operation=="retain_kernel_report"&&code.starts_with("results.kernel_report."))||
           (error.operation=="retain_kernel_approval"&&code.starts_with("results.kernel_approval.")))
            return !cleanup.value_or(false);
        if(error.operation!="acquire_kernel_evidence"||code!="results.preparation.kernel_acquisition")return false;
        const auto& c=error.context;
        if(!c.is_object()||c.size()!=4||!c.contains("kind")||!c.contains("stage")||
           !c.contains("error_number")||!c.contains("cleanup")||!c["cleanup"].is_boolean())return false;
        const auto* kind=c["kind"].get_ptr<const std::string*>();
        if(!kind||(*kind!="admission"&&*kind!="unconfigured"&&*kind!="acquisition"&&*kind!="digest"))return false;
        const bool secondary=c["cleanup"].get<bool>();
        if(cleanup&&*cleanup!=secondary)return false;
        if(*kind!="acquisition")return !secondary&&c["stage"].is_null()&&c["error_number"].is_null();
        const auto* stage=c["stage"].get_ptr<const std::string*>();
        if(!stage||(*stage!="admission"&&*stage!="open"&&*stage!="identity"&&*stage!="read"&&*stage!="close"))return false;
        if(secondary&&*stage!="close")return false;
        if(c["error_number"].is_number_unsigned())return c["error_number"].get<std::uint64_t>()<=INT_MAX;
        return c["error_number"].is_number_integer()&&c["error_number"].get<std::int64_t>()>=INT_MIN&&
            c["error_number"].get<std::int64_t>()<=INT_MAX;
    }
    if(target=="preflight_admission"&&error.operation=="acquire_controller_checks"&&code=="results.preparation.controller_checks") {
        try {
            const auto& value=error.context;
            check_keys(value,{"channel","stage","error_number","cleanup","source_failure","observations"});
            check_token(value.at("channel"),"page_faults|context_switches|temperature|frequency");
            check_token(value.at("stage"),"admission|open|identity|read|close|perf_map|perf_metadata|perf_unmap|clock");
            static_cast<void>(check_integer<int>(value.at("error_number")));
            check_shape(value.at("cleanup").is_boolean());
            if(cleanup)check_shape(value.at("cleanup")==*cleanup);
            const auto& snapshot=value.at("observations");check_keys(snapshot,{"profile","attempts"});
            check_shape(snapshot.at("profile")=="epycbench.controller-checks/v1");
            const auto& attempts=snapshot.at("attempts");check_shape(attempts.is_array()&&attempts.size()<=4);
            return check_failure_at(error,snapshot,{});
        } catch(...) {return false;}
    }
    if(cleanup.value_or(false))return false;
    if(target=="preflight_admission")return error.operation=="admit_preflight"&&code.starts_with("results.preflight.");
    if(target!="execution_plan"||error.operation!="preparation_observation")return false;
    std::uint64_t maximum{};
    if(code=="results.preparation.execution_plan")maximum=static_cast<unsigned>(execution_plan::Error::encoding);
    else if(code=="results.preparation.implementation_selection")maximum=static_cast<unsigned>(implementation_selection::Error::encoding);
    else if(code=="results.preparation.build_object_association")maximum=static_cast<unsigned>(build_object_association::Error::encoding);
    else return false;
    const auto& context=error.context;
    return context.is_object()&&context.size()==1&&context.contains("producer_error")&&
        context["producer_error"].is_number_unsigned()&&context["producer_error"].get<std::uint64_t>()<=maximum;
}
namespace {
namespace wire=runtime::wire_v1;
using J=nlohmann::json;
using SV=std::string_view;
void fields(const J& value,std::initializer_list<SV> keys) {
    require(value.is_object()&&value.size()==keys.size(),Error::schema);
    for(const auto key:keys)require(value.contains(key),Error::schema);
}
auto string(const J& value)->const std::string& {require(value.is_string(),Error::schema);return value.get_ref<const std::string&>();}
auto number(const J& value)->std::uint64_t {
    const auto& text=string(value);require(!text.empty()&&(text.size()==1U||text.front()!='0'),Error::schema);
    std::uint64_t result{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size(),Error::schema);return result;
}
// LocalResourceLimits already checks representability. This probe asserts only
// that arithmetic reload must not reserve the runner/controller evidence buffers.
auto capacity_consistency_probe(std::size_t,std::size_t) noexcept->bool {return true;}
auto escaped(SV value,Work& work)->wire::CapacityCount {
    work.debit(multiply(value.size(),6U));return {J(std::string{value}).dump().size()-2U};
}
void equal_number(const J& value,std::uint64_t expected,Work& work) {
    work.debit(string(value).size());require(number(value)==expected,Error::association);
}
void equal_digest(const J& value,const core::Sha256Digest& expected,Work& work) {
    require(work.equal(string(value),expected.to_hex()),Error::identity);
}
#define EB_COUNTS(X) \
 X(blocks) X(factual_observations) X(failed_observations) X(stopping_evaluations) \
 X(workers) X(placements) X(migrations) X(validity_checks) X(finalized_workers) \
 X(failed_observation_errors) X(operational_terminal_errors) X(raw_page_acquisitions) \
 X(raw_context_acquisitions) X(context_events) X(context_offsets) X(raw_temperature_acquisitions) \
 X(frequency_domains) X(frequency_acquisitions) X(frequency_cpu_memberships) X(frequency_metadata) \
 X(classified_page_workers) X(classified_context_workers) X(classified_frequency_domains) \
 X(first_resident_page_counts) X(second_resident_page_counts)
#define EB_STOCKS(X) \
 X(persistent_worker_scopes) X(page_sources) X(context_sources) X(context_coverage_slots) \
 X(temperature_sources) X(frequency_sources) X(frequency_metadata_slots) X(persistent_frequency_cpu_ids) \
 X(preflight_channel_dispositions) X(page_pair_scratch) X(context_array_scratch) X(temperature_view_scratch) \
 X(frequency_view_scratch) X(frequency_result_scratch) X(whole_stop_cleanup) X(preparation_rollback_cleanup) \
 X(completion_abort_cleanup) X(cpufreq_paths) X(cpufreq_directory_entries) X(cpufreq_operation_slots) \
 X(hwmon_directory_entries) X(hwmon_operation_slots) X(dynamic_frequency_raw_bytes) X(dynamic_frequency_escaped_bytes)
#define EB_DESTINATIONS(X) \
 X(provenance) X(policies) X(calibration) X(warmup) X(blocks) X(stopping_evaluations) \
 X(terminal) X(finalization) X(monitoring_raw) X(monitoring_classified) X(errors) X(memory_finalization)
#define EB_ESCAPED(X) \
 X(scenario_profile) X(scenario_digest) X(resolved_plan_digest) X(workload_id) X(first_variant_id) \
 X(second_variant_id) X(monitoring_implementation_ids) X(frequency_driver_ids) X(frequency_governor_ids) X(stable_numa_node_ids)
void offered_totals(const J& offered,const wire::CalculatedEvidenceCapacity& calculated,Work& work) {
    const auto& maxima=offered.at("logical_wire_maxima");const auto& shape=calculated.logical_shape;
    equal_number(maxima.at("canonical_logical_bytes"),shape.canonical_logical_bytes.value,work);
    equal_number(maxima.at("typed_nodes"),shape.typed_nodes.value,work);
    equal_number(maxima.at("parser_depth"),shape.parser_depth.value,work);
    const auto& counts=maxima.at("component_maxima");const auto& components=calculated.prerequisite.components;
#define CHECK_COUNT(name) equal_number(counts.at(#name),components.name.value,work);
    EB_COUNTS(CHECK_COUNT)
#undef CHECK_COUNT
    const auto& stocks=offered.at("monitoring_admission_stocks");const auto& lower=calculated.prerequisite.monitoring_lower_stocks;
#define CHECK_STOCK(name) equal_number(stocks.at(#name),lower.name.value,work);
    EB_STOCKS(CHECK_STOCK)
#undef CHECK_STOCK
    const auto& resource=offered.at("resource_plan");const auto& plan=calculated.resource_plan;
    equal_number(resource.at("controller_receive_bytes"),plan.controller_receive_bytes.value,work);
    equal_number(resource.at("controller_overflow_probe_bytes"),plan.controller_overflow_probe_bytes.value,work);
    equal_number(resource.at("controller_node_slots"),plan.controller_node_slots.value,work);
    equal_number(resource.at("runner_encode_bytes"),plan.runner_encode_bytes.value,work);
    equal_number(resource.at("runner_overflow_probe_bytes"),plan.runner_overflow_probe_bytes.value,work);
    equal_number(resource.at("runner_node_slots"),plan.runner_node_slots.value,work);
#define CHECK_DEST(name) \
    equal_number(resource.at("controller_decode_destinations").at(#name),plan.controller_decode_destinations.name.value,work); \
    equal_number(resource.at("runner_evidence_destinations").at(#name),plan.runner_evidence_destinations.name.value,work);
    EB_DESTINATIONS(CHECK_DEST)
#undef CHECK_DEST
}
} // namespace

auto reconstruct_capacity(const J& raw,const wire::detail::ParsedCapacityOffer& offer,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    const topology::TopologySnapshot& topology,const monitoring::MonitoringExecutionPolicy& policy,
    const CapacityFactsLimits& limits,Work& work)->wire::CalculatedEvidenceCapacity {
    fields(raw,{"base_page_bytes","kernel_maximum_cpu","present_cpu_ids","domain_cpu_ids"});
    const auto base=number(raw.at("base_page_bytes")),maximum=number(raw.at("kernel_maximum_cpu"));
    const auto& present=raw.at("present_cpu_ids");const auto& partition=raw.at("domain_cpu_ids");
    require(present.is_array()&&partition.is_array(),Error::schema);
    const auto workers=request.first().workers().size();const auto cpus=present.size();const auto domains=partition.size();
    require(workers&&workers==request.second().workers().size()&&workers<=limits.workers&&
        cpus&&cpus<=limits.cpus&&domains&&domains<=limits.domains&&domains==std::min(workers,cpus),Error::capacity);
    require(limits.size_max<=SIZE_MAX&&limits.byte_container_max<=std::vector<std::byte>{}.max_size()&&
        limits.node_container_max<=std::vector<std::uint64_t>{}.max_size()&&limits.parser_depth_max<=64U,Error::capacity);
    const auto raw_stock=add(multiply(cpus,sizeof(wire::CpuId)*12U),
        add(multiply(domains,add(sizeof(std::vector<wire::CpuId>)*8U,sizeof(std::span<const wire::CpuId>))),
        multiply(workers,sizeof(wire::PageCount)*20U)));
    const auto strings=add(offer.canonical_body.size(),offer.canonical_capacity_body.size());
    require(strings<=limits.text,Error::capacity);
    Limits lexical;lexical.body_bytes=limits.text;lexical.nodes=limits.text;lexical.depth=64U;
    lexical.text_bytes=limits.text;lexical.candidate_bytes=limits.simultaneous;
    const auto parsed_shape=shape(offer.canonical_body,lexical,work);
    const auto peak=add(raw_stock,add(multiply(workspace(parsed_shape),4U),sizeof(wire::CalculatedEvidenceCapacity)*8U));
    require(peak<=limits.simultaneous,Error::capacity);work.reserve_peak(peak);
    work.debit(add(multiply(cpus,add(32U,multiply(static_cast<std::size_t>(std::bit_width(cpus)),4U))),add(multiply(workers,32U),multiply(strings,8U))));
    std::vector<wire::CpuId> cpu_ids;cpu_ids.reserve(cpus);
    auto retained_cpu=topology.machine().present_cpus.begin();
    for(const auto& cpu:present){const auto id=number(cpu);require(retained_cpu!=topology.machine().present_cpus.end()&&id==(*retained_cpu).value(),Error::association);++retained_cpu;cpu_ids.push_back({id});}
    require(retained_cpu==topology.machine().present_cpus.end(),Error::association);
    std::vector<std::vector<wire::CpuId>> domain_ids;domain_ids.reserve(domains);
    std::vector<std::span<const wire::CpuId>> domain_views;domain_views.reserve(domains);
    for(std::size_t d{};d<domains;++d){const auto& row=partition[d];
        const auto count=d+1U==domains?cpus-d:1U;require(row.is_array()&&row.size()==count,Error::association);
        auto& target=domain_ids.emplace_back();target.reserve(count);
        for(std::size_t i{};i<count;++i){require(number(row[i])==cpu_ids[d+i].value,Error::association);target.push_back(cpu_ids[d+i]);}
        domain_views.emplace_back(target);
    }
    require(!topology.machine().present_nodes.empty(),Error::association);
    std::uint64_t maxnode{};for(const auto node:topology.machine().present_nodes)maxnode=std::max(maxnode,static_cast<std::uint64_t>(node.value()));
    require(maxnode<UINT32_MAX,Error::capacity);++maxnode;
    std::vector<wire::PageCount> first_pages,second_pages;std::vector<wire::TargetCount> first_targets,second_targets;
    first_pages.reserve(workers);second_pages.reserve(workers);first_targets.reserve(workers);second_targets.reserve(workers);
    std::size_t stable_size{};
    auto pages=[&](const auto& input,auto& page_values,auto& targets){for(const auto& worker:input.workers()){
        const auto& region=worker.region();const auto bytes=region.bytes().value(),page=region.page_size_bytes().value();
        require(bytes&&page&&bytes%page==0U&&!region.nodes().empty(),Error::association);
        page_values.push_back({bytes/page});targets.push_back({region.nodes().size()});
        for(const auto& node:region.nodes()){const auto& stable=node.stable_numa_node_id().value();
            require(stable.size()<=limits.text,Error::capacity);stable_size=std::max(stable_size,static_cast<std::size_t>(escaped(stable,work).value));}
    }};
    pages(request.first(),first_pages,first_targets);pages(request.second(),second_pages,second_targets);
    const auto* provenance=std::get_if<runtime::ScenarioPlacementProvenance>(&request.first().provenance());require(provenance,Error::association);
    const auto& experiment=request.first().experiment();const auto workload=experiment.workload_id().to_string();
    const auto first=experiment.id().to_string()+":first",second=experiment.id().to_string()+":second";
    require(base<=UINT64_MAX/2U,Error::capacity);wire::CapacityCount implementation{};
    for(const SV token:{"linux-proc-task-stat","linux-perf-context-switch-records","linux-hwmon-k10temp-tctl",
        "linux-cpufreq-cpuinfo-cur-freq","linux-cpufreq-scaling-cur-freq"})implementation.value=std::max(implementation.value,escaped(token,work).value);
    const wire::RequestKnownEscapedBytes escaped_bytes{escaped("epycbench.scenario/v0.1",work),
        escaped(provenance->scenario_digest().text(),work),escaped(provenance->resolved_scenario_plan_digest().text(),work),
        escaped(workload,work),escaped(first,work),escaped(second,work),implementation,{base*2U},{base*2U},{stable_size}};
    const auto active=[&](monitoring::MonitoringChannel channel){return policy.disposition(channel)!=monitoring::MonitoringChannelDisposition::disabled;};
    const wire::ChannelFlags channels{active(monitoring::MonitoringChannel::page_faults),active(monitoring::MonitoringChannel::context_switches),
        active(monitoring::MonitoringChannel::temperature),active(monitoring::MonitoringChannel::frequency)};
    const auto& measurement=request.first().execution_inputs().measurement();
    const auto interval=policy.during_interval();auto rounds=wire::detail::derive_monitoring_round_capacity(channels.context,channels.temperature,
        channels.frequency,measurement.observation_maximum_duration().value(),interval?std::optional{interval->value()}:std::nullopt);
    require(rounds.has_value(),Error::capacity);
    const wire::CapacityParameterCandidate candidate{{workers},{measurement.repetition_maximum_blocks()},{measurement.warmup_maximum()},
        {base},{maximum},{maxnode},*rounds,channels,escaped_bytes,cpu_ids,domain_views,first_pages,first_targets,second_pages,second_targets};
    auto normalized=wire::normalize_capacity_parameters(candidate);require(normalized.has_value(),Error::association);
    const auto identity_capacity=offer.source_manifest_sha256.to_hex()=="e6faf94e4be36587ba52efc862fb055355ef77209074ba50abd8f21838f264b8"?
        wire::KernelIdentityCapacity::legacy_builtin_tokens:wire::KernelIdentityCapacity::plugin_tokens;
    auto calculated=wire::calculate_evidence_capacity(*normalized,{limits.size_max,limits.byte_container_max,limits.node_container_max,
        limits.parser_depth_max,&capacity_consistency_probe},identity_capacity);require(calculated.has_value(),Error::capacity);
    require(raw_stock<=limits.retained,Error::capacity);
    // The parser enforces closed grammar; independently reparse the bounded retained
    // body to compare every calculated field and fixed selected identity.
    auto reparsed=wire::detail::parse_capacity_offer_facts(offer.canonical_body,{limits.text,limits.text,64U});
    require(reparsed.has_value()&&reparsed->session_id==offer.session_id&&reparsed->request_id==offer.request_id&&
        reparsed->request_body_sha256==offer.request_body_sha256&&reparsed->source_manifest_sha256==offer.source_manifest_sha256&&
        reparsed->projection_context_profile_sha256==offer.projection_context_profile_sha256&&
        reparsed->body_sha256==offer.body_sha256&&reparsed->capacity_body_sha256==offer.capacity_body_sha256&&
        work.equal(reparsed->canonical_capacity_body,offer.canonical_capacity_body),Error::association);
    auto offered=parse_tree(reparsed->canonical_capacity_body,lexical,work);core::detail::JsonCleanup offered_cleanup{offered};
    const bool legacy_wire=offer.source_manifest_sha256.to_hex()=="e6faf94e4be36587ba52efc862fb055355ef77209074ba50abd8f21838f264b8"&&
        string(offered.at("wire_registry_sha256"))=="2639a505882a42779853bba12287b7c72ebe56b40c3e96c245a12a8c58fd34d5";
    const bool plugin_wire=offer.source_manifest_sha256.to_hex()=="2c5cf4461e02b8d8aa3c95a0b3918237bbb7d3af8c6a4c631008de08ea56c09b"&&
        string(offered.at("wire_registry_sha256"))=="626c6355f54a5f7abea5a67cd6c8db95efd423540291145dc050c976c9038f48";
    require(offer.request_id==request.first_request_id()&&offer.request_body_sha256==request.body_sha256()&&
        (legacy_wire||plugin_wire)&&
        offer.projection_context_profile_sha256==core::sha256(wire::detail::capacity_projection_context_profile),Error::identity);
    using Admission=runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess;
    equal_digest(offered.at("finalized_error_profile_sha256"),Admission::expected_profile_identity(),work);
    equal_digest(offered.at("monitoring_capacity_profile_sha256"),Admission::expected_capacity_identity(),work);
    equal_number(offered.at("numa_domain_maxnode"),maxnode,work);
    const auto& page_inputs=offered.at("request_page_inputs");equal_number(page_inputs.at("worker_count"),workers,work);
    const auto family=request.first().experiment().kernel_id();require(family==request.second().experiment().kernel_id(),Error::association);
    require(string(page_inputs.at("family"))==(family.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency"),Error::association);
    auto compare_rows=[&](SV name,const auto& expected){const auto& rows=page_inputs.at(name);require(rows.size()==expected.size(),Error::association);
        for(std::size_t i{};i<expected.size();++i)equal_number(rows[i],expected[i].value,work);};
    compare_rows("first_total_pages",first_pages);compare_rows("second_total_pages",second_pages);
    compare_rows("first_target_counts",first_targets);compare_rows("second_target_counts",second_targets);
    const auto& escaped_value=offered.at("request_known_escaped_bytes");
#define CHECK_ESCAPED(name) equal_number(escaped_value.at(#name),escaped_bytes.name.value,work);
    EB_ESCAPED(CHECK_ESCAPED)
#undef CHECK_ESCAPED
    offered_totals(offered,*calculated,work);return std::move(*calculated);
}
#undef EB_COUNTS
#undef EB_STOCKS
#undef EB_DESTINATIONS
#undef EB_ESCAPED
namespace {
auto unhex(const J& value,Work& work)->std::string {
    const auto& bytes=string(value);require(bytes.size()%2U==0U,Error::schema);work.debit(bytes.size());
    const auto nibble=[](char c)->unsigned {if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');
        if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);
        throw Error::schema;};
    std::string decoded(bytes.size()/2U,'\0');
    for(std::size_t i{};i<decoded.size();++i)decoded[i]=static_cast<char>((nibble(bytes[2U*i])<<4U)|nibble(bytes[2U*i+1U]));
    return decoded;
}
void same_private(const J& value,SV expected,Work& work){require(work.equal(unhex(value,work),expected),Error::identity);}
auto context_header(const J& value)->runtime::wire_v1::detail::ReconstructedEvidenceHeader {
    fields(value,{"provenance","statistics_policy","monitoring_policy","repetition_policy","comparison_order"});
    fields(value.at("provenance"),{"experiment_id","scenario_profile","scenario_digest","resolved_plan_digest","workload_id","first_variant_id","second_variant_id"});
    const auto& statistics=value.at("statistics_policy");
    fields(statistics,{"comparison_bootstrap_policy","stopping_bootstrap_policy","effective_practical_effect_fraction","practical_effect_delta","relative_half_width_target"});
    for(const auto key:{"comparison_bootstrap_policy","stopping_bootstrap_policy"})fields(statistics.at(key),{"replicate_count","confidence","seed"});
    fields(value.at("monitoring_policy"),{"page_faults","context_switches","temperature","frequency","before_observation","after_observation","during_observation","during_interval","optional_unsupported"});
    fields(value.at("repetition_policy"),{"minimum_blocks","maximum_blocks","minimum_valid_observations_per_variant","stopping_enabled"});
    fields(value.at("comparison_order"),{"strategy","seed"});
    auto decoded=runtime::wire_v1::detail::reconstruct_evidence_header(value);require(decoded.has_value(),Error::child);return std::move(*decoded);
}
auto service(const J& value,const topology::LoadedTopology& loaded,Work& work)->runtime::ServiceThreadAssignment {
    fields(value,{"role","stable_pu","stable_core","processing_unit","core","linux_cpu"});
    const auto& token=string(value.at("role"));require(token=="control_monitor"||token=="measurement_monitor",Error::schema);
    const auto pu=number(value.at("processing_unit")),core=number(value.at("core")),cpu=number(value.at("linux_cpu"));
    require(pu<=UINT32_MAX&&core<=UINT32_MAX&&cpu<=UINT32_MAX,Error::schema);
    const auto pu_text=unhex(value.at("stable_pu"),work),core_text=unhex(value.at("stable_core"),work);
    const auto pus=loaded.stable_ids.processing_units();const auto cores=loaded.stable_ids.cores();
    work.debit(add(pus.size(),cores.size()));
    const auto stable_pu=std::ranges::find_if(pus,[&](const auto& row){return work.equal(row.semantic_id.value(),pu_text);});
    const auto stable_core=std::ranges::find_if(cores,[&](const auto& row){return work.equal(row.semantic_id.value(),core_text);});
    require(stable_pu!=pus.end()&&stable_core!=cores.end(),Error::association);
    const auto found=loaded.topology.processing_unit(stable_pu->handle);
    require(found&&found->core_id==stable_core->handle&&found->linux_cpu_id.value()==cpu,Error::association);
    return {token=="control_monitor"?runtime::ServiceRole::control_monitor:runtime::ServiceRole::measurement_monitor,
        stable_pu->semantic_id,stable_core->semantic_id,core::ProcessingUnitId{static_cast<std::uint32_t>(pu)},
        core::CoreId{static_cast<std::uint32_t>(core)},core::LinuxCpuId{static_cast<std::uint32_t>(cpu)}};
}
} // namespace

auto PairContextDecoder::prepare(const DecodedContext& context,block_evidence::PairLimits limits,
    observation_timing::Limits timing)->std::expected<block_evidence::PairPrefix,block_evidence::Error> {
    return block_evidence::PairPrefix::prepare_decoded(context.lease.retain(),limits,
        context.header.statistics_policy,context.header.repetition_policy,timing);
}

auto PairContextDecoder::decode(SV bytes,const normalization_serialization::Owner& normalization,
    const PlanFacts& plan,std::size_t pair_index,const PairContextLimits& limits,Work& work,
    std::optional<SV> expected_origin)->DecodedContext {
    Limits lexical;lexical.body_bytes=limits.bytes;lexical.nodes=limits.nodes;lexical.depth=limits.depth;
    lexical.text_bytes=limits.text;lexical.candidate_bytes=limits.simultaneous;
    const auto lexical_shape=shape(bytes,lexical,work);require(limits.depth<=64U,Error::capacity);
    const auto initial=add(multiply(workspace(lexical_shape),12U),add(multiply(limits.workers,sizeof(block_evidence::ExpectedMemoryCell)*8U),
        multiply(limits.targets,sizeof(runtime::ipc::PlacementNumaTarget)*8U)));
    require(normalization.topology()&&normalization.topology()->topology(),Error::association);
    const auto topology_stock=normalization.topology()->usage().topology_decode_bytes;
    const auto peak=add(topology_stock,add(initial,add(limits.capacity.simultaneous,limits.association.preparation_bytes)));
    require(peak<=limits.simultaneous,Error::capacity);work.reserve_peak(peak);
    work.debit(add(multiply(lexical_shape.nodes,16U),multiply(lexical_shape.text,16U)));
    auto value=parse_tree(bytes,lexical,work);core::detail::JsonCleanup value_cleanup{value};
    auto encoded=core::canonicalize_json_jcs(value,{limits.bytes,limits.depth,limits.nodes});core::detail::ExpectedErrorCleanup encoded_cleanup{encoded};
    require(encoded&&work.equal(*encoded,bytes),Error::canonical);
    fields(value,{"origin","run","provenance","request_body","capacity_offer","capacity_inputs","capacity_profile","header","cpus","workers",
        "first_assignments","second_assignments","first_memory","second_memory","control","monitoring","complete"});
    const auto& context_origin=string(value.at("origin"));
    require(!expected_origin||context_origin==*expected_origin,Error::association);
    require((context_origin=="supplied_synthetic"||(context_origin=="actual_availability"&&normalization.provenance()&&
        normalization.provenance()->domain==process_subject::Domain::supplied_observation))&&normalization.provenance()&&normalization.plan()&&normalization.topology()&&
        normalization.topology()->topology()&&pair_index<plan.pairs.size(),Error::association);
    const auto& topology_evidence=*normalization.topology()->topology();
    work.debit(topology_stock);auto reloaded=topology::deserialize_topology(topology_evidence.canonical(),topology::TopologyJsonInputPolicy::require_canonical);
    core::detail::ExpectedErrorCleanup reloaded_cleanup{reloaded};
    require(reloaded.has_value()&&topology_evidence.fingerprints()&&reloaded->fingerprints==*topology_evidence.fingerprints(),Error::child);
    const auto& loaded=*reloaded;
    require(work.equal(string(value.at("run")),normalization.provenance()->run),Error::identity);
    auto run=journal::RunId::parse(string(value.at("run")));core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::schema);
    auto request=runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::parse_request(string(value.at("request_body")));
    require(request.has_value(),Error::child);auto header=context_header(value.at("header"));
    const auto& provenance=value.at("provenance");fields(provenance,{"experiment_id","scenario_profile","scenario_digest","resolved_plan_digest","workload_id","first_variant_id","second_variant_id"});
    equal_number(provenance.at("experiment_id"),header.provenance.experiment_id.value(),work);
    same_private(provenance.at("scenario_profile"),header.provenance.scenario_profile,work);
    same_private(provenance.at("scenario_digest"),header.provenance.scenario_digest,work);
    same_private(provenance.at("resolved_plan_digest"),header.provenance.resolved_plan_digest,work);
    same_private(provenance.at("workload_id"),header.provenance.workload_id,work);
    same_private(provenance.at("first_variant_id"),header.provenance.first_variant_id,work);
    same_private(provenance.at("second_variant_id"),header.provenance.second_variant_id,work);
    const auto& first=request->first();const auto& second=request->second();const auto count=first.workers().size();
    require(count&&count<=limits.workers&&count<=UINT32_MAX&&count==second.workers().size(),Error::capacity);
    require(first.topology().physical_fingerprint()==loaded.fingerprints.physical&&first.topology().effective_fingerprint()==loaded.fingerprints.effective&&
        second.topology()==first.topology(),Error::association);
    const auto* scenario=std::get_if<runtime::ScenarioPlacementProvenance>(&first.provenance());require(scenario,Error::association);
    require(header.provenance.experiment_id.value()==first.experiment().index()&&header.provenance.scenario_profile=="epycbench.scenario/v0.1"&&
        header.provenance.scenario_digest==scenario->scenario_digest().text()&&header.provenance.resolved_plan_digest==scenario->resolved_scenario_plan_digest().text()&&
        header.provenance.workload_id==first.experiment().workload_id().to_string()&&header.provenance.first_variant_id==first.experiment().id().to_string()+":first"&&
        header.provenance.second_variant_id==first.experiment().id().to_string()+":second",Error::identity);
    auto offer=wire::detail::parse_capacity_offer_facts(string(value.at("capacity_offer")),{limits.bytes,limits.nodes,limits.depth});require(offer.has_value(),Error::child);
    require(string(value.at("capacity_profile"))==wire::capacity_profile,Error::identity);
    auto capacity=reconstruct_capacity(value.at("capacity_inputs"),*offer,*request,loaded.topology,header.monitoring_policy,limits.capacity,work);
    fields(value.at("complete"),{"first_request","second_request","comparison_seed"});
    equal_number(value.at("complete").at("first_request"),request->first_request_id().value(),work);
    equal_number(value.at("complete").at("second_request"),request->second_request_id().value(),work);
    equal_number(value.at("complete").at("comparison_seed"),request->resolved_comparison_order_seed(),work);
    auto control=service(value.at("control"),loaded,work);require(control.role==runtime::ServiceRole::control_monitor,Error::association);
    const auto& monitoring=value.at("monitoring");fields(monitoring,{"policy","assignment","physical","effective","scenario"});
    require(monitoring.at("policy")==value.at("header").at("monitoring_policy"),Error::association);
    require(string(monitoring.at("physical"))==loaded.fingerprints.physical.to_string()&&string(monitoring.at("effective"))==loaded.fingerprints.effective.to_string(),Error::identity);
    const auto monitoring_provenance=runtime::detail::PairwiseMonitoringHostCapacityAdmissionAccess::provenance_for(first.provenance());
    require(monitoring_provenance.has_value(),Error::identity);const auto& scenario_hash=monitoring_provenance->authority_digest();
    equal_digest(monitoring.at("scenario"),scenario_hash,work);
    std::vector<runtime::ServiceThreadAssignment> services;services.reserve(2U);services.push_back(control);
    std::optional<runtime::ServiceThreadAssignment> measurement_service;
    if(!monitoring.at("assignment").is_null()){measurement_service=service(monitoring.at("assignment"),loaded,work);
        require(measurement_service->role==runtime::ServiceRole::measurement_monitor,Error::association);services.push_back(*measurement_service);}
    for(std::size_t a{};a<services.size();++a)for(std::size_t b{};b<a;++b){work.debit(2U);
        require((services[a].processing_unit==services[b].processing_unit)==(services[a].stable_pu==services[b].stable_pu)&&
            (services[a].core==services[b].core)==(services[a].stable_core==services[b].stable_core),Error::association);}
    request_association::Usage association_usage{};const request_association::detail::PlanView plan_view{*normalization.plan(),plan.workloads,plan.bindings,plan.pairs[pair_index],loaded.topology,loaded.stable_ids,loaded.fingerprints};
    const request_association::detail::RequestView request_view{first,second,request->first_request_id().value(),request->second_request_id().value(),
        request->resolved_comparison_order_seed(),request->statistics_source(),header.statistics_policy,header.monitoring_policy,services,
        runtime::detail::production_monitoring_needs_service(header.monitoring_policy)};
    require(work.used<=work.limit,Error::capacity);
    auto association_limits=limits.association;
    association_limits.work=std::min(association_limits.work,work.limit-work.used);
    auto associated=request_association::detail::compare(plan_view,request_view,plan.counts.root_seed,association_limits,association_usage);
    if(!associated)require(associated.error()!=request_association::Error::capacity,Error::capacity);
    require(associated.has_value(),Error::association);work.debit(association_usage.work);
    const auto& measurement=first.execution_inputs().measurement();
    require(header.repetition_policy.minimum_blocks()==measurement.repetition_minimum_blocks()&&header.repetition_policy.maximum_blocks()==measurement.repetition_maximum_blocks()&&
        header.repetition_policy.minimum_valid_observations_per_variant()==measurement.minimum_valid_observations_per_variant()&&
        header.repetition_policy.stopping_enabled()==measurement.stopping_enabled()&&header.comparison_order.comparison_order_seed()==request->resolved_comparison_order_seed(),Error::association);
    switch(measurement.comparison_order()){
    case scenario::MatrixComparisonOrder::randomized_abba:require(header.comparison_order.strategy()==measurement::ComparisonOrderStrategy::randomized_abba,Error::association);break;
    case scenario::MatrixComparisonOrder::randomized_baab:require(header.comparison_order.strategy()==measurement::ComparisonOrderStrategy::randomized_baab,Error::association);break;
    case scenario::MatrixComparisonOrder::randomized_balanced:require(header.comparison_order.strategy()==measurement::ComparisonOrderStrategy::randomized_balanced,Error::association);break;
    }
    auto backing=std::make_shared<block_evidence::DecodedPairFacts>(block_evidence::DecodedPairFacts{std::move(*run),header.provenance,
        first.execution_inputs().kernel(),second.execution_inputs().kernel(),std::move(capacity),{},{},{},{},{},{},{},control,
        block_evidence::ExpectedMonitoringPreparation{header.monitoring_policy,measurement_service,loaded.fingerprints.physical,loaded.fingerprints.effective,scenario_hash},
        block_evidence::ExpectedCompletePreparation{measurement,header.statistics_policy,request->first_request_id().value(),request->second_request_id().value(),request->resolved_comparison_order_seed()}});
    backing->domain=normalization.provenance()->domain;
    for(const auto key:{"cpus","workers","first_assignments","second_assignments","first_memory","second_memory"})require(value.at(key).is_array()&&value.at(key).size()==count,Error::association);
    std::size_t target_count{};
    for(std::size_t i{};i<count;++i){
        equal_number(value.at("cpus")[i],first.workers()[i].cpu().linux_cpu().value(),work);equal_number(value.at("workers")[i],i,work);
        backing->cpus.push_back(first.workers()[i].cpu().linux_cpu());backing->workers.emplace_back(static_cast<std::uint32_t>(i));
        for(unsigned side{};side<2U;++side){const auto& input=side?second:first;const auto& worker=input.workers()[i];
            const auto& assignment=value.at(side?"second_assignments":"first_assignments")[i];fields(assignment,{"stable_pu","stable_core","linux_cpu"});
            same_private(assignment.at("stable_pu"),worker.cpu().stable_pu_id().value(),work);same_private(assignment.at("stable_core"),worker.cpu().stable_core_id().value(),work);
            equal_number(assignment.at("linux_cpu"),worker.cpu().linux_cpu().value(),work);
            (side?backing->second_assignments:backing->first_assignments).push_back(worker.cpu());
            const auto& memory=value.at(side?"second_memory":"first_memory")[i];fields(memory,{"request","resolved_nodes"});
            auto region=core::detail::clone_json_as<J>(input.wire().at("workers").at(i).at("region"));core::detail::JsonCleanup region_cleanup{region};
            require(memory.at("request")==region,Error::association);
            const auto& targets=memory.at("resolved_nodes");require(targets.is_array()&&targets.size()==worker.region().nodes().size(),Error::association);
            target_count=add(target_count,targets.size());require(target_count<=limits.targets,Error::capacity);
            block_evidence::ExpectedMemoryCell cell{worker.region(),{}};cell.resolved_nodes.reserve(targets.size());
            for(std::size_t n{};n<targets.size();++n){const auto& target=worker.region().nodes()[n];
                const auto source_handle=number(targets[n]);require(source_handle<=UINT32_MAX,Error::schema);
                auto id=loaded.stable_ids.numa_node(target.stable_numa_node_id());require(id.has_value(),Error::association);
                const auto node=loaded.topology.numa_node(*id);
                require(node&&node->linux_node_id.value()&&*node->linux_node_id.value()==target.linux_numa_node().value(),Error::association);
                auto same_mapping=[&](const auto& previous){for(std::size_t old{};old<previous.resolved_nodes.size();++old){work.debit(1U);
                    const bool equal_stable=work.equal(previous.request.nodes()[old].stable_numa_node_id().value(),target.stable_numa_node_id().value());
                    require((previous.resolved_nodes[old].value()==source_handle)==equal_stable,Error::association);}};
                for(const auto& previous:backing->first_memory)same_mapping(previous);
                for(const auto& previous:backing->second_memory)same_mapping(previous);
                same_mapping(cell);cell.resolved_nodes.emplace_back(static_cast<std::uint32_t>(source_handle));
            }
            (side?backing->second_memory:backing->first_memory).push_back(std::move(cell));
        }
    }
    const auto retained=add(initial,bytes.size());require(retained<=limits.retained,Error::capacity);
    backing->usage={retained,peak,lexical_shape.text,add(count,target_count)};
    DecodedContext decoded{block_evidence::PairFactsLease{std::move(backing)},std::move(header),std::string{bytes},
        string(value.at("request_body")),*monitoring_provenance,*associated,services.size(),std::string{offer->session_id.text()}};
    require(plan.pairs[pair_index].workload_index<plan.workloads.size(),Error::association);
    decoded.implementation=plan.workloads[plan.pairs[pair_index].workload_index].implementation;
    auto owned=add(decoded.lease.usage().retained_bytes,sizeof(DecodedContext));
    const auto charge=[&](const std::string& text){owned=add(owned,add(text.capacity(),1U));};
    charge(decoded.canonical);charge(decoded.request_body);charge(decoded.session);
    const auto& owned_provenance=decoded.header.provenance;
    for(const auto* text:{&owned_provenance.scenario_profile,&owned_provenance.scenario_digest,&owned_provenance.resolved_plan_digest,
        &owned_provenance.workload_id,&owned_provenance.first_variant_id,&owned_provenance.second_variant_id})charge(*text);
    require(owned<=limits.retained,Error::capacity);decoded.retained_bytes=owned;
    return decoded;
}
namespace {
auto u32(const J& value)->std::uint32_t {const auto n=number(value);require(n<=UINT32_MAX,Error::schema);return static_cast<std::uint32_t>(n);}
auto boolean(const J& value)->bool {require(value.is_boolean(),Error::schema);return value.get<bool>();}
template<class T> auto stable(const J& value,Work& work)->T {
    auto parsed=T::parse(unhex(value,work));core::detail::ExpectedErrorCleanup cleanup{parsed};
    require(parsed.has_value(),Error::schema);return std::move(*parsed);
}
auto tid(const J& value)->platform_linux::LinuxThreadId {
    auto parsed=platform_linux::LinuxThreadId::create(number(value));core::detail::ExpectedErrorCleanup cleanup{parsed};
    require(parsed.has_value(),Error::schema);return *parsed;
}
auto affinity(const J& value,Work& work)->core::CpuSet {
    // Readiness admission requires singleton affinity. Bound before allocating.
    require(value.is_array()&&value.size()==1U,Error::association);work.debit(1U);
    return core::CpuSet{core::LinuxCpuId{u32(value.front())}};
}
auto ratio(const J& value)->memory::ExactRatio {
    fields(value,{"numerator","denominator"});const auto n=number(value.at("numerator")),d=number(value.at("denominator"));
    require(d&&n<=d,Error::schema);auto made=memory::ExactRatio::create(n,d);
    core::detail::ExpectedErrorCleanup cleanup{made};require(made.has_value(),Error::schema);return *made;
}
auto observed(const J& value,std::size_t& pages,const PairContextLimits& limits,Work& work)->memory::RegionPlacementObservation {
    fields(value,{"total_pages","resident_pages","unresolved_pages","resident_page_counts"});
    const auto total=number(value.at("total_pages")),resident=number(value.at("resident_pages")),unresolved=number(value.at("unresolved_pages"));
    require(resident<=total&&unresolved<=total-resident,Error::schema);
    const auto& rows=value.at("resident_page_counts");require(rows.is_array(),Error::schema);
    pages=add(pages,rows.size());require(pages<=limits.targets,Error::capacity);work.debit(rows.size());
    std::vector<memory::LinuxNodePageCount> result;result.reserve(rows.size());std::uint64_t sum{};
    for(const auto& row:rows){fields(row,{"linux_node","page_count"});const auto node=u32(row.at("linux_node"));const auto count=number(row.at("page_count"));
        require((result.empty()||result.back().linux_node().value()<node)&&count<=UINT64_MAX-sum,Error::schema);
        sum+=count;result.push_back(memory::detail::PairwiseMemoryAccess::node_count(core::LinuxNumaNodeId{node},count));}
    require(sum==resident,Error::association);
    return memory::detail::PairwiseMemoryAccess::observation(total,resident,unresolved,std::move(result));
}
} // namespace

auto PairContextDecoder::workers(const J& value,const PairContextLimits& limits,Work& work)
    ->std::vector<runtime::WorkerReadyEvidence> {
    require(value.is_array()&&value.size()<=limits.workers,Error::capacity);
    work.debit(multiply(value.size(),16U));std::vector<runtime::WorkerReadyEvidence> result;result.reserve(value.size());
    for(const auto& row:value){fields(row,{"worker_index","worker_id","stable_pu","stable_core","requested_cpu","tid","observed_affinity","observed_cpu"});
        const auto index=number(row.at("worker_index"));require(index<=SIZE_MAX,Error::schema);
        result.push_back({static_cast<std::size_t>(index),core::WorkerId{u32(row.at("worker_id"))},
            stable<topology::StableProcessingUnitId>(row.at("stable_pu"),work),stable<topology::StableCoreId>(row.at("stable_core"),work),
            core::LinuxCpuId{u32(row.at("requested_cpu"))},tid(row.at("tid")),affinity(row.at("observed_affinity"),work),core::LinuxCpuId{u32(row.at("observed_cpu"))}});}
    return result;
}
auto PairContextDecoder::control(const J& row,Work& work)->runtime::ServiceThreadReadyEvidence {
    fields(row,{"role","stable_pu","stable_core","assigned_cpu","tid","observed_affinity","observed_cpu"});
    require(string(row.at("role"))=="control_monitor",Error::association);work.debit(16U);
    return {runtime::ServiceRole::control_monitor,stable<topology::StableProcessingUnitId>(row.at("stable_pu"),work),
        stable<topology::StableCoreId>(row.at("stable_core"),work),core::LinuxCpuId{u32(row.at("assigned_cpu"))},
        tid(row.at("tid")),affinity(row.at("observed_affinity"),work),core::LinuxCpuId{u32(row.at("observed_cpu"))}};
}
auto PairContextDecoder::memory(const J& value,const DecodedContext& context,bool second,
    const PairContextLimits& limits,Work& work)->std::vector<block_evidence::MemoryPreparationRow> {
    const auto expected=context.lease.memory_requests(second);
    require(value.is_array()&&value.size()==expected.size()&&value.size()<=limits.workers,Error::capacity);
    std::vector<block_evidence::MemoryPreparationRow> result;result.reserve(value.size());std::size_t targets{},pages{};
    for(std::size_t i{};i<value.size();++i){work.debit(64U);const auto& row=value[i];
        fields(row,{"request","binding","initialization","state","observation","evaluation","qualification","protection"});
        const auto& request=row.at("request");fields(request,{"worker_id","requested_bytes","page_size","placement_policy","page_mode","numa_targets","placement_threshold","prefault","verify_before","verify_after"});
        require(string(request.at("placement_policy"))=="bind"&&string(request.at("page_mode"))=="base",Error::schema);
        const auto& nodes=request.at("numa_targets");require(nodes.is_array()&&nodes.size()==expected[i].resolved_nodes.size(),Error::association);
        targets=add(targets,nodes.size());require(targets<=limits.targets,Error::capacity);work.debit(nodes.size());
        std::vector<memory::RegionNumaTarget> typed;typed.reserve(nodes.size());
        for(std::size_t n{};n<nodes.size();++n){const auto& node=nodes[n];fields(node,{"stable_node","linux_node","topology_node"});
            auto stable_node=stable<topology::StableNumaNodeId>(node.at("stable_node"),work);
            const core::LinuxNumaNodeId linux_node{u32(node.at("linux_node"))};const core::NumaNodeId source{u32(node.at("topology_node"))};
            require(stable_node==expected[i].request.nodes()[n].stable_numa_node_id()&&linux_node==expected[i].request.nodes()[n].linux_numa_node()&&source==expected[i].resolved_nodes[n],Error::association);
            typed.push_back({std::move(stable_node),linux_node,source});}
        auto made=memory::RegionRequest::create({core::WorkerId{u32(request.at("worker_id"))},core::ByteCount::from_base_units(number(request.at("requested_bytes"))),
            core::ByteCount::from_base_units(number(request.at("page_size"))),memory::PlacementPolicy::bind,memory::PageMode::base,std::move(typed),ratio(request.at("placement_threshold")),
            boolean(request.at("prefault")),boolean(request.at("verify_before")),boolean(request.at("verify_after"))});
        core::detail::ExpectedErrorCleanup made_cleanup{made};require(made.has_value(),Error::child);
        const auto& binding=row.at("binding");fields(binding,{"policy","range","policy_call_accepted","migration_requested","strict_verification_requested"});
        require(string(binding.at("policy"))=="bind"&&string(binding.at("range"))=="full_mapping"&&boolean(binding.at("policy_call_accepted"))&&
            !boolean(binding.at("migration_requested"))&&!boolean(binding.at("strict_verification_requested")),Error::association);
        const auto& init=row.at("initialization");fields(init,{"range","prefault_requested","prefault_pass_performed","initialization_completed","padding_touched"});
        require(string(init.at("range"))=="logical_requested_region"&&boolean(init.at("prefault_requested"))==made->prefault()&&boolean(init.at("prefault_pass_performed"))==made->prefault()&&
            boolean(init.at("initialization_completed"))&&!boolean(init.at("padding_touched")),Error::association);
        const auto& protection=row.at("protection");fields(protection,{"protection_kind","range","protection_accepted","writable","executable"});
        require(string(protection.at("protection_kind"))=="read_only"&&string(protection.at("range"))=="full_mapping"&&boolean(protection.at("protection_accepted"))&&
            !boolean(protection.at("writable"))&&!boolean(protection.at("executable")),Error::association);
        const auto& state=string(row.at("state"));require(state=="skipped"||state=="observed",Error::schema);
        std::optional<memory::RegionPlacementObservation> observation;
        if(!row.at("observation").is_null())observation=observed(row.at("observation"),pages,limits,work);
        std::optional<memory::RegionPlacementEvaluation> evaluation;
        if(!row.at("evaluation").is_null()){const auto& e=row.at("evaluation");fields(e,{"requested_node_pages","unexpected_node_pages","unresolved_pages","observed_ratio"});
            evaluation=memory::detail::PairwiseMemoryAccess::placement_evaluation(number(e.at("requested_node_pages")),number(e.at("unexpected_node_pages")),number(e.at("unresolved_pages")),ratio(e.at("observed_ratio")));}
        std::optional<memory::PlacementQualificationState> qualification;
        if(!row.at("qualification").is_null()){const auto& q=string(row.at("qualification"));require(q=="skipped"||q=="qualified"||q=="not_qualified",Error::schema);
            qualification=q=="skipped"?memory::PlacementQualificationState::skipped:q=="qualified"?memory::PlacementQualificationState::qualified:memory::PlacementQualificationState::not_qualified;}
        result.push_back({std::move(*made),memory::detail::PairwiseMemoryAccess::binding_evidence(),memory::detail::PairwiseMemoryAccess::initialization_evidence(boolean(init.at("prefault_requested"))),
            state=="skipped"?memory::PreRunDiagnosticState::skipped:memory::PreRunDiagnosticState::observed,std::move(observation),std::move(evaluation),qualification,memory::detail::PairwiseMemoryAccess::protection_evidence()});
    }
    return result;
}


auto PairContextDecoder::monitoring(block_evidence::PairPrefix& prefix,std::uint64_t sequence,const journal::Timestamp& time,
    const J& value,const DecodedContext& context,const PairContextLimits& limits,Work& work)->std::expected<void,block_evidence::Error> {
    using namespace monitoring;using namespace runtime::detail;
    fields(value,{"domain","policy","channels","service","physical","effective","provenance","capacity","profile","workers","frequency"});
    const auto& domain_token=string(value.at("domain"));
    require(domain_token=="synthetic"||domain_token=="source",Error::association);
    const auto fact_domain=domain_token=="synthetic"?MonitoringPreparationFactDomain::synthetic:MonitoringPreparationFactDomain::source;
    require(domain_token=="synthetic"||context.lease.decoded_domain()==process_subject::Domain::supplied_observation,Error::association);
    const auto* expected=context.lease.monitoring_preparation();require(expected,Error::association);
    const auto& policy=expected->policy;const auto& p=value.at("policy");
    fields(p,{"page_faults","context_switches","temperature","frequency","before_observation","after_observation","during_observation","during_interval","optional_unsupported"});
    const std::array<SV,4> names{"page_faults","context_switches","temperature","frequency"};
    for(std::size_t i{};i<names.size();++i){const auto d=policy.disposition(static_cast<MonitoringChannel>(i));
        require(string(p.at(names[i]))==(d==MonitoringChannelDisposition::disabled?"disabled":d==MonitoringChannelDisposition::optional?"optional":"required"),Error::association);}
    require(boolean(p.at("before_observation"))==policy.before_observation()&&boolean(p.at("after_observation"))==policy.after_observation()&&
        boolean(p.at("during_observation"))==policy.during_observation(),Error::association);
    if(policy.during_interval())equal_number(p.at("during_interval"),policy.during_interval()->value(),work);
    else require(p.at("during_interval").is_null(),Error::association);
    const auto unsupported=policy.optional_unsupported();require(string(p.at("optional_unsupported"))==
        (unsupported==OptionalUnavailableSourcePolicy::fail?"fail":unsupported==OptionalUnavailableSourcePolicy::warn?"warn":"disable"),Error::association);
    require(string(value.at("physical"))==expected->physical.to_string()&&string(value.at("effective"))==expected->effective.to_string(),Error::identity);
    fields(value.at("provenance"),{"kind","authority_digest"});require(string(value.at("provenance").at("kind"))=="scenario",Error::identity);
    equal_digest(value.at("provenance").at("authority_digest"),context.monitoring_provenance.authority_digest(),work);
    const auto capacity=PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity();
    const auto profile=PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity();
    equal_digest(value.at("capacity"),capacity,work);equal_digest(value.at("profile"),profile,work);
    const auto& channels=value.at("channels");require(channels.is_array()&&channels.size()==4U,Error::schema);
    std::array<MonitoringPreparationChannelFact,4> channel_values{};std::array<bool,4> supported{};
    for(std::size_t i{};i<4U;++i){const auto& row=channels[i];fields(row,{"support","effective"});const auto& support=string(row.at("support"));const auto& effective=string(row.at("effective"));
        require(support=="not_requested"||support=="supported"||support=="unsupported",Error::schema);
        require(effective=="active"||effective=="disabled"||effective=="unsupported_warning",Error::schema);
        const auto disposition=policy.disposition(static_cast<MonitoringChannel>(i));supported[i]=support=="supported";
        if(disposition==MonitoringChannelDisposition::disabled)require(support=="not_requested"&&effective=="disabled",Error::association);
        else if(supported[i])require(effective=="active",Error::association);
        else require(i>=2U&&support=="unsupported"&&disposition==MonitoringChannelDisposition::optional&&unsupported!=OptionalUnavailableSourcePolicy::fail&&
            effective==(unsupported==OptionalUnavailableSourcePolicy::warn?"unsupported_warning":"disabled"),Error::association);
        channel_values[i]={support=="not_requested"?MonitoringPreparationSupport::not_requested:supported[i]?MonitoringPreparationSupport::supported:MonitoringPreparationSupport::unsupported,
            effective=="active"?MonitoringEffectiveChannelDisposition::active:effective=="disabled"?MonitoringEffectiveChannelDisposition::disabled:MonitoringEffectiveChannelDisposition::unsupported_warning};
    }
    std::optional<MonitoringPreparationServiceFact> service_value;const auto& service_json=value.at("service");
    require(!service_json.is_null()==(supported[1]||supported[2]||supported[3]),Error::association);
    if(!service_json.is_null()){fields(service_json,{"assigned","verified_singleton","observed"});
        service_value.emplace(core::LinuxCpuId{u32(service_json.at("assigned"))},core::LinuxCpuId{u32(service_json.at("verified_singleton"))},core::LinuxCpuId{u32(service_json.at("observed"))});}
    const auto& worker_rows=value.at("workers");require(worker_rows.is_array()&&!worker_rows.empty()&&worker_rows.size()<=limits.workers,Error::capacity);
    work.debit(multiply(worker_rows.size(),8U));std::vector<MonitoringWorkerScope> workers;workers.reserve(worker_rows.size());
    for(const auto& row:worker_rows){fields(row,{"worker","cpu","tid"});workers.push_back(MonitoringWorkerScope::create(core::WorkerId{u32(row.at("worker"))},tid(row.at("tid")),core::LinuxCpuId{u32(row.at("cpu"))}));}
    const auto& frequencies=value.at("frequency");require(frequencies.is_array()&&frequencies.size()<=limits.capacity.domains&&supported[3]==!frequencies.empty(),Error::capacity);
    std::vector<block_evidence::MonitoringPreparationFrequency> frequency;frequency.reserve(frequencies.size());std::size_t members{},texts{};
    for(const auto& row:frequencies){fields(row,{"cpus","kind","driver","governor","minimum_hz","maximum_hz"});
        const auto& cpus=row.at("cpus");require(cpus.is_array(),Error::schema);members=add(members,cpus.size());require(members<=limits.capacity.cpus,Error::capacity);work.debit(cpus.size());
        std::vector<core::LinuxCpuId> ids;ids.reserve(cpus.size());for(const auto& cpu:cpus)ids.emplace_back(u32(cpu));
        const auto& kind=string(row.at("kind"));require(kind=="hardware_current"||kind=="scaling_current",Error::schema);
        texts=add(texts,add(string(row.at("driver")).size(),string(row.at("governor")).size()));require(texts<=limits.text,Error::capacity);
        frequency.push_back({std::move(ids),kind=="hardware_current"?FrequencySourceKind::hardware_current:FrequencySourceKind::scaling_current,
            unhex(row.at("driver"),work),unhex(row.at("governor"),work),number(row.at("minimum_hz")),number(row.at("maximum_hz"))});}
    const block_evidence::MonitoringFactsView facts{fact_domain,policy,channel_values,service_value,
        expected->physical,expected->effective,context.monitoring_provenance,capacity,profile,workers,frequency.size(),&frequency,
        [](const void* data,std::size_t index) noexcept->std::optional<MonitoringPreparationFrequencyView>{
            const auto& rows=*static_cast<const std::vector<block_evidence::MonitoringPreparationFrequency>*>(data);
            if(index>=rows.size())return std::nullopt;
            const auto& row=rows[index];return MonitoringPreparationFrequencyView{row.cpus,row.kind,row.driver,row.governor,row.minimum_hz,row.maximum_hz};}};
    return prefix.monitoring_preparation_facts(sequence,time,facts);
}

namespace {
auto slice(const J& value,std::size_t bound)->runtime::detail::PreparationSlice {
    fields(value,{"offset","size"});const auto offset=number(value.at("offset")),size=number(value.at("size"));
    require(offset<=bound&&size<=bound-offset,Error::capacity);return {static_cast<std::size_t>(offset),static_cast<std::size_t>(size)};
}
auto graph_policies(const J& value,const DecodedContext& context,Work& work)->runtime::detail::PreparationGraphPolicies {
    fields(value,{"request_body","header","calibration","warmup","validity","implementation","root_seed","latency_seed","calibration_enabled"});
    require(work.equal(string(value.at("request_body")),context.request_body),Error::identity);
    auto request=runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::parse_request(string(value.at("request_body")));require(request.has_value(),Error::child);
    auto header=context_header(value.at("header"));const auto* preparation=context.lease.complete_preparation();require(preparation,Error::association);
    const auto& a=header.provenance;const auto& b=context.header.provenance;
    require(a.experiment_id==b.experiment_id&&work.equal(a.scenario_profile,b.scenario_profile)&&work.equal(a.scenario_digest,b.scenario_digest)&&
        work.equal(a.resolved_plan_digest,b.resolved_plan_digest)&&work.equal(a.workload_id,b.workload_id)&&work.equal(a.first_variant_id,b.first_variant_id)&&work.equal(a.second_variant_id,b.second_variant_id),Error::identity);
    const auto& m=request->first().execution_inputs().measurement();const auto& k=request->first().execution_inputs().kernel();
    require(m==preparation->measurement&&k==context.lease.kernel(false)&&request->second().execution_inputs().kernel()==context.lease.kernel(true)&&
        header.statistics_policy==context.header.statistics_policy&&header.monitoring_policy==context.header.monitoring_policy&&
        header.repetition_policy==context.header.repetition_policy&&header.comparison_order==context.header.comparison_order,Error::association);
    const auto& c=value.at("calibration");fields(c,{"minimum","target","maximum"});
    const auto& w=value.at("warmup");fields(w,{"minimum","maximum","window","threshold"});
    const auto& v=value.at("validity");fields(v,{"numerator","denominator"});
    equal_number(c.at("minimum"),m.observation_minimum_duration().value(),work);equal_number(c.at("target"),m.observation_target_duration().value(),work);
    equal_number(c.at("maximum"),m.observation_maximum_duration().value(),work);
    equal_number(w.at("minimum"),m.warmup_minimum(),work);equal_number(w.at("maximum"),m.warmup_maximum(),work);equal_number(w.at("window"),m.warmup_stability_window(),work);
    fields(w.at("threshold"),{"numerator","denominator"});equal_number(w.at("threshold").at("numerator"),m.warmup_stability_threshold().numerator(),work);
    equal_number(w.at("threshold").at("denominator"),m.warmup_stability_threshold().denominator(),work);
    equal_number(v.at("numerator"),m.maximum_harness_overhead().numerator(),work);equal_number(v.at("denominator"),m.maximum_harness_overhead().denominator(),work);
    auto calibration=measurement::CalibrationDurationPolicy::create(m.observation_minimum_duration(),m.observation_target_duration(),m.observation_maximum_duration());core::detail::ExpectedErrorCleanup c_cleanup{calibration};
    auto threshold=measurement::WarmupStabilityThreshold::create(m.warmup_stability_threshold().numerator(),m.warmup_stability_threshold().denominator());core::detail::ExpectedErrorCleanup threshold_cleanup{threshold};
    auto overhead=measurement::MaximumHarnessOverhead::create(m.maximum_harness_overhead().numerator(),m.maximum_harness_overhead().denominator());core::detail::ExpectedErrorCleanup overhead_cleanup{overhead};
    require(calibration&&threshold&&overhead,Error::child);
    auto warmup=measurement::WarmupPolicy::create(m.warmup_minimum(),m.warmup_maximum(),m.warmup_stability_window(),*threshold);core::detail::ExpectedErrorCleanup w_cleanup{warmup};require(warmup.has_value(),Error::child);
    const auto& implementation_token=string(value.at("implementation"));
    const auto family=k.index()==1U?kernel::KernelFamily::latency:kernel::KernelFamily::bandwidth;
    const auto implementation=implementation_token=="0"||implementation_token=="1"?
        std::optional<kernel::KernelImplementationId>{kernel::KernelImplementationId{static_cast<unsigned>(number(value.at("implementation")))}}:
        kernel::parse_kernel_implementation_id(implementation_token,family);
    require(implementation&&implementation->family()==family,Error::schema);
    require(work.equal(kernel::kernel_implementation_id_token(*implementation),
        kernel::kernel_implementation_id_token(context.implementation)),Error::association);
    return {*calibration,*warmup,header.repetition_policy,header.comparison_order,measurement::ObservationValidityPolicy::create(*overhead),
        header.statistics_policy,header.monitoring_policy,k,*implementation,number(value.at("root_seed")),number(value.at("latency_seed")),boolean(value.at("calibration_enabled"))};
}
}
auto PairContextDecoder::graph(block_evidence::PairPrefix& prefix,std::uint64_t sequence,const journal::Timestamp& time,
    const J& value,const DecodedContext& context,const PairContextLimits& limits,Work& work)->std::expected<void,block_evidence::Error> {
    using namespace runtime::detail;
    fields(value,{"policies","monitoring","provenance","measurement_request_body","experiment","first_request","second_request","workers","memory","kernels","targets","pages","frequency","cpus","strings","frequency_count"});
    const auto* ready=prefix.readiness_event();const auto* first=prefix.memory_preparation_event(false);const auto* second=prefix.memory_preparation_event(true);
    const auto* monitoring=prefix.monitoring_preparation_event();require(ready&&first&&second&&monitoring,Error::association);
    const auto count=ready->workers.size();require(count&&count<=limits.workers,Error::capacity);
    for(const auto key:{"provenance","workers","memory","kernels","targets","pages","frequency","cpus"})require(value.at(key).is_array(),Error::schema);
    require(value.at("provenance").size()==8U&&value.at("workers").size()==count&&value.at("memory").size()==multiply(count,2U)&&value.at("kernels").size()==multiply(count,2U)&&
        value.at("targets").size()<=limits.targets&&value.at("pages").size()<=limits.targets&&value.at("frequency").size()<=limits.capacity.domains&&
        value.at("cpus").size()<=limits.capacity.cpus&&string(value.at("strings")).size()<=limits.text,Error::capacity);
    const auto stock=add(multiply(count,sizeof(PreparationWorkerCell)+2U*sizeof(PreparationMemoryCell)+2U*sizeof(PreparationKernelCell)),
        add(add(multiply(value.at("targets").size(),sizeof(PreparationTargetCell)),multiply(value.at("pages").size(),sizeof(PreparationPageCountCell))),add(multiply(value.at("frequency").size(),sizeof(PreparationFrequencyCell)),
        add(multiply(value.at("cpus").size(),sizeof(core::LinuxCpuId)),string(value.at("strings")).size()))));
    require(stock<=limits.retained&&multiply(stock,3U)<=limits.simultaneous,Error::capacity);work.reserve_peak(multiply(stock,3U));work.debit(stock);
    auto result=std::make_shared<block_evidence::DecodedPreparationGraph>();result->retained=add(stock,sizeof(*result));result->domain=monitoring->domain;
    result->workers.reserve(count);result->memory.reserve(multiply(count,2U));result->kernels.reserve(multiply(count,2U));
    result->targets.reserve(value.at("targets").size());result->pages.reserve(value.at("pages").size());
    result->frequency.reserve(value.at("frequency").size());result->cpus.reserve(value.at("cpus").size());
    result->policies.emplace(graph_policies(value.at("policies"),context,work));
    require(work.equal(string(value.at("measurement_request_body")),context.request_body),Error::identity);
    auto request=PairwiseMonitoringHostCapacityStartupAccess::parse_request(string(value.at("measurement_request_body")));require(request.has_value(),Error::child);
    require(request->first().execution_inputs().measurement()==context.lease.complete_preparation()->measurement,Error::association);
    result->measurement.emplace(request->first().execution_inputs().measurement());
    result->experiment=number(value.at("experiment"));result->first_request=number(value.at("first_request"));result->second_request=number(value.at("second_request"));
    const auto& identity=value.at("monitoring");fields(identity,{"channels","service","physical","effective","provenance","capacity","profile"});
    require(identity.at("channels").is_array()&&identity.at("channels").size()==4U,Error::schema);
    for(std::size_t i{};i<4U;++i){const auto& channel=identity.at("channels")[i];fields(channel,{"support","effective"});const auto& expected=monitoring->channels[i];
        require(string(channel.at("support"))==(expected.support==MonitoringPreparationSupport::not_requested?"not_requested":expected.support==MonitoringPreparationSupport::supported?"supported":"unsupported")&&
            string(channel.at("effective"))==(expected.effective==monitoring::MonitoringEffectiveChannelDisposition::active?"active":expected.effective==monitoring::MonitoringEffectiveChannelDisposition::disabled?"disabled":"unsupported_warning"),Error::association);}
    const auto& service=identity.at("service");require(service.is_null()==!monitoring->service,Error::association);
    if(monitoring->service){fields(service,{"assigned","verified_singleton","observed"});equal_number(service.at("assigned"),monitoring->service->assigned.value(),work);
        equal_number(service.at("verified_singleton"),monitoring->service->verified_singleton.value(),work);equal_number(service.at("observed"),monitoring->service->observed.value(),work);}
    require(string(identity.at("physical"))==monitoring->physical.to_string()&&string(identity.at("effective"))==monitoring->effective.to_string(),Error::identity);
    fields(identity.at("provenance"),{"kind","authority_digest"});require(string(identity.at("provenance").at("kind"))=="scenario",Error::identity);
    equal_digest(identity.at("provenance").at("authority_digest"),monitoring->provenance.authority_digest(),work);
    equal_digest(identity.at("capacity"),monitoring->capacity,work);equal_digest(identity.at("profile"),monitoring->profile,work);
    result->monitoring.emplace(monitoring->channels,monitoring->service,monitoring->physical,monitoring->effective,monitoring->provenance,monitoring->capacity,monitoring->profile);
    const auto text=unhex(value.at("strings"),work);result->strings.assign(text.begin(),text.end());
    for(std::size_t i{};i<8U;++i)result->provenance[i]=slice(value.at("provenance")[i],result->strings.size());
    for(const auto& cpu:value.at("cpus"))result->cpus.emplace_back(u32(cpu));
    for(const auto& row:value.at("targets")){fields(row,{"linux_node","topology_node","stable"});result->targets.push_back({u32(row.at("linux_node")),u32(row.at("topology_node")),slice(row.at("stable"),result->strings.size())});}
    for(const auto& row:value.at("pages")){fields(row,{"node","count"});result->pages.push_back({u32(row.at("node")),number(row.at("count"))});}
    for(const auto& row:value.at("workers")){fields(row,{"index","worker","tid","requested_cpu","observed_cpu","pu","core","affinity","captured"});
        const auto index=number(row.at("index"));require(index==result->workers.size()&&boolean(row.at("captured")),Error::association);
        result->workers.push_back({static_cast<std::size_t>(index),number(row.at("worker")),number(row.at("tid")),u32(row.at("requested_cpu")),u32(row.at("observed_cpu")),
            slice(row.at("pu"),result->strings.size()),slice(row.at("core"),result->strings.size()),slice(row.at("affinity"),result->cpus.size()),true});}
    for(const auto& row:value.at("kernels")){fields(row,{"index","worker","second","latency","captured"});const auto position=result->kernels.size();
        const auto index=number(row.at("index"));require(index==position%count&&boolean(row.at("second"))==(position>=count)&&boolean(row.at("captured")),Error::association);
        result->kernels.push_back({static_cast<std::size_t>(index),number(row.at("worker")),position>=count,boolean(row.at("latency")),true});}
    for(const auto& row:value.at("memory")){fields(row,{"index","worker","bytes","page_size","numerator","denominator","second","prefault","verify_before","verify_after","captured","page_mode","placement_policy","targets","page_storage","binding","initialization","query","observation","evaluation","qualification","protection"});
        const auto position=result->memory.size();const bool side=position>=count;const auto index=number(row.at("index"));
        require(index==position%count&&boolean(row.at("second"))==side&&boolean(row.at("captured"))&&string(row.at("page_mode"))=="base"&&string(row.at("placement_policy"))=="bind",Error::association);
        const auto& source=(side?second:first)->workers[static_cast<std::size_t>(index)];
        const auto& binding=row.at("binding");fields(binding,{"policy","range","policy_call_accepted","migration_requested","strict_verification_requested"});
        require(string(binding.at("policy"))=="bind"&&string(binding.at("range"))=="full_mapping"&&boolean(binding.at("policy_call_accepted"))&&
            !boolean(binding.at("migration_requested"))&&!boolean(binding.at("strict_verification_requested")),Error::association);
        const auto& init=row.at("initialization");fields(init,{"range","prefault_requested","prefault_pass_performed","initialization_completed","padding_touched"});
        require(string(init.at("range"))=="logical_requested_region"&&boolean(init.at("prefault_requested"))==source.initialization.prefault_requested()&&
            boolean(init.at("prefault_pass_performed"))==source.initialization.prefault_pass_performed()&&boolean(init.at("initialization_completed"))&&!boolean(init.at("padding_touched")),Error::association);
        const auto& protection=row.at("protection");fields(protection,{"protection_kind","range","protection_accepted","writable","executable"});
        require(string(protection.at("protection_kind"))=="read_only"&&string(protection.at("range"))=="full_mapping"&&boolean(protection.at("protection_accepted"))&&
            !boolean(protection.at("writable"))&&!boolean(protection.at("executable")),Error::association);
        const auto& query=string(row.at("query"));require(query=="skipped"||query=="observed",Error::schema);
        const auto& qualification=string(row.at("qualification"));require(qualification=="skipped"||qualification=="qualified"||qualification=="not_qualified",Error::schema);
        PreparationMemoryCell cell;cell.index=static_cast<std::size_t>(index);cell.worker=number(row.at("worker"));cell.bytes=number(row.at("bytes"));cell.page_size=number(row.at("page_size"));
        cell.numerator=number(row.at("numerator"));cell.denominator=number(row.at("denominator"));cell.second=side;cell.prefault=boolean(row.at("prefault"));
        cell.verify_before=boolean(row.at("verify_before"));cell.verify_after=boolean(row.at("verify_after"));cell.captured=true;cell.page_mode=memory::PageMode::base;cell.placement_policy=memory::PlacementPolicy::bind;
        cell.targets=slice(row.at("targets"),result->targets.size());cell.page_storage=slice(row.at("page_storage"),result->pages.size());
        cell.binding=source.binding;cell.initialization=source.initialization;cell.protection=source.protection;
        cell.query=query=="skipped"?memory::PreRunPlacementQueryState::skipped:memory::PreRunPlacementQueryState::observed;
        cell.qualification=qualification=="skipped"?memory::PlacementQualificationState::skipped:qualification=="qualified"?memory::PlacementQualificationState::qualified:memory::PlacementQualificationState::not_qualified;
        if(!row.at("observation").is_null()){const auto& o=row.at("observation");fields(o,{"total","resident","unresolved","counts"});auto counts=slice(o.at("counts"),result->pages.size());
            require(counts.offset==cell.page_storage.offset&&counts.size<=cell.page_storage.size,Error::association);
            cell.observation.emplace(number(o.at("total")),number(o.at("resident")),number(o.at("unresolved")),counts);}
        if(!row.at("evaluation").is_null()){const auto& e=row.at("evaluation");fields(e,{"requested","unexpected","unresolved","numerator","denominator"});
            cell.evaluation.emplace(number(e.at("requested")),number(e.at("unexpected")),number(e.at("unresolved")),number(e.at("numerator")),number(e.at("denominator")));}
        result->memory.push_back(std::move(cell));
    }
    const auto frequency_count=number(value.at("frequency_count"));require(frequency_count<=value.at("frequency").size(),Error::capacity);
    result->frequency_count=static_cast<std::size_t>(frequency_count);
    for(const auto& row:value.at("frequency")){fields(row,{"cpus","driver","governor","kind","minimum","maximum"});const auto& kind=string(row.at("kind"));require(kind=="hardware_current"||kind=="scaling_current",Error::schema);
        result->frequency.push_back({slice(row.at("cpus"),result->cpus.size()),slice(row.at("driver"),result->strings.size()),slice(row.at("governor"),result->strings.size()),
            kind=="hardware_current"?monitoring::FrequencySourceKind::hardware_current:monitoring::FrequencySourceKind::scaling_current,number(row.at("minimum")),number(row.at("maximum"))});}
    const auto disjoint=[&](std::size_t n,const auto& get){work.debit(multiply(n,n));
        for(std::size_t i{};i<n;++i){const auto a=get(i);if(!a.size)continue;
            for(std::size_t j{};j<i;++j){const auto b=get(j);if(!b.size)continue;
                require(a.offset>=b.offset+b.size||b.offset>=a.offset+a.size,Error::association);}}};
    disjoint(result->memory.size(),[&](std::size_t i){return result->memory[i].targets;});
    disjoint(result->memory.size(),[&](std::size_t i){return result->memory[i].page_storage;});
    disjoint(add(count,result->frequency.size()),[&](std::size_t i){return i<count?result->workers[i].affinity:result->frequency[i-count].cpus;});
    const auto text_slices=add(8U,add(multiply(count,2U),add(result->targets.size(),multiply(result->frequency.size(),2U))));
    disjoint(text_slices,[&](std::size_t i){if(i<8U)return result->provenance[i];i-=8U;
        if(i<2U*count)return i%2U?result->workers[i/2U].core:result->workers[i/2U].pu;
        i-=2U*count;if(i<result->targets.size())return result->targets[i].stable;i-=result->targets.size();
        return i%2U?result->frequency[i/2U].governor:result->frequency[i/2U].driver;});
    return prefix.preparation_association_facts(sequence,time,block_evidence::PreparationFactsLease{std::move(result)});
}

auto PairContextDecoder::observation(block_evidence::PairPrefix& prefix,std::uint64_t sequence,const journal::Timestamp& handoff,
    bool failed,SV component,const J& value,const DecodedContext& context,const error_serialization::Limits& error_limits,Work& work)
    ->std::expected<void,block_evidence::Error> {
    namespace timing=observation_timing;
    require(value.is_object(),Error::schema);
    for(const auto key:{"identity","boundary","disposition","calibration_ref"})require(value.contains(key),Error::schema);
    const auto& boundary=string(value.at("boundary"));require(boundary==(failed?"failed_attempt_start":"completed_enclosing_start"),Error::association);
    const auto& disposition=string(value.at("disposition"));require(disposition=="available"||disposition=="not_recorded"||disposition=="acquisition_failed",Error::schema);
    const bool available=disposition=="available",error=disposition=="acquisition_failed";
    require(value.contains("clock")==!failed&&value.contains("raw_start")==!failed&&value.contains("utc")==available&&value.contains("error")==error&&
        value.size()==4U+(failed?0U:2U)+(available?1U:0U)+(error?1U:0U),Error::schema);
    const auto* calibration=prefix.calibration_event();require(calibration,Error::association);equal_number(value.at("calibration_ref"),calibration->sequence,work);
    const auto& identity=value.at("identity");fields(identity,{"run","domain","experiment","scenario_profile","scenario_digest","resolved_plan_digest","workload","first_variant","second_variant","family","block","slot","variant","observation"});
    const auto observed=context.lease.decoded_domain()==process_subject::Domain::supplied_observation;
    const auto domain=observed?block_evidence::Domain::supplied_observation:block_evidence::Domain::synthetic_execution;
    const auto& p=context.lease.provenance();require(work.equal(string(identity.at("run")),context.lease.run().text())&&
        string(identity.at("domain"))==(observed?"supplied_observation":"synthetic_execution"),Error::identity);
    equal_number(identity.at("experiment"),p.experiment_id.value(),work);
    same_private(identity.at("scenario_profile"),p.scenario_profile,work);same_private(identity.at("scenario_digest"),p.scenario_digest,work);same_private(identity.at("resolved_plan_digest"),p.resolved_plan_digest,work);
    same_private(identity.at("workload"),p.workload_id,work);same_private(identity.at("first_variant"),p.first_variant_id,work);same_private(identity.at("second_variant"),p.second_variant_id,work);
    const auto family=context.lease.kernel(false).index()==0U?timing::Family::bandwidth:timing::Family::latency;
    require(string(identity.at("family"))==(family==timing::Family::bandwidth?"bandwidth":"latency"),Error::identity);
    const auto& variant=string(identity.at("variant"));require(variant=="first"||variant=="second",Error::schema);
    timing::Input input{{&context.lease.run(),domain,p.experiment_id,p.scenario_profile,p.scenario_digest,p.resolved_plan_digest,
        p.workload_id,p.first_variant_id,p.second_variant_id,family,kernel::DependentLoadLatencyBlockId{number(identity.at("block"))},number(identity.at("slot")),
        variant=="first"?measurement::ComparisonVariantSlot::first:measurement::ComparisonVariantSlot::second,core::ObservationId{number(identity.at("observation"))}},
        failed?timing::Boundary::failed_attempt_start:timing::Boundary::completed_enclosing_start,
        available?timing::Disposition::available:error?timing::Disposition::acquisition_failed:timing::Disposition::not_recorded};
    if(!failed){require(string(value.at("clock"))=="monotonic_raw",Error::schema);input.clock=timing::Clock::monotonic_raw;
        input.raw_start=platform_linux::RawMonotonicTimestamp::from_nanoseconds(number(value.at("raw_start")));}
    std::optional<journal::Timestamp> utc;
    if(available){auto parsed=journal::Timestamp::parse(string(value.at("utc")));core::detail::ExpectedErrorCleanup cleanup{parsed};require(parsed.has_value(),Error::schema);utc.emplace(std::move(*parsed));input.utc=&*utc;}
    if(error){const auto& bytes=string(value.at("error"));work.debit(bytes.size());
        work.reserve_peak(error_limits.consumer_simultaneous_bytes);error_serialization::Usage usage{};
        auto retained=error_serialization::detail::decode_value(bytes,error_limits,usage);require(retained.has_value(),Error::child);
        work.debit(add(usage.nodes,usage.text_bytes));input.error.emplace(std::move(*retained));}
    const auto measured=timing::detail::measure(input,context.lease,domain);require(measured.has_value(),Error::association);
    return failed?prefix.failed(sequence,handoff,component,input):prefix.factual(sequence,handoff,component,input);
}
}

namespace epycbench::results::event_profile::detail {
namespace {
auto event_reference(const J& value,SV kind,std::span<const Record> records,Kind expected,Work& work)->const Record& {
    fields(value,{"sequence","ordinal","kind"});work.debit(1U);
    require(string(value.at("kind"))==kind&&number(value.at("ordinal"))==0U,Error::reference);
    const auto sequence=number(value.at("sequence"));require(sequence&&sequence<=records.size(),Error::reference);
    const auto& record=records[static_cast<std::size_t>(sequence-1U)];require(record.kind==expected,Error::reference);return record;
}

auto component_value(wire::EvidenceComponentKind kind,SV bytes,const DecodedContext& context,const CompositionLimits& limits,Work& work)->wire::LogicalBody {
    const auto measured=wire::detail::count_component_wire_shape(bytes,limits.pair.component.depth);require(measured.has_value(),Error::child);
    require(measured->bytes<=limits.pair.component.canonical_bytes&&measured->nodes<=limits.pair.component.nodes&&
        measured->strings<=limits.pair.component.strings&&measured->elements<=limits.pair.component.array_elements,Error::capacity);
    const auto dom=wire::detail::component_dom_stock(measured->nodes,measured->strings);
    const auto parser=wire::detail::component_parser_stock(measured->bytes,measured->depth);
    const auto render=wire::detail::component_renderer_stock(measured->depth,measured->members,measured->bytes);
    require(dom&&parser&&render,Error::capacity);const auto peak=add(*dom,add(*parser,*render));
    require(peak<=limits.simultaneous,Error::capacity);work.reserve_peak(peak);work.debit(add(bytes.size(),measured->nodes));
    auto body=wire::decode_evidence_component(kind,wire::EvidenceComponentSide::first,bytes,context.lease.capacity());
    require(body.has_value(),Error::child);return std::move(*body);
}
auto sample_count(const J& capture,Work& work)->std::size_t {
    const auto& raw=capture.at("raw");std::size_t count{};
    for(const auto key:{"page_fault_before","page_fault_after","context_switches","temperature"})count=add(count,raw.at(key).size());
    for(const auto& group:raw.at("frequency")){work.debit(1U);count=add(count,group.at("acquisitions").size());}
    work.debit(count);return count;
}
void sample_references(const J& samples,std::size_t count,std::uint64_t capture,Work& work) {
    require(samples.is_array()&&samples.size()==count,Error::reference);
    for(std::size_t i{};i<count;++i){work.debit(1U);const auto& sample=samples[i];fields(sample,{"sequence","ordinal","kind"});
        require(number(sample.at("sequence"))==capture&&number(sample.at("ordinal"))==i&&string(sample.at("kind"))=="sample",Error::reference);}
}
auto terminal_value(const J& value,const DecodedContext& context,const CompositionLimits& limits)->runtime::PairwiseRunTerminalEvidence {
    if(string(value.at("kind"))=="normal"){
        const auto& outcome=value.at("repetition_outcome");const auto& policy=outcome.at("policy");const auto& expected=context.header.repetition_policy;
        require(number(policy.at("minimum_blocks"))==expected.minimum_blocks()&&number(policy.at("maximum_blocks"))==expected.maximum_blocks()&&
            number(policy.at("minimum_valid_observations_per_variant"))==expected.minimum_valid_observations_per_variant()&&
            policy.at("stopping_enabled").get<bool>()==expected.stopping_enabled(),Error::association);
        const auto& disposition=string(outcome.at("disposition"));
        const auto selected=disposition=="precision_target_reached"?measurement::RepetitionDisposition::precision_target_reached:
            disposition=="maximum_blocks_reached"?measurement::RepetitionDisposition::maximum_blocks_reached:measurement::RepetitionDisposition::selected_work_unavailable;
        return runtime::NormalRunTerminalEvidence{measurement::detail::MeasurementLeafAccess::repetition_outcome(
            core::ExperimentId{number(outcome.at("experiment_id"))},expected,selected,number(outcome.at("completed_blocks")),number(outcome.at("complete_valid_blocks")),
            number(outcome.at("valid_first_variant_observations")),number(outcome.at("valid_second_variant_observations")))};
    }
    require(string(value.at("kind"))=="operational",Error::schema);
    auto primary=wire::detail::reconstruct_finalized_error_value(value.at("primary_error"));require(primary.has_value(),Error::child);
    const auto& raw=value.at("secondary_errors");require(raw.size()<=limits.pair.terminal_errors,Error::capacity);
    std::vector<runtime::FinalizedEvidenceError> secondary;secondary.reserve(raw.size());
    for(const auto& row:raw){auto error=wire::detail::reconstruct_finalized_error_value(row);require(error.has_value(),Error::child);secondary.push_back(std::move(*error));}
    std::optional<kernel::DependentLoadLatencyBlockId> active;if(!value.at("active_block_id").is_null())active.emplace(number(value.at("active_block_id")));
    return runtime::OperationalRunTerminalEvidence{std::move(*primary),std::move(secondary),active};
}
void project_analysis(ComposedPair& pair,SV supplied,const CompositionLimits& limits,Work& work) {
    namespace stock=wire::detail;
    require(pair.prefix&&pair.context&&pair.root_record&&!pair.analysis,Error::association);
    const auto& bound=limits.analysis;const auto& cap=bound.values;const auto bytes=pair.prefix->canonical_root();
    require(cap.records&&supplied.size()<=cap.canonical_bytes&&supplied.size()<=cap.input_bytes&&bytes.size()<=cap.root_bytes&&cap.root_depth<=64U,Error::capacity);
    const auto measured=stock::count_component_wire_shape(bytes,cap.root_depth);require(measured.has_value(),Error::child);
    require(measured->nodes<=cap.root_nodes&&measured->strings<=cap.root_strings&&measured->elements<=cap.root_elements,Error::capacity);
    const auto dom=stock::component_dom_stock(measured->nodes,measured->strings);
    const auto parser=stock::component_parser_stock(measured->bytes,measured->depth);
    const auto renderer=stock::component_renderer_stock(measured->depth,measured->members,measured->bytes);
    require(dom&&parser&&renderer,Error::capacity);
    auto phase=[&](std::size_t extra){const auto peak=add(pair.prefix->usage().retained,add(*dom,add(*parser,add(*renderer,extra))));
        require(peak<=cap.simultaneous_bytes&&peak<=limits.simultaneous,Error::capacity);work.reserve_peak(peak);work.debit(extra);};
    phase(bytes.size());work.debit(add(bytes.size(),measured->nodes));
    auto logical=wire::decode_evidence_body(bytes,pair.context->lease.capacity());require(logical.has_value(),Error::child);
    auto counted=stock::count_root_reconstruction(logical->value());require(counted&&stock::check_root_reconstruction_limits(*counted,bound.root),Error::capacity);
    phase(add(counted->typed,add(counted->scratch,counted->ledger)));
    auto root=stock::reconstruct_finalized_evidence_bounded(*logical,pair.context->lease.cpus(),bound.root);require(root.has_value(),Error::child);
    pair_analysis::detail::ProjectionDimensions dimensions{};dimensions.blocks=root->blocks().size();dimensions.historical=root->stopping_evaluations().size();
    for(const auto& block:root->blocks())if(block.completion==runtime::BlockAttemptCompletion::complete_valid)++dimensions.valid;
    if(root->finalization())dimensions.workers=add(root->finalization()->first.workers.size(),root->finalization()->second.workers.size());
    dimensions.metric_size=std::max(sizeof(statistics::Bandwidth),sizeof(statistics::DependentLoadLatency));
    if(dimensions.valid&&std::holds_alternative<runtime::NormalRunTerminalEvidence>(root->terminal()))
        dimensions.comparison_replicates=root->statistics_policy().comparison_bootstrap_policy().replicate_count().value();
    if(dimensions.historical)dimensions.stopping_replicates=root->statistics_policy().stopping_bootstrap_policy().replicate_count().value();
    require(dimensions.blocks<=cap.blocks&&multiply(dimensions.valid,2U)<=cap.observations&&dimensions.historical<=cap.historical&&
        dimensions.comparison_replicates<=cap.comparison_replicates&&dimensions.stopping_replicates<=cap.stopping_replicates,Error::capacity);
    auto projection=pair_analysis::detail::projection_stock(dimensions,*counted);require(projection.has_value(),Error::capacity);
    require(projection->result<=cap.result_bytes&&projection->workspace<=cap.workspace_bytes,Error::capacity);
    phase(add(counted->typed,add(counted->ledger,add(projection->result,projection->workspace))));
    auto result=project_pairwise_run_result(*root);core::detail::ExpectedErrorCleanup result_cleanup{result};require(result.has_value(),Error::child);
    const auto analysis_dom=stock::component_dom_stock(bound.serialization.nodes,bound.serialization.output_bytes);require(analysis_dom.has_value(),Error::capacity);
    const auto output_allowance=add(*analysis_dom,add(bound.serialization.output_bytes,multiply(bound.serialization.nodes,sizeof(std::uint64_t))));
    phase(add(counted->typed,add(counted->ledger,add(projection->result,output_allowance))));
    auto canonical=analysis::serialize_pairwise_analysis(*result,bound.serialization);core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    require(canonical&&work.equal(*canonical,supplied),Error::association);
    pair.analysis_retained=add(projection->result,add(canonical->capacity(),1U));require(pair.analysis_retained<=cap.retained_bytes,Error::capacity);
    pair.analysis.emplace(std::move(*result));pair.analysis_canonical=std::move(*canonical);
}
struct FailureDescriptor {
    std::size_t stage{},phase{};
    SV kind;
    bool secondary{},process{},cleanup{},spawn{},reap{};
};
template<std::size_t N> auto closed_token(const J& value,const std::array<SV,N>& tokens)->std::size_t {
    const auto& text=string(value);const auto found=std::ranges::find(tokens,text);require(found!=tokens.end(),Error::schema);
    return static_cast<std::size_t>(found-tokens.begin());
}
auto failure_descriptor(const Record& record,std::span<const Record> records,Work& work)->FailureDescriptor {
    require(record.kind==Kind::failure,Error::association);
    if(!record.event_facts){
        const auto* artifact=std::get_if<error_serialization::Owner>(&record.artifact);require(artifact&&artifact->provenance()&&artifact->provenance()->association,Error::association);
        const auto& provenance=*artifact->provenance();const auto& association=*provenance.association;
        bool before_start=true;for(const auto& prior:records){work.debit(1U);if(prior.sequence>=record.sequence)break;if(prior.kind==Kind::pair_started)before_start=false;}
        bool preparation=before_start&&provenance.entry_producer==process_subject::Producer::controller&&association.producer==process_subject::Producer::controller&&
            association.targets.size()==1U&&association.targets[0].category==process_subject::Category::topology&&association.targets[0].selector==process_subject::Selector::owner_preparation;
        if(preparation&&is_progress(record.version)&&provenance.domain==process_subject::Domain::supplied_observation) {
            bool bound{};
            for(const auto& retained:records) {
                work.debit(1U);
                if(retained.kind!=Kind::topology)continue;
                const auto* topology=std::get_if<topology_serialization::Owner>(&retained.artifact);
                if(topology&&topology->failure_error()&&work.equal(topology->failure_error()->canonical(),artifact->canonical()))bound=true;
            }
            preparation=bound;
        }
        // This is an eligibility rank established by the actual preparation target
        // before any start. It is not a reconstructed Runtime phase or stage.
        return {preparation?0U:SIZE_MAX,0U,"ordinary_unclassified",record.role==Role::secondary,false,false};
    }
    const auto& facts=*record.event_facts;fields(facts,{"origin","source","role","primary_ref","artifact"});
    const auto& source=string(facts.at("source"));require(source=="synthetic_lifecycle"||source=="native_lifecycle"||source=="finalized_error"||source=="controller_preparation"||source=="private_error",Error::unsupported);
    require(record.producer==(source=="finalized_error"?process_subject::Producer::runner:process_subject::Producer::controller),Error::association);
    const auto& role=string(facts.at("role"));require(role=="primary"||role=="secondary",Error::schema);
    if(role=="primary")require(facts.at("primary_ref").is_null(),Error::reference);
    else{const auto& primary=event_reference(facts.at("primary_ref"),"failure",records,Kind::failure,work);
        require(primary.sequence<record.sequence&&primary.scope.pair==record.scope.pair&&primary.scope.experiment==record.scope.experiment&&
            (primary.event_facts?string(primary.event_facts->at("role"))=="primary":
                source=="controller_preparation"&&primary.role==Role::primary),Error::reference);}
    if(source=="controller_preparation"||source=="private_error") {
        require(is_progress(record.version)&&record.scope==Scope{}&&record.availability&&
            record.availability->source==AvailabilitySource::controller_observation&&
            string(facts.at("origin"))=="actual_availability"&&!records.empty(),Error::association);
        const auto* genesis=std::get_if<run_creation_serialization::Owner>(&records.front().artifact);
        require(genesis&&genesis->provenance()&&genesis->provenance()->domain==process_subject::Domain::supplied_observation,Error::association);
        const Record* first{};std::size_t delivered{};
        for(const auto& prior:records) {
            work.debit(1U);if(prior.sequence>=record.sequence)break;
            require(prior.kind!=Kind::pair_started,Error::association);
            if(source=="private_error"&&prior.kind==Kind::failure)
                require(prior.event_facts&&string(prior.event_facts->at("source"))=="private_error",Error::association);
            if(prior.kind==Kind::failure&&prior.event_facts&&string(prior.event_facts->at("source"))=="private_error")++delivered;
            if(prior.kind==Kind::failure&&prior.scope==Scope{}&&
               (prior.event_facts?string(prior.event_facts->at("role"))=="primary":prior.role==Role::primary)&&!first)first=&prior;
        }
        if(source=="private_error") {
            require(delivered<genesis->failures().size(),Error::association);
            const auto& original=genesis->failures()[delivered];const auto* provenance=original.provenance();
            require(provenance&&provenance->association&&provenance->kind==error_serialization::Kind::creation_failure&&
                provenance->creation_ordinal==delivered&&provenance->sequence==genesis->provenance()->sequence&&
                provenance->run==genesis->provenance()->run&&provenance->domain==genesis->provenance()->domain&&
                provenance->entry_producer==process_subject::Producer::controller&&record.timestamp>=provenance->time&&
                work.equal(string(facts.at("artifact")),original.canonical()),Error::association);
            const auto& association=*provenance->association;
            require(association.producer==process_subject::Producer::controller&&
                (!association.process_sequence||association.process_sequence==genesis->provenance()->sequence),Error::association);
            bool bound{};for(const auto& binding:genesis->bindings()){work.debit(1U);if(binding.failure_ordinal==delivered)bound=true;}
            require(bound&&!association.targets.empty(),Error::association);
            require((role=="secondary")==association.secondary_sequence.has_value(),Error::reference);
            if(association.secondary_sequence) {
                require(*association.secondary_sequence==genesis->provenance()->sequence&&association.secondary_creation_ordinal&&
                    *association.secondary_creation_ordinal<delivered,Error::reference);
                const auto& primary=event_reference(facts.at("primary_ref"),"failure",records,Kind::failure,work);
                require(primary.scope==Scope{}&&primary.event_facts&&string(primary.event_facts->at("source"))=="private_error"&&
                    work.equal(string(primary.event_facts->at("artifact")),genesis->failures()[static_cast<std::size_t>(*association.secondary_creation_ordinal)].canonical()),Error::reference);
            }
            return {0U,0U,"creation_alias",role=="secondary",false,false};
        }
        require(delivered==genesis->failures().size(),Error::association);
        require((role=="primary")==!first,Error::reference);
        if(first) {
            require(event_reference(facts.at("primary_ref"),"failure",records,Kind::failure,work).sequence==first->sequence&&first->scope==record.scope,Error::reference);
            if(!first->event_facts)require(failure_descriptor(*first,records,work).stage==0U,Error::association);
        }
        const auto& artifact=facts.at("artifact");fields(artifact,{"profile","target","error"});
        require(string(artifact.at("profile"))=="epycbench.controller-preparation-failure/v1",Error::schema);
        constexpr std::array<SV,3> targets{"kernel_evidence","execution_plan","preflight_admission"};
        (void)closed_token(artifact.at("target"),targets);(void)string(artifact.at("error"));
        return {0U,0U,"controller_preparation",role=="secondary",false,false};
    }
    if(source=="finalized_error")return {SIZE_MAX,0U,"registry_unclassified",role=="secondary",false,false};
    if(source=="native_lifecycle") {
        require(is_progress(record.version)&&record.availability&&
            record.availability->source==AvailabilitySource::controller_observation&&
            string(facts.at("origin"))=="actual_availability"&&!records.empty(),Error::association);
        const auto* genesis=std::get_if<run_creation_serialization::Owner>(&records.front().artifact);
        require(genesis&&genesis->provenance()&&genesis->provenance()->domain==process_subject::Domain::supplied_observation,Error::association);
        const auto& value=facts.at("artifact");fields(value,{"kind","stage","phase","native"});
        constexpr std::array<SV,18> kinds{"invalid_state","invalid_input","capacity","preparation","clock_overflow","clock_regression",
            "native","protocol","backend","encoding","transport","reconstruction","results","cancelled","timeout","process_nonzero","process_signaled","unreapable"};
        constexpr std::array<SV,18> stages{"preparation","channels","spawn","parent_close","startup_control","startup_events",
            "cancellation","backend","encoding","evidence","eof","reconstruction","results","process","endpoint_close","terminate","kill","reap"};
        constexpr std::array<SV,5> phases{"startup","execution","cancellation","completion","termination"};
        const auto kind=closed_token(value.at("kind"),kinds),stage=closed_token(value.at("stage"),stages),phase=closed_token(value.at("phase"),phases);
        const auto& native=value.at("native");
        if(!native.is_null()) {
            fields(native,{"kind","error_number"});
            constexpr std::array<SV,8> native_kinds{"invalid_state","invalid_argument","malformed_result","native_failure","interrupted","would_block","wrong_child","invalid_status"};
            (void)closed_token(native.at("kind"),native_kinds);
            const auto& text=string(native.at("error_number"));int number{};
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),number);
            require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&std::to_string(number)==text,Error::schema);
        }
        // Coarse native stages retain their original tokens. This rank only joins
        // the existing election; no finer startup/transport observation is inferred.
        constexpr std::array<std::size_t,18> rank{0U,1U,2U,3U,7U,11U,12U,12U,13U,15U,17U,18U,19U,20U,21U,21U,21U,21U};
        work.debit(1U);
        return {rank[stage],phase,kinds[kind],role=="secondary",kind==15U||kind==16U,stage>=14U,stage==2U,stage==17U||kind==17U};
    }
    const auto& value=facts.at("artifact");fields(value,{"kind","stage","phase","evidence"});
    constexpr std::array<SV,26> kinds{"invalid_state","invalid_config","clock_overflow","clock_regression","callback","preparation","channels","spawn","close",
        "protocol","backend","encoding","transport","reconstruction","results","cancelled","timeout","cancel_write","cancel_response",
        "process_nonzero","process_signaled","terminate","kill","reap","wrong_child","unreapable"};
    constexpr std::array<SV,22> stages{"preparation","channels","spawn","parent_close","host_offer","capacity_offer","prepared_offer","request",
        "host_ack","capacity_ack","prepared_ack","accepted","backend","encoding","manifest","chunks","completion","eof","reconstruction","results","process","cleanup"};
    constexpr std::array<SV,5> phases{"startup","execution","cancellation","completion","termination"};
    const auto kind=closed_token(value.at("kind"),kinds);const auto stage=closed_token(value.at("stage"),stages);const auto phase=closed_token(value.at("phase"),phases);
    const auto& evidence=value.at("evidence");
    if(!evidence.is_null()){
        fields(evidence,{"stage","transport","codec"});constexpr std::array<SV,6> active{"ownership","backend","canonical_encoding","transport","reconstruction","results"};
        (void)closed_token(evidence.at("stage"),active);
        if(!evidence.at("transport").is_null()){
            constexpr std::array<SV,24> transport{"invalid_capacity","arithmetic_overflow","invalid_encoding","invalid_envelope","unsupported_protocol","unexpected_message",
                "unsupported_profile","sequence_mismatch","identity_mismatch","invalid_payload","frame_over_limit","stream_incomplete","extra_event","digest_mismatch",
                "allocation","contained_exception","contained_unknown_failure","io","timeout","cancelled","clean_eof","truncated_frame","invalid_progress","one_use"};
            (void)closed_token(evidence.at("transport"),transport);
        }
        const auto& codec=evidence.at("codec");if(!codec.is_null()){
            fields(codec,{"kind","stage","sequence_index","limit","observed","normalized_os_error"});
            constexpr std::array<SV,17> codec_kinds{"capacity_invariant","malformed","noncanonical","registry_shape","registry_domain","registry_cardinality","typed_factory",
                "ledger_validation","reencode_mismatch","results_projection","results_recalculation","transport_state","io","timeout","cancelled","peer_exit","cleanup"};
            constexpr std::array<SV,3> codec_stages{"prepared","active","encoded"};(void)closed_token(codec.at("kind"),codec_kinds);(void)closed_token(codec.at("stage"),codec_stages);
            (void)number(codec.at("sequence_index"));(void)number(codec.at("limit"));(void)number(codec.at("observed"));
            if(!codec.at("normalized_os_error").is_null()){
                const auto& text=string(codec.at("normalized_os_error"));std::int32_t error{};
                const auto parsed=std::from_chars(text.data(),text.data()+text.size(),error);
                require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&std::to_string(error)==text,Error::schema);
            }
        }
    }
    work.debit(1U);return {stage,phase,kinds[kind],role=="secondary",kind==19U||kind==20U,stage==21U};
}
auto governing_failure(std::span<const Record> records,std::optional<std::uint64_t> ordinal,
    const ComposedPair* pair,const CompositionLimits& limits,Work& work)->std::pair<const Record*,std::size_t> {
    const Record* winner{};FailureDescriptor selected{};
    for(const auto& record:records){work.debit(1U);
        if(record.kind!=Kind::failure||record.scope.pair!=ordinal)continue;
        auto failure=failure_descriptor(record,records,work);if(failure.secondary)continue;
        if(failure.kind=="registry_unclassified"&&pair&&pair->prefix&&pair->prefix->terminal_event()&&pair->context){
            const auto* terminal=std::get_if<runtime::OperationalRunTerminalEvidence>(&pair->prefix->terminal_event()->evidence);
            if(terminal){auto body=component_value(wire::EvidenceComponentKind::finalized_error,string(record.event_facts->at("artifact")),*pair->context,limits,work);
                auto error=wire::detail::reconstruct_finalized_error_value(body.value());require(error.has_value(),Error::child);
                if(*error==terminal->primary_error){failure.stage=12U;
                    std::size_t secondary{};
                    for(const auto& candidate:records){work.debit(1U);if(candidate.kind!=Kind::failure||!candidate.event_facts||candidate.scope.pair!=record.scope.pair)continue;
                        const auto& facts=*candidate.event_facts;
                        if(string(facts.at("source"))!="finalized_error"||facts.at("primary_ref").is_null())continue;
                        if(number(facts.at("primary_ref").at("sequence"))!=record.sequence)continue;
                        require(secondary<terminal->secondary_errors.size(),Error::association);
                        auto nested=component_value(wire::EvidenceComponentKind::finalized_error,string(facts.at("artifact")),*pair->context,limits,work);
                        auto parsed=wire::detail::reconstruct_finalized_error_value(nested.value());require(parsed&&*parsed==terminal->secondary_errors[secondary],Error::association);++secondary;
                    }
                    require(secondary==terminal->secondary_errors.size(),Error::association);
                }
            }
        }
        // Retention alone does not establish an operational phase. An unresolved
        // primary must keep closure pending rather than silently lose an election.
        require(failure.stage!=SIZE_MAX,Error::association);
        if(failure.process&&(!pair||!pair->request_record||!pair->completion_record||!pair->clean_eof||!pair->root_record||!pair->analysis))continue;
        if(!winner||failure.stage<selected.stage||
            (failure.stage==selected.stage&&failure.phase==selected.phase&&record.timestamp==winner->timestamp&&
                failure.kind=="cancelled"&&selected.kind=="timeout")){winner=&record;selected=failure;}
    }
    return {winner,winner?selected.stage:SIZE_MAX};
}
auto supplied_exit(const J& value,std::uint64_t child)->SuppliedExit {
    fields(value,{"child_token","signaled","value"});
    require(value.at("signaled").is_boolean(),Error::schema);
    SuppliedExit result{number(value.at("child_token")),number(value.at("value")),value.at("signaled").get<bool>()};
    require(result.child==child&&result.value<=UINT32_MAX,Error::identity);return result;
}
auto receipt(const Record& record)->const J& { return record.event_facts->at("cleanup"); }
void cleanup_receipt(ComposedPair& pair,const Record& record,std::span<const Record> records,Work& work) {
    const auto& value=receipt(record);
    fields(value,{"primary_ref","secondary_refs","operations","close_returns","terminate_return","kill_return","reap_return","fatal_unreapable"});
    const auto failure_ref=[&](const J& ref)->const Record&{
        const auto& failure=event_reference(ref,"failure",records,Kind::failure,work);
        require(failure.scope.pair==record.scope.pair&&failure.scope.experiment==record.scope.experiment,Error::reference);
        (void)failure_descriptor(failure,records,work);return failure;};
    if(!value.at("primary_ref").is_null())require(!failure_descriptor(failure_ref(value.at("primary_ref")),records,work).secondary,Error::reference);
    const auto& secondaries=value.at("secondary_refs");require(secondaries.is_array()&&secondaries.size()<=8U,Error::schema);
    const auto& creation=std::get<run_creation_serialization::Owner>(records.front().artifact);
    const bool observed_controller=is_progress(record.version)&&creation.provenance()&&
        creation.provenance()->domain==process_subject::Domain::supplied_observation&&
        record.availability&&record.availability->source==AvailabilitySource::controller_observation;
    const bool failed_spawn=pair.child_token==0U;
    if(failed_spawn) {
        const auto& start=records[static_cast<std::size_t>(pair.start_record-1U)];
        require(observed_controller&&record.producer==process_subject::Producer::controller&&
            record.kind==Kind::lifecycle_progress&&record.event_facts->at("transition")=="cleanup"&&
            start.event_facts->at("spawn").at("state")=="failed"&&
            value.at("primary_ref")==start.event_facts->at("spawn").at("failure_ref"),Error::association);
        require(!value.at("fatal_unreapable").get<bool>(),Error::association);
    }
    constexpr std::array<SV,10> sources{"backend_retirement","runner_control","runner_event","controller_control","controller_event","graceful_termination","forced_escalation","exact_reap","parent_runner_control","parent_runner_event"};
    std::array<bool,10> secondary_seen{};
    for(const auto& secondary:secondaries){fields(secondary,{"source","failure_ref"});const auto index=closed_token(secondary.at("source"),sources);
        require(!secondary_seen[index],Error::reference);secondary_seen[index]=true;
        const auto& failure=failure_ref(secondary.at("failure_ref"));
        if(index>=8U)require(observed_controller&&failure.event_facts->at("source")=="native_lifecycle"&&
            failure.event_facts->at("artifact").at("stage")=="parent_close",Error::association);
        require(failure_descriptor(failure,records,work).secondary&&failure.event_facts->at("primary_ref")==value.at("primary_ref"),Error::reference);
    }
    require(value.at("fatal_unreapable").is_boolean(),Error::schema);
    constexpr std::array<SV,6> returns{"absent","success","failed","exception","wrong_child","inconsistent_exit"};
    constexpr std::array<SV,10> kinds{"parent_runner_control","parent_runner_event","controller_control","controller_event","runner_event","runner_control",
        "backend_retirement","graceful_termination","forced_escalation","exact_reap"};
    const auto& operations=value.at("operations");const auto& closes=value.at("close_returns");
    require(operations.is_array()&&operations.size()<=kinds.size()&&closes.is_array()&&closes.size()==6U,Error::schema);
    std::array<bool,10> seen{};std::array<SV,10> returned{};returned.fill("absent");
    std::size_t secondary_index{};
    for(const auto& operation:operations){work.debit(1U);fields(operation,{"kind","return","failure_ref"});
        const auto& kind=string(operation.at("kind"));const auto at=std::ranges::find(kinds,kind);require(at!=kinds.end(),Error::schema);
        const auto index=static_cast<std::size_t>(at-kinds.begin());require(!seen[index],Error::association);
        if(failed_spawn)require(index<4U,Error::association);
        if(index==7U||index==8U)require(seen[2]&&seen[3],Error::association);
        const auto disposition=closed_token(operation.at("return"),returns);require(disposition!=0U,Error::association);
        require(disposition<4U||index==9U,Error::association);
        if(disposition==1U)require(operation.at("failure_ref").is_null(),Error::reference);
        else{const auto& failure=failure_ref(operation.at("failure_ref"));
            if(failure_descriptor(failure,records,work).secondary){require(secondary_index<secondaries.size()&&
                secondaries[secondary_index].at("source")==operation.at("kind")&&secondaries[secondary_index].at("failure_ref")==operation.at("failure_ref"),Error::reference);++secondary_index;}
            else require(operation.at("failure_ref")==value.at("primary_ref"),Error::reference);}
        seen[index]=true;returned[index]=returns[disposition];
    }
    for(std::size_t i{};i<6U;++i)require(string(closes[i])==returned[i],Error::association);
    require(string(value.at("terminate_return"))==returned[7]&&
        string(value.at("kill_return"))==returned[8]&&string(value.at("reap_return"))==returned[9],Error::association);
    require(secondary_index==secondaries.size(),Error::reference);
    if(value.at("fatal_unreapable").get<bool>())require(seen[9]&&returned[9]!="success",Error::association);
    if(pair.cleanup_record){const auto& previous=receipt(records[static_cast<std::size_t>(pair.cleanup_record-1U)]);
        require(previous.at("primary_ref").is_null()||previous.at("primary_ref")==value.at("primary_ref"),Error::association);
        require(!previous.at("fatal_unreapable").get<bool>()||value.at("fatal_unreapable").get<bool>(),Error::association);
        const auto& prior_secondary=previous.at("secondary_refs");require(prior_secondary.size()<=secondaries.size(),Error::association);
        for(std::size_t i{};i<prior_secondary.size();++i)require(prior_secondary[i]==secondaries[i],Error::association);
        const auto& earlier=previous.at("operations");
        require(earlier.size()<=operations.size(),Error::association);
        for(std::size_t i{};i<earlier.size();++i){work.debit(1U);require(earlier[i]==operations[i],Error::association);}}
    pair.cleanup_record=record.sequence;
    const auto success=[](SV returned_value){return returned_value=="success";};
    const bool controller_cleanup_complete=std::ranges::all_of(returned.begin(),returned.begin()+4,success)&&
        returned[4]=="absent"&&returned[5]=="absent"&&returned[6]=="absent";
    const bool legacy_cleanup_complete=std::ranges::all_of(returned.begin(),returned.begin()+7,success);
    // Remote close/retirement results remain absent. Exact exit, stream integrity,
    // analysis, cancellation and governing failures are checked at run closure.
    pair.cleanup_complete=(observed_controller?controller_cleanup_complete:legacy_cleanup_complete)&&returned[9]=="success"&&
        value.at("primary_ref").is_null()&&secondaries.empty()&&!value.at("fatal_unreapable").get<bool>();
}
auto replay_lifecycle(ComposedPair& pair,const Record& record,std::span<const Record> records,Work& work)->bool {
    if(record.kind!=Kind::lifecycle_progress&&record.kind!=Kind::event_eof&&record.kind!=Kind::child_reaped)return false;
    require(record.producer==process_subject::Producer::controller&&!record.scope.block&&!record.scope.observation,Error::association);
    const auto& facts=*record.event_facts;
    require(event_reference(facts.at("start_ref"),"pair_start",records,Kind::pair_started,work).sequence==pair.start_record,Error::reference);
    if(record.kind==Kind::event_eof){
        fields(facts,{"origin","start_ref","disposition"});require(!pair.eof_record,Error::association);
        const auto& disposition=string(facts.at("disposition"));require(disposition=="clean"||disposition=="premature",Error::schema);
        if(disposition=="clean")require(pair.completion_record,Error::association);
        pair.clean_eof=disposition=="clean";pair.eof_record=record.sequence;
    }else if(record.kind==Kind::child_reaped){
        fields(facts,{"origin","start_ref","exit","cleanup"});require(!pair.reap_record&&!pair.reap_failure_record,Error::association);
        auto exit=supplied_exit(facts.at("exit"),pair.child_token);require(!pair.exit||*pair.exit==exit,Error::association);
        cleanup_receipt(pair,record,records,work);require(string(facts.at("cleanup").at("reap_return"))=="success",Error::association);
        pair.exit=exit;pair.reap_record=record.sequence;
    }else{
        const auto& transition=string(facts.at("transition"));
        if(transition=="completion_received"){
            fields(facts,{"origin","start_ref","transition"});require(!pair.completion_record&&!pair.eof_record&&pair.request_record,Error::association);
            pair.completion_record=record.sequence;
        }else if(transition=="observed_exit"){
            fields(facts,{"origin","start_ref","transition","exit"});auto exit=supplied_exit(facts.at("exit"),pair.child_token);
            require(!pair.exit||*pair.exit==exit,Error::association);pair.exit=exit;
        }else if(transition=="cleanup"){
            fields(facts,{"origin","start_ref","transition","cleanup"});cleanup_receipt(pair,record,records,work);
        }else if(transition=="cancellation"){
            fields(facts,{"origin","start_ref","transition","reason","sent","runner_observed","response_received"});
            constexpr std::array<SV,3> reasons{"user_interrupt","controller_shutdown","controller_timeout"};(void)closed_token(facts.at("reason"),reasons);
            for(const auto key:{"sent","runner_observed","response_received"})require(facts.at(key).is_boolean(),Error::schema);
            require(!facts.at("runner_observed").get<bool>()||facts.at("sent").get<bool>(),Error::association);
            require(!facts.at("response_received").get<bool>()||facts.at("runner_observed").get<bool>(),Error::association);
            if(pair.cancellation_record){const auto& previous=*records[static_cast<std::size_t>(pair.cancellation_record-1U)].event_facts;
                require(previous.at("reason")==facts.at("reason"),Error::association);
                for(const auto key:{"sent","runner_observed","response_received"})require(!previous.at(key).get<bool>()||facts.at(key).get<bool>(),Error::association);}
            pair.cancellation_record=record.sequence;
        }else if(transition=="reap_failed"){
            fields(facts,{"origin","start_ref","transition","failure_ref"});require(!pair.reap_record&&!pair.reap_failure_record,Error::association);
            const auto& failure=event_reference(facts.at("failure_ref"),"failure",records,Kind::failure,work);
            const auto descriptor=failure_descriptor(failure,records,work);require(failure.scope.pair==record.scope.pair&&failure.scope.experiment==record.scope.experiment&&
                (descriptor.reap||descriptor.kind=="reap"||descriptor.kind=="wrong_child"||descriptor.kind=="unreapable"),Error::reference);
            pair.reap_failure_record=record.sequence;
        }else throw Error::unsupported;
    }
    pair.events.push_back(record.sequence);return true;
}
void replay_component(ComposedPair& pair,const Record& record,std::span<const Record> records,
    const CompositionLimits& limits,Work& work) {
    require(record.event_facts!=nullptr,Error::association);
    work.debit(record.canonical.size());
    if(replay_lifecycle(pair,record,records,work))return;
    require(!pair.eof_record||record.producer!=process_subject::Producer::runner||record.kind==Kind::finalization,Error::association);
    require(pair.prefix&&pair.context&&record.event_facts&&record.producer==(record.kind==Kind::analysis?process_subject::Producer::results:process_subject::Producer::runner),Error::association);
    const auto& facts=*record.event_facts;auto& prefix=*pair.prefix;
    const auto child=static_cast<std::uint64_t>(pair.children.size()+1U);
    const auto time=journal::Timestamp::parse(record.timestamp);require(time.has_value(),Error::schema);
    const auto same_request=[&]{require(event_reference(facts.at("request_ref"),"request",records,Kind::request_accepted,work).sequence==pair.request_record,Error::reference);};
    const auto earlier=[&](const J& ref,SV token,Kind kind,std::uint64_t expected){
        const auto& target=event_reference(ref,token,records,kind,work);
        require(target.scope.pair==record.scope.pair&&target.scope.experiment==record.scope.experiment,Error::reference);
        work.debit(pair.children.size());const auto found=std::ranges::find(pair.children,target.sequence);
        require(found!=pair.children.end()&&static_cast<std::uint64_t>(found-pair.children.begin()+1)==expected,Error::reference);
    };
    // Admit the configured child phase and complete retained-prefix overlap before
    // any per-event typed reconstruction, copied graph arena or timing Error.
    const auto child_phase=add(limits.context.simultaneous,add(limits.pair.simultaneous_bytes,limits.error.consumer_simultaneous_bytes));
    require(child_phase<=limits.simultaneous,Error::capacity);
    // Scratch ceilings remain reusable peak reservations. Cumulative work meters
    // the actual replayed bytes and the decoder's field/node/reference visits.
    work.debit(record.canonical.size());
    work.reserve_peak(add(pair.retained,child_phase));
    std::expected<void,block_evidence::Error> result;
    switch(record.kind){
    case Kind::placement:{
        const auto& phase=string(facts.at("phase"));
        const auto key=phase=="worker_readiness"?"workers":phase=="control_readiness"?"control":
            phase=="first_memory"||phase=="second_memory"?"memory":phase=="monitoring_preparation"?"monitoring":
            phase=="memory_finalization"?"component":"";
        require(*key,Error::schema);fields(facts,{"origin","request_ref","phase",key});same_request();
        require(!record.scope.block&&!record.scope.observation,Error::association);
        if(phase=="worker_readiness"){auto rows=PairContextDecoder::workers(facts.at(key),limits.context,work);result=prefix.worker_readiness(child,*time,rows);}
        else if(phase=="control_readiness"){auto row=PairContextDecoder::control(facts.at(key),work);result=prefix.control_readiness(child,*time,row);}
        else if(phase=="first_memory"||phase=="second_memory"){
            const bool second=phase=="second_memory";auto rows=PairContextDecoder::memory(facts.at(key),*pair.context,second,limits.context,work);
            result=prefix.memory_preparation(child,*time,second,rows);
        }else if(phase=="monitoring_preparation")result=PairContextDecoder::monitoring(prefix,child,*time,facts.at(key),*pair.context,limits.context,work);
        else result=prefix.finalization(child,*time,string(facts.at(key)));
        break;}
    case Kind::preparation:{
        fields(facts,{"origin","request_ref","preparation_refs","preparation_graph"});same_request();
        const auto& refs=facts.at("preparation_refs");require(refs.is_array()&&refs.size()==5U,Error::reference);
        require(prefix.control_readiness_event()&&prefix.readiness_event()&&prefix.memory_preparation_event(false)&&
            prefix.memory_preparation_event(true)&&prefix.monitoring_preparation_event(),Error::reference);
        const std::array<std::uint64_t,5> expected{prefix.control_readiness_event()->sequence,prefix.readiness_event()->sequence,
            prefix.memory_preparation_event(false)->sequence,prefix.memory_preparation_event(true)->sequence,prefix.monitoring_preparation_event()->sequence};
        constexpr std::array<SV,5> names{"control","workers","first_memory","second_memory","monitoring_preparation"};
        for(std::size_t i{};i<refs.size();++i)earlier(refs[i],names[i],Kind::placement,expected[i]);
        result=PairContextDecoder::graph(prefix,child,*time,facts.at("preparation_graph"),*pair.context,limits.context,work);break;}
    case Kind::calibration:
        fields(facts,{"origin","preparation_ref","component"});require(prefix.preparation_event(),Error::reference);
        earlier(facts.at("preparation_ref"),"preparation",Kind::preparation,prefix.preparation_event()->sequence);
        result=prefix.calibration(child,*time,string(facts.at("component")));break;
    case Kind::warmup:
        fields(facts,{"origin","calibration_ref","component"});require(prefix.calibration_event(),Error::reference);
        earlier(facts.at("calibration_ref"),"calibration",Kind::calibration,prefix.calibration_event()->sequence);
        result=prefix.warmup(child,*time,string(facts.at("component")));break;
    case Kind::block_opened:
        fields(facts,{"origin","request_ref","calibration_ref","warmup_ref","component"});same_request();
        require(prefix.calibration_event()&&prefix.warmup_event()&&record.scope.block&&!record.scope.observation,Error::reference);
        earlier(facts.at("calibration_ref"),"calibration",Kind::calibration,prefix.calibration_event()->sequence);
        earlier(facts.at("warmup_ref"),"warmup",Kind::warmup,prefix.warmup_event()->sequence);
        result=prefix.open(child,*time,string(facts.at("component")));break;
    case Kind::monitoring:{
        fields(facts,{"origin","request_ref","observation_ref","preparation","component","samples"});same_request();
        require(facts.at("observation_ref").is_null()&&prefix.monitoring_preparation_event(),Error::reference);
        earlier(facts.at("preparation"),"monitoring_preparation",Kind::placement,prefix.monitoring_preparation_event()->sequence);
        auto capture=component_value(wire::EvidenceComponentKind::monitoring_result,string(facts.at("component")),*pair.context,limits,work);
        sample_references(facts.at("samples"),sample_count(capture.value(),work),record.sequence,work);
        require(pair.captures.size()<limits.events,Error::capacity);pair.captures.push_back({record.sequence,0U});break;}
    case Kind::observation:case Kind::failed_observation:{
        fields(facts,{"origin","opened_ref","monitoring_refs","timing","component"});
        require(record.scope.block&&record.scope.observation&&prefix.block_count()&&prefix.calibration_event(),Error::reference);
        const auto events=prefix.events(prefix.block_count()-1U);require(!events.empty(),Error::reference);
        earlier(facts.at("opened_ref"),"block",Kind::block_opened,events.front().sequence);
        const auto& opened=event_reference(facts.at("opened_ref"),"block",records,Kind::block_opened,work);
        require(opened.scope.block==record.scope.block,Error::reference);
        const auto& monitoring=facts.at("monitoring_refs");require(monitoring.is_array(),Error::schema);
        const bool failed=record.kind==Kind::failed_observation;
        require(monitoring.size()==(failed?0U:1U),Error::reference);
        if(!failed){const auto& reference=monitoring[0];fields(reference,{"sequence","ordinal","kind"});
            const auto sequence=number(reference.at("sequence"));require(number(reference.at("ordinal"))==0U&&string(reference.at("kind"))=="monitoring",Error::reference);
            if(sequence!=record.sequence){
                const auto& capture=event_reference(reference,"monitoring",records,Kind::monitoring,work);
                require(capture.scope.pair==record.scope.pair&&capture.scope.experiment==record.scope.experiment&&
                    (!capture.scope.block||capture.scope.block==record.scope.block)&&(!capture.scope.observation||capture.scope.observation==record.scope.observation),Error::reference);
                work.debit(pair.captures.size());auto associated=std::ranges::find_if(pair.captures,[&](const auto& value){return value.capture==sequence;});
                require(associated!=pair.captures.end()&&!associated->observation,Error::reference);
                auto observation=component_value(wire::EvidenceComponentKind::factual_observation,string(facts.at("component")),*pair.context,limits,work);
                const auto& raw_monitoring=observation.value().at("monitoring");require(!raw_monitoring.is_null(),Error::association);
                auto canonical=core::canonicalize_json_jcs(raw_monitoring,{limits.pair.component.canonical_bytes,limits.pair.component.depth,limits.pair.component.nodes});
                core::detail::ExpectedErrorCleanup cleanup{canonical};require(canonical&&work.equal(*canonical,string(capture.event_facts->at("component"))),Error::association);
                sample_references(capture.event_facts->at("samples"),sample_count(raw_monitoring,work),sequence,work);
                associated->observation=record.sequence;
            }
        }
        auto timing=core::detail::clone_json(facts.at("timing"));core::detail::JsonCleanup timing_cleanup{timing};
        require(timing.is_object()&&timing.contains("identity")&&timing.contains("calibration_ref"),Error::schema);
        earlier(timing.at("calibration_ref"),"calibration",Kind::calibration,prefix.calibration_event()->sequence);
        timing["calibration_ref"]=std::to_string(prefix.calibration_event()->sequence);
        result=PairContextDecoder::observation(prefix,child,*time,failed,string(facts.at("component")),timing,*pair.context,limits.error,work);
        if(result){const auto updated=prefix.events(prefix.block_count()-1U);require(!updated.empty()&&updated.back().timing,Error::association);
            const auto identity=updated.back().timing->identity();require(identity&&identity->block.value()==*record.scope.block&&identity->observation.value()==*record.scope.observation,Error::association);
            if(const auto* factual=updated.back().component.factual())std::visit([&](const auto& measured){
                require(work.equal(kernel::kernel_implementation_id_token(measured.implementation_id()),
                    kernel::kernel_implementation_id_token(pair.context->implementation)),Error::association);
            },factual->observation);
        }
        break;}
    case Kind::block_closed:{
        fields(facts,{"origin","opened_ref","completion","observations"});
        require(record.scope.block&&!record.scope.observation&&prefix.block_count(),Error::reference);
        const auto events=prefix.events(prefix.block_count()-1U);require(!events.empty(),Error::reference);
        earlier(facts.at("opened_ref"),"block",Kind::block_opened,events.front().sequence);
        require(events.front().component.plan()&&events.front().component.plan()->block_id().value()==*record.scope.block,Error::association);
        const auto& observed=facts.at("observations");require(observed.is_array()&&observed.size()==events.size()-1U,Error::reference);
        for(std::size_t i{};i<observed.size();++i){const bool failed=events[i+1U].kind==block_evidence::EventKind::failed;
            earlier(observed[i],failed?"failed_observation":"observation",failed?Kind::failed_observation:Kind::observation,events[i+1U].sequence);}
        const auto& completion=string(facts.at("completion"));require(completion=="complete_valid"||completion=="complete_invalid"||completion=="operationally_incomplete",Error::schema);
        result=prefix.close(child,*time,completion=="complete_valid"?runtime::BlockAttemptCompletion::complete_valid:
            completion=="complete_invalid"?runtime::BlockAttemptCompletion::complete_invalid:runtime::BlockAttemptCompletion::operationally_incomplete);break;}
    case Kind::stopping:{
        fields(facts,{"origin","request_ref","closed_blocks","component"});same_request();
        require(!record.scope.block&&!record.scope.observation,Error::association);
        const auto& closed=facts.at("closed_blocks");require(closed.is_array()&&closed.size()==prefix.block_count(),Error::reference);
        for(std::size_t i{};i<closed.size();++i){const auto events=prefix.events(i);require(!events.empty()&&events.back().kind==block_evidence::EventKind::closed,Error::reference);
            earlier(closed[i],"block",Kind::block_closed,events.back().sequence);}
        result=prefix.stopping(child,*time,string(facts.at("component")));break;}
    case Kind::pair_terminal:{
        fields(facts,{"origin","start_ref","terminal"});require(!record.scope.block&&!record.scope.observation,Error::association);
        require(event_reference(facts.at("start_ref"),"pair_start",records,Kind::pair_started,work).sequence==pair.start_record,Error::reference);
        auto body=component_value(wire::EvidenceComponentKind::terminal,string(facts.at("terminal")),*pair.context,limits,work);
        auto terminal=terminal_value(body.value(),*pair.context,limits);result=prefix.terminal(child,*time,terminal);break;}
    case Kind::finalization:{
        const bool compact=record.version==Version::compact_v3;
        if(compact)fields(facts,{"origin","request_ref","terminal_ref","placement_refs","component_refs","canonical_root_digest"});
        else fields(facts,{"origin","request_ref","terminal_ref","placement_refs","component_refs","canonical_root"});
        same_request();
        require(!record.scope.block&&!record.scope.observation&&!pair.root_record&&prefix.terminal_event(),Error::association);
        earlier(facts.at("terminal_ref"),"terminal",Kind::pair_terminal,prefix.terminal_event()->sequence);
        const auto& references=facts.at("component_refs");const auto& placements=facts.at("placement_refs");
        require(references.is_array()&&references.size()==add(pair.children.size(),pair.captures.size())&&placements.is_array(),Error::reference);
        std::size_t placed{},component_index{};
        for(const auto sequence:pair.events){const auto& prior=records[static_cast<std::size_t>(sequence-1U)];
            if(prior.kind==Kind::lifecycle_progress||prior.kind==Kind::event_eof||prior.kind==Kind::child_reaped)continue;
            SV token;
            switch(prior.kind){
            case Kind::placement:{const auto& phase=string(prior.event_facts->at("phase"));
                token=phase=="control_readiness"?"control":phase=="worker_readiness"?"workers":phase=="memory_finalization"?"finalization":SV{phase};break;}
            case Kind::preparation:token="preparation";break;case Kind::calibration:token="calibration";break;case Kind::warmup:token="warmup";break;
            case Kind::block_opened:case Kind::block_closed:token="block";break;
            case Kind::observation:token="observation";break;case Kind::failed_observation:token="failed_observation";break;
            case Kind::stopping:token="stopping";break;case Kind::pair_terminal:token="terminal";break;case Kind::monitoring:token="monitoring";break;
            default:throw Error::reference;}
            require(component_index<references.size()&&event_reference(references[component_index],token,records,prior.kind,work).sequence==prior.sequence,Error::reference);
            if(prior.kind==Kind::placement){require(placed<placements.size()&&placements[placed]==references[component_index],Error::reference);++placed;}
            ++component_index;
        }
        require(placed==placements.size()&&component_index==references.size(),Error::reference);
        if(compact) {
            auto expected=core::Sha256Digest::parse(string(facts.at("canonical_root_digest")));
            core::detail::ExpectedErrorCleanup cleanup{expected};require(expected.has_value(),Error::schema);
            result=prefix.reconcile_digest(*expected,pair.context->header);
        } else result=prefix.reconcile(string(facts.at("canonical_root")));
        if(result)pair.root_record=record.sequence;
        break;}
    case Kind::analysis:
        fields(facts,{"origin","finalization_ref","profile","canonical"});require(pair.root_record&&!record.scope.block&&!record.scope.observation,Error::association);
        require(event_reference(facts.at("finalization_ref"),"finalization",records,Kind::finalization,work).sequence==pair.root_record&&
            string(facts.at("profile"))=="epycbench.pairwise-derived-analysis/v1",Error::reference);
        project_analysis(pair,string(facts.at("canonical")),limits,work);pair.analysis_record=record.sequence;break;
    default:throw Error::unsupported;
    }
    if(!result)throw PairAdmissionFailure{result.error()};
    if(record.kind==Kind::block_opened){
        require(prefix.block_count(),Error::association);const auto events=prefix.events(prefix.block_count()-1U);
        require(!events.empty()&&events.front().component.plan()&&
            events.front().component.plan()->block_id().value()==*record.scope.block,Error::association);
    }
    if(record.kind!=Kind::finalization&&record.kind!=Kind::analysis&&record.kind!=Kind::monitoring)pair.children.push_back(record.sequence);
    pair.events.push_back(record.sequence);
}
}
auto compose(std::span<const Record> records,const Record& incoming,
    const std::shared_ptr<const CompositionState>& previous,const CompositionLimits& limits,Work& work)
    ->std::shared_ptr<const CompositionState> {
    require(incoming.event_facts&&!records.empty(),Error::association);
    if(incoming.kind==Kind::planning||incoming.kind==Kind::preflight)
        require(incoming.scope==Scope{}&&incoming.producer==process_subject::Producer::controller,Error::association);
    require(records.front().kind==Kind::creation,Error::reference);
    const auto& genesis=std::get<run_creation_serialization::Owner>(records.front().artifact);
    require(genesis.provenance()&&(genesis.provenance()->domain==process_subject::Domain::synthetic_execution||
        (is_progress(incoming.version)&&genesis.provenance()->domain==process_subject::Domain::supplied_observation)),Error::association);
    require(!previous||!previous->closed_record,Error::state);
    const auto& facts=*incoming.event_facts;
    if((incoming.kind==Kind::planning||incoming.kind==Kind::preflight||incoming.kind==Kind::run_closed)&&
       string(facts.at("origin"))=="actual_availability")
        require(is_progress(incoming.version)&&genesis.provenance()->domain==process_subject::Domain::supplied_observation&&
            incoming.scope==Scope{}&&incoming.availability&&incoming.availability->source==AvailabilitySource::controller_observation,Error::association);
    const auto allowance=add(add(sizeof(CompositionState),multiply(limits.pairs,sizeof(std::shared_ptr<const ComposedPair>))),
        add(limits.planning.retained,limits.preflight.retained));
    auto peak=add(allowance,std::max(limits.planning.simultaneous,limits.preflight.simultaneous));
    if(incoming.kind!=Kind::planning&&incoming.kind!=Kind::preflight){
        const auto pair_stock=add(sizeof(ComposedPair),add(limits.context.retained,
            add(limits.context.text,add(limits.pair.retained_bytes,multiply(limits.events,sizeof(std::uint64_t)*4U)))));
        peak=add(peak,add(pair_stock,add(limits.context.simultaneous,limits.pair.simultaneous_bytes)));
        if(incoming.kind==Kind::analysis)peak=add(peak,limits.analysis.values.simultaneous_bytes);
    }
    require(allowance<=limits.retained&&peak<=limits.simultaneous,Error::capacity);
    work.reserve_peak(peak);work.debit(sizeof(CompositionState));
    auto next=previous?std::make_shared<CompositionState>(*previous):std::make_shared<CompositionState>();
    if(next->check_failure_count<next->check_failure_total)require(incoming.kind==Kind::failure&&facts.at("source")=="controller_preparation",Error::association);
    if(incoming.kind==Kind::failure){
        const auto failure=failure_descriptor(incoming,records,work);
        if(failure.kind=="controller_preparation") {
            const auto& bytes=string(facts.at("artifact").at("error"));
            work.debit(bytes.size());work.reserve_peak(limits.error.consumer_simultaneous_bytes);
            error_serialization::Usage usage{};
            auto value_limits=limits.error;value_limits.targets=0U;value_limits.provenance_bytes=0U;
            auto decoded=error_serialization::detail::decode_value(bytes,value_limits,usage);
            if(decoded&&decoded->source()->operation=="acquire_controller_checks")
                require(controller_check_snapshot_valid(decoded->source()->context.at("observations"),value_limits),Error::schema);
            require(decoded.has_value(),Error::child);
            const auto* error=decoded->source();require(error&&error->category==core::ErrorCategory::results,Error::association);
            const auto& target=string(facts.at("artifact").at("target"));
            require(preparation_failure_matches(*error,target),Error::association);
            if(error->operation=="acquire_controller_checks") {
                const auto& snapshot=error->context.at("observations");
                const auto encoded=snapshot.dump();work.debit(encoded.size());const auto digest=core::sha256(encoded);
                const auto total=each_check_failure(snapshot,[](auto,const auto&,auto,const auto&,const auto&){});
                if(next->check_failure_digest)require(*next->check_failure_digest==digest,Error::association);
                else {next->check_failure_digest=digest;next->check_failure_total=total;}
                require(next->check_failure_total==total&&next->check_failure_count<total&&
                    check_failure_at(*error,snapshot,next->check_failure_count),Error::association);
                if(next->check_failure_count)require(facts.at("role")=="secondary",Error::reference);
                ++next->check_failure_count;
            } else require(next->check_failure_count==next->check_failure_total,Error::association);
            if(error->operation=="acquire_kernel_evidence"&&error->context.at("cleanup")==true)
                require(facts.at("role")=="secondary",Error::reference);
        }
        if(failure.kind=="registry_unclassified"){
            require(incoming.scope.pair&&*incoming.scope.pair<next->pairs.size()&&next->pairs[*incoming.scope.pair]&&next->pairs[*incoming.scope.pair]->context,Error::association);
            const auto& pair=*next->pairs[*incoming.scope.pair];
            auto body=component_value(wire::EvidenceComponentKind::finalized_error,string(facts.at("artifact")),*pair.context,limits,work);
            require(wire::detail::reconstruct_finalized_error_value(body.value()).has_value(),Error::child);
        }
        if(incoming.scope.pair){require(next->planning&&*incoming.scope.pair<next->pairs.size()&&incoming.scope.experiment&&
            *incoming.scope.experiment==next->planning->pairs[*incoming.scope.pair].experiment_index,Error::association);}
        if(failure.process){require(incoming.scope.pair&&next->pairs[*incoming.scope.pair]&&next->pairs[*incoming.scope.pair]->exit,Error::association);
            const auto& exit=*next->pairs[*incoming.scope.pair]->exit;
            require(failure.stage==20U&&(failure.kind=="process_signaled"?exit.signaled:!exit.signaled&&exit.value!=0U),Error::association);}
        else next->operational_failure=true;
    }else if(incoming.kind==Kind::planning){
        fields(facts,{"origin","artifact","normalization_ref","topology_ref","build_ref"});
        require(!next->planning,Error::association);
        const auto& normal_record=event_reference(facts.at("normalization_ref"),"scenario_resolved",records,Kind::normalization,work);
        const auto& top_record=event_reference(facts.at("topology_ref"),"topology_retained",records,Kind::topology,work);
        const auto& normal=std::get<normalization_serialization::Owner>(normal_record.artifact);
        const auto& topology=std::get<topology_serialization::Owner>(top_record.artifact);
        require(normal.topology()&&work.equal(normal.topology()->canonical(),topology.canonical()),Error::association);
        const auto& build=facts.at("build_ref");fields(build,{"sequence","ordinal","kind"});
        require(number(build.at("sequence"))==incoming.sequence&&number(build.at("ordinal"))==0U&&string(build.at("kind"))=="base_build",Error::reference);
        Limits artifact_limits;artifact_limits.body_bytes=limits.planning.bytes;
        artifact_limits.nodes=limits.planning.nodes;artifact_limits.depth=limits.planning.depth;
        artifact_limits.text_bytes=limits.planning.text;artifact_limits.candidate_bytes=limits.planning.simultaneous;
        std::string reconstructed;
        const auto artifact=artifact_bytes(Kind::planning,facts.at("artifact"),incoming.version,artifact_limits,work,reconstructed);
        auto plan=decode_plan(artifact,normal,limits.planning,work,
            reconstructed.empty()?0U:add(reconstructed.capacity(),1U));
        require(plan.base&&plan.pairs.size()<=limits.pairs,Error::capacity);
        require(genesis.base_build()&&plan.base->provenance()&&genesis.base_build()->provenance(),Error::association);
        const auto& selected_build=*plan.base->provenance();
        const auto& created_build=*genesis.base_build()->provenance();
        require(selected_build.kind==created_build.kind&&selected_build.sequence==created_build.sequence&&
            selected_build.acquisition==created_build.acquisition&&
            work.equal(plan.base->canonical(),genesis.base_build()->canonical()),Error::association);
        next->planning=std::make_shared<const PlanFacts>(std::move(plan));
        next->normalization_record=normal_record.sequence;next->planning_record=incoming.sequence;
        next->pairs.resize(next->planning->pairs.size());
    }else if(incoming.kind==Kind::preflight){
        const bool observed_checks=facts.contains("controller_checks");
        if(observed_checks) {
            fields(facts,{"origin","source_entry","references","started_at","ended_at","scopes","workers","caches","nodes","checks","temperatures","overrides","warnings","failures","monitoring","status","controller_checks"});
            require(is_progress(incoming.version)&&genesis.provenance()->domain==process_subject::Domain::supplied_observation&&
                incoming.availability&&incoming.availability->source==AvailabilitySource::controller_observation&&
                facts.at("origin")=="actual_availability",Error::association);
            require(controller_check_snapshot_valid(facts.at("controller_checks"),limits.error),Error::schema);
        } else fields(facts,{"origin","source_entry","references","started_at","ended_at","scopes","workers","caches","nodes","checks","temperatures","overrides","warnings","failures","monitoring","status"});
        require(next->planning&&!next->preflight&&genesis.host(),Error::association);
        const auto& normal=std::get<normalization_serialization::Owner>(records[static_cast<std::size_t>(next->normalization_record-1U)].artifact);
        const auto& entry=facts.at("source_entry");fields(entry,{"sequence","time"});
        const auto source_sequence=number(entry.at("sequence"));
        const auto source_time=journal::Timestamp::parse(string(entry.at("time")));require(source_time.has_value(),Error::schema);
        require(source_sequence>std::max({genesis.provenance()->sequence,normal.provenance()->sequence,next->planning->base->provenance()->sequence}),Error::reference);
        const auto& refs=facts.at("references");fields(refs,{"controller","normalization","system_state","base_build"});
        require(number(refs.at("controller"))==genesis.provenance()->sequence&&
            number(refs.at("normalization"))==normal.provenance()->sequence&&
            number(refs.at("system_state"))==genesis.host()->provenance()->sequence&&
            number(refs.at("base_build"))==next->planning->base->provenance()->sequence,Error::reference);
        auto payload=core::detail::clone_json(facts);core::detail::JsonCleanup payload_cleanup{payload};
        if(observed_checks){core::detail::retire_json(payload.at("controller_checks"));payload.erase("controller_checks");}
        payload.erase("origin");core::detail::retire_json(payload.at("source_entry"));payload.erase("source_entry");
        auto canonical=core::canonicalize_json_jcs(payload,{limits.preflight.bytes,limits.preflight.depth,limits.preflight.nodes});
        core::detail::ExpectedErrorCleanup cleanup{canonical};require(canonical.has_value(),Error::canonical);work.debit(canonical->size());
        auto decoded=decode_preflight(*canonical,preflight_context(normal,*genesis.host(),*next->planning->base),limits.preflight,work);
        if(observed_checks)require(decoded->monitoring&&controller_check_snapshot_matches(
            facts.at("controller_checks"),*decoded->monitoring,facts.at("temperatures")),Error::association);
        require(decoded->scopes.size()==next->planning->pairs.size(),Error::association);
        for(const auto& pair:next->planning->pairs){
            work.debit(decoded->scopes.size());
            const auto count=std::ranges::count_if(decoded->scopes,[&](const auto& scope){return
                scope.matrix==(pair.kind==execution_plan::PairKind::matrix)&&scope.experiment_index==pair.experiment_index&&
                scope.baseline_index==pair.baseline_index&&scope.candidate_index==pair.candidate_index;});
            require(count==1,Error::association);
        }
        next->preflight=std::move(decoded);next->preflight_record=incoming.sequence;
    }else if(incoming.kind==Kind::pair_started){
        fields(facts,{"origin","planning_ref","preflight_ref","controller_ref","runner_ref","session_id","pair_ordinal","spawn"});
        require(incoming.producer==process_subject::Producer::controller&&next->planning&&next->preflight,Error::association);
        require(next->preflight->status==preflight::Status::accepted||next->preflight->status==preflight::Status::accepted_with_warnings,Error::association);
        const auto ordinal=number(facts.at("pair_ordinal"));require(ordinal<next->pairs.size()&&!next->pairs[ordinal],Error::reference);
        const auto& edge=next->planning->pairs[ordinal];
        require(incoming.scope==Scope{edge.experiment_index,ordinal,{},{}},Error::association);
        require(event_reference(facts.at("planning_ref"),"planning",records,Kind::planning,work).sequence==next->planning_record&&
            event_reference(facts.at("preflight_ref"),"preflight",records,Kind::preflight,work).sequence==next->preflight_record&&
            event_reference(facts.at("controller_ref"),"creation",records,Kind::creation,work).sequence==1U,Error::reference);
        require(facts.at("runner_ref").is_null(),Error::reference); // No runner-creation event exists in this supplied profile.
        const auto& session=string(facts.at("session_id"));auto identity=runtime::wire_v1::detail::StartupSessionId::parse(session);require(identity.has_value(),Error::identity);
        const auto& spawn=facts.at("spawn");const auto& state=string(spawn.at("state"));std::uint64_t child{};
        if(state=="created"){fields(spawn,{"state","child_token"});child=number(spawn.at("child_token"));require(child,Error::identity);}
        else{require(state=="failed",Error::schema);fields(spawn,{"state","failure_ref"});
            const auto& failure=event_reference(spawn.at("failure_ref"),"failure",records,Kind::failure,work);
            const auto descriptor=failure_descriptor(failure,records,work);
            require(failure.scope.pair==incoming.scope.pair&&failure.scope.experiment==incoming.scope.experiment&&
                (descriptor.spawn||descriptor.kind=="spawn"),Error::reference);}
        for(const auto& pair:next->pairs)if(pair){work.debit(1U);require(pair->session!=session&&(!child||pair->child_token!=child),Error::identity);}
        auto pair=std::make_shared<ComposedPair>();pair->start_record=incoming.sequence;pair->child_token=child;pair->session=session;
        pair->retained=add(sizeof(ComposedPair),add(session.size(),1U));next->pairs[ordinal]=std::move(pair);
    }else if(incoming.kind==Kind::request_accepted){
        fields(facts,{"origin","start_ref","request","context","association"});
        if(incoming.availability&&incoming.availability->source==AvailabilitySource::controller_observation)
            require(is_progress(incoming.version)&&genesis.provenance()->domain==process_subject::Domain::supplied_observation,Error::association);
        require(incoming.producer==process_subject::Producer::runner&&next->planning&&incoming.scope.pair&&incoming.scope.experiment&&
            !incoming.scope.block&&!incoming.scope.observation,Error::association);
        const auto ordinal=*incoming.scope.pair;require(ordinal<next->pairs.size()&&next->pairs[ordinal]&&!next->pairs[ordinal]->context,Error::reference);
        const auto& old=*next->pairs[ordinal];const auto& edge=next->planning->pairs[ordinal];require(old.child_token&&*incoming.scope.experiment==edge.experiment_index,Error::identity);
        require(event_reference(facts.at("start_ref"),"pair_start",records,Kind::pair_started,work).sequence==old.start_record,Error::reference);
        const auto& planned_origin=string(records[static_cast<std::size_t>(next->planning_record-1U)].event_facts->at("origin"));
        const auto& checked_origin=string(records[static_cast<std::size_t>(next->preflight_record-1U)].event_facts->at("origin"));
        require(planned_origin==checked_origin&&(planned_origin=="supplied_synthetic"||planned_origin=="actual_availability"),Error::association);
        if(planned_origin=="actual_availability")
            require(string(facts.at("origin"))=="actual_availability"&&incoming.availability&&
                incoming.availability->source==AvailabilitySource::controller_observation,Error::association);
        const auto& normal=std::get<normalization_serialization::Owner>(records[static_cast<std::size_t>(next->normalization_record-1U)].artifact);
        auto decoded=PairContextDecoder::decode(string(facts.at("context")),normal,*next->planning,static_cast<std::size_t>(ordinal),limits.context,work,planned_origin);
        require(work.equal(string(facts.at("request")),decoded.request_body),Error::association);
        auto request=runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::parse_request(decoded.request_body);require(request.has_value(),Error::child);
        require(decoded.session==old.session,Error::identity);
        const auto& association=facts.at("association");fields(association,{"state","planning_ref","pair_ordinal","first_request","second_request","statistics_source","services"});
        require(string(association.at("state"))=="replay_consistent"&&number(association.at("pair_ordinal"))==ordinal&&
            event_reference(association.at("planning_ref"),"planning",records,Kind::planning,work).sequence==next->planning_record&&
            number(association.at("first_request"))==request->first_request_id().value()&&number(association.at("second_request"))==request->second_request_id().value(),Error::association);
        // This exact subtree already passed the closed request parser above.
        Limits request_limits;request_limits.body_bytes=limits.context.bytes;request_limits.nodes=limits.context.nodes;
        request_limits.depth=limits.context.depth;request_limits.text_bytes=limits.context.text;request_limits.candidate_bytes=limits.context.simultaneous;
        auto request_json=parse_tree(decoded.request_body,request_limits,work);core::detail::JsonCleanup request_cleanup{request_json};
        require(!request_json.is_discarded()&&association.at("statistics_source")==request_json.at("payload").at("statistics_source"),Error::association);
        const auto& services=association.at("services");require(services.is_array()&&services.size()==decoded.service_count,Error::association);
        for(std::size_t i{};i<services.size();++i){const auto& row=services[i];const auto& expected=decoded.services[i];
            fields(row,{"role","reserved_cpu","reserved_core","pool","pool_cpu"});
            require(string(row.at("role"))==(expected.role==runtime::ServiceRole::control_monitor?"control_monitor":"measurement_monitor")&&
                number(row.at("reserved_cpu"))==expected.reserved_cpu&&number(row.at("reserved_core"))==expected.reserved_core&&
                number(row.at("pool"))==expected.pool&&number(row.at("pool_cpu"))==expected.pool_cpu,Error::association);}
        auto prefix=PairContextDecoder::prepare(decoded,limits.pair,limits.timing);require(prefix.has_value(),Error::child);
        auto pair=std::make_shared<ComposedPair>();pair->start_record=old.start_record;pair->child_token=old.child_token;pair->session=old.session;
        pair->request_record=incoming.sequence;pair->context=std::make_shared<const DecodedContext>(std::move(decoded));pair->prefix.emplace(std::move(*prefix));
        pair->events.reserve(old.events.size());
        for(const auto sequence:old.events){const auto& event=records[static_cast<std::size_t>(sequence-1U)];
            require(replay_lifecycle(*pair,event,records,work),Error::association);}
        pair->retained=add(old.retained,add(pair->context->retained_bytes,pair->prefix->usage().retained));next->pairs[ordinal]=std::move(pair);
    }else if(incoming.kind==Kind::monitoring||incoming.kind==Kind::placement||incoming.kind==Kind::preparation||incoming.kind==Kind::calibration||
        incoming.kind==Kind::warmup||incoming.kind==Kind::block_opened||incoming.kind==Kind::observation||
        incoming.kind==Kind::failed_observation||incoming.kind==Kind::block_closed||incoming.kind==Kind::stopping||incoming.kind==Kind::pair_terminal||incoming.kind==Kind::finalization||incoming.kind==Kind::analysis||incoming.kind==Kind::lifecycle_progress||incoming.kind==Kind::event_eof||incoming.kind==Kind::child_reaped){
        require(next->planning&&incoming.scope.pair&&incoming.scope.experiment,Error::association);
        const auto ordinal=*incoming.scope.pair;require(ordinal<next->pairs.size()&&next->pairs[ordinal],Error::reference);
        if(!next->pairs[ordinal]->child_token)require(incoming.kind==Kind::lifecycle_progress&&
            is_progress(incoming.version)&&genesis.provenance()->domain==process_subject::Domain::supplied_observation&&
            incoming.producer==process_subject::Producer::controller&&incoming.availability&&
            incoming.availability->source==AvailabilitySource::controller_observation&&facts.at("transition")=="cleanup",Error::reference);
        const auto& old=*next->pairs[ordinal];
        require(old.context||incoming.kind==Kind::lifecycle_progress||incoming.kind==Kind::event_eof||incoming.kind==Kind::child_reaped,Error::association);require(*incoming.scope.experiment==next->planning->pairs[ordinal].experiment_index,Error::identity);
        require(old.events.size()<limits.events&&old.events.size()<UINT64_MAX,Error::capacity);
        const auto replay_stock=add(limits.context.retained,add(limits.pair.simultaneous_bytes,multiply(add(old.events.size(),1U),sizeof(std::uint64_t)*4U)));
        require(replay_stock<=limits.simultaneous,Error::capacity);work.reserve_peak(add(allowance,replay_stock));
        auto pair=std::make_shared<ComposedPair>();pair->start_record=old.start_record;pair->request_record=old.request_record;
        pair->child_token=old.child_token;pair->session=old.session;pair->context=old.context;
        if(old.context){auto prefix=PairContextDecoder::prepare(*old.context,limits.pair,limits.timing);require(prefix.has_value(),Error::child);pair->prefix.emplace(std::move(*prefix));}
        pair->events.reserve(add(old.events.size(),1U));pair->children.reserve(add(old.children.size(),1U));pair->captures.reserve(add(old.captures.size(),1U));
        for(const auto sequence:old.events){const auto& event=records[static_cast<std::size_t>(sequence-1U)];
            work.debit(event.canonical.size());replay_component(*pair,event,records,limits,work);}
        work.debit(incoming.canonical.size());replay_component(*pair,incoming,records,limits,work);
        pair->retained=add(sizeof(ComposedPair),add(add(pair->session.size(),1U),add(pair->context?pair->context->retained_bytes:0U,
            add(add(pair->prefix?pair->prefix->usage().retained:0U,pair->analysis_retained),add(multiply(add(pair->events.capacity(),pair->children.capacity()),sizeof(std::uint64_t)),multiply(pair->captures.capacity(),sizeof(CaptureAssociation)))))));
        next->pairs[ordinal]=std::move(pair);
    }else if(incoming.kind==Kind::run_closed){
        fields(facts,{"origin","execution","plan_disposition","pairs","primary_ref","secondary_refs"});
        require(incoming.producer==process_subject::Producer::results&&incoming.scope==Scope{},Error::association);
        const auto& execution=string(facts.at("execution"));require(execution=="completed"||execution=="failed",Error::schema);
        const bool successful=execution=="completed";
        const auto& disposition=string(facts.at("plan_disposition"));require(disposition=="available"||disposition=="unavailable",Error::schema);
        const bool available=disposition=="available";require(available?bool(next->planning):!next->planning,Error::association);
        if(successful)require(available&&next->preflight,Error::association);
        require(facts.at("secondary_refs").is_array(),Error::reference);
        auto [run_primary,run_rank]=governing_failure(records,std::nullopt,nullptr,limits,work);
        if(successful)require(next->preflight->status==preflight::Status::accepted||next->preflight->status==preflight::Status::accepted_with_warnings,Error::association);
        const auto& rows=facts.at("pairs");require(rows.is_array()&&rows.size()==next->pairs.size(),Error::association);
        for(std::size_t i{};i<rows.size();++i){work.debit(1U);const auto& row=rows[i];
            const bool completed=string(row.at("state"))=="completed";require(!successful||completed,Error::association);
            if(completed)fields(row,{"state","start_ref","request_ref","root_ref","analysis_ref","eof_ref","reap_ref","cleanup_ref"});
            else fields(row,{"state","failure_ref","start_ref","request_ref","root_ref","analysis_ref","eof_ref","reap_ref","cleanup_ref"});
            if(!next->pairs[i]){
                require(!successful&&string(row.at("state"))=="unstarted",Error::association);
                auto [governing,rank]=governing_failure(records,static_cast<std::uint64_t>(i),nullptr,limits,work);if(!governing){governing=run_primary;rank=run_rank;}
                require(governing&&event_reference(row.at("failure_ref"),"failure",records,Kind::failure,work).sequence==governing->sequence,Error::reference);
                for(const auto key:{"start_ref","request_ref","root_ref","analysis_ref","eof_ref","reap_ref","cleanup_ref"})require(row.at(key).is_null(),Error::reference);
                if(!run_primary||rank<run_rank){run_primary=governing;run_rank=rank;}
                continue;
            }
            const auto& pair=*next->pairs[i];
            const auto [governing,rank]=governing_failure(records,static_cast<std::uint64_t>(i),&pair,limits,work);
            if(completed)require(!governing&&pair.analysis&&pair.completion_record&&pair.clean_eof&&pair.reap_record&&pair.cleanup_complete&&pair.exit&&
                !pair.exit->signaled&&pair.exit->value==0U&&!pair.cancellation_record,Error::association);
            else{
                require(string(row.at("state"))=="failed"&&governing,Error::association);
                require(event_reference(row.at("failure_ref"),"failure",records,Kind::failure,work).sequence==governing->sequence,Error::reference);
                if(!run_primary||rank<run_rank){run_primary=governing;run_rank=rank;}
            }
            const std::array<SV,6> keys{"start_ref","request_ref","root_ref","analysis_ref","eof_ref","reap_ref"};
            const std::array<SV,6> kinds{"pair_start","request","finalization","analysis","eof","reap"};
            const std::array<Kind,6> expected{Kind::pair_started,Kind::request_accepted,Kind::finalization,Kind::analysis,Kind::event_eof,Kind::child_reaped};
            const std::array<std::uint64_t,6> sequences{pair.start_record,pair.request_record,pair.root_record,pair.analysis_record,pair.eof_record,pair.reap_record};
            for(std::size_t j{};j<keys.size();++j){if(!sequences[j])require(row.at(keys[j]).is_null(),Error::reference);
                else require(event_reference(row.at(keys[j]),kinds[j],records,expected[j],work).sequence==sequences[j],Error::reference);}
            const auto& cleanup=row.at("cleanup_ref");if(!pair.cleanup_record)require(cleanup.is_null(),Error::reference);
            else{fields(cleanup,{"sequence","ordinal","kind"});
                require(number(cleanup.at("sequence"))==pair.cleanup_record&&number(cleanup.at("ordinal"))==0U&&string(cleanup.at("kind"))=="cleanup",Error::reference);}
        }
        if(successful)require(!run_primary&&facts.at("primary_ref").is_null()&&facts.at("secondary_refs").empty(),Error::reference);
        else{
            require(run_primary&&event_reference(facts.at("primary_ref"),"failure",records,Kind::failure,work).sequence==run_primary->sequence,Error::reference);
            const auto& secondaries=facts.at("secondary_refs");std::size_t index{};
            for(const auto& record:records)if(record.kind==Kind::failure){
                const bool linked=record.event_facts?record.event_facts->at("primary_ref")==facts.at("primary_ref"):
                    record.primary&&record.primary->sequence==run_primary->sequence;
                if(linked){require(index<secondaries.size()&&event_reference(secondaries[index],"failure",records,Kind::failure,work).sequence==record.sequence,Error::reference);++index;}}
            require(index==secondaries.size(),Error::reference);next->operational_failure=true;
        }
        next->closed_record=incoming.sequence;
    }else throw Error::unsupported;
    next->retained=add(sizeof(CompositionState),add(next->planning?next->planning->retained:0U,next->preflight?next->preflight->usage.retained_bytes:0U));
    next->retained=add(next->retained,multiply(next->pairs.capacity(),sizeof(std::shared_ptr<const ComposedPair>)));
    for(const auto& pair:next->pairs)if(pair)next->retained=add(next->retained,pair->retained);
    require(next->retained<=limits.retained,Error::capacity);return next;
}
}
