#include "results/producer_progress.hpp"
#include "results/incremental_event_pair.hpp"
#include "results/pairwise_analysis_serialization.hpp"
#include "core/json_ownership.hpp"
#include <charconv>
#include <type_traits>

namespace epycbench::results::producer_progress {
namespace ep = event_profile;
namespace rt = runtime::detail;
namespace fs = filesystem_journal;
namespace {
using Json = nlohmann::json;
void require(bool condition, Error error) { if (!condition) throw error; }
auto text(const Json& value) -> const std::string& {
    require(value.is_string(),Error::schema); return value.get_ref<const std::string&>();
}
auto number(const Json& value) -> std::uint64_t {
    const auto& bytes = text(value);
    require(!bytes.empty() && (bytes.size()==1 || bytes.front()!='0'),Error::schema);
    std::uint64_t result{};
    const auto parsed = std::from_chars(bytes.data(),bytes.data()+bytes.size(),result);
    require(parsed.ec==std::errc{} && parsed.ptr==bytes.data()+bytes.size(),Error::schema);
    return result;
}
auto reference(std::uint64_t sequence, std::string_view kind) -> Json {
    return {{"sequence",std::to_string(sequence)},{"ordinal","0"},{"kind",kind}};
}
void fields(const Json& value, std::initializer_list<std::string_view> names) {
    require(value.is_object() && value.size()==names.size(),Error::schema);
    for (auto name:names) require(value.contains(name),Error::schema);
}
void copy(Json& destination, std::string_view key, const Json& value) {
    core::detail::set_json(destination,key,value);
}
void push(Json& destination, const Json& value) {
    auto owned=core::detail::clone_json(value); core::detail::JsonCleanup cleanup{owned};
    destination.push_back(std::move(owned));
}
auto slice(const Json& value, std::size_t size) -> std::pair<std::size_t,std::size_t> {
    fields(value,{"offset","size"}); const auto offset=number(value.at("offset")), count=number(value.at("size"));
    require(offset<=size && count<=size-offset,Error::schema);
    return {static_cast<std::size_t>(offset),static_cast<std::size_t>(count)};
}
auto text_slice(const Json& graph, const Json& value) -> std::string {
    const auto& bytes=text(graph.at("strings")); require(bytes.size()%2U==0,Error::schema);
    const auto [offset,count]=slice(value,bytes.size()/2U);
    return bytes.substr(offset*2U,count*2U);
}
auto array_slice(const Json& values, const Json& selection) -> Json {
    require(values.is_array(),Error::schema); const auto [offset,count]=slice(selection,values.size());
    Json result=Json::array(); core::detail::JsonCleanup cleanup{result};
    for (std::size_t i=0;i<count;++i) push(result,values[offset+i]);
    return Json(std::move(result));
}
void enum_token(Json& value, std::initializer_list<std::string_view> tokens) {
    const auto ordinal=number(value); require(ordinal<tokens.size(),Error::schema);
    value=*(tokens.begin()+static_cast<std::ptrdiff_t>(ordinal));
}
auto hex(std::string_view value) -> std::string {
    std::string result; require(value.size()<=result.max_size()/2U,Error::capacity);
    result.reserve(value.size()*2U); constexpr std::string_view digits="0123456789abcdef";
    for (char raw:value) {const auto byte=static_cast<unsigned char>(raw);result.push_back(digits[byte>>4U]);result.push_back(digits[byte&15U]);}
    return result;
}
}
void detail::reserve_reconciliation(std::string_view canonical, ep::Version version,
    const Limits& limits, std::size_t events, core::JcsLimits analysis_limits,
    ep::detail::Work& work) {
    const auto output = ep::detail::add(ep::detail::multiply(events,1024U),
        ep::detail::multiply(analysis_limits.output_bytes,2U));
    if (version == ep::Version::compact_v3) {
        // take_validated_bytes borrows the controller's one-shot backing. SAX
        // validation overlaps that backing, but creates no canonical-root DOM.
        // Reduce the child allowance before its lexer can allocate, rather than
        // checking the combined peak only after the scan has completed.
        work.reserve_peak(canonical.size());
        work.debit(0U);
        ep::detail::Work scan{0,work.limit-work.used,0,work.peak_limit-canonical.size()};
        (void)ep::detail::shape(canonical,limits.parse,scan);
        work.debit(scan.used);
        work.reserve_peak(ep::detail::add(canonical.size(),scan.peak));
        // Facts are retired before analysis serialization. Retain the existing
        // conservative refs/two-output allowance, including the held input.
        work.reserve_peak(ep::detail::add(canonical.size(),output));
        work.debit(canonical.size()); // The separate SHA-256 pass below.
    } else {
        const auto shape=ep::detail::shape(canonical,limits.parse,work);
        work.reserve_peak(ep::detail::add(ep::detail::multiply(ep::detail::workspace(shape),3U),output));
    }
}
struct Adapter::Data {
    fs::Writer* writer;
    progress::Offer offer;
    ep::Scope scope;
    TimestampSource time;
    Limits limits;
    std::shared_ptr<const ep::DecodedContext> context;
    std::shared_ptr<const Json> request_facts;
    std::uint64_t start{}, request{}, received{}, persisted{};
    std::optional<Error> failure;
    std::optional<ep::Error> semantic_failure;
    std::optional<ProgressFailure> failure_snapshot;
    auto prior(ep::Kind kind, std::optional<std::uint64_t> block = {}) const -> std::uint64_t {
        const auto records = writer->records();
        for (auto at=records.rbegin();at!=records.rend();++at)
            if (at->kind==kind && at->scope.experiment==scope.experiment && at->scope.pair==scope.pair &&
                (!block || at->scope.block==block)) return at->sequence;
        throw Error::state;
    }
    auto append(const journal::Timestamp& stamp, ep::Kind kind, const Json& facts,
        ep::Scope selected, const ep::ProducerAdmission& admission,
        process_subject::Producer reporter = process_subject::Producer::runner) -> fs::Receipt {
        const auto result = writer->append(ep::SuppliedEvent{stamp,kind,reporter,
            selected,facts,&admission});
        if (!result) semantic_failure=result.error().semantic;
        require(result.has_value(),Error::journal);
        require(result->complete.records==result->semantic.records && result->complete.digest,Error::journal);
        return *result;
    }
    auto preparation(const journal::Timestamp& stamp, Json& input, const ep::ProducerAdmission& admission,
        ep::detail::Work& work) -> fs::Receipt {
        fields(input,{"control","graph"}); auto& graph=input.at("graph");
        auto expected=ep::detail::parse_tree(context->canonical,limits.parse,work);
        core::detail::JsonCleanup expected_cleanup{expected};
        auto& policies=graph.at("policies"); auto& header=policies.at("header");
        fields(header,{"comparison_order","monitoring_policy","repetition_policy","statistics_policy"});
        for (const auto key:{"comparison_order","monitoring_policy","repetition_policy","statistics_policy"})
            require(header.at(key)==expected.at("header").at(key),Error::identity);
        copy(policies,"header",expected.at("header")); policies["request_body"]=context->request_body;
        graph["measurement_request_body"]=context->request_body;
        auto monitor=core::detail::clone_json(graph.at("monitoring")); core::detail::JsonCleanup monitor_cleanup{monitor};
        fields(monitor,{"capacity","channels","effective_digest","physical_digest","profile","provenance","service"});
        const auto* monitoring=context->lease.monitoring_preparation(); require(monitoring,Error::state);
        require(text(monitor.at("physical_digest"))==monitoring->physical.digest().to_hex() &&
            text(monitor.at("effective_digest"))==monitoring->effective.digest().to_hex(),Error::identity);
        monitor.erase("physical_digest"); monitor.erase("effective_digest");
        monitor["physical"]=monitoring->physical.to_string(); monitor["effective"]=monitoring->effective.to_string();
        const auto provenance=text(monitor.at("provenance"));
        monitor["provenance"]={{"kind","scenario"},{"authority_digest",provenance}};
        for (auto& channel:monitor.at("channels")) {
            enum_token(channel.at("support"),{"not_requested","supported","unsupported"});
            enum_token(channel.at("effective"),{"active","unsupported_warning","disabled"});
        }
        copy(graph,"monitoring",monitor);
        monitor["domain"]=offer.platform==progress::Platform::native?"source":"synthetic"; copy(monitor,"policy",expected.at("header").at("monitoring_policy"));
        monitor["workers"]=Json::array(); monitor["frequency"]=Json::array();
        Json workers=Json::array(); core::detail::JsonCleanup workers_cleanup{workers};
        for (const auto& row:graph.at("workers")) {
            auto affinity=array_slice(graph.at("cpus"),row.at("affinity")); core::detail::JsonCleanup affinity_cleanup{affinity};
            Json worker{{"worker_index",row.at("index")},{"worker_id",row.at("worker")},
                {"stable_pu",text_slice(graph,row.at("pu"))},{"stable_core",text_slice(graph,row.at("core"))},
                {"requested_cpu",row.at("requested_cpu")},{"tid",row.at("tid")},{"observed_cpu",row.at("observed_cpu")}};
            core::detail::JsonCleanup worker_cleanup{worker}; copy(worker,"observed_affinity",affinity); push(workers,worker);
            monitor["workers"].push_back({{"worker",row.at("worker")},{"cpu",row.at("observed_cpu")},{"tid",row.at("tid")}});
        }
        const auto frequency_count=number(graph.at("frequency_count"));
        require(graph.at("frequency").is_array() && frequency_count<=graph.at("frequency").size(),Error::schema);
        for (std::size_t i=0;i<frequency_count;++i) {
            const auto& row=graph.at("frequency")[i];
            auto cpus=array_slice(graph.at("cpus"),row.at("cpus")); core::detail::JsonCleanup cpus_cleanup{cpus};
            Json frequency{{"kind",row.at("kind")},{"driver",text_slice(graph,row.at("driver"))},
                {"governor",text_slice(graph,row.at("governor"))},{"minimum_hz",row.at("minimum")},{"maximum_hz",row.at("maximum")}};
            core::detail::JsonCleanup frequency_cleanup{frequency}; copy(frequency,"cpus",cpus); push(monitor["frequency"],frequency);
        }
        Json first=Json::array(),second=Json::array();
        core::detail::JsonCleanup first_cleanup{first},second_cleanup{second};
        for (auto& row:graph.at("memory")) {
            enum_token(row.at("page_mode"),{"base"}); enum_token(row.at("placement_policy"),{"bind"});
            enum_token(row.at("binding").at("policy"),{"bind"}); enum_token(row.at("binding").at("range"),{"full_mapping"});
            enum_token(row.at("initialization").at("range"),{"logical_requested_region"});
            enum_token(row.at("protection").at("protection_kind"),{"read_only"}); enum_token(row.at("protection").at("range"),{"full_mapping"});
            enum_token(row.at("query"),{"skipped","observed"}); enum_token(row.at("qualification"),{"skipped","qualified","not_qualified"});
            Json region{{"worker_id",row.at("worker")},{"requested_bytes",row.at("bytes")},{"page_size",row.at("page_size")},
                {"placement_policy",row.at("placement_policy")},{"page_mode",row.at("page_mode")},
                {"placement_threshold",{{"numerator",row.at("numerator")},{"denominator",row.at("denominator")}}},
                {"prefault",row.at("prefault")},{"verify_before",row.at("verify_before")},{"verify_after",row.at("verify_after")},
                {"numa_targets",Json::array()}};
            core::detail::JsonCleanup region_cleanup{region};
            const auto [offset,count]=slice(row.at("targets"),graph.at("targets").size());
            for (std::size_t i=0;i<count;++i) {const auto& target=graph.at("targets")[offset+i];
                region["numa_targets"].push_back({{"stable_node",text_slice(graph,target.at("stable"))},
                    {"linux_node",target.at("linux_node")},{"topology_node",target.at("topology_node")}});}
            Json memory{{"state",row.at("query")},{"observation",nullptr},{"evaluation",nullptr},{"qualification",nullptr}};
            core::detail::JsonCleanup memory_cleanup{memory}; copy(memory,"request",region);
            for (const auto key:{"binding","initialization","protection"}) copy(memory,key,row.at(key));
            if (!row.at("observation").is_null()) {
                const auto& observation=row.at("observation");
                auto counts=array_slice(graph.at("pages"),observation.at("counts")); core::detail::JsonCleanup counts_cleanup{counts};
                for (auto& page:counts) {page["linux_node"]=page.at("node"); page["page_count"]=page.at("count");page.erase("node");page.erase("count");}
                memory["observation"]={{"total_pages",observation.at("total")},{"resident_pages",observation.at("resident")},
                    {"unresolved_pages",observation.at("unresolved")}};
                copy(memory["observation"],"resident_page_counts",counts);
            }
            if (!row.at("evaluation").is_null()) {const auto& evaluation=row.at("evaluation");
                memory["evaluation"]={{"requested_node_pages",evaluation.at("requested")},{"unexpected_node_pages",evaluation.at("unexpected")},
                    {"unresolved_pages",evaluation.at("unresolved")},{"observed_ratio",{{"numerator",evaluation.at("numerator")},{"denominator",evaluation.at("denominator")}}}};}
            if (text(row.at("query"))=="observed") copy(memory,"qualification",row.at("qualification"));
            else require(row.at("observation").is_null() && row.at("evaluation").is_null() && !row.at("verify_before").get<bool>(),Error::schema);
            require(row.at("second").is_boolean(),Error::schema); push(row.at("second").get<bool>()?second:first,memory);
        }
        Json facts{{"origin","actual_availability"},{"request_ref",reference(request,"request")}};
        core::detail::JsonCleanup cleanup{facts};
        std::array<std::uint64_t,5> sequences{}; fs::Receipt receipt;
        const std::array<const Json*,5> rows{&input.at("control"),&workers,&first,&second,&monitor};
        constexpr std::array phases{"control_readiness","worker_readiness","first_memory","second_memory","monitoring_preparation"};
        constexpr std::array keys{"control","workers","memory","memory","monitoring"};
        for (std::size_t i=0;i<rows.size();++i) {
            facts["phase"]=phases[i]; copy(facts,keys[i],*rows[i]);
            receipt=append(stamp,ep::Kind::placement,facts,scope,admission); sequences[i]=receipt.complete.records;
            core::detail::retire_json(facts.at(keys[i])); facts.erase(keys[i]);
        }
        facts.erase("phase"); facts["preparation_refs"]=Json::array();
        constexpr std::array references{"control","workers","first_memory","second_memory","monitoring_preparation"};
        for (std::size_t i=0;i<sequences.size();++i) facts["preparation_refs"].push_back(reference(sequences[i],references[i]));
        copy(facts,"preparation_graph",graph);
        return append(stamp,ep::Kind::preparation,facts,scope,admission);
    }
    auto component(const journal::Timestamp& stamp, const progress::Transition& transition,
        Json& value, const ep::ProducerAdmission& admission, ep::detail::Work& work) -> fs::Receipt {
        Json facts{{"origin","actual_availability"}};
        core::detail::JsonCleanup cleanup{facts};
        auto selected = scope;
        ep::Kind kind{};
        switch (transition.kind) {
        case progress::Kind::preparation: return preparation(stamp,value,admission,work);
        case progress::Kind::calibration:
            kind=ep::Kind::calibration;
            facts["preparation_ref"]=reference(prior(ep::Kind::preparation),"preparation"); break;
        case progress::Kind::warmup:
            kind=ep::Kind::warmup;
            facts["calibration_ref"]=reference(prior(ep::Kind::calibration),"calibration"); break;
        case progress::Kind::block_opened:
            kind=ep::Kind::block_opened; selected.block=number(value.at("block_id"));
            facts["request_ref"]=reference(request,"request");
            facts["calibration_ref"]=reference(prior(ep::Kind::calibration),"calibration");
            facts["warmup_ref"]=reference(prior(ep::Kind::warmup),"warmup"); break;
        case progress::Kind::observation: case progress::Kind::failed_observation: {
            const bool failed=transition.kind==progress::Kind::failed_observation;
            kind=failed?ep::Kind::failed_observation:ep::Kind::observation;
            const auto& slot=value.at("slot");
            selected.block=number(slot.at("block_id")); selected.observation=number(value.at("observation_id"));
            facts["opened_ref"]=reference(prior(ep::Kind::block_opened,selected.block),"block");
            facts["monitoring_refs"]=Json::array();
            const auto next=writer->receipt().complete.records;
            require(next!=UINT64_MAX,Error::capacity);
            if (!failed) facts["monitoring_refs"].push_back(reference(next+1U,"monitoring"));
            const auto& provenance=context->lease.provenance();
            Json timing{{"boundary",failed?"failed_attempt_start":"completed_enclosing_start"},
                {"disposition","not_recorded"},{"calibration_ref",reference(prior(ep::Kind::calibration),"calibration")},
                {"identity",{{"run",context->lease.run().text()},{"domain",context->lease.decoded_domain()==process_subject::Domain::supplied_observation?"supplied_observation":"synthetic_execution"},
                    {"experiment",std::to_string(provenance.experiment_id.value())},
                    {"scenario_profile",hex(provenance.scenario_profile)},{"scenario_digest",hex(provenance.scenario_digest)},
                    {"resolved_plan_digest",hex(provenance.resolved_plan_digest)},{"workload",hex(provenance.workload_id)},
                    {"first_variant",hex(provenance.first_variant_id)},{"second_variant",hex(provenance.second_variant_id)},
                    {"family",context->lease.kernel(false).index()==0U?"bandwidth":"latency"},
                    {"block",slot.at("block_id")},{"slot",slot.at("slot_index")},{"variant",slot.at("variant_slot")},
                    {"observation",value.at("observation_id")}}}};
            core::detail::JsonCleanup timing_cleanup{timing};
            if (!failed) {timing["clock"]="monotonic_raw";copy(timing,"raw_start",value.at("measurement").at("enclosing_interval").at("start"));}
            copy(facts,"timing",timing); break;
        }
        case progress::Kind::block_closed: {
            fields(value,{"block_id","completion","failed_observation","observations"});
            kind=ep::Kind::block_closed; selected.block=number(value.at("block_id"));
            const auto opened=prior(ep::Kind::block_opened,selected.block);
            facts["opened_ref"]=reference(opened,"block");copy(facts,"completion",value.at("completion"));
            facts["observations"]=Json::array(); require(value.at("observations").is_array(),Error::schema);
            std::size_t observed{}; bool saw_failure{};
            for (const auto& record:writer->records()) {
                if(record.sequence<=opened || record.scope.pair!=scope.pair || record.scope.experiment!=scope.experiment ||
                    record.scope.block!=selected.block) continue;
                if(record.kind==ep::Kind::observation) {
                    require(!saw_failure && observed<value.at("observations").size() && record.scope.observation &&
                        *record.scope.observation==number(value.at("observations")[observed]),Error::identity);
                    ++observed;facts["observations"].push_back(reference(record.sequence,"observation"));
                } else if(record.kind==ep::Kind::failed_observation) {
                    require(!saw_failure && !value.at("failed_observation").is_null() && record.scope.observation &&
                        *record.scope.observation==number(value.at("failed_observation")),Error::identity);
                    saw_failure=true;facts["observations"].push_back(reference(record.sequence,"failed_observation"));
                }
            }
            require(observed==value.at("observations").size() && saw_failure==!value.at("failed_observation").is_null(),Error::identity);
            return append(stamp,kind,facts,selected,admission);
        }
        case progress::Kind::stopping:
            kind=ep::Kind::stopping;facts["request_ref"]=reference(request,"request");facts["closed_blocks"]=Json::array();
            for(const auto& record:writer->records())
                if(record.kind==ep::Kind::block_closed && record.scope.pair==scope.pair && record.scope.experiment==scope.experiment)
                    facts["closed_blocks"].push_back(reference(record.sequence,"block"));
            break;
        case progress::Kind::finalization:
            kind=ep::Kind::placement; facts["phase"]="memory_finalization";
            facts["request_ref"]=reference(request,"request"); break;
        case progress::Kind::terminal: {
            if(text(value.at("kind"))=="operational") {
                fields(value,{"kind","primary_error","secondary_errors","active_block_id"});
                const auto& secondary=value.at("secondary_errors");require(secondary.is_array(),Error::schema);
                const auto count=ep::detail::add(secondary.size(),2U);
                const auto* transaction=fs::detail::Access::transaction(*writer);
                require(transaction && count<=transaction->remaining_records(),Error::capacity);
                // This wire terminal identifies no earlier failure-record chain.
                // Never infer occurrence identity by comparing equal error values.
                for(const auto& record:writer->records()) {
                    work.debit(1U);
                    require(record.kind!=ep::Kind::failure || record.scope.pair!=scope.pair ||
                        record.scope.experiment!=scope.experiment,Error::identity);
                }
                const auto bytes=transition.canonical.size();
                work.reserve_peak(ep::detail::add(work.peak,ep::detail::add(bytes,
                    ep::detail::multiply(count,sizeof(std::string)+2048U))));
                work.debit(ep::detail::add(ep::detail::multiply(bytes,2U),count));
                std::vector<std::string> errors;errors.reserve(count-1U);
                const auto encode=[&](const Json& input) {
                    auto rendered=core::canonicalize_json_jcs(input,
                        {limits.parse.body_bytes,limits.parse.depth,limits.parse.nodes});
                    core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};
                    require(rendered.has_value(),Error::schema);errors.push_back(std::move(*rendered));
                };
                encode(value.at("primary_error"));for(const auto& error:secondary)encode(error);
                const auto primary=writer->receipt().complete.records+1U;
                Json failure_facts{{"origin","actual_availability"},{"source","finalized_error"},
                    {"role","primary"},{"primary_ref",nullptr},{"artifact",nullptr}};
                core::detail::JsonCleanup failure_cleanup{failure_facts};
                for(std::size_t index{};index<errors.size();++index) {
                    if(index) {failure_facts["role"]="secondary";failure_facts["primary_ref"]=reference(primary,"failure");}
                    failure_facts["artifact"]=errors[index];
                    static_cast<void>(append(stamp,ep::Kind::failure,failure_facts,scope,admission));
                }
            }
            kind=ep::Kind::pair_terminal; facts["start_ref"]=reference(start,"pair_start");
            facts["terminal"]=std::string_view{transition.canonical.data(),transition.canonical.size()};
            return append(stamp,kind,facts,selected,admission);
        }
        default: throw Error::unsupported;
        }
        facts["component"]=std::string_view{transition.canonical.data(),transition.canonical.size()};
        return append(stamp,kind,facts,selected,admission);
    }
};
Adapter::Adapter(std::unique_ptr<Data> data) noexcept : data_{std::move(data)} {}
Adapter::~Adapter()=default;
Adapter::Adapter(Adapter&&) noexcept=default;
auto Adapter::prepare(fs::Writer& writer, const progress::Offer& offer, ep::Scope scope,
    TimestampSource time, Limits limits) -> std::expected<Adapter,Error> {
    try {
        const auto* transaction=fs::detail::Access::transaction(writer);
        require(transaction && transaction->available() && ep::is_progress(transaction->version()) &&
            writer.receipt().state==fs::State::open,Error::state);
        require(scope.experiment && scope.pair && !scope.block && !scope.observation && time.read &&
            time.origin==offer.utc,Error::identity);
        const auto bound=progress::bind_offer(offer); require(bound && *bound==offer,Error::identity);
        const auto* composition=transaction->composition(); require(composition,Error::state);
        const ep::detail::ComposedPair* selected{};
        for (const auto& pair:composition->pairs) {
            if (!pair) continue;
            const auto& start=transaction->records()[static_cast<std::size_t>(pair->start_record-1U)];
            if(start.scope.experiment==scope.experiment && start.scope.pair==scope.pair) selected=pair.get();
        }
        require(selected && selected->context && selected->request_record && !selected->root_record,Error::state);
        const auto& context=*selected->context;
        const auto* preparation=context.lease.complete_preparation();
        require(preparation && preparation->first_request==offer.identity.first && preparation->second_request==offer.identity.second &&
            context.session==std::string_view{offer.identity.session.data(),offer.identity.session.size()},Error::identity);
        require(limits.scratch_bytes>=sizeof(Data) && limits.work!=0,Error::capacity);
        const auto& request=transaction->records()[static_cast<std::size_t>(selected->request_record-1U)];
        auto data=std::make_unique<Data>(Data{&writer,offer,scope,time,limits,selected->context,request.event_facts,
            selected->start_record,selected->request_record,0,0,{},{},{}});
        return Adapter{std::move(data)};
    } catch (Error error) { return std::unexpected{error}; }
    catch (...) { return std::unexpected{Error::capacity}; }
}
auto Adapter::consumer() noexcept -> rt::NativeProgressConsumer {
    if (!data_) return {};
    return {{data_->offer.limits,data_->offer.durability,data_->offer.platform,data_->offer.utc},this,
        +[](void* self,const rt::ValidatedProgressView& view) noexcept {
            return static_cast<Adapter*>(self)->consume(view);
        }};
}
auto Adapter::failure() const noexcept -> std::optional<Error> {return data_?data_->failure:std::optional{Error::state};}
auto Adapter::semantic_failure() const noexcept -> std::optional<ep::Error> {return data_?data_->semantic_failure:std::nullopt;}
auto Adapter::failure_snapshot() const noexcept -> std::optional<ProgressFailure> {
    static_assert(std::is_nothrow_copy_constructible_v<ProgressFailure>);
    static_assert(std::is_nothrow_copy_assignable_v<ProgressFailure>);
    static_assert(std::is_nothrow_move_constructible_v<ProgressFailure>);
    return data_?data_->failure_snapshot:std::nullopt;
}
auto Adapter::received() const noexcept -> std::uint64_t {return data_?data_->received:0;}
auto Adapter::persisted() const noexcept -> std::uint64_t {return data_?data_->persisted:0;}
auto Adapter::reconcile(rt::PairwiseV2NativeController& controller, const PairwiseRunResult& result,
    core::JcsLimits analysis_limits) -> std::expected<fs::Receipt,Error> {
    if (!data_ || data_->failure) return std::unexpected{Error::state};
    auto& data=*data_;
    try {
        require(data.received && data.received<=UINT64_MAX-2U,Error::capacity);
        const auto bytes=controller.take_validated_bytes(); require(bytes.has_value(),Error::state);
        const auto* transaction=fs::detail::Access::transaction(*data.writer);
        require(transaction && transaction->composition(),Error::state);
        const ep::detail::ComposedPair* pair{};
        for(const auto& candidate:transaction->composition()->pairs)
            if(candidate && candidate->start_record==data.start) pair=candidate.get();
        require(pair && pair->prefix && pair->prefix->terminal_event() && !pair->root_record,Error::state);
        ep::detail::Work work{0,data.limits.work,0,data.limits.scratch_bytes};
        const std::string_view canonical{reinterpret_cast<const char*>(bytes->data()),bytes->size()};
        detail::reserve_reconciliation(canonical,transaction->version(),data.limits,
            pair->events.size(),analysis_limits,work);
        std::array<char,30> time{}; require(data.time.read(data.time.context,time),Error::timestamp);
        auto stamp=journal::Timestamp::parse({time.data(),time.size()}); core::detail::ExpectedErrorCleanup stamp_cleanup{stamp};
        require(stamp.has_value(),Error::timestamp);
        const ep::ProducerAdmission root_admission{{ep::AvailabilitySource::controller_observation,0,
            data.received+1U,data.received+1U,{},data.offer.platform==progress::Platform::native,
            data.offer.utc==progress::Utc::acquired}};
        Json facts{{"origin","actual_availability"},{"request_ref",reference(data.request,"request")},
            {"terminal_ref",reference(data.prior(ep::Kind::pair_terminal),"terminal")},
            {"placement_refs",Json::array()},{"component_refs",Json::array()}};
        core::detail::JsonCleanup cleanup{facts};
        if(transaction->version()==ep::Version::compact_v3)
            facts["canonical_root_digest"]=core::sha256(canonical).to_hex();
        else facts["canonical_root"]=canonical;
        for(const auto sequence:pair->events) {
            work.debit(1U);
            const auto& record=transaction->records()[static_cast<std::size_t>(sequence-1U)];
            std::string_view token;
            switch(record.kind) {
            case ep::Kind::placement: {
                const auto& phase=text(record.event_facts->at("phase"));
                token=phase=="control_readiness"?"control":phase=="worker_readiness"?"workers":
                    phase=="memory_finalization"?"finalization":std::string_view{phase}; break;
            }
            case ep::Kind::preparation: token="preparation"; break;
            case ep::Kind::calibration: token="calibration"; break;
            case ep::Kind::warmup: token="warmup"; break;
            case ep::Kind::block_opened: case ep::Kind::block_closed: token="block"; break;
            case ep::Kind::observation: token="observation"; break;
            case ep::Kind::failed_observation: token="failed_observation"; break;
            case ep::Kind::stopping: token="stopping"; break;
            case ep::Kind::pair_terminal: token="terminal"; break;
            case ep::Kind::monitoring: token="monitoring"; break;
            case ep::Kind::lifecycle_progress: case ep::Kind::event_eof: case ep::Kind::child_reaped: continue;
            default: throw Error::identity;
            }
            auto ref=reference(sequence,token); core::detail::JsonCleanup ref_cleanup{ref};
            push(facts["component_refs"],ref);
            if(record.kind==ep::Kind::placement) push(facts["placement_refs"],ref);
        }
        const auto root=data.append(*stamp,ep::Kind::finalization,facts,data.scope,root_admission,
            process_subject::Producer::runner);
        core::detail::retire_json(facts);
        auto serialized=analysis::serialize_pairwise_analysis(result,analysis_limits);
        core::detail::ExpectedErrorCleanup serialized_cleanup{serialized}; require(serialized.has_value(),Error::schema);
        facts={{"origin","actual_availability"},{"finalization_ref",reference(root.complete.records,"finalization")},
            {"profile","epycbench.pairwise-derived-analysis/v1"},{"canonical",*serialized}};
        const ep::ProducerAdmission analysis_admission{{ep::AvailabilitySource::controller_observation,0,
            data.received+2U,data.received+2U,{},data.offer.platform==progress::Platform::native,
            data.offer.utc==progress::Utc::acquired}};
        return data.append(*stamp,ep::Kind::analysis,facts,data.scope,analysis_admission,
            process_subject::Producer::results);
    } catch(Error error) {data.failure=error;}
    catch(ep::Error) {data.failure=Error::capacity;}
    catch(...) {data.failure=Error::capacity;}
    return std::unexpected{*data.failure};
}
auto Adapter::consume(const rt::ValidatedProgressView& view) noexcept -> rt::ProgressConsumeResult {
    if (!data_ || data_->failure) return {};
    auto& data=*data_;
    try {
        require(view.offer==data.offer && data.received!=UINT64_MAX && view.transition.ordinal==data.received+1U,Error::identity);
        require(view.transition.availability && view.transition.epoch &&
            core::sha256(std::string_view{view.transition.canonical.data(),view.transition.canonical.size()}).bytes()==view.transition.digest,Error::identity);
        ++data.received; // Receipt precedes parsing/persistence; failure never erases it.
        const ep::ProducerAdmission admission{{ep::AvailabilitySource::producer_callback,
            view.transition.operation,view.transition.availability,view.transition.ordinal,view.transition.epoch,
            view.offer.platform==progress::Platform::native,view.offer.utc==progress::Utc::acquired}};
        std::array<char,30> time{}; require(data.time.read(data.time.context,time),Error::timestamp);
        auto stamp=journal::Timestamp::parse({time.data(),time.size()}); core::detail::ExpectedErrorCleanup stamp_cleanup{stamp};
        require(stamp.has_value(),Error::timestamp);
        ep::detail::Work work{0,data.limits.work,0,data.limits.scratch_bytes};
        const std::string_view bytes{view.transition.canonical.data(),view.transition.canonical.size()};
        const auto shape=ep::detail::shape(bytes,data.limits.parse,work);
        auto scratch=ep::detail::multiply(ep::detail::workspace(shape),8U);
        if (view.transition.kind==progress::Kind::preparation) {
            const auto context_shape=ep::detail::shape(data.context->canonical,data.limits.parse,work);
            scratch=ep::detail::add(scratch,ep::detail::multiply(ep::detail::workspace(context_shape),3U));
        }
        work.reserve_peak(scratch);
        auto value=ep::detail::parse_tree(bytes,data.limits.parse,work); core::detail::JsonCleanup cleanup{value};
        require(data.writer->receipt().complete.records!=UINT64_MAX,Error::capacity);
        const auto first=data.writer->receipt().complete.records+1U;
        const auto receipt=data.component(*stamp,view.transition,value,admission,work);
        const bool synchronized=view.offer.durability==progress::Durability::persistent_block &&
            view.transition.kind==progress::Kind::block_closed;
        if(synchronized) require(receipt.synchronized && receipt.synchronized->records==receipt.complete.records &&
            receipt.synchronized->digest==receipt.complete.digest,Error::journal);
        data.persisted=view.transition.ordinal;
        return {rt::ProgressConsumeResult::State::continue_run,{view.transition.ordinal,first,receipt.complete.records,
            view.transition.digest,receipt.complete.digest->bytes(),synchronized?progress::Receipt::synchronized_boundary:progress::Receipt::complete_write}};
    } catch (Error error) {data.failure=error;}
    catch (ep::Error) {data.failure=Error::schema;}
    catch (...) {data.failure=Error::capacity;}
    if(!data.failure_snapshot) {
        const auto* transaction=fs::detail::Access::transaction(*data.writer);
        data.failure_snapshot=ProgressFailure{view.transition.kind,view.transition.ordinal,
            *data.failure,data.semantic_failure,transaction?transaction->pair_failure():std::nullopt};
    }
    return {};
}
}
