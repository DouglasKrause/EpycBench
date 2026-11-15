#include "cli/benchmark_run_composition.hpp"
#include "results/incremental_event_pair.hpp"
#include "core/json_ownership.hpp"
#include "runtime/finalized_evidence_prepared_direct_codec.hpp"
#include "runtime/pairwise_v2_production_entry.hpp"
#include "runtime/controller_signal_cancellation.hpp"
#include "runtime/controller_lifecycle_policy.hpp"
#include "epycbench/runtime/pairwise_production_composition.hpp"

#include <array>
#include <charconv>
#include <ostream>

namespace epycbench::cli::benchmark_run_composition {
namespace {
constexpr std::size_t path_limit=4095U, component_limit=255U, component_count=256U;
auto root(std::string_view value) noexcept -> bool {
    if(value.size()<2U||value.size()>path_limit||value.front()!='/'||value.back()=='/'||
       value.find('\0')!=std::string_view::npos)return false;
    std::size_t offset=1U,count{};
    while(offset<value.size()) {
        const auto end=value.find('/',offset);
        const auto part=value.substr(offset,end==std::string_view::npos?value.size()-offset:end-offset);
        if(part.empty()||part.size()>component_limit||part=="."||part==".."||++count>component_count)return false;
        if(end==std::string_view::npos)break;
        offset=end+1U;
    }
    return true;
}
auto identifier(std::string_view value) noexcept -> bool {
    if(value.empty()||value.size()>64U||value.front()<'a'||value.front()>'z')return false;
    for(const char c:value)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))return false;
    return true;
}
}
auto validate_identity(const results::process_subject::Session& session,
    const platform_linux::detail::RunStartIdentity& identity) noexcept -> std::expected<void,Error> {
    if(session.domain()!=results::process_subject::Domain::supplied_observation||
       session.run_id()!=identity.run_id()||session.entries().empty())return std::unexpected(Error::identity);
    const auto& entry=session.entries().front();
    if(entry.sequence!=1U||entry.timestamp!=identity.utc_start()||!entry.run_creation||
       entry.producer!=results::process_subject::Producer::controller)return std::unexpected(Error::identity);
    const auto* facts=entry.run_creation->facts();
    if(!facts||facts->run!=identity.run_id()||facts->domain!=results::process_subject::Domain::supplied_observation)
        return std::unexpected(Error::identity);
    return {};
}
auto prepare_output(const nlohmann::ordered_json& scenario,std::string_view xdg,
    const platform_linux::detail::RunStartIdentity& identity) -> std::expected<Output,Error> {
    if(!scenario.is_object()||!scenario.contains("metadata")||!scenario["metadata"].is_object()||
       !scenario["metadata"].contains("id")||!scenario["metadata"]["id"].is_string()||
       !scenario.contains("output")||!scenario["output"].is_object())return std::unexpected(Error::output_policy);
    const auto& output=scenario["output"];
    if(!output.contains("directory")||!output["directory"].is_string()||
       (output.contains("format")&&output["format"]!="json"))return std::unexpected(Error::output_policy);
    const auto& id=scenario["metadata"]["id"].get_ref<const std::string&>();
    const auto& directory=output["directory"].get_ref<const std::string&>();
    constexpr std::string_view suffix="/epycbench";
    if(!identifier(id)||!root(directory)||!root(xdg)||xdg.size()>path_limit-suffix.size())
        return std::unexpected(Error::output_policy);
    // Check the final component count before allocating either owned pathname.
    std::size_t separators{};for(const char c:xdg)if(c=='/')++separators;
    if(separators>=component_count)return std::unexpected(Error::output_policy);
    if(output.contains("retain")) {
        const auto& retained=output["retain"];
        if(!retained.is_object())return std::unexpected(Error::output_policy);
        constexpr std::array fields{"authored_scenario","normalized_scenario","resolved_plan",
            "topology_snapshot","warmups","raw_observations","monitoring_samples"};
        for(const auto* field:fields)if(retained.contains(field)&&retained[field]!=true)
            return std::unexpected(Error::output_policy);
    }
    Output result;
    result.journal_root.reserve(xdg.size()+suffix.size());result.journal_root.append(xdg);result.journal_root.append(suffix);
    result.final_root=directory;
    result.final_leaf.reserve(id.size()+85U);
    result.final_leaf.append(id).append("__").append(identity.utc_start()).append("__")
        .append(identity.run_id()).append(".epycbench.json");
    return result;
}
auto append_retained(results::filesystem_journal::Writer& writer,
    const results::process_subject::Session& session,const results::execution_plan::Owner& plan,
    const results::event_profile::Limits& limits) -> std::expected<RetainedReferences,Failure> {
    namespace event=results::event_profile;
    namespace subject=results::process_subject;
    const auto* transaction=results::filesystem_journal::detail::Access::transaction(writer);
    if(!transaction||!event::is_progress(transaction->version())||!writer.records().empty()||
       !limits.composition||session.domain()!=subject::Domain::supplied_observation||
       session.entries().empty()||!session.entries().front().run_creation||!plan.evidence())
        return std::unexpected(Failure{Error::source,{}});
    const subject::Entry* normalized{};const subject::Entry* assessed{};
    for(const auto& entry:session.entries()) {
        if(entry.normalization) {if(normalized)return std::unexpected(Failure{Error::source,{}});normalized=&entry;}
        if(entry.preflight) {if(assessed)return std::unexpected(Failure{Error::source,{}});assessed=&entry;}
    }
    if(!normalized||!assessed||!assessed->preflight->facts())return std::unexpected(Failure{Error::source,{}});
    const auto& created=session.entries().front();
    auto creation=session.serialize_run_creation(created.sequence,limits.child.creation);
    auto normalization=session.serialize_normalization(normalized->sequence,limits.child.normalization);
    if(!creation||!normalization||!normalization->topology())return std::unexpected(Failure{Error::source,{}});
    auto creation_time=results::journal::Timestamp::parse(created.timestamp);
    core::detail::ExpectedErrorCleanup creation_cleanup{creation_time};
    auto normalization_time=results::journal::Timestamp::parse(normalized->timestamp);
    core::detail::ExpectedErrorCleanup normalization_cleanup{normalization_time};
    auto preflight_time=results::journal::Timestamp::parse(assessed->timestamp);
    core::detail::ExpectedErrorCleanup preflight_cleanup{preflight_time};
    if(!creation_time||!normalization_time||!preflight_time)return std::unexpected(Failure{Error::source,{}});
    RetainedReferences refs;
    const auto artifact=[&](const event::Input& input,std::uint64_t& sequence) -> std::optional<Failure> {
        auto added=writer.append(input);if(!added)return Failure{Error::journal,added.error()};
        sequence=added->semantic.records;return {};
    };
    if(auto failure=artifact({*creation_time,&*creation,{},{},{}},refs.creation))return std::unexpected(*failure);
    const event::Reference creation_ref{refs.creation,0U,event::RefKind::creation};
    if(auto failure=artifact({*normalization_time,normalization->topology(),creation_ref,{},{}},refs.topology))return std::unexpected(*failure);
    const event::Reference topology_ref{refs.topology,0U,event::RefKind::topology};
    if(auto failure=artifact({*normalization_time,&*normalization,creation_ref,topology_ref,{}},refs.normalization))return std::unexpected(*failure);
    const auto reference=[](std::uint64_t sequence,std::string_view kind) -> nlohmann::json {
        return {{"sequence",std::to_string(sequence)},{"ordinal","0"},{"kind",kind}};
    };
    // The planning artifact includes the real selected Build child. Its local
    // base-build reference is the incoming planning record, as C2 requires.
    nlohmann::json planned{{"origin","supplied_synthetic"},{"artifact",plan.canonical()},
        {"normalization_ref",reference(refs.normalization,"scenario_resolved")},
        {"topology_ref",reference(refs.topology,"topology_retained")},
        {"build_ref",reference(writer.receipt().semantic.records+1U,"base_build")}};
    core::detail::JsonCleanup planned_cleanup{planned};
    auto planning=writer.append(event::SuppliedEvent{*normalization_time,event::Kind::planning,subject::Producer::controller,{},planned});
    if(!planning)return std::unexpected(Failure{Error::journal,planning.error()});
    refs.planning=planning->semantic.records;
    try {
        event::detail::Work work{0U,limits.work};
        auto encoded=event::detail::encode_preflight(*assessed->preflight->facts(),limits.composition->preflight,work);
        auto facts=event::detail::parse_tree(encoded,limits,work);core::detail::JsonCleanup facts_cleanup{facts};
        facts["origin"]="supplied_synthetic";
        facts["source_entry"]={{"sequence",std::to_string(assessed->sequence)},{"time",assessed->timestamp}};
        auto preflight=writer.append(event::SuppliedEvent{*preflight_time,event::Kind::preflight,subject::Producer::controller,{},facts});
        if(!preflight)return std::unexpected(Failure{Error::journal,preflight.error()});
        refs.preflight=preflight->semantic.records;
    } catch(event::Error error) {
        return std::unexpected(Failure{Error::source,results::filesystem_journal::Failure{
            results::filesystem_journal::Error::semantic,error,{}}});
    }
    return refs;
}
auto prepared_references(const results::filesystem_journal::Writer& writer,
    const results::process_subject::Session& session,const results::execution_plan::Owner& plan,
    const results::event_profile::Limits& limits) -> std::expected<RetainedReferences,Failure> {
    namespace ep=results::event_profile;
    const auto receipt=writer.receipt();const auto rows=writer.records();
    const auto refuse=[] {return std::unexpected{Failure{Error::source,{},{}}};};
    if(receipt.state!=results::filesystem_journal::State::open||receipt.failure||receipt.partial_frame_bytes||
       receipt.complete.records!=receipt.semantic.records||receipt.complete.digest!=receipt.semantic.digest||
       rows.size()!=5||!limits.composition||session.entries().empty()||!session.entries().front().run_creation||
       session.domain()!=results::process_subject::Domain::supplied_observation||!plan.evidence())return refuse();
    constexpr std::array kinds{ep::Kind::creation,ep::Kind::topology,ep::Kind::normalization,ep::Kind::planning,ep::Kind::preflight};
    for(std::size_t i=0;i<rows.size();++i)
        if(rows[i].kind!=kinds[i]||rows[i].sequence!=i+1||!ep::is_progress(rows[i].version)||rows[i].version!=limits.version)return refuse();
    const results::process_subject::Entry* normalized{};const results::process_subject::Entry* assessed{};
    for(const auto& entry:session.entries()) {
        if(entry.normalization){if(normalized)return refuse();normalized=&entry;}
        if(entry.preflight){if(assessed)return refuse();assessed=&entry;}
    }
    if(!normalized||!assessed||!assessed->preflight->facts()||!rows[3].event_facts||!rows[4].event_facts)return refuse();
    const auto status=assessed->preflight->facts()->status;
    if(status!=results::preflight::Status::accepted&&status!=results::preflight::Status::accepted_with_warnings)return refuse();
    try {
        auto creation=session.serialize_run_creation(session.entries().front().sequence,limits.child.creation);
        auto normalization=session.serialize_normalization(normalized->sequence,limits.child.normalization);
        const auto* retained_creation=std::get_if<results::run_creation_serialization::Owner>(&rows[0].artifact);
        const auto* retained_topology=std::get_if<results::topology_serialization::Owner>(&rows[1].artifact);
        const auto* retained_normal=std::get_if<results::normalization_serialization::Owner>(&rows[2].artifact);
        if(!creation||!normalization||!normalization->topology()||!retained_creation||!retained_topology||!retained_normal||
           retained_creation->canonical()!=creation->canonical()||retained_normal->canonical()!=normalization->canonical()||
           retained_topology->canonical()!=normalization->topology()->canonical())return refuse();
        const auto& planned=*rows[3].event_facts;
        const auto* transaction=results::filesystem_journal::detail::Access::transaction(writer);
        const auto* composition=transaction?transaction->composition():nullptr;
        // C2 has already admitted the exact original parent bytes. V3 stores
        // a closed UTF-8 projection, so compare its immutable decoded evidence
        // instead of equating the presentation object with a legacy string.
        if(!composition||!composition->planning||composition->planning_record!=rows[3].sequence||
           composition->planning->canonical!=plan.canonical()||planned.at("origin")!="actual_availability"||
           !rows[3].availability||rows[3].availability->source!=ep::AvailabilitySource::controller_observation)return refuse();
        ep::detail::Work work{0,limits.work};
        auto body=ep::detail::encode_preflight(*assessed->preflight->facts(),limits.composition->preflight,work);
        auto facts=ep::detail::parse_tree(body,limits,work);core::detail::JsonCleanup cleanup{facts};
        facts["origin"]="actual_availability";
        facts["source_entry"]={{"sequence",std::to_string(assessed->sequence)},{"time",assessed->timestamp}};
        if(rows[4].event_facts->contains("controller_checks")) {
            const auto& snapshot=rows[4].event_facts->at("controller_checks");
            const auto& assessment=assessed->preflight->facts()->monitoring;
            if(!assessment||!ep::detail::controller_check_snapshot_valid(snapshot,limits.composition->error)||
               !ep::detail::controller_check_snapshot_matches(snapshot,*assessment,facts.at("temperatures")))return refuse();
            facts["controller_checks"]=core::detail::clone_json(snapshot);
        }
        if(facts!=*rows[4].event_facts||!rows[4].availability||
           rows[4].availability->source!=ep::AvailabilitySource::controller_observation)return refuse();
        return RetainedReferences{1,2,3,4,5};
    } catch(...) {return refuse();}
}
auto finalize(results::filesystem_journal::Writer& writer,const Output& output,
    const results::journal::RunId& run,const results::final_document::FinalizationFacts& facts,
    results::filesystem_journal::Limits journal_limits,results::final_document::Limits limits,
    std::optional<platform_linux::journal_file::Limits> publication_limits) -> FinalReport {
    namespace journal=results::filesystem_journal;
    namespace document=results::final_document;
    FinalReport result;
    const auto sealed=writer.seal();
    if(!sealed)result.primary=Failure{Error::journal,sealed.error(),{}};
    const auto closed=writer.close();
    result.writer=writer.receipt();
    if(!result.primary&&closed.primary)result.primary=Failure{Error::journal,*closed.primary,{}};
    if(!result.primary&&closed.count)result.primary=Failure{Error::journal,journal::Failure{
        journal::Error::io,{},platform_linux::journal_file::Failure{closed.secondary[0],{},false}}, {}};
    // Even after write/close failure, a fresh read may establish a smaller valid
    // prefix. Preserve the original failure while attempting factual recovery.
    auto recovered=journal::Recovery::read(output.journal_root,run,limits.events,journal_limits);
    if(!recovered) {
        result.recovery_failure=recovered.error();
        if(!result.primary)result.primary=Failure{Error::journal,recovered.error(),{}};
        return result;
    }
    auto assembled=document::assemble(*recovered,facts,limits);
    const auto recovery_close=recovered->close();
    result.recovery=recovered->receipt();
    if(!result.primary&&recovery_close.primary)result.primary=Failure{Error::journal,*recovery_close.primary,{}};
    if(!result.primary&&recovery_close.count)result.primary=Failure{Error::journal,journal::Failure{
        journal::Error::io,{},platform_linux::journal_file::Failure{recovery_close.secondary[0],{},false}}, {}};
    if(!assembled) {
        result.final_failure=assembled.error();
        if(!result.primary)result.primary=Failure{Error::finalization,{},assembled.error()};
        return result;
    }
    // Assembly itself validates, then publication consumes that exact one-use
    // Validated. Existing C4 rejects mismatched leaf names and replacement.
    auto published=document::publish(std::move(*assembled),output.final_root,output.final_leaf,
        publication_limits.value_or(journal_limits.file),journal::Durability::process);
    if(!published) {
        result.final_failure=published.error();
        if(!result.primary)result.primary=Failure{Error::finalization,{},published.error()};
        return result;
    }
    if(published->receipt.failure) {
        result.final_failure=*published->receipt.failure;
        if(!result.primary)result.primary=Failure{Error::finalization,{},*published->receipt.failure};
    }
    result.publication.emplace(std::move(*published));
    return result;
}
auto close_completed(results::filesystem_journal::Writer& writer,const results::journal::Timestamp& time,
    results::preparation_observation::Adapter* observer)
    -> std::expected<results::filesystem_journal::Receipt,Failure> {
    namespace event=results::event_profile;
    if(observer&&!observer->bound_to(writer))return std::unexpected(Failure{Error::identity,{},{}});
    const auto* transaction=results::filesystem_journal::detail::Access::transaction(writer);
    const auto* state=transaction?transaction->composition():nullptr;
    if(!state||!state->planning||!state->preflight||state->closed_record||state->operational_failure)
        return std::unexpected(Failure{Error::source,{}});
    nlohmann::json rows=nlohmann::json::array();core::detail::JsonCleanup rows_cleanup{rows};
    const auto reference=[](std::uint64_t sequence,std::string_view kind) -> nlohmann::json {
        if(!sequence)return nlohmann::json(nullptr);
        return {{"sequence",std::to_string(sequence)},{"ordinal","0"},{"kind",kind}};
    };
    for(const auto& pair:state->pairs) {
        if(!pair||!pair->analysis||!pair->completion_record||!pair->clean_eof||!pair->cleanup_complete||
           !pair->exit||pair->exit->signaled||pair->exit->value||pair->cancellation_record)
            return std::unexpected(Failure{Error::source,{}});
        rows.push_back({{"state","completed"},{"start_ref",reference(pair->start_record,"pair_start")},
            {"request_ref",reference(pair->request_record,"request")},{"root_ref",reference(pair->root_record,"finalization")},
            {"analysis_ref",reference(pair->analysis_record,"analysis")},{"eof_ref",reference(pair->eof_record,"eof")},
            {"reap_ref",reference(pair->reap_record,"reap")},{"cleanup_ref",reference(pair->cleanup_record,"cleanup")}});
    }
    nlohmann::json facts{{"origin",observer?"actual_availability":"supplied_synthetic"},{"execution","completed"},{"plan_disposition","available"},
        {"pairs",std::move(rows)},{"primary_ref",nullptr},{"secondary_refs",nlohmann::json::array()}};
    core::detail::JsonCleanup facts_cleanup{facts};
    if(observer) {
        auto closed=observer->close(time,facts);
        if(!closed)return std::unexpected(Failure{Error::journal,{},{}});
        return *closed;
    }
    auto result=writer.append(event::SuppliedEvent{time,event::Kind::run_closed,results::process_subject::Producer::results,{},facts});
    if(!result)return std::unexpected(Failure{Error::journal,result.error()});
    return *result;
}
auto prepare_request_facts(const results::block_evidence::ExpectedPairContext& context,
    const runtime::AdmittedPairwiseProductionInputs& inputs,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    const runtime::wire_v1::detail::AuthoredCapacityOffer& offer,
    const results::execution_plan::Owner& plan,std::size_t ordinal,RetainedReferences refs,
    const results::event_profile::Limits& limits,bool actual_preparation) -> std::expected<std::string,Error> {
    namespace event=results::event_profile;namespace codec=runtime::prepared_codec_v1;
    using Json=nlohmann::json;
    if(!limits.composition||!context.available()||!context.complete_preparation()||!context.monitoring_preparation()||
       !context.control_assignment()||!plan.evidence()||!plan.evidence()->selection()||!plan.counts()||
       ordinal>=plan.pairs().size()||!refs.planning)return std::unexpected(Error::source);
    const auto& bounds=limits.composition->context;
    if(!bounds.bytes||context.workers().size()>bounds.workers||request.canonical_body().size()>bounds.bytes||
       offer.canonical_body.size()>bounds.bytes||context.usage().retained_bytes>bounds.retained)
        return std::unexpected(Error::capacity);
    const auto& selected=*plan.evidence()->selection();
    if(!selected.normalization()||!selected.normalization()->plan())return std::unexpected(Error::source);
    const results::request_association::detail::PlanView plan_view{*selected.normalization()->plan(),
        selected.workloads(),selected.bindings(),plan.pairs()[ordinal],
        inputs.pairwise_request().first().context().topology(),
        inputs.pairwise_request().first().context().stable_ids(),
        inputs.pairwise_request().first().context().fingerprints()};
    const results::request_association::detail::RequestView request_view{request.first(),request.second(),
        request.first_request_id().value(),request.second_request_id().value(),request.resolved_comparison_order_seed(),
        request.statistics_source(),inputs.statistics_policy(),inputs.monitoring_policy(),
        inputs.service_placement().assignments(),inputs.measurement_monitor_required()};
    results::request_association::Usage association_usage{};
    const auto services=results::request_association::detail::compare(plan_view,request_view,plan.counts()->root_seed,
        bounds.association,association_usage);
    if(!services)return std::unexpected(Error::source);
    const auto& measurement=inputs.pairwise_request().first().execution_inputs().measurement();
    auto repetition=measurement::RepetitionPolicy::create(measurement.repetition_minimum_blocks(),
        measurement.repetition_maximum_blocks(),measurement.minimum_valid_observations_per_variant(),measurement.stopping_enabled());
    core::detail::ExpectedErrorCleanup repetition_cleanup{repetition};
    if(!repetition)return std::unexpected(Error::source);
    const auto comparison=measurement::ComparisonOrderPolicy::create(
        static_cast<measurement::ComparisonOrderStrategy>(measurement.comparison_order()),request.resolved_comparison_order_seed());
    auto writer=codec::PreparedCanonicalWriter::prepare_progress(bounds.bytes);
    if(!writer||!writer->activate()||!codec::write_progress_policy_fields(*writer,inputs.statistics_policy(),
        inputs.monitoring_policy(),*repetition,comparison))return std::unexpected(Error::capacity);
    const auto policy=writer->progress_slice(0U);if(!policy)return std::unexpected(Error::capacity);
    try {
        event::detail::Work work{0U,limits.work};
        const std::string_view policy_bytes{reinterpret_cast<const char*>(policy->data()),policy->size()};
        auto header=event::detail::parse_tree(policy_bytes,limits,work);core::detail::JsonCleanup header_cleanup{header};
        const auto& provenance=context.provenance();
        header["provenance"]={{"experiment_id",std::to_string(provenance.experiment_id.value())},
            {"scenario_profile",provenance.scenario_profile},{"scenario_digest",provenance.scenario_digest},
            {"resolved_plan_digest",provenance.resolved_plan_digest},{"workload_id",provenance.workload_id},
            {"first_variant_id",provenance.first_variant_id},{"second_variant_id",provenance.second_variant_id}};
        const auto hex=[&](std::string_view value) {
            event::detail::require(value.size()<=bounds.text/2U,event::Error::capacity);
            constexpr std::string_view digits="0123456789abcdef";std::string encoded;encoded.reserve(value.size()*2U);
            for(char c:value){const auto byte=static_cast<unsigned char>(c);encoded.push_back(digits[byte>>4U]);encoded.push_back(digits[byte&15U]);}
            return encoded;
        };
        const auto service=[&](const runtime::ServiceThreadAssignment& row) -> Json {
            return {{"role",row.role==runtime::ServiceRole::control_monitor?"control_monitor":"measurement_monitor"},
                {"stable_pu",hex(row.stable_pu.value())},{"stable_core",hex(row.stable_core.value())},
                {"processing_unit",std::to_string(row.processing_unit.value())},{"core",std::to_string(row.core.value())},
                {"linux_cpu",std::to_string(row.linux_cpu.value())}};
        };
        const auto& monitoring=*context.monitoring_preparation();const auto& complete=*context.complete_preparation();
        Json facts{{"origin",actual_preparation?"actual_availability":"supplied_synthetic"},{"run",context.run().text()},
            {"provenance",{{"experiment_id",std::to_string(provenance.experiment_id.value())},
                {"scenario_profile",hex(provenance.scenario_profile)},{"scenario_digest",hex(provenance.scenario_digest)},
                {"resolved_plan_digest",hex(provenance.resolved_plan_digest)},{"workload_id",hex(provenance.workload_id)},
                {"first_variant_id",hex(provenance.first_variant_id)},{"second_variant_id",hex(provenance.second_variant_id)}}},
            {"request_body",request.canonical_body()},{"capacity_offer",offer.canonical_body},{"capacity_profile",runtime::wire_v1::capacity_profile},
            {"header",std::move(header)},{"control",service(*context.control_assignment())},
            {"monitoring",{{"assignment",monitoring.assignment?service(*monitoring.assignment):Json(nullptr)},
                {"physical",monitoring.physical.to_string()},{"effective",monitoring.effective.to_string()},{"scenario",monitoring.scenario.to_hex()}}},
            {"complete",{{"first_request",std::to_string(complete.first_request)},{"second_request",std::to_string(complete.second_request)},
                {"comparison_seed",std::to_string(complete.comparison_seed)}}}};
        core::detail::JsonCleanup facts_cleanup{facts};
        facts["monitoring"]["policy"]=core::detail::clone_json(facts["header"]["monitoring_policy"]);
        for(const auto* name:{"cpus","workers","first_assignments","second_assignments","first_memory","second_memory"})facts[name]=Json::array();
        const auto& parameters=offer.capacity.projected().capacity.prerequisite.parameters;
        facts["capacity_inputs"]={{"base_page_bytes",std::to_string(parameters.base_page_bytes.value)},
            {"kernel_maximum_cpu",std::to_string(parameters.maximum_cpu_index.value)},
            {"present_cpu_ids",Json::array()},{"domain_cpu_ids",Json::array()}};
        for(auto cpu:parameters.present_cpu_ids)facts["capacity_inputs"]["present_cpu_ids"].push_back(std::to_string(cpu.value));
        for(const auto& domain:parameters.domain_cpu_ids){Json row=Json::array();core::detail::JsonCleanup row_cleanup{row};
            for(auto cpu:domain)row.push_back(std::to_string(cpu.value));
            facts["capacity_inputs"]["domain_cpu_ids"].push_back(std::move(row));}
        for(auto cpu:context.cpus())facts["cpus"].push_back(std::to_string(cpu.value()));
        for(auto worker:context.workers())facts["workers"].push_back(std::to_string(worker.value()));
        for(unsigned side{};side<2U;++side) {
            for(const auto& assignment:context.assignments(side!=0U))facts[side?"second_assignments":"first_assignments"].push_back({
                {"stable_pu",hex(assignment.stable_pu_id().value())},{"stable_core",hex(assignment.stable_core_id().value())},
                {"linux_cpu",std::to_string(assignment.linux_cpu().value())}});
            std::size_t index{};const auto& wire=(side?request.second():request.first()).wire();
            for(const auto& memory:context.memory_requests(side!=0U)) {
                Json nodes=Json::array();core::detail::JsonCleanup nodes_cleanup{nodes};
                for(auto node:memory.resolved_nodes)nodes.push_back(std::to_string(node.value()));
                facts[side?"second_memory":"first_memory"].push_back({{"request",Json(wire.at("workers").at(index).at("region"))},
                    {"resolved_nodes",std::move(nodes)}});++index;
            }
        }
        auto canonical=core::canonicalize_json_jcs(facts,{bounds.bytes,bounds.depth,bounds.nodes});
        core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};if(!canonical)return std::unexpected(Error::capacity);
        auto wire=event::detail::parse_tree(request.canonical_body(),limits,work);core::detail::JsonCleanup wire_cleanup{wire};
        Json rows=Json::array();core::detail::JsonCleanup rows_cleanup{rows};
        for(std::size_t i{};i<inputs.service_placement().assignments().size();++i){const auto& row=(*services)[i];
            rows.push_back({{"role",row.role==runtime::ServiceRole::control_monitor?"control_monitor":"measurement_monitor"},
                {"reserved_cpu",std::to_string(row.reserved_cpu)},{"reserved_core",std::to_string(row.reserved_core)},
                {"pool",std::to_string(row.pool)},{"pool_cpu",std::to_string(row.pool_cpu)}});}
        Json accepted{{"origin","supplied_synthetic"},{"start_ref",nullptr},{"request",request.canonical_body()},
            {"context",*canonical},{"association",{{"state","replay_consistent"},
                {"planning_ref",{{"sequence",std::to_string(refs.planning)},{"ordinal","0"},{"kind","planning"}}},
                {"pair_ordinal",std::to_string(ordinal)},{"first_request",std::to_string(request.first_request_id().value())},
                {"second_request",std::to_string(request.second_request_id().value())},
                {"statistics_source",std::move(wire["payload"]["statistics_source"])},{"services",std::move(rows)}}}};
        core::detail::JsonCleanup accepted_cleanup{accepted};
        auto result=core::canonicalize_json_jcs(accepted,{limits.body_bytes,limits.depth,limits.nodes});
        core::detail::ExpectedErrorCleanup result_cleanup{result};
        if(!result)return std::unexpected(Error::capacity);
        return std::move(*result);
    }catch(event::Error){return std::unexpected(Error::source);}
}
auto execute_pair(results::filesystem_journal::Writer& journal,const results::journal::RunId& run,
    const scenario::ScenarioDigestedPlan& scenario,const results::execution_plan::Owner& plan,
    std::size_t ordinal,RetainedReferences refs,const runtime::AdmittedPairwiseProductionInputs& inputs,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,const runtime::RunnerContext& runner,
    runtime::detail::PairwiseMonitoringHostCapacityStartupAccess::LowerOperations operations,
    runtime::ControllerLifecycleConfig policy,runtime::detail::NativePairwiseDependencies dependencies,
    results::block_evidence::ContextLimits context_limits,results::producer_progress::TimestampSource timestamp,
    results::producer_progress::Limits limits,core::JcsLimits analysis) -> PairReport {
    namespace native=runtime::detail;PairReport report;
    if(ordinal>=plan.pairs().size()||plan.pairs()[ordinal].ordinal!=ordinal||dependencies.progress||
       dependencies.lifecycle.capture||dependencies.lifecycle.before_startup||!timestamp.read) {
        report.composition=Error::source;return report;
    }
    const auto records=journal.records();
    if(!refs.planning||!refs.preflight||refs.planning>records.size()||refs.preflight>records.size()) {
        report.composition=Error::source;return report;
    }
    const auto& planning=records[static_cast<std::size_t>(refs.planning-1)];
    const auto& preflight=records[static_cast<std::size_t>(refs.preflight-1)];
    if(planning.kind!=results::event_profile::Kind::planning||preflight.kind!=results::event_profile::Kind::preflight||
       !planning.event_facts||!preflight.event_facts||!planning.event_facts->contains("origin")||
       !preflight.event_facts->contains("origin")||planning.event_facts->at("origin")!=preflight.event_facts->at("origin")) {
        report.composition=Error::source;return report;
    }
    const bool actual_preparation=planning.event_facts->at("origin")=="actual_availability";
    if(!actual_preparation&&planning.event_facts->at("origin")!="supplied_synthetic") {
        report.composition=Error::source;return report;
    }
    auto session=native::stage_b_startup_session(run.text(),ordinal);
    if(!session){report.composition=Error::identity;return report;}
    // Declared first, therefore retired after the controller on every exception
    // path. No observer/consumer can outlive its stable forwarding context.
    std::optional<results::native_lifecycle::Adapter> adapter;
    struct Forward {std::optional<results::native_lifecycle::Adapter>* value;} forward{&adapter};
    dependencies.lifecycle={&forward,
        [](void* raw,const native::NativeLifecycleObservation& fact) noexcept {
            auto& value=*static_cast<Forward*>(raw)->value;if(!value)return false;
            const auto observer=value->observer();return observer.capture(observer.context,fact);
        },[](void* raw) noexcept {
            auto& value=*static_cast<Forward*>(raw)->value;if(!value)return false;
            const auto observer=value->observer();return observer.before_startup(observer.context);
        }};
    dependencies.policy=native::stage_b_policy::Selection::stage_b_v1;
    dependencies.runner_mode=native::NativePairwiseRunnerMode::prepared_production;
    auto controller=native::PairwiseV2NativeController::prepare(*session,request,runner,operations,policy,
        dependencies,{native::PairwiseV2NativeController::auxiliary_bytes()});
    if(!controller){report.preparation=controller.error();return report;}
    const auto* offer=controller->prepared_capacity_offer();const auto* progress=controller->progress_before_ack();
    if(!offer||!progress||timestamp.origin!=progress->utc){report.composition=Error::source;return report;}
    auto context=results::block_evidence::ExpectedPairContext::prepare(run,scenario,inputs,
        offer->capacity.projected().capacity,context_limits);
    if(!context){report.composition=Error::source;return report;}
    auto facts=prepare_request_facts(*context,inputs,request,*offer,plan,ordinal,refs,limits.parse,actual_preparation);
    if(!facts){report.composition=facts.error();return report;}
    const native::progress::Configuration configuration{progress->limits,progress->durability,progress->platform,progress->utc};
    auto prepared=results::native_lifecycle::Adapter::prepare(journal,
        {refs.planning,refs.preflight,refs.creation,ordinal,std::string(session->text()),std::move(*facts)},
        {plan.pairs()[ordinal].experiment_index,ordinal,{},{}},timestamp,limits,configuration,
        progress->platform==native::progress::Platform::native);
    if(!prepared){report.journal=prepared.error();return report;}
    adapter.emplace(std::move(*prepared));
    auto bound=controller->bind_progress_consumer(adapter->consumer());
    if(!bound){report.preparation=bound.error();return report;}
    // start failure still owns the same exact cleanup/reap through finish.
    const auto started=controller->start();static_cast<void>(started);
    report.execution.emplace(controller->finish());
    if(!adapter->drain_retired())report.journal=adapter->failure().value_or(results::native_lifecycle::Error::state);
    if(!report.journal&&report.execution->result) {
        auto reconciled=adapter->reconcile(*controller,*report.execution->result,analysis);
        if(!reconciled)report.journal=reconciled.error();
    }
    report.progress_failure=adapter->progress_failure();
    return report;
}
namespace {
auto execute_impl(const results::process_subject::Session& session,const results::execution_plan::Owner& plan,
    const scenario::ScenarioDigestedPlan& scenario,const runtime::RunnerContext& context,
    runtime::ControllerProcessProductionAuthority& authority,const platform_linux::detail::RunStartIdentity& identity,
    platform_linux::detail::ControllerSignalScope& signals,platform_linux::detail::CurrentProcessImage& image,
    std::string_view xdg,RunLimits limits,RunDependencies dependencies,
    results::filesystem_journal::Writer* prepared_writer,results::preparation_observation::Adapter* observer) -> RunReport {
    namespace native=runtime::detail;
    namespace journal=results::filesystem_journal;
    RunReport report;std::copy(identity.run_id().begin(),identity.run_id().end(),report.run_id.begin());
    std::optional<journal::Writer> owned_writer;
    auto* writer=prepared_writer;
    const auto fail=[&](Error kind,int code) {if(!report.primary){report.primary=Failure{kind,{},{}};report.exit_code=code;}};
    // All paths retire the same borrowed entry owners, after child/monitor owners.
    // Explicit close is consumed here; their destructors remain fallback only.
    const auto retire=[&]() {
        if(writer) {
            if(writer->receipt().state!=journal::State::closed) {
                const auto closed=writer->close();
                if(closed.primary){report.secondary_failure|=report.primary.has_value();
                    if(!report.primary){report.primary=Failure{Error::journal,*closed.primary,{}};report.exit_code=9;}}
            }
            report.journal=writer->receipt();
        }
        if(signals.observe()!=platform_linux::detail::ControllerSignal::none)fail(Error::execution,8);
        auto closed=image.close();
        if(!closed){report.secondary_failure|=report.primary.has_value();report.image_failure.emplace(std::move(closed.error()));fail(Error::execution,7);}
        auto restored=signals.close();
        if(!restored){report.secondary_failure|=report.primary.has_value();report.signal=restored.error().facts;report.signal_failure=restored.error();fail(Error::execution,7);}
        else report.signal=*restored;
    };
    try {
        const auto signal_valid=signals.validate();
        if(!signal_valid){report.signal_failure=signal_valid.error();fail(Error::execution,7);retire();return report;}
        if(!validate_identity(session,identity)||!image.active())throw Error::identity;
        if(prepared_writer&&(!observer||!observer->bound_to(session,*prepared_writer)))throw Error::identity;
        if(dependencies.controller.authority_source==native::PairwiseV2AuthoritySource::supplied_operations&&
           !dependencies.final_identity)throw Error::source;
        if(plan.pairs().size()>limits.pair_reports||scenario.canonical_scenario_json().size()>limits.scenario_bytes||
           limits.total_written_bytes==0U)throw Error::capacity;
        // Digest projection deliberately excludes output policy. Borrow the
        // actual retained normalized Scenario, as the request factory does.
        const auto& document=scenario.matrix_expanded().kernel_dispatched().memory_placement_resolved()
            .service_cpu_resolved().resource_resolved().default_expanded().value();
        auto output=prepare_output(document,xdg,identity);if(!output)throw output.error();
        auto run=results::journal::RunId::parse(identity.run_id());core::detail::ExpectedErrorCleanup run_cleanup{run};
        if(!run)throw Error::identity;
        if(signals.observe()!=platform_linux::detail::ControllerSignal::none){report.execution=ExecutionDisposition::interrupted;fail(Error::execution,8);retire();return report;}
        for(const auto* path:{&output->journal_root,&output->final_root}) {
            auto admitted=platform_linux::journal_file::File::preflight_root(*path,limits.journal.file);
            if(!admitted){report.primary=Failure{Error::output_policy,journal::Failure{journal::Error::io,{},admitted.error()},{}};
                report.exit_code=9;throw Error::output_policy;}
        }
        report.pairs.resize(plan.pairs().size());
        limits.journal.file.file_bytes=std::min(limits.journal.file.file_bytes,limits.total_written_bytes);
        if(!writer) {
            auto created=journal::Writer::create(output->journal_root,*run,limits.final.events,limits.journal,journal::Durability::process);
            if(!created){report.primary=Failure{Error::journal,created.error(),{}};report.exit_code=9;retire();return report;}
            owned_writer.emplace(std::move(*created));writer=&*owned_writer;
        }
        auto refs=prepared_writer?prepared_references(*writer,session,plan,limits.final.events):
            append_retained(*writer,session,plan,limits.final.events);
        if(!refs){report.primary=refs.error();report.exit_code=9;}
        constexpr std::array<std::string_view,2> arguments{"epycbench","__runner"};
        dependencies.controller.image=&image;dependencies.controller.arguments=arguments;
        dependencies.controller.cancellation=native::controller_signal_cancellation(signals);
        runtime::PairwiseProductionCompositionContext factory{context,authority.request_ids()};
        for(std::size_t ordinal=0;!report.primary&&ordinal<plan.pairs().size();++ordinal) {
            const auto& pair=plan.pairs()[ordinal];
            if(pair.ordinal!=ordinal)throw Error::source;
            if(signals.observe()!=platform_linux::detail::ControllerSignal::none){fail(Error::execution,8);break;}
            auto admission=[&]() -> std::expected<runtime::PairwiseProductionAdmission,core::Error> {
                if(pair.kind==results::execution_plan::PairKind::comparison) {
                    const auto& row=scenario.matrix_expanded().comparison_experiments().at(pair.child_experiment_index);
                    return runtime::compose_pairwise_production_inputs(scenario,
                        scenario::ComparisonVariantId{row.experiment_id,row.variants.at(pair.candidate_index).variant_id},factory);
                }
                const auto& row=scenario.matrix_expanded().matrix_experiments().at(pair.child_experiment_index);
                return runtime::compose_pairwise_production_inputs(scenario,row.cells.at(pair.candidate_index).id,factory);
            }();
            core::detail::ExpectedErrorCleanup admission_cleanup{admission};
            if(!admission){fail(Error::execution,7);break;}
            const auto& inputs=admission->inputs();const auto& admitted_pair=inputs.pairwise_request();
            auto request=native::PairwiseMonitoringHostCapacityStartupAccess::prepare_request(
                admitted_pair.first().request_id(),admitted_pair.second().request_id(),
                admission->first_request(),admission->second_request(),pair.order_seed,inputs.statistics_source());
            if(!request){fail(Error::execution,7);break;}
            report.execution=ExecutionDisposition::failed;
            auto& pair_report=report.pairs[ordinal].emplace(execute_pair(*writer,*run,scenario,plan,ordinal,*refs,inputs,*request,context,
                dependencies.capacity,native::controller_lifecycle_policy_v1(),dependencies.controller,
                limits.context,dependencies.timestamp,limits.progress,limits.analysis));
            if(pair_report.execution&&pair_report.execution->primary) {
                const auto kind=pair_report.execution->primary->kind;
                fail(kind==native::NativePairwiseFailureKind::results?Error::journal:Error::execution,
                    kind==native::NativePairwiseFailureKind::results?9:kind==native::NativePairwiseFailureKind::cancelled?8:7);break;
            }
            if(pair_report.journal||pair_report.composition){fail(Error::journal,9);break;}
            if(pair_report.preparation){fail(Error::execution,7);break;}
            if(!pair_report.execution){fail(Error::execution,7);break;}
            if(!pair_report.execution->succeeded()){fail(Error::execution,7);break;}
            // Lifecycle success does not imply operational measurement success.
            // Preserve earlier Native/Results primaries; normal terminals,
            // including completed-invalid measurements, remain eligible.
            if(std::holds_alternative<runtime::OperationalRunTerminalEvidence>(
                pair_report.execution->result->terminal())) {fail(Error::execution,7);break;}
        }
        if(signals.observe()!=platform_linux::detail::ControllerSignal::none)fail(Error::execution,8);
        if(report.exit_code==8)report.execution=ExecutionDisposition::interrupted;
        else if(!report.primary)report.execution=ExecutionDisposition::completed;
        if(observer&&!report.primary) {
            std::array<char,30> observed{};
            if(!dependencies.timestamp.read||!dependencies.timestamp.read(dependencies.timestamp.context,observed))throw Error::finalization;
            auto argument=results::journal::Timestamp::parse({observed.data(),observed.size()});
            core::detail::ExpectedErrorCleanup argument_cleanup{argument};
            if(!argument)throw Error::identity;
            auto closed=close_completed(*writer,*argument,observer);
            if(!closed){report.primary=closed.error();report.exit_code=9;}
        }
        const auto final_identity=dependencies.final_identity?
            platform_linux::detail::RunStartIdentityAccess::acquire(dependencies.final_identity->clock,dependencies.final_identity->entropy):
            platform_linux::detail::RunStartIdentity::acquire();
        if(!final_identity||final_identity->run_id()==identity.run_id())throw Error::finalization;
        auto final_id=results::journal::RunId::parse(final_identity->run_id());core::detail::ExpectedErrorCleanup final_id_cleanup{final_id};
        auto final_time=results::journal::Timestamp::parse(final_identity->utc_start());core::detail::ExpectedErrorCleanup final_time_cleanup{final_time};
        if(!final_id||!final_time)throw Error::finalization;
        if(signals.observe()!=platform_linux::detail::ControllerSignal::none)fail(Error::execution,8);
        if(!observer&&!report.primary){auto closed=close_completed(*writer,*final_time);if(!closed){report.primary=closed.error();report.exit_code=9;}}
        auto publication_limits=limits.journal.file;
        const auto written=writer->receipt().written_bytes;
        if(written>=limits.total_written_bytes)throw Error::capacity;
        // Recovery must still admit the original journal; publication's remaining
        // cumulative write allowance is passed separately below.
        publication_limits.file_bytes=std::min(publication_limits.file_bytes,limits.total_written_bytes-written);
        report.final.emplace(finalize(*writer,*output,*run,{*final_id,*final_time},limits.journal,limits.final,publication_limits));
        if(report.final->primary){if(report.primary)report.secondary_failure=true;else{report.primary=report.final->primary;report.exit_code=9;}}
    }catch(Error kind){fail(kind,9);}
    catch(...){fail(Error::capacity,9);}
    retire();return report;
}
}
auto execute(const results::process_subject::Session& session,const results::execution_plan::Owner& plan,
    const scenario::ScenarioDigestedPlan& scenario,const runtime::RunnerContext& context,
    runtime::ControllerProcessProductionAuthority& authority,const platform_linux::detail::RunStartIdentity& identity,
    platform_linux::detail::ControllerSignalScope& signals,platform_linux::detail::CurrentProcessImage& image,
    std::string_view xdg,RunLimits limits,RunDependencies dependencies) -> RunReport {
    return execute_impl(session,plan,scenario,context,authority,identity,signals,image,xdg,
        std::move(limits),std::move(dependencies),nullptr,nullptr);
}
auto execute(const results::process_subject::Session& session,const results::execution_plan::Owner& plan,
    const scenario::ScenarioDigestedPlan& scenario,const runtime::RunnerContext& context,
    runtime::ControllerProcessProductionAuthority& authority,const platform_linux::detail::RunStartIdentity& identity,
    platform_linux::detail::ControllerSignalScope& signals,platform_linux::detail::CurrentProcessImage& image,
    std::string_view xdg,RunLimits limits,RunDependencies dependencies,results::filesystem_journal::Writer& writer,
    results::preparation_observation::Adapter& observer) -> RunReport {
    return execute_impl(session,plan,scenario,context,authority,identity,signals,image,xdg,
        std::move(limits),std::move(dependencies),&writer,&observer);
}

auto write_summary(const RunReport& report,std::ostream& output,std::ostream& errors) noexcept -> int {
    constexpr std::array<std::string_view,10> codes{"ok","internal_failure","usage_failure","stream_io_failure",
        "topology_discovery_failure","topology_invalid","scenario_invalid","runtime_failure","interrupted","results_failure"};
    const auto status=report.exit_code>=0&&report.exit_code<static_cast<int>(codes.size())?report.exit_code:1;
    std::array<char,512> bytes{};std::size_t size{};
    const auto append=[&](std::string_view text) {if(text.size()>bytes.size()-size)return false;
        std::copy(text.begin(),text.end(),bytes.begin()+static_cast<std::ptrdiff_t>(size));size+=text.size();return true;};
    const auto number=[&](auto value) {
        const auto encoded=std::to_chars(bytes.data()+size,bytes.data()+bytes.size(),value);
        if(encoded.ec!=std::errc{})return false;
        size=static_cast<std::size_t>(encoded.ptr-bytes.data());return true;
    };
    const auto failure=[&](const runtime::detail::NativePairwiseFailure& value) {
        if(!append("[")||!number(static_cast<unsigned>(value.kind))||!append(",")||
           !number(static_cast<unsigned>(value.stage))||!append(",")||
           !number(static_cast<unsigned>(value.phase))||!append(","))return false;
        if(value.native) {
            if(!number(static_cast<unsigned>(value.native->kind))||!append(",")||
               !number(value.native->error_number))return false;
        }else if(!append("null,null"))return false;
        if(!append(","))return false;
        return (value.startup?number(static_cast<unsigned>(*value.startup)):append("null"))&&append("]");
    };
    const auto render=[&](int code,unsigned detail) {
        size=0;
        const auto publication=report.final&&report.final->publication?&report.final->publication->receipt:nullptr;
        auto destination=std::string_view{"not_attempted"};
        if(publication) {
            switch(publication->residual) {
            case results::final_document::Residual::none:break;
            case results::final_document::Residual::temporary:destination="temporary";break;
            case results::final_document::Residual::final:destination="final";break;
            case results::final_document::Residual::uncertain:destination="uncertain";break;
            }
        }
        bool secondary=report.secondary_failure;
        const PairReport* selected{};
        const runtime::detail::NativePairwiseFailure* primary{};
        for(const auto& pair:report.pairs)if(pair) {
            secondary|=pair->execution&&pair->execution->cleanup_count!=0U;
            secondary|=pair->journal.has_value()&&pair->execution&&pair->execution->primary&&
                pair->execution->primary->kind!=runtime::detail::NativePairwiseFailureKind::results;
            if(!primary) {
                if(pair->preparation){selected=&*pair;primary=&*pair->preparation;}
                else if(pair->execution&&pair->execution->primary){selected=&*pair;primary=&*pair->execution->primary;}
            }
        }
        if(!selected)for(const auto& pair:report.pairs)if(pair&&(pair->journal||pair->progress_failure||
            (pair->execution&&pair->execution->cleanup_count))) {
            selected=&*pair;break;
        }
        if(selected&&selected->execution&&selected->execution->cleanup_count>selected->execution->cleanup.size())return false;
        const auto execution=report.execution==ExecutionDisposition::completed?"completed":
            report.execution==ExecutionDisposition::interrupted?"interrupted":
            report.execution==ExecutionDisposition::failed?"failed":"not_started";
        const bool created_without_writer=report.primary&&report.primary->journal&&
            report.primary->journal->io&&report.primary->journal->io->created;
        const auto journal=report.journal?(report.journal->persistence_uncertain?"uncertain":"retained"):
            created_without_writer?"retained":"not_created";
        if(!append("{\"schema\":\"epycbench.run-summary/v3\",\"code\":\"")||!append(codes[static_cast<std::size_t>(code)])||
           !append("\",\"run_id\":"))return false;
        if(report.run_id[0]) {if(!append("\"")||!append({report.run_id.data(),report.run_id.size()})||!append("\""))return false;}
        else if(!append("null"))return false;
        if(!(append(",\"execution\":\"")&&append(execution)&&append("\",\"publication\":\"")&&append(destination)&&
            append("\",\"journal\":\"")&&append(journal)&&append("\",\"secondary_failure\":")&&
            append(secondary?"true":"false")&&append(",\"native\":")))return false;
        if(!(primary?failure(*primary):append("null"))||!append(",\"cleanup\":["))return false;
        if(selected&&selected->execution)for(std::size_t index=0;index<selected->execution->cleanup_count;++index) {
            if((index&&!append(","))||!failure(selected->execution->cleanup[index]))return false;
        }
        if(!append("]"))return false;
        if(detail&&selected&&(selected->journal||selected->progress_failure)) {
            const auto* progress=selected->progress_failure?&*selected->progress_failure:nullptr;
            const auto* block=progress&&progress->block?&*progress->block:nullptr;
            const auto optional_number=[&](const auto& value) {
                return value?number(static_cast<unsigned>(*value)):append("null");
            };
            if(!append(",\"results\":[")||!optional_number(selected->journal)||!append(",")||
               !(progress?number(static_cast<unsigned>(progress->kind)):append("null"))||!append(",")||
               !(progress?number(progress->ordinal):append("null"))||!append(",")||
               !(progress?optional_number(progress->error):append("null"))||!append(",")||
               !(progress?optional_number(progress->semantic):append("null")))return false;
            for(const auto value:{block?std::optional{static_cast<unsigned>(block->code)}:std::nullopt,
                                 block?std::optional{static_cast<unsigned>(block->phase)}:std::nullopt,
                                 block?std::optional{static_cast<unsigned>(block->detail_code)}:std::nullopt})
                if(!append(",")||!optional_number(value))return false;
            if(!append(",")||!(block?number(block->limit):append("null"))||!append(",")||
               !(block?number(block->observed):append("null"))||!append(","))return false;
            const auto* diagnostic=detail==2U&&block?&block->diagnostic:nullptr;
            if(diagnostic) {
                if(diagnostic->identity_size>diagnostic->identity.size()||
                   diagnostic->ordinal_count>diagnostic->ordinals.size())return false;
                const std::string_view identity{diagnostic->identity.data(),diagnostic->identity_size};
                // Lower reconstruction supplies closed schema identifiers only.
                // Do not render candidate text or filesystem/error-message paths.
                for(const auto c:identity)
                    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                         c=='_'||c=='.'||c=='['||c==']'))return false;
                if(!append("\"")||!append(identity)||!append("\""))return false;
            } else if(!append("null"))return false;
            if(!append(",["))return false;
            if(diagnostic)for(std::size_t index{};index<diagnostic->ordinal_count;++index)
                if((index&&!append(","))||!number(diagnostic->ordinals[index]))return false;
            if(!append("]]"))return false;
        }
        return append("}\n");
    };
    const auto bounded_render=[&](int code) {
        // Diagnostic detail is best available within the unchanged bound. Every
        // attempt retains the complete original primary and ordered cleanup.
        return render(code,2U)||render(code,1U)||render(code,0U);
    };
    const auto write=[&](std::ostream& stream) noexcept {
        try {stream.write(bytes.data(),static_cast<std::streamsize>(size));stream.flush();return static_cast<bool>(stream);}
        catch(...){return false;}
    };
    if(!bounded_render(status))return status?status:1;
    if(write(status?errors:output))return status;
    if(status)return status;
    // The first sink may already have emitted bytes; there is no rollback.
    if(bounded_render(3))static_cast<void>(write(errors));
    return 3;
}

}
