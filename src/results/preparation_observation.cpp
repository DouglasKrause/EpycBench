#include "results/preparation_observation.hpp"
#include "results/incremental_event_pair.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <utility>

namespace epycbench::results::preparation_observation {
namespace ep=event_profile;
namespace ps=process_subject;
namespace fs=filesystem_journal;
namespace {
using Json=nlohmann::json;
auto reference(std::uint64_t sequence,std::string_view kind="failure") -> Json {
    return {{"sequence",std::to_string(sequence)},{"ordinal","0"},{"kind",kind}};
}
auto state_error() -> core::Error {
    return {.category=core::ErrorCategory::results,.code=core::StableErrorCode{"results.preparation.state"},
        .message="Preparation observer is unavailable",.operation="preparation_observation",
        .os_error={},.context=Json::object(),.severity=core::ErrorSeverity::error,
        .continuation=core::ContinuationPolicy::abort_command};
}
auto primary(const ep::Record& record) -> bool {
    return record.kind==ep::Kind::failure && record.scope==ep::Scope{} &&
        (record.event_facts?record.event_facts->at("role")=="primary":record.role==ep::Role::primary);
}
}
Adapter::Adapter(ps::Session& session,fs::Writer& writer,ep::Limits limits,bool native,bool utc,TimestampSource clock) noexcept
    :session_{&session},writer_{&writer},limits_{std::move(limits)},native_platform_{native},acquired_utc_{utc},clock_{clock} {}
Adapter::Adapter(Adapter&& other) noexcept
    :session_{std::exchange(other.session_,nullptr)},writer_{std::exchange(other.writer_,nullptr)},
     limits_{std::move(other.limits_)},ordinal_{other.ordinal_},native_platform_{other.native_platform_},
     acquired_utc_{other.acquired_utc_},clock_{other.clock_},failure_{other.failure_},pending_checks_{std::exchange(other.pending_checks_,nullptr)},
     pending_digest_{other.pending_digest_},pending_assessment_{other.pending_assessment_},checks_called_{other.checks_called_} {}
auto Adapter::observe(const journal::Timestamp& supplied) -> std::optional<journal::Timestamp> {
    try {
        if(!acquired_utc_)return supplied;
        std::array<char,30> bytes{};
        if(clock_.read&&clock_.read(clock_.context,bytes)) {
            auto parsed=journal::Timestamp::parse({bytes.data(),bytes.size()});core::detail::ExpectedErrorCleanup cleanup{parsed};
            if(parsed)return std::move(*parsed);
        }
    } catch(const std::bad_alloc&) {failure_=Error::capacity;return {};}
      catch(...) {}
    failure_=Error::clock;return {};
}
auto Adapter::valid(bool before_start) const -> bool {
    if(!session_||!writer_||failure_||session_->domain()!=ps::Domain::supplied_observation||session_->entries().empty())return false;
    const auto* transaction=fs::detail::Access::transaction(*writer_);
    if(!transaction||!ep::is_progress(transaction->version())||writer_->receipt().state!=fs::State::open||writer_->records().empty())return false;
    const auto* created=std::get_if<run_creation_serialization::Owner>(&writer_->records().front().artifact);
    if(!created||!created->provenance()||created->provenance()->run!=session_->run_id()||created->provenance()->domain!=ps::Domain::supplied_observation)return false;
    for(const auto& record:writer_->records())
        if(record.kind==ep::Kind::run_closed||(before_start&&record.kind==ep::Kind::pair_started))return false;
    return true;
}
auto Adapter::prepare(ps::Session& session,fs::Writer& writer,ep::Limits limits,bool native,bool utc,TimestampSource clock)
    -> std::expected<Adapter,Error> {
    const auto* transaction=fs::detail::Access::transaction(writer);
    if(!transaction||!ep::is_progress(transaction->version())||transaction->version()!=limits.version||
       !limits.composition||session.domain()!=ps::Domain::supplied_observation||session.entries().empty()||
       !session.entries().front().run_creation||(utc&&!clock.read)||(!utc&&clock.read))return std::unexpected{Error::state};
    const auto& entry=session.entries().front();
    auto created=session.serialize_run_creation(entry.sequence,limits.child.creation);
    if(!created)return std::unexpected{Error::source};
    if(writer.records().empty()) {
        auto stamp=journal::Timestamp::parse(entry.timestamp);core::detail::ExpectedErrorCleanup cleanup{stamp};
        if(!stamp)return std::unexpected{Error::source};
        const auto receipt=writer.append(ep::Input{*stamp,&*created,{},{},{}});
        if(!receipt)return std::unexpected{Error::journal};
    } else {
        const auto* existing=std::get_if<run_creation_serialization::Owner>(&writer.records().front().artifact);
        if(!existing||existing->canonical()!=created->canonical())return std::unexpected{Error::identity};
    }
    Adapter result{session,writer,std::move(limits),native,utc,clock};
    if(!result.valid())return std::unexpected{Error::state};
    for(const auto& record:writer.records())
        if(record.scope==ep::Scope{}&&record.availability)
            result.ordinal_=std::max(result.ordinal_,std::max(record.availability->transition,record.availability->delivery));
    return result;
}
auto Adapter::append(const journal::Timestamp& stamp,ep::Kind kind,ps::Producer reporter,const Json& facts)
    -> std::expected<fs::Receipt,Error> {
    if(!valid(kind!=ep::Kind::run_closed))return std::unexpected{Error::state};
    if(ordinal_==UINT64_MAX){failure_=Error::capacity;return std::unexpected{*failure_};}
    ++ordinal_;
    const ep::ProducerAdmission admission{{ep::AvailabilitySource::controller_observation,0,ordinal_,ordinal_,{},native_platform_,acquired_utc_}};
    auto result=writer_->append(ep::SuppliedEvent{stamp,kind,reporter,{},facts,&admission});
    if(!result){failure_=Error::journal;return std::unexpected{*failure_};}
    return *result;
}
auto Adapter::creation_failures(const journal::Timestamp& stamp) -> std::expected<void,Error> {
    if(!valid())return std::unexpected{Error::state};
    const auto failures=std::get<run_creation_serialization::Owner>(writer_->records().front().artifact).failures();
    std::size_t delivered{};
    for(const auto& record:writer_->records())
        if(record.kind==ep::Kind::failure&&record.event_facts&&record.event_facts->at("source")=="private_error")++delivered;
    if(delivered!=0U)return std::unexpected{Error::state};
    for(const auto& error:failures) {
        const auto observed=observe(stamp);if(!observed)return std::unexpected{*failure_};
        const auto* provenance=error.provenance();
        if(!provenance||!provenance->association)return std::unexpected{Error::source};
        const auto& association=*provenance->association;
        const ep::Record* prior{};
        if(association.secondary_sequence) {
            const auto ordinal=association.secondary_creation_ordinal.value_or(UINT64_MAX);
            if(ordinal>=failures.size())return std::unexpected{Error::source};
            const auto& original=failures[static_cast<std::size_t>(ordinal)];
            for(const auto& record:writer_->records())
                if(record.kind==ep::Kind::failure&&record.event_facts&&record.event_facts->at("source")=="private_error"&&
                   record.event_facts->at("artifact")==original.canonical())prior=&record;
            if(!prior)return std::unexpected{Error::identity};
        }
        Json facts{{"origin","actual_availability"},{"source","private_error"},
            {"role",prior?"secondary":"primary"},{"primary_ref",prior?reference(prior->sequence):Json(nullptr)},
            {"artifact",error.canonical()}};
        core::detail::JsonCleanup cleanup{facts};
        auto receipt=append(*observed,ep::Kind::failure,ps::Producer::controller,facts);
        if(!receipt)return std::unexpected{receipt.error()};
    }
    return {};
}
void Adapter::failed(const journal::Timestamp& stamp,std::string_view target,const core::Error& error) {
    if(!valid()) {if(!failure_)failure_=Error::state;return;}
    try {
        if(!ep::detail::preparation_failure_matches(error,target)){failure_=Error::source;return;}
        auto bounds=limits_.composition->error;
        // The complete Error value has no artifact provenance or target table.
        bounds.targets=0U;bounds.provenance_bytes=0U;
        auto owned=private_error::retain(error,{bounds.context_nodes,bounds.context_depth,bounds.string_bytes,bounds.binary_bytes,bounds.retained_bytes});
        core::detail::ExpectedErrorCleanup cleanup{owned};
        if(!owned){failure_=Error::serialization;return;}
        error_serialization::Usage usage{};auto encoded=error_serialization::detail::encode_value(*owned,bounds,usage);
        if(!encoded){failure_=Error::serialization;return;}
        const auto records=writer_->records();const auto prior=std::ranges::find_if(records,primary);
        Json facts{{"origin","actual_availability"},{"source","controller_preparation"},
            {"role",prior==records.end()?"primary":"secondary"},
            {"primary_ref",prior==records.end()?Json(nullptr):reference(prior->sequence)},
            {"artifact",{{"profile","epycbench.controller-preparation-failure/v1"},{"target",target},{"error",std::move(*encoded)}}}};
        core::detail::JsonCleanup facts_cleanup{facts};
        static_cast<void>(append(stamp,ep::Kind::failure,ps::Producer::controller,facts));
    } catch(const std::bad_alloc&) {failure_=Error::capacity;}
}
auto Adapter::report(const journal::Timestamp& stamp,std::string_view bytes,kernel_report::Limits limits)
    -> std::expected<kernel_report::Owner,core::Error> {
    if(!valid())return std::unexpected{state_error()};
    auto result=kernel_report::retain(bytes,limits);
    if(!result)if(const auto observed=observe(stamp))failed(*observed,"kernel_evidence",result.error());
    return result;
}
auto Adapter::approval(const journal::Timestamp& stamp,std::string_view bytes,kernel_report::Owner&& gcc,
    kernel_report::Owner&& clang,std::array<std::string_view,2> human,kernel_approval::Limits limits)
    -> std::expected<kernel_approval::Owner,core::Error> {
    if(!valid())return std::unexpected{state_error()};
    auto result=kernel_approval::retain(bytes,std::move(gcc),std::move(clang),human,limits);
    if(!result)if(const auto observed=observe(stamp))failed(*observed,"kernel_evidence",result.error());
    return result;
}
auto Adapter::kernel_evidence(const journal::Timestamp& stamp,void* context,KernelCall call)
    -> std::expected<void,Error> {
    if(!valid()||!call)return std::unexpected{Error::state};
    // The producer executes outside post-result capture containment. Its caller
    // owns the returned facts and every partial input throughout publication.
    CallFailures failures;
    try {failures=call(context,*session_);}
    catch(Error capture) {failure_=capture;return std::unexpected{capture};}
    if(failures.cleanup.size()>4||(!failures.primary&&!failures.cleanup.empty())||
       (failures.primary&&!ep::detail::preparation_failure_matches(*failures.primary,"kernel_evidence",false)))
        {failure_=Error::source;return std::unexpected{*failure_};}
    for(const auto& error:failures.cleanup)
        if(!ep::detail::preparation_failure_matches(error,"kernel_evidence",true))
            {failure_=Error::source;return std::unexpected{*failure_};}
    if(!failures.primary)return {};
    const auto observed=observe(stamp);if(!observed)return std::unexpected{*failure_};
    failed(*observed,"kernel_evidence",*failures.primary);
    for(const auto& error:failures.cleanup) {
        if(failure_)break;
        failed(*observed,"kernel_evidence",error);
    }
    if(failure_)return std::unexpected{*failure_};
    return {};
}
auto Adapter::controller_checks(const journal::Timestamp& stamp,void* context,CheckCall call)
    -> std::expected<void,Error> {
    if(!valid()||!call||checks_called_)return std::unexpected{Error::state};
    checks_called_=true;
    CheckCallResult result;
    try {result=call(context,*session_);}catch(Error error){failure_=error;return std::unexpected{error};}
    try {
        if(!result.snapshot||!ep::detail::controller_check_snapshot_valid(*result.snapshot,limits_.composition->error))
            {failure_=Error::source;return std::unexpected{*failure_};}
        const auto& errors=result.failures;
        if((!errors.primary&&!errors.cleanup.empty())||errors.cleanup.size()>=limits_.records||
           writer_->records().size()>limits_.records-errors.cleanup.size()-(errors.primary?1U:0U))
            {failure_=Error::capacity;return std::unexpected{*failure_};}
        if(!ep::detail::controller_check_failure_batch(*result.snapshot,errors.primary,errors.cleanup))
            {failure_=Error::source;return std::unexpected{*failure_};}
        if(!errors.primary) {
            if(!result.assessment||result.assessment->run_id()!=session_->run_id()||!result.assessment->digest())
                {failure_=Error::identity;return std::unexpected{*failure_};}
            pending_checks_=result.snapshot;pending_digest_=core::sha256(result.snapshot->dump());
            pending_assessment_=*result.assessment->digest();return {};
        }
        const auto observed=observe(stamp);if(!observed)return std::unexpected{*failure_};
        failed(*observed,"preflight_admission",*errors.primary);
        for(const auto& error:errors.cleanup){if(failure_)break;failed(*observed,"preflight_admission",error);}
        if(failure_)return std::unexpected{*failure_};
        return {};
    } catch(const std::bad_alloc&) {failure_=Error::capacity;return std::unexpected{*failure_};}
      catch(...) {failure_=Error::source;return std::unexpected{*failure_};}
}
void Adapter::typed_failure(const journal::Timestamp& stamp,std::string_view code,unsigned value) {
    try {
        const auto observed=observe(stamp);if(!observed)return;
        auto error=state_error();core::detail::JsonCleanup cleanup{error.context};
        error.code=core::StableErrorCode{std::string{code}};error.message="Preparation producer failed";
        error.context={{"producer_error",value}};failed(*observed,"execution_plan",error);
    } catch(const std::bad_alloc&) {failure_=Error::capacity;}
}
auto Adapter::selection(const journal::Timestamp& stamp,normalization_serialization::Owner&& child,
    implementation_selection::Limits limits) -> std::expected<implementation_selection::Owner,implementation_selection::Error> {
    if(!valid()||!child.provenance()||child.provenance()->run!=session_->run_id())
        return std::unexpected{implementation_selection::Error::origin};
    auto original=session_->serialize_normalization(child.provenance()->sequence,limits_.child.normalization);
    if(!original||original->canonical()!=child.canonical())return std::unexpected{implementation_selection::Error::origin};
    auto result=implementation_selection::retain(std::move(child),limits);
    if(!result)typed_failure(stamp,"results.preparation.implementation_selection",static_cast<unsigned>(result.error()));
    return result;
}
auto Adapter::association(const journal::Timestamp& stamp,implementation_selection::Owner&& selected,
    base_build_serialization::Owner&& base,build_object_association::ApprovalInputs&& approvals,
    build_object_association::Limits limits) -> std::expected<build_object_association::Owner,build_object_association::Error> {
    if(!valid()||!selected.normalization()||!selected.normalization()->provenance()||!base.provenance()||
       selected.normalization()->provenance()->run!=session_->run_id()||base.provenance()->run!=session_->run_id())
        return std::unexpected{build_object_association::Error::origin};
    auto original=session_->serialize_normalization(selected.normalization()->provenance()->sequence,limits_.child.normalization);
    auto build=session_->serialize_base_build(base.provenance()->sequence,limits_.child.creation.base_build);
    if(!original||original->canonical()!=selected.normalization()->canonical()||!build||build->canonical()!=base.canonical())
        return std::unexpected{build_object_association::Error::origin};
    auto result=build_object_association::retain(std::move(selected),std::move(base),std::move(approvals),limits);
    if(!result)typed_failure(stamp,"results.preparation.build_object_association",static_cast<unsigned>(result.error()));
    return result;
}
auto Adapter::execution_plan(const journal::Timestamp& stamp,build_object_association::Owner&& association,
    results::execution_plan::Limits limits) -> std::expected<results::execution_plan::Owner,results::execution_plan::Error> {
    if(!valid()||!association.selection()||!association.selection()->normalization()||
       !association.selection()->normalization()->provenance()||
       association.selection()->normalization()->provenance()->run!=session_->run_id())
        return std::unexpected{results::execution_plan::Error::origin};
    const auto& normalization=*association.selection()->normalization();
    auto original=session_->serialize_normalization(normalization.provenance()->sequence,limits_.child.normalization);
    if(!original||original->canonical()!=normalization.canonical()||!association.base()||!association.base()->provenance())
        return std::unexpected{results::execution_plan::Error::origin};
    auto base=session_->serialize_base_build(association.base()->provenance()->sequence,limits_.child.creation.base_build);
    if(!base||base->canonical()!=association.base()->canonical())return std::unexpected{results::execution_plan::Error::origin};
    auto result=results::execution_plan::produce(std::move(association),limits);
    if(!result) {
        try {
            const auto observed=observe(stamp);if(!observed)return result;
            auto error=state_error();core::detail::JsonCleanup cleanup{error.context};
            error.code=core::StableErrorCode{"results.preparation.execution_plan"};error.message="Execution plan production failed";
            error.context={{"producer_error",static_cast<unsigned>(result.error())}};
            failed(*observed,"execution_plan",error);
        } catch(const std::bad_alloc&) {failure_=Error::capacity;}
    }
    return result;
}
auto Adapter::preflight(std::uint64_t sequence,const journal::Timestamp& stamp,results::preflight::Input& input,const nlohmann::json* snapshot)
    -> std::expected<void,core::Error> {
    if(!valid())return std::unexpected{state_error()};
    try {
        if(snapshot!=pending_checks_||(checks_called_&&!pending_checks_)||
           (pending_checks_&&(!input.monitoring||!input.monitoring->digest()||
             *input.monitoring->digest()!=*pending_assessment_||core::sha256(snapshot->dump())!=*pending_digest_)))
            return std::unexpected{state_error()};
    } catch(const std::bad_alloc&) {failure_=Error::capacity;return std::unexpected{state_error()};}
    pending_checks_=nullptr;pending_assessment_.reset();pending_digest_.reset();
    auto result=session_->append_preflight(sequence,stamp,input);
    const auto observed=observe(stamp);if(!observed)return result;
    if(!result){failed(*observed,"preflight_admission",result.error());return result;}
    const auto& entry=session_->entries().back();
    try {
        ep::detail::Work work{0,limits_.work};
        auto body=ep::detail::encode_preflight(*entry.preflight->facts(),limits_.composition->preflight,work);
        auto facts=ep::detail::parse_tree(body,limits_,work);core::detail::JsonCleanup cleanup{facts};
        if(snapshot) {
            if(!entry.preflight->facts()->monitoring||!ep::detail::controller_check_snapshot_matches(
                *snapshot,*entry.preflight->facts()->monitoring,facts.at("temperatures")))
                {failure_=Error::identity;return result;}
            facts["controller_checks"]=*snapshot;
        }
        facts["origin"]="actual_availability";facts["source_entry"]={{"sequence",std::to_string(sequence)},{"time",entry.timestamp}};
        static_cast<void>(append(*observed,ep::Kind::preflight,ps::Producer::controller,facts));
    } catch(ep::Error) {failure_=Error::serialization;}
      catch(const std::bad_alloc&) {failure_=Error::capacity;}
    return result;
}
auto Adapter::planning(const journal::Timestamp& stamp,const results::execution_plan::Owner& plan)
    -> std::expected<fs::Receipt,Error> {
    if(!valid()||!plan.evidence()||!plan.evidence()->selection()||!plan.evidence()->selection()->normalization())
        return std::unexpected{Error::state};
    const auto& selected=*plan.evidence()->selection()->normalization();
    if(!selected.provenance()||selected.provenance()->run!=session_->run_id())return std::unexpected{Error::identity};
    auto original=session_->serialize_normalization(selected.provenance()->sequence,limits_.child.normalization);
    if(!original||original->canonical()!=selected.canonical())return std::unexpected{Error::identity};
    const ep::Record* normal{};const ep::Record* topology{};
    for(const auto& record:writer_->records()) {
        if(record.kind==ep::Kind::normalization)normal=&record;
        if(record.kind==ep::Kind::topology)topology=&record;
    }
    if(!normal||!topology||std::get<normalization_serialization::Owner>(normal->artifact).canonical()!=selected.canonical())
        return std::unexpected{Error::identity};
    const auto observed=observe(stamp);if(!observed)return std::unexpected{*failure_};
    const auto next=writer_->receipt().semantic.records;
    if(next==UINT64_MAX)return std::unexpected{Error::capacity};
    Json facts{{"origin","actual_availability"},{"artifact",plan.canonical()},
        {"normalization_ref",reference(normal->sequence,"scenario_resolved")},
        {"topology_ref",reference(topology->sequence,"topology_retained")},
        {"build_ref",reference(next+1U,"base_build")}};
    core::detail::JsonCleanup cleanup{facts};
    return append(*observed,ep::Kind::planning,ps::Producer::controller,facts);
}
auto Adapter::close(const journal::Timestamp& stamp,const Json& facts) -> std::expected<fs::Receipt,Error> {
    if(!valid(false))return std::unexpected{Error::state};
    const auto observed=observe(stamp);if(!observed)return std::unexpected{*failure_};
    return append(*observed,ep::Kind::run_closed,ps::Producer::results,facts);
}

}
