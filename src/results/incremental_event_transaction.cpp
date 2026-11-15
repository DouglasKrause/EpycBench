#include "results/incremental_event_transaction.hpp"
#include "results/incremental_event_pair.hpp"

#include <algorithm>
#include <tuple>
#include <type_traits>

namespace epycbench::results::event_profile {
namespace {
using detail::add;
using detail::multiply;
using detail::require;
namespace es=error_serialization;
namespace ts=topology_serialization;
namespace ss=source_serialization;
namespace ps=process_subject;
enum class Family { creation,source,build,host,topology,normalization,outcome,error };
struct Address {
    Family family{};
    unsigned kind{};
    std::uint64_t sequence{};
    std::optional<std::uint64_t> ordinal;
    auto operator<=>(const Address&) const = default;
};
struct Origin {
    Address address;
    std::string_view canonical;
    Reference reference;
    // Provenance lives in the child's heap state, unlike the movable Owner
    // handle stored inline in Record's vector.
    const es::Provenance* failure{};
};
auto error_address(const es::Owner& o) -> Address {
    const auto& p=*o.provenance();
    return {Family::error,static_cast<unsigned>(p.kind),p.sequence,p.creation_ordinal};
}
auto failure_address(std::uint64_t sequence,std::optional<std::uint64_t> ordinal) -> Address {
    return {Family::error,static_cast<unsigned>(ordinal?es::Kind::creation_failure:es::Kind::ordinary_failure),sequence,ordinal};
}
auto find(std::span<const Origin> origins,Address address,detail::Work& work) -> const Origin* {
    const auto it=std::lower_bound(origins.begin(),origins.end(),address,[&](const Origin& a,const Address& b) {
        work.debit(1); return a.address<b;
    });
    return it!=origins.end() && it->address==address?&*it:nullptr;
}
}
struct Transaction::State {
    std::string run;
    Limits limits;
    Usage usage;
    std::vector<Record> records;
    std::vector<Origin> origins;
    std::shared_ptr<const detail::CompositionState> composition;
    std::unique_ptr<char[]> body;
    std::size_t body_size{},body_capacity{},pending_work{};
    Summary summary;
    bool live{true},inside{},writing{},reading{},source_done{},topology_done{};
    std::optional<ps::Domain> domain;
    std::optional<block_evidence::Error> pair_failure;
};
namespace {
void progress_chronology(const Transaction::State& s,const Record& record,detail::Work& work) {
    const auto preparation=[](const Record& value) {
        if(value.scope!=Scope{}||!value.availability||value.availability->source!=AvailabilitySource::controller_observation)return false;
        if(value.kind==Kind::planning||value.kind==Kind::preflight||value.kind==Kind::run_closed)return true;
        return value.kind==Kind::failure&&value.event_facts&&
            (value.event_facts->at("source")=="controller_preparation"||value.event_facts->at("source")=="private_error");
    };
    if(preparation(record)) {
        const Record* prior{};
        for(auto it=s.records.rbegin();it!=s.records.rend();++it){work.debit(1U);if(preparation(*it)){prior=&*it;break;}}
        const auto& current=*record.availability;
        require(current.delivery==current.transition,Error::sequence);
        if(!prior)require(current.delivery==1U,Error::sequence);
        else {
            const auto& previous=*prior->availability;
            require(previous.delivery!=UINT64_MAX&&current.delivery==previous.delivery+1U&&
                current.native_platform==previous.native_platform&&current.acquired_utc==previous.acquired_utc,Error::sequence);
        }
        return;
    }
    if (!record.availability || record.availability->source!=AvailabilitySource::producer_callback) return;
    const auto& current=*record.availability;
    require(record.scope.experiment && record.scope.pair,Error::association);
    const Record* prior{};
    for(auto it=s.records.rbegin();it!=s.records.rend();++it) {
        work.debit(1U);
        if(it->availability && it->availability->source==AvailabilitySource::producer_callback &&
            it->scope.experiment==record.scope.experiment && it->scope.pair==record.scope.pair) {prior=&*it;break;}
    }
    if (!prior) {
        require(current.delivery==1 && current.transition==1 && current.operation==0 &&
            current.quiescence_epoch && *current.quiescence_epoch!=0 && record.kind==Kind::placement,Error::sequence);
        return;
    }
    const auto& previous=*prior->availability;
    if(current.delivery==previous.delivery) {
        require(current==previous && work.equal(record.timestamp,prior->timestamp) &&
            prior->sequence==s.records.back().sequence,Error::sequence);
        return;
    }
    require(previous.delivery!=UINT64_MAX && previous.transition!=UINT64_MAX &&
        current.delivery==previous.delivery+1U && current.transition==previous.transition+1U &&
        current.operation>=previous.operation && (current.operation!=0 || record.kind==Kind::block_opened) &&
        current.quiescence_epoch>=previous.quiescence_epoch &&
        current.native_platform==previous.native_platform && current.acquired_utc==previous.acquired_utc,Error::sequence);
}
auto creation(const Transaction::State& s) -> const run_creation_serialization::Owner& {
    require(!s.records.empty() && s.records.front().kind==Kind::creation,Error::reference);
    return std::get<run_creation_serialization::Owner>(s.records.front().artifact);
}
void metadata(std::string_view run,ps::Domain domain,const Transaction::State& s) {
    require(run==s.run && (!s.domain || domain==*s.domain),Error::identity);
}
auto root(const Transaction::State& s,const std::optional<Reference>& ref,RefKind kind) -> const Record& {
    require(ref && ref->kind==kind && ref->ordinal==0 && ref->sequence && ref->sequence<=s.records.size(),Error::reference);
    const auto& r=s.records[static_cast<std::size_t>(ref->sequence-1)];
    require((kind==RefKind::creation && r.kind==Kind::creation) ||
            (kind==RefKind::topology && r.kind==Kind::topology),Error::reference);
    return r;
}
void same_source(const ss::Owner* a,const ss::Owner* b,detail::Work& work) {
    require(a && b && a->source() && b->source(),Error::association);
    const auto x=a->source()->input(),y=b->source()->input();
    require(x.availability==y.availability && x.format==y.format &&
        x.acquisition_failure==y.acquisition_failure && a->provenance()->acquisition==b->provenance()->acquisition &&
        work.equal(x.bytes,y.bytes) && work.equal(x.name,y.name) &&
        x.rejected_overrides.size()==y.rejected_overrides.size(),Error::association);
    for(std::size_t i=0;i<x.rejected_overrides.size();++i)
        require(work.equal(x.rejected_overrides[i],y.rejected_overrides[i]),Error::association);
    const auto* ae=a->acquisition_error(); const auto* be=b->acquisition_error();
    require(bool(ae)==bool(be),Error::association);
    if(ae) require(work.equal(ae->canonical(),be->canonical()),Error::association);
}
struct Delta {
    std::vector<Origin> origins;
    std::shared_ptr<const detail::CompositionState> composition;
    Usage usage;
    bool failed{},source_done{},topology_done{};
    ps::Domain domain{};
};
auto prepare(Transaction::State& s,detail::Candidate& c,detail::Work& work,std::size_t frame_bytes) -> Delta {
    const auto& r=c.record;
    require(r.sequence==s.records.size()+1,Error::sequence);
    require((s.reading && s.records.empty()) || r.version==s.limits.version,Error::schema);
    require((s.records.empty())==(r.kind==Kind::creation),Error::association);
    progress_chronology(s,r,work);
    std::visit([&](const auto& owner){if constexpr(std::is_same_v<std::decay_t<decltype(owner)>,std::monostate>)require(r.event_facts!=nullptr,Error::state);
        else{const auto* p=owner.provenance();require(p!=nullptr,Error::child);metadata(p->run,p->domain,s);}},r.artifact);
    const auto domain=std::visit([&](const auto& owner){if constexpr(std::is_same_v<std::decay_t<decltype(owner)>,std::monostate>){require(s.domain.has_value(),Error::identity);return *s.domain;}else return owner.provenance()->domain;},r.artifact);
    std::size_t additions=4;
    if(r.kind==Kind::creation) additions=add(5,std::get<run_creation_serialization::Owner>(r.artifact).failures().size());
    const auto upper=add(s.origins.size(),additions);
    // Reserve a bounded candidate index. Duplicate child addresses do not consume
    // retained entries, but this transient upper bound is charged before allocation.
    const auto index_bytes=multiply(upper,sizeof(Origin));
    const auto new_records=multiply(add(s.records.size(),1),sizeof(Record));
    const auto parser=detail::workspace(c.shape);
    const auto candidate=std::max(work.peak,add(add(add(parser,c.child_peak),index_bytes),frame_bytes));
    const auto simultaneous=add(add(add(s.usage.retained_bytes,s.body_capacity),candidate),new_records);
    require(candidate<=s.limits.candidate_bytes && simultaneous<=s.limits.simultaneous_bytes,Error::capacity);
    // Every following composition phase admits its stock before allocating it.
    // The parsed envelope, candidate index and old committed prefix stay live.
    if(r.event_facts) work.peak_limit=std::min(s.limits.candidate_bytes-candidate,
        s.limits.simultaneous_bytes-simultaneous);
    Delta d; d.usage=s.usage; d.failed=s.summary.recovery==Recovery::failed;
    d.composition=s.composition;
    d.source_done=s.source_done; d.topology_done=s.topology_done; d.domain=domain;
    require(upper<=d.origins.max_size(),Error::capacity);
    work.debit(s.origins.size());
    d.origins.reserve(upper); d.origins.insert(d.origins.end(),s.origins.begin(),s.origins.end());
    auto register_origin=[&](Address address,std::string_view canonical,Reference ref,const es::Provenance* failure=nullptr) {
        work.debit(d.origins.size());
        auto at=std::lower_bound(d.origins.begin(),d.origins.end(),address,[](const Origin& x,const Address& y){return x.address<y;});
        if(at!=d.origins.end() && at->address==address) {
            require(work.equal(at->canonical,canonical),Error::association); return;
        }
        require(d.origins.size()<s.limits.origin_entries,Error::capacity);
        d.origins.insert(at,{address,canonical,ref,failure});
    };
    auto existing_error=[&](const es::Owner* error) {
        if(!error) return;
        const auto* found=find(d.origins,error_address(*error),work);
        require(found && found->failure && work.equal(found->canonical,error->canonical()),Error::reference);
    };
    auto register_error=[&](const es::Owner& e,Reference ref) {
        metadata(e.provenance()->run,e.provenance()->domain,s);
        register_origin(error_address(e),e.canonical(),ref,e.provenance());
        d.failed=true;
    };
    auto register_source=[&](const ss::Owner* source) {
        if(!source) return;
        const auto& p=*source->provenance(); metadata(p.run,p.domain,s);
        register_origin({Family::source,static_cast<unsigned>(p.kind),p.sequence,{}},source->canonical(),{r.sequence,0,RefKind::creation});
        existing_error(source->acquisition_error());
    };
    auto register_topology=[&](const ts::Owner& t,Reference ref) {
        const auto& p=*t.provenance(); metadata(p.run,p.domain,s);
        require(p.reporter==ps::Producer::controller,Error::association);
        register_origin({Family::topology,static_cast<unsigned>(p.kind),p.sequence,{}},t.canonical(),ref);
        existing_error(t.failure_error());
    };
    auto validate_association=[&](const es::Owner& e,const std::optional<Reference>& explicit_primary,bool standalone) {
        const auto& p=*e.provenance(); require(p.association.has_value(),Error::association);
        const auto& a=*p.association;
        const auto controller=r.kind==Kind::creation ? std::get<run_creation_serialization::Owner>(r.artifact).provenance()->sequence : creation(s).provenance()->sequence;
        require(!a.process_sequence || *a.process_sequence==controller,Error::association);
        if(a.secondary_sequence) {
            const auto* prior=find(d.origins,failure_address(*a.secondary_sequence,a.secondary_creation_ordinal),work);
            require(prior && prior->failure,Error::reference);
            const auto* q=prior->failure;
            require(q->association && !q->association->secondary_sequence && q->association->process_sequence==a.process_sequence,Error::association);
            if(standalone) require(explicit_primary && *explicit_primary==prior->reference && r.role==Role::secondary,Error::reference);
        } else if(standalone) {
            if(explicit_primary) {
                require(is_progress(r.version)&&p.domain==ps::Domain::supplied_observation&&r.scope==Scope{}&&
                    r.role==Role::secondary&&explicit_primary->kind==RefKind::ordinary_failure&&explicit_primary->ordinal==0U&&
                    explicit_primary->sequence>0U&&explicit_primary->sequence<=s.records.size(),Error::reference);
                const auto& primary=s.records[static_cast<std::size_t>(explicit_primary->sequence-1U)];
                require(primary.kind==Kind::failure&&primary.scope==Scope{}&&primary.event_facts&&
                    primary.event_facts->at("source")=="controller_preparation"&&primary.event_facts->at("role")=="primary",Error::reference);
                // The ordinary artifact keeps its original Session association.
                // Its journal disposition references the earlier admitted call failure.
                for(const auto& prior:s.records) {
                    work.debit(1U);require(prior.kind!=Kind::pair_started,Error::association);
                    if(prior.kind==Kind::failure&&prior.sequence<primary.sequence)throw Error::reference;
                }
            } else require(r.role==Role::primary,Error::reference);
        }
    };
    switch(r.kind) {
    case Kind::creation: {
        const auto& a=std::get<run_creation_serialization::Owner>(r.artifact); const auto& p=*a.provenance();
        require(p.reporter==ps::Producer::controller,Error::association);
        register_origin({Family::creation,0,p.sequence,{}},a.canonical(),{r.sequence,0,RefKind::creation});
        std::uint64_t ordinal{};
        for(const auto& error:a.failures()) {
            validate_association(error,{},false);
            register_error(error,{r.sequence,ordinal++,RefKind::creation_failure});
        }
        register_source(a.source());
        if(a.base_build()) register_origin({Family::build,0,p.sequence,{}},a.base_build()->canonical(),{r.sequence,0,RefKind::creation});
        if(a.host()) {
            register_origin({Family::host,0,p.sequence,{}},a.host()->canonical(),{r.sequence,0,RefKind::creation});
            require(a.host()->topology()!=nullptr,Error::child);
            register_topology(*a.host()->topology(),{r.sequence,0,RefKind::topology});
        }
        break;
    }
    case Kind::topology: {
        (void)root(s,r.creation,RefKind::creation); require(!s.topology_done,Error::association);
        const auto& a=std::get<ts::Owner>(r.artifact); const auto& p=*a.provenance();
        if(p.kind==ts::Kind::genesis) {
            const auto* host=creation(s).host(); require(host && host->topology(),Error::association);
            require(work.equal(a.canonical(),host->topology()->canonical()),Error::association);
        }
        register_topology(a,{r.sequence,0,RefKind::topology}); d.topology_done=true; break;
    }
    case Kind::normalization: {
        (void)root(s,r.creation,RefKind::creation); require(!s.source_done,Error::association);
        const auto& top=std::get<ts::Owner>(root(s,r.topology,RefKind::topology).artifact);
        const auto& a=std::get<normalization_serialization::Owner>(r.artifact); const auto& p=*a.provenance();
        require(top.topology() && a.topology() && p.topology_sequence==top.provenance()->sequence &&
                work.equal(top.canonical(),a.topology()->canonical()),Error::association);
        same_source(creation(s).source(),a.source(),work);
        register_origin({Family::normalization,0,p.sequence,{}},a.canonical(),{r.sequence,0,RefKind::creation});
        register_source(a.source()); register_topology(*a.topology(),*r.topology); d.source_done=true; break;
    }
    case Kind::source_outcome: {
        (void)root(s,r.creation,RefKind::creation); require(!s.source_done,Error::association);
        const auto& a=std::get<source_outcome_serialization::Owner>(r.artifact); const auto& p=*a.provenance();
        require(a.outcome().has_value() && a.outcome()!=authored_source::State::resolved,Error::association);
        same_source(creation(s).source(),a.source(),work);
        register_origin({Family::outcome,0,p.sequence,{}},a.canonical(),{r.sequence,0,RefKind::creation});
        // Acquisition failure and resolution failure are different identities.
        register_source(a.source());
        if(a.upstream_error()) register_error(*a.upstream_error(),{r.sequence,0,RefKind::source_resolution_failure});
        d.source_done=true;
        if(a.outcome()!=authored_source::State::not_attempted) d.failed=true;
        break;
    }
    case Kind::failure: {
        if(r.event_facts){require(s.limits.composition!=nullptr,Error::unsupported);
            d.composition=detail::compose(s.records,r,s.composition,*s.limits.composition,work);
            if(d.composition->operational_failure)d.failed=true;
            break;}
        const auto& a=std::get<es::Owner>(r.artifact); const auto& p=*a.provenance();
        require(p.kind==es::Kind::ordinary_failure && !find(d.origins,error_address(a),work),Error::association);
        validate_association(a,r.primary,true);
        register_error(a,{r.sequence,0,RefKind::ordinary_failure}); break;
    }
    default:
        require(s.limits.composition && r.event_facts,Error::unsupported);
        d.composition=detail::compose(s.records,r,s.composition,*s.limits.composition,work);
        if(d.composition->operational_failure)d.failed=true;
        break;
    }
    d.usage.canonical_bytes=add(d.usage.canonical_bytes,r.canonical.size());
    d.usage.nodes=add(d.usage.nodes,c.shape.nodes); d.usage.text_bytes=add(d.usage.text_bytes,c.shape.text);
    d.usage.origin_entries=d.origins.size();
    if(r.event_facts){
        const auto count_references=[&](auto&& self,const nlohmann::json& value,std::size_t depth)->std::size_t{
            require(depth<=64U,Error::capacity);work.debit(1U);
            if(value.is_object()&&value.size()==3U&&value.contains("sequence")&&value.contains("ordinal")&&value.contains("kind"))return 1U;
            std::size_t count{};if(value.is_structured())for(const auto& child:value)count=add(count,self(self,child,add(depth,1U)));return count;
        };
        d.usage.references=add(d.usage.references,count_references(count_references,*r.event_facts,1U));
    }
    d.usage.references=add(d.usage.references,static_cast<std::size_t>(r.creation.has_value())+
        static_cast<std::size_t>(r.topology.has_value())+static_cast<std::size_t>(r.primary.has_value()));
    // Vector storage is charged separately from child heap state, so child handles
    // already inside Record do not count a second time.
    const auto handle=std::visit([](const auto& a){return sizeof(a);},r.artifact);
    require(c.child_retained>=handle,Error::capacity);
    const auto prior_index=multiply(s.origins.capacity(),sizeof(Origin));
    const auto prior_records=multiply(s.records.capacity(),sizeof(Record));
    require(s.usage.retained_bytes>=add(prior_index,prior_records),Error::capacity);
    auto retained=s.usage.retained_bytes-prior_index-prior_records;
    retained=add(retained,add(add(add(r.canonical.capacity(),1),add(r.timestamp.capacity(),1)),c.child_retained-handle));
    retained=add(retained,add(multiply(d.origins.capacity(),sizeof(Origin)),new_records));
    if(s.composition){require(retained>=s.composition->retained,Error::capacity);retained-=s.composition->retained;}
    if(d.composition)retained=add(retained,d.composition->retained);
    // Ordinary composition overlaps the outer parsed record, candidate origin index,
    // and child replay. Their conservative logical stocks are additive.
    const auto composed_peak=r.event_facts?add(candidate,work.peak):std::max(candidate,work.peak);
    require(composed_peak<=s.limits.candidate_bytes &&
        add(add(s.usage.retained_bytes,s.body_capacity),add(composed_peak,new_records))<=s.limits.simultaneous_bytes,Error::capacity);
    d.usage.retained_bytes=retained;
    d.usage.candidate_bytes=std::max(d.usage.candidate_bytes,composed_peak);
    d.usage.simultaneous_bytes=std::max(d.usage.simultaneous_bytes,add(add(s.usage.retained_bytes,s.body_capacity),add(composed_peak,new_records)));
    work.debit(s.records.size()); // noexcept moves at the final vector growth.
    d.usage.work=work.used;
    require(d.usage.canonical_bytes<=s.limits.canonical_bytes && d.usage.nodes<=s.limits.nodes &&
        d.usage.text_bytes<=s.limits.text_bytes && d.usage.references<=s.limits.references &&
        retained<=s.limits.retained_bytes,Error::capacity);
    return d;
}
void publish(Transaction::State& s,detail::Candidate&& c,Delta&& d) {
    static_assert(std::is_nothrow_move_constructible_v<Record>);
    // Last throwing operation. No inspection field changes before it succeeds.
    s.records.reserve(s.records.size()+1);
    s.records.push_back(std::move(c.record));
    s.limits.version=s.records.back().version;
    s.origins=std::move(d.origins); s.composition=std::move(d.composition); s.usage=d.usage;
    s.domain=d.domain; s.source_done=d.source_done; s.topology_done=d.topology_done;
    s.summary.records=static_cast<std::uint64_t>(s.records.size());
    s.summary.digest=s.records.back().digest;
    s.summary.recovery=d.failed?Recovery::failed:s.composition&&s.composition->closed_record?Recovery::completed:Recovery::interrupted;
}
void consume(Transaction::State& s,Error e) noexcept {
    s.live=false;
    if(e!=Error::capacity && e!=Error::limits && e!=Error::state) {
        s.summary.tail=Tail::corrupt; s.summary.recovery=Recovery::failed;
    }
}
auto remaining(const Transaction::State& s) -> Limits {
    auto limits=s.limits;
    require(s.usage.retained_bytes<=limits.simultaneous_bytes,Error::capacity);
    const auto free=limits.simultaneous_bytes-s.usage.retained_bytes;
    require(s.body_capacity<=free,Error::capacity);
    limits.candidate_bytes=std::min(limits.candidate_bytes,free-s.body_capacity);
    return limits;
}
}
auto Transaction::composition() const noexcept -> const detail::CompositionState* {
    return state_ ? state_->composition.get() : nullptr;
}
Transaction::Transaction(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Transaction::~Transaction()=default;
Transaction::Transaction(Transaction&&) noexcept=default;
auto Transaction::operator=(Transaction&&) noexcept -> Transaction& = default;
auto Transaction::create(const journal::RunId& run,Limits limits) -> std::expected<Transaction,Error> {
    try {
        require(limits.version==Version::supplied_v1 || is_progress(limits.version),Error::limits);
        require(limits.depth<=64 && limits.body_bytes<=std::string{}.max_size()-2,Error::limits);
        const auto retained=add(sizeof(State),run.text().size());
        require(retained<=limits.retained_bytes && retained<=limits.simultaneous_bytes,Error::capacity);
        auto state=std::make_unique<State>(); state->run=run.text(); state->limits=limits;
        state->usage.retained_bytes=retained; return Transaction{std::move(state)};
    } catch(Error e) { return std::unexpected{e}; }
}
auto Transaction::append(const Input& input) -> std::expected<std::string,Error> {
    if(!available()) return std::unexpected{Error::state};
    auto& s=*state_;
    try {
        require(!s.reading&&(!s.composition||!s.composition->closed_record),Error::state); s.writing=true;
        require(s.summary.records<s.limits.records && s.summary.records<UINT64_MAX,Error::capacity);
        detail::Work work{s.usage.work,s.limits.work}; const auto limits=remaining(s);
        auto body=detail::encode(input,s.run,s.summary.records+1,s.summary.digest,limits,work);
        auto candidate=detail::decode(body,s.run,s.summary.records+1,s.summary.digest,limits,work);
        auto delta=prepare(s,candidate,work,add(body.size(),2));
        std::string frame; frame.reserve(add(body.size(),2)); frame.push_back('\x1e'); frame.append(body); frame.push_back('\n');
        publish(s,std::move(candidate),std::move(delta)); return frame;
    } catch(const detail::PairAdmissionFailure& failure) {
        if(!s.pair_failure)s.pair_failure=failure.error;
        consume(s,Error::association);return std::unexpected{Error::association};
    } catch(Error e) {consume(s,e); return std::unexpected{e};}
    catch(...) {s.live=false; throw;}
}
auto Transaction::append(const SuppliedEvent& input) -> std::expected<std::string,Error> {
    if(!available()) return std::unexpected{Error::state};
    auto& s=*state_;
    try {
        require(!s.reading&&(!s.composition||!s.composition->closed_record),Error::state); s.writing=true;
        require(s.summary.records<s.limits.records && s.summary.records<UINT64_MAX,Error::capacity);
        detail::Work work{s.usage.work,s.limits.work}; const auto limits=remaining(s);
        auto body=detail::encode(input,s.run,s.summary.records+1,s.summary.digest,limits,work);
        auto candidate=detail::decode(body,s.run,s.summary.records+1,s.summary.digest,limits,work);
        auto delta=prepare(s,candidate,work,add(body.size(),2));
        std::string frame; frame.reserve(add(body.size(),2)); frame.push_back('\x1e'); frame.append(body); frame.push_back('\n');
        publish(s,std::move(candidate),std::move(delta)); return frame;
    } catch(const detail::PairAdmissionFailure& failure) {
        if(!s.pair_failure)s.pair_failure=failure.error;
        consume(s,Error::association);return std::unexpected{Error::association};
    } catch(Error e) {consume(s,e); return std::unexpected{e};}
    catch(...) {s.live=false; throw;}
}
auto Transaction::feed(std::string_view input) -> std::expected<Feed,Error> {
    if(!available()) return std::unexpected{Error::state};
    auto& s=*state_;
    try {
        require(!s.writing,Error::state); s.reading=true;
        std::size_t consumed{};
        for(char byte:input) {
            require(!s.composition||!s.composition->closed_record,Error::framing);
            ++consumed;
            if(!s.inside) {
                require(byte=='\x1e',Error::framing);
                require(s.summary.records<s.limits.records && s.summary.records<UINT64_MAX,Error::capacity);
                s.inside=true; s.summary.fragment_bytes=1; continue;
            }
            if(byte=='\n') {
                detail::Work work{add(s.usage.work,s.pending_work),s.limits.work};
                auto candidate=detail::decode({s.body?s.body.get():"",s.body_size},s.run,s.summary.records+1,s.summary.digest,remaining(s),work);
                auto delta=prepare(s,candidate,work,0);
                publish(s,std::move(candidate),std::move(delta));
                s.body.reset();s.body_size=0;s.body_capacity=0;s.pending_work=0;
                s.inside=false; s.summary.fragment_bytes=0;
                return Feed{consumed,true};
            }
            require(byte!='\x1e' && s.body_size<s.limits.body_bytes,byte=='\x1e'?Error::framing:Error::capacity);
            const auto size=add(s.body_size,1);
            const bool grow=size>s.body_capacity;
            auto capacity=s.body_capacity;
            if(grow) {
                const auto doubled=capacity>s.limits.body_bytes/2?s.limits.body_bytes:multiply(capacity,2);
                capacity=std::min(s.limits.body_bytes,std::max(size,std::max(std::size_t{32},doubled)));
                require(add(s.usage.retained_bytes,add(s.body_capacity,capacity))<=s.limits.simultaneous_bytes,Error::capacity);
            }
            const auto pending=add(s.pending_work,add(1,grow?s.body_size:0));
            require(add(s.usage.work,pending)<=s.limits.work,Error::capacity);
            if(grow) {
                auto replacement=std::make_unique_for_overwrite<char[]>(capacity);
                if(s.body_size) std::copy_n(s.body.get(),s.body_size,replacement.get());
                s.body=std::move(replacement);s.body_capacity=capacity;
            }
            s.body[s.body_size++]=byte;s.pending_work=pending;
            s.summary.fragment_bytes=add(size,1);
        }
        return Feed{consumed,false};
    } catch(const detail::PairAdmissionFailure& failure) {
        if(!s.pair_failure)s.pair_failure=failure.error;
        consume(s,Error::association);return std::unexpected{Error::association};
    } catch(Error e) {consume(s,e); return std::unexpected{e};}
    catch(...) {s.live=false; throw;}
}
auto Transaction::finish() -> std::expected<Summary,Error> {
    if(!available()) return std::unexpected{Error::state};
    state_->live=false; state_->summary.tail=state_->inside?Tail::incomplete_tail:Tail::clean_prefix;
    return state_->summary;
}
auto Transaction::records() const noexcept -> std::span<const Record> {return state_?std::span<const Record>{state_->records}:std::span<const Record>{};}
auto Transaction::usage() const noexcept -> Usage {return state_?state_->usage:Usage{};}
auto Transaction::summary() const noexcept -> Summary {return state_?state_->summary:Summary{};}
auto Transaction::available() const noexcept -> bool {return state_ && state_->live;}
auto Transaction::version() const noexcept -> Version {return state_ ? state_->limits.version : Version::supplied_v1;}
auto Transaction::pair_failure() const noexcept -> std::optional<block_evidence::Error> {
    static_assert(std::is_nothrow_copy_constructible_v<block_evidence::Error>);
    static_assert(std::is_nothrow_copy_assignable_v<block_evidence::Error>);
    return state_ ? state_->pair_failure : std::nullopt;
}
auto Transaction::remaining_records() const noexcept -> std::uint64_t {
    return available() && state_->records.size() <= state_->limits.records
        ? state_->limits.records-static_cast<std::uint64_t>(state_->records.size()) : 0U;
}
} // namespace epycbench::results::event_profile
