#include "results/native_lifecycle.hpp"
#include "results/incremental_event_pair.hpp"
#include "core/json_ownership.hpp"

namespace epycbench::results::native_lifecycle {
namespace ep = event_profile;
namespace rt = runtime::detail;
namespace fs = filesystem_journal;
namespace {
using Json = nlohmann::json;
using View = std::string_view;
void require(bool condition, Error error) { if (!condition) throw error; }
auto reference(std::uint64_t sequence, View kind) -> Json {
    return {{"sequence",std::to_string(sequence)},{"ordinal","0"},{"kind",kind}};
}
template<class E, std::size_t N>
auto token(E value, const std::array<View,N>& names) -> View {
    const auto index=static_cast<std::size_t>(value);
    require(index<N,Error::schema); return names[index];
}
constexpr std::array<View,18> failure_names{"invalid_state","invalid_input","capacity","preparation","clock_overflow","clock_regression",
    "native","protocol","backend","encoding","transport","reconstruction","results","cancelled","timeout","process_nonzero","process_signaled","unreapable"};
constexpr std::array<View,18> stage_names{"preparation","channels","spawn","parent_close","startup_control","startup_events",
    "cancellation","backend","encoding","evidence","eof","reconstruction","results","process","endpoint_close","terminate","kill","reap"};
constexpr std::array<View,5> phase_names{"startup","execution","cancellation","completion","termination"};
constexpr std::array<View,8> native_names{"invalid_state","invalid_argument","malformed_result","native_failure","interrupted","would_block","wrong_child","invalid_status"};
constexpr std::array<View,4> endpoint_names{"parent_runner_control","parent_runner_event","controller_control","controller_event"};
}
struct Adapter::Data {
    struct Pending {
        rt::NativeLifecycleObservation observation;
        std::array<char,30> utc{};
        std::uint64_t transition{};
    };
    fs::Writer* writer{};
    Predecessors predecessors;
    ep::Scope scope;
    producer_progress::TimestampSource time;
    producer_progress::Limits limits;
    rt::progress::Configuration configuration;
    bool native_platform{}, retired{}, accepted{}, completion{};
    std::array<Pending,16> pending{};
    std::size_t count{};
    std::uint64_t transition{}, start{}, primary{}, latest_failure{};
    std::optional<Error> error;
    std::optional<producer_progress::Adapter> progress;
    std::optional<producer_progress::ProgressFailure> progress_failure;
    Json cleanup{{"primary_ref",nullptr},{"secondary_refs",Json::array()},{"operations",Json::array()},
        {"close_returns",{"absent","absent","absent","absent","absent","absent"}},
        {"terminate_return","absent"},{"kill_return","absent"},{"reap_return","absent"},{"fatal_unreapable",false}};
    ~Data() { core::detail::retire_json(cleanup); }
    auto capture(const rt::NativeLifecycleObservation& observation) noexcept -> bool {
        if(error)return false;
        if(observation.kind==rt::NativeLifecycleKind::failure&&observation.unselected_failure)return true;
        if(count==pending.size()||transition==UINT64_MAX){error=Error::capacity;return false;}
        auto& slot=pending[count];
        if(!time.read(time.context,slot.utc)){error=Error::timestamp;return false;}
        slot.observation=observation;slot.transition=++transition;++count;
        if(observation.kind==rt::NativeLifecycleKind::retired)retired=true;
        return true;
    }
    auto append(const Pending& event, ep::Kind kind, const Json& facts,
        process_subject::Producer reporter=process_subject::Producer::controller) -> std::uint64_t {
        auto stamp=journal::Timestamp::parse({event.utc.data(),event.utc.size()});
        core::detail::ExpectedErrorCleanup stamp_cleanup{stamp};require(stamp.has_value(),Error::timestamp);
        const auto admitted=Adapter::admission({ep::AvailabilitySource::controller_observation,0,
            event.transition,event.transition,{},native_platform,time.origin==rt::progress::Utc::acquired});
        auto result=writer->append(ep::SuppliedEvent{*stamp,kind,reporter,scope,facts,&admitted});
        require(result.has_value(),Error::journal);return result->semantic.records;
    }
    auto base() const -> Json {
        return {{"origin","actual_availability"},{"start_ref",reference(start,"pair_start")}};
    }
    void operation(View name, bool success, std::optional<std::size_t> endpoint={}) {
        const auto failure=success?Json(nullptr):reference(latest_failure,"failure");
        require(success||latest_failure,Error::state);
        cleanup["operations"].push_back({{"kind",name},{"return",success?"success":"failed"},{"failure_ref",failure}});
        if(endpoint)cleanup["close_returns"][*endpoint]=success?"success":"failed";
        else if(name=="exact_reap")cleanup["reap_return"]=success?"success":"failed";
        else if(name=="graceful_termination")cleanup["terminate_return"]=success?"success":"failed";
        else if(name=="forced_escalation")cleanup["kill_return"]=success?"success":"failed";
        if(!success&&latest_failure!=primary)
            cleanup["secondary_refs"].push_back({{"source",name},{"failure_ref",failure}});
    }
    void deliver(const Pending& event) {
        const auto& value=event.observation;
        using Kind=rt::NativeLifecycleKind;
        if(value.kind==Kind::failure) {
            require(value.failure.has_value(),Error::state);const auto& detail=*value.failure;
            Json native=nullptr;core::detail::JsonCleanup native_cleanup{native};
            if(detail.native)native={{"kind",token(detail.native->kind,native_names)},{"error_number",std::to_string(detail.native->error_number)}};
            Json facts{{"origin","actual_availability"},{"source","native_lifecycle"},
                {"role",value.secondary_failure?"secondary":"primary"},
                {"primary_ref",value.secondary_failure?reference(primary,"failure"):Json(nullptr)},
                {"artifact",{{"kind",token(detail.kind,failure_names)},{"stage",token(detail.stage,stage_names)},
                    {"phase",token(detail.phase,phase_names)},{"native",std::move(native)}}}};
            core::detail::JsonCleanup cleanup_facts{facts};
            require(!value.secondary_failure||primary,Error::state);
            latest_failure=append(event,ep::Kind::failure,facts);
            if(!value.secondary_failure){require(!primary,Error::state);primary=latest_failure;cleanup["primary_ref"]=reference(primary,"failure");}
            return;
        }
        if(value.kind==Kind::spawned||value.kind==Kind::spawn_failed) {
            require(!start,Error::state);
            Json facts{{"origin","actual_availability"},{"planning_ref",reference(predecessors.planning,"planning")},
                {"preflight_ref",reference(predecessors.preflight,"preflight")},{"controller_ref",reference(predecessors.controller,"creation")},
                {"runner_ref",nullptr},{"session_id",predecessors.session_id},{"pair_ordinal",std::to_string(predecessors.pair_ordinal)},
                {"spawn",value.kind==Kind::spawned?Json{{"state","created"},{"child_token",std::to_string(predecessors.pair_ordinal+1U)}}:
                    Json{{"state","failed"},{"failure_ref",reference(latest_failure,"failure")}}}};
            core::detail::JsonCleanup cleanup_facts{facts};start=append(event,ep::Kind::pair_started,facts);return;
        }
        if(value.kind==Kind::request_sent)return; // No invented request-send event kind.
        if(value.kind==Kind::retired&&!start)return; // Preparation may fail before spawn.
        require(start!=0,Error::state);
        if(value.kind==Kind::accepted) {
            require(!accepted,Error::state);
            ep::detail::Work work{0,limits.work,0,limits.scratch_bytes};
            auto facts=ep::detail::parse_tree(predecessors.request_facts,limits.parse,work);
            core::detail::JsonCleanup cleanup_facts{facts};
            require(facts.is_object(),Error::schema);facts["origin"]="actual_availability";facts["start_ref"]=reference(start,"pair_start");
            static_cast<void>(append(event,ep::Kind::request_accepted,facts,process_subject::Producer::runner));accepted=true;return;
        }
        auto facts=base();core::detail::JsonCleanup cleanup_facts{facts};
        if(value.kind==Kind::endpoint_closed) {
            require(value.endpoint.has_value(),Error::state);const auto index=static_cast<std::size_t>(*value.endpoint);
            operation(token(*value.endpoint,endpoint_names),value.succeeded,index);
            facts["transition"]="cleanup";core::detail::set_json(facts,"cleanup",cleanup);
            static_cast<void>(append(event,ep::Kind::lifecycle_progress,facts));
        }else if(value.kind==Kind::completion) {
            facts["transition"]="completion_received";static_cast<void>(append(event,ep::Kind::lifecycle_progress,facts));completion=true;
        }else if(value.kind==Kind::eof) {
            facts["disposition"]=completion?"clean":"premature";static_cast<void>(append(event,ep::Kind::event_eof,facts));
        }else if(value.kind==Kind::reaped) {
            require(value.exit.has_value(),Error::state);operation("exact_reap",true);
            const auto* exited=std::get_if<platform_linux::ProcessExited>(&*value.exit);
            const auto status=exited?static_cast<std::uint64_t>(exited->code):static_cast<std::uint64_t>(std::get<platform_linux::ProcessSignaled>(*value.exit).signal);
            facts["exit"]={{"child_token",std::to_string(predecessors.pair_ordinal+1U)},{"signaled",!exited},{"value",std::to_string(status)}};
            core::detail::set_json(facts,"cleanup",cleanup);static_cast<void>(append(event,ep::Kind::child_reaped,facts));
        }else if(value.kind==Kind::termination_sent||value.kind==Kind::kill_sent) {
            operation(value.kind==Kind::termination_sent?"graceful_termination":"forced_escalation",value.succeeded);
            facts["transition"]="cleanup";core::detail::set_json(facts,"cleanup",cleanup);static_cast<void>(append(event,ep::Kind::lifecycle_progress,facts));
        }else if(value.kind==Kind::cancelled) {
            require(value.cancellation.has_value(),Error::state);
            constexpr std::array<View,3> reasons{"user_interrupt","controller_shutdown","controller_timeout"};
            facts["transition"]="cancellation";facts["reason"]=token(*value.cancellation,reasons);
            facts["sent"]=value.cancel_sent;facts["runner_observed"]=value.cancelled_received;
            facts["response_received"]=value.cancelled_received;
            static_cast<void>(append(event,ep::Kind::lifecycle_progress,facts));
        }else if(value.kind!=Kind::retired)throw Error::state;
    }
    auto drain() noexcept -> bool {
        if(error)return false;
        try {
            for(std::size_t i{};i<count;++i)deliver(pending[i]);
            count=0;return true;
        }catch(Error failure){error=failure;}
        catch(...){error=Error::capacity;}
        return false; // Persisted prefix remains; consumed callbacks never replay.
    }
};
Adapter::Adapter(std::unique_ptr<Data> data) noexcept:data_{std::move(data)}{}
Adapter::Adapter(Adapter&&) noexcept=default;
Adapter::~Adapter()=default;
auto Adapter::prepare(fs::Writer& writer,Predecessors predecessors,ep::Scope scope,
    producer_progress::TimestampSource time,producer_progress::Limits limits,
    rt::progress::Configuration configuration,bool native_platform) -> std::expected<Adapter,Error> {
    try {
        const auto* transaction=fs::detail::Access::transaction(writer);
        require(transaction&&ep::is_progress(transaction->version())&&transaction->available()&&
            writer.receipt().state==fs::State::open&&!writer.records().empty(),Error::state);
        const auto* creation=std::get_if<run_creation_serialization::Owner>(&writer.records().front().artifact);
        require(creation&&creation->provenance()&&creation->provenance()->domain==process_subject::Domain::supplied_observation,Error::state);
        require(time.read&&scope.experiment&&scope.pair&&!scope.block&&!scope.observation&&
            *scope.pair==predecessors.pair_ordinal&&predecessors.pair_ordinal!=UINT64_MAX,Error::state);
        const auto bytes=ep::detail::add(sizeof(Data),ep::detail::add(predecessors.session_id.size(),predecessors.request_facts.size()));
        require(bytes<=limits.scratch_bytes&&limits.work&&predecessors.request_facts.size()<=limits.parse.body_bytes,Error::capacity);
        require(predecessors.planning&&predecessors.preflight&&predecessors.controller==1U&&
            runtime::wire_v1::detail::StartupSessionId::parse(predecessors.session_id).has_value(),Error::schema);
        auto data=std::make_unique<Data>();data->writer=&writer;data->predecessors=std::move(predecessors);
        data->scope=scope;data->time=time;data->limits=limits;data->configuration=configuration;data->native_platform=native_platform;
        return Adapter{std::move(data)};
    }catch(Error failure){return std::unexpected{failure};}catch(...){return std::unexpected{Error::capacity};}
}
auto Adapter::observer() noexcept -> rt::NativeLifecycleObserver {
    return {data_.get(),[](void* raw,const rt::NativeLifecycleObservation& value) noexcept {
        return raw&&static_cast<Data*>(raw)->capture(value);},[](void* raw) noexcept {
        return raw&&static_cast<Data*>(raw)->drain();}};
}
auto Adapter::consumer() noexcept -> rt::NativeProgressConsumer {
    return {data_?data_->configuration:rt::progress::Configuration{},data_.get(),
        [](void* raw,const rt::ValidatedProgressView& value) noexcept -> rt::ProgressConsumeResult {
            if(!raw)return {};
            auto& data=*static_cast<Data*>(raw);
            const auto failed=[&]() noexcept -> rt::ProgressConsumeResult {
                if(!data.progress_failure&&(!data.progress||!data.progress->failure_snapshot())) {
                    const auto receipt=data.writer->receipt();
                    const auto* transaction=fs::detail::Access::transaction(*data.writer);
                    data.progress_failure=producer_progress::ProgressFailure{value.transition.kind,value.transition.ordinal,{},
                        receipt.failure?receipt.failure->semantic:std::nullopt,
                        transaction?transaction->pair_failure():std::nullopt};
                }
                return {};
            };
            if(!data.drain()||!data.accepted)return failed();
            try {
                if(!data.progress){auto prepared=producer_progress::Adapter::prepare(*data.writer,value.offer,data.scope,data.time,data.limits);
                    if(!prepared){if(!data.progress_failure)data.progress_failure=producer_progress::ProgressFailure{
                        value.transition.kind,value.transition.ordinal,prepared.error(),{}, {}};
                        data.error=Error::progress;return failed();}data.progress.emplace(std::move(*prepared));}
                const auto callback=data.progress->consumer();const auto result=callback.consume_progress(callback.context,value);
                if(result.state==rt::ProgressConsumeResult::State::failed)return failed();
                return result;
            }catch(...){data.error=Error::capacity;return failed();}
        }};
}
auto Adapter::drain_retired() noexcept -> bool {return data_&&data_->retired&&data_->drain();}
auto Adapter::failure() const noexcept -> std::optional<Error> {return data_?data_->error:std::optional{Error::state};}
auto Adapter::progress_failure() const noexcept -> std::optional<producer_progress::ProgressFailure> {
    if(!data_)return std::nullopt;
    if(data_->progress_failure)return data_->progress_failure;
    return data_->progress?data_->progress->failure_snapshot():std::nullopt;
}
auto Adapter::reconcile(rt::PairwiseV2NativeController& controller,const PairwiseRunResult& result,core::JcsLimits limits)
    -> std::expected<fs::Receipt,Error> {
    if(!drain_retired()||!data_->progress)return std::unexpected{Error::state};
    auto reconciled=data_->progress->reconcile(controller,result,limits);
    if(!reconciled)return std::unexpected{Error::progress};
    return *reconciled;
}
}
