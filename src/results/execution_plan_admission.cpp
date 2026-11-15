#include "results/process_subject_admission.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace epycbench::results::planning {
namespace {
using Z=std::size_t; using U=std::uint64_t; using SV=std::string_view;
namespace ps=process_subject; namespace ep=execution_plan; namespace ns=normalization_serialization;
namespace ss=source_serialization; namespace ts=topology_serialization; namespace bs=base_build_serialization;
namespace pp=private_plan_composition;
using Entry=ps::Entry;
void require(bool value,Error error) { if(!value) throw error; }
struct Work {
    Usage usage;
    const Limits& limits;
    void debit(Z amount=1) {
        require(usage.association_work<=limits.association_work && amount<=limits.association_work-usage.association_work,Error::capacity);
        usage.association_work+=amount;
    }
    auto add(Z left,Z right)->Z {
        debit(); auto result=detail::checked_add(left,right); require(result.has_value(),Error::capacity); return *result;
    }
    auto multiply(Z left,Z right)->Z {
        debit(); auto result=detail::checked_multiply(left,right); require(result.has_value(),Error::capacity); return *result;
    }
    void bound(Z value,Z limit) { debit(); require(value<=limit,Error::capacity); }
    auto equal(SV left,SV right)->bool {
        const auto length=detail::checked_add(left.size(),right.size()); require(length.has_value(),Error::capacity);
        const auto charge=detail::checked_add(*length,1); require(charge.has_value(),Error::capacity);
        require(usage.comparison_bytes<=limits.comparison_bytes && *length<=limits.comparison_bytes-usage.comparison_bytes,Error::capacity);
        debit(*charge); usage.comparison_bytes+=*length; return left==right;
    }
};
auto lookup(std::span<const Entry> entries,U sequence,Work& work)->const Entry* {
    Z low=0,high=entries.size();
    while(low<high){ work.debit(); const auto mid=low+(high-low)/2;
        if(entries[mid].sequence<sequence)low=mid+1;else high=mid; }
    work.debit(); return low<entries.size() && entries[low].sequence==sequence?&entries[low]:nullptr;
}
auto host(const Entry* entry)->const host_assembly::Owner* {
    if(!entry)return nullptr;
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();return !entry->host && facts && facts->host?&*facts->host:nullptr;}
    return entry->host?&*entry->host:nullptr;
}
auto base(const Entry* entry)->const ps::BaseBuild* {
    if(!entry)return nullptr;
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();return !entry->base_build && facts && facts->build?&*facts->build:nullptr;}
    return entry->base_build?&*entry->base_build:nullptr;
}
auto planning_record(std::span<const Entry> entries,U marker,Work& work)->const Record& {
    work.debit(); require(marker!=0,Error::reference);
    const auto* entry=lookup(entries,marker,work);
    work.debit(); require(entry && entry->planning,Error::reference); return *entry->planning;
}
struct Roots {
    const ep::Owner* input;
    const build_object_association::Owner* evidence;
    const implementation_selection::Owner* selection;
    const ns::Owner* normalization;
    const ss::Owner* source;
    const pp::Owner* plan;
    const ts::Owner* topology;
    const bs::Owner* build;
};
auto roots(const ep::Owner& input,Work& work)->Roots {
    work.debit(); require(input.digest() && input.execution_digest() && input.counts(),Error::state);
    Roots result{&input,input.evidence(),nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
    work.debit(); require(result.evidence,Error::state);
    result.selection=result.evidence->selection();result.build=result.evidence->base();
    work.debit(); require(result.selection && result.build,Error::state);
    result.normalization=result.selection->normalization();
    work.debit(); require(result.normalization,Error::state);
    result.source=result.normalization->source();result.plan=result.normalization->plan();result.topology=result.normalization->topology();
    work.debit(); require(result.source && result.plan && result.topology,Error::state); return result;
}
auto associate(std::span<const Entry> entries,SV run,ps::Domain domain,U sequence,
    const ep::Owner& incoming,Work& work)->References {
    const auto r=roots(incoming,work);
    work.debit(); require(r.normalization->origin()==ns::Origin::session_selected && r.source->origin()==ss::Origin::session_selected &&
        r.topology->origin()==ts::Origin::session_selected && r.build->origin()==bs::Origin::session_selected,Error::identity);
    const auto* norm=r.normalization->provenance();const auto* source=r.source->provenance();
    const auto* topology=r.topology->provenance();const auto* build=r.build->provenance();
    work.debit(); require(norm && source && topology && build,Error::state);
    const References refs{1,norm->sequence,norm->topology_sequence,build->sequence};
    work.debit(); require(refs.normalization<sequence && refs.topology<sequence && refs.base_build<sequence,Error::reference);
    const auto* controller=lookup(entries,1,work);const auto* normalized=lookup(entries,refs.normalization,work);
    const auto* retained_topology=lookup(entries,refs.topology,work);const auto* retained_build=lookup(entries,refs.base_build,work);
    work.debit(); require(controller && std::holds_alternative<ps::ControllerCreated>(controller->creation) &&
        normalized && normalized->normalization && normalized->producer==ps::Producer::controller &&
        retained_topology && retained_topology->producer==ps::Producer::controller && retained_build &&
        retained_build->producer==ps::Producer::controller,Error::reference);
    const auto& record=normalized->normalization->source;
    work.debit(); require(record.state()==authored_source::State::resolved && record.source() && record.plan() &&
        record.diagnostics().empty() && !record.upstream_error() && normalized->normalization->topology_event_sequence==refs.topology,Error::reference);
    work.debit(); require(norm->domain==domain && source->domain==domain && topology->domain==domain && build->domain==domain &&
        source->kind==ss::Kind::normalization_source && source->sequence==refs.normalization && !source->acquisition &&
        topology->sequence==refs.topology && topology->reporter==ps::Producer::controller && !topology->failure && !topology->reason &&
        build->sequence==refs.base_build && !build->acquisition,Error::identity);
    require(work.equal(norm->run,run) && work.equal(source->run,run) && work.equal(topology->run,run) && work.equal(build->run,run) &&
        work.equal(norm->time,normalized->timestamp) && work.equal(source->time,normalized->timestamp) &&
        work.equal(topology->time,retained_topology->timestamp) && work.equal(build->time,retained_build->timestamp),Error::identity);
    work.debit(); require(topology->kind==(retained_topology->run_creation?ts::Kind::genesis:ts::Kind::ordinary) &&
        build->kind==(retained_build->run_creation?bs::Kind::genesis:bs::Kind::ordinary) &&
        r.topology->slot_state()==ts::SlotState::retained && !r.topology->failure_error() &&
        r.build->disposition()==ps::BuildState::available && !r.build->acquisition_error(),Error::identity);
    work.debit(); require(r.source->source() && !r.source->acquisition_error(),Error::identity);
    const auto first=record.source()->input();const auto second=r.source->source()->input();
    work.debit(); require(first.availability==authored_source::Availability::available && second.availability==first.availability &&
        first.format==second.format && !first.acquisition_failure && !second.acquisition_failure &&
        first.rejected_overrides.empty() && second.rejected_overrides.empty(),Error::identity);
    require(work.equal(first.bytes,second.bytes) && work.equal(first.name,second.name),Error::identity);
    work.debit(); require(r.plan->scenario_digest() && r.plan->resolved_digest() &&
        *r.plan->scenario_digest()==record.plan()->scenario_digest() && *r.plan->resolved_digest()==record.plan()->resolved_scenario_plan_digest(),Error::identity);
    require(work.equal(r.plan->canonical_scenario(),record.plan()->canonical_scenario_json()) &&
        work.equal(r.plan->canonical_resolved_plan(),record.plan()->canonical_resolved_scenario_plan_json()),Error::identity);
    const auto* retained_host=host(retained_topology);
    work.debit(); require(retained_host && retained_host->categories(),Error::reference);
    const auto* actual_topology=std::get_if<topology_evidence::Owner>(&retained_host->categories()->topology);
    const auto* supplied_topology=r.topology->topology();
    work.debit(); require(actual_topology && supplied_topology && actual_topology->digest() && supplied_topology->digest() &&
        actual_topology->fingerprints() && supplied_topology->fingerprints() &&
        *actual_topology->digest()==*supplied_topology->digest() && *actual_topology->fingerprints()==*supplied_topology->fingerprints(),Error::identity);
    require(work.equal(actual_topology->canonical(),supplied_topology->canonical()),Error::identity);
    const auto* actual_build=base(retained_build);const auto* supplied_manifest=r.build->manifest();
    work.debit(); require(actual_build && actual_build->state==ps::BuildState::available && actual_build->manifest &&
        !actual_build->acquisition_failure && supplied_manifest && actual_build->manifest->digest() && supplied_manifest->digest() &&
        *actual_build->manifest->digest()==*supplied_manifest->digest(),Error::identity);
    require(work.equal(actual_build->manifest->canonical(),supplied_manifest->canonical()),Error::identity);
    return refs;
}
// Named logical cells mirror the accepted source-level call inventory. These
// describe argument/return overlap as well as application-owned locals.
struct AppendCall { ps::Session* session;ep::Owner* input;const journal::Timestamp* time;U sequence;
    std::expected<void,Error> result;std::unexpected<Error> failure;Error error;bool live; };
struct AppendLocals { ps::Usage before,next;Usage candidate;ep::Usage child;std::optional<Entry> staging;References refs;
    Z old_capacity,new_capacity,old_spare,new_block,incoming_retained,incoming_canonical,timestamp_bytes,prior_retained,next_retained,peak;
    bool growth;std::vector<Entry>* entries;Z entry_cursor; };
struct RootCells { Roots caller,local,returned;const ep::Owner* receiver;Work* work;const authored_source::ResolutionRecord* record;const scenario::ScenarioDigestedPlan* plan;
    std::array<const Entry*,4> entries;std::array<const authored_source::SourceBundle*,2> sources;
    std::array<const topology_evidence::Owner*,2> topologies;std::array<const build_manifest::Owner*,2> builds;
    const host_assembly::Owner* host;const ps::BaseBuild* base; };
struct ProvenanceCells { const ns::Provenance* normalization;const ss::Provenance* source;const ts::Provenance* topology;const bs::Provenance* build;
    std::optional<ns::Origin> normalization_origin;std::optional<ss::Origin> source_origin;std::optional<ts::Origin> topology_origin;
    std::optional<bs::Origin> build_origin;std::optional<ps::BuildState> disposition;std::optional<ts::SlotState> slot;
    authored_source::Input first,second;U sequence;bool genesis; };
struct LookupCells { std::span<const Entry> entries;U wanted;Z low,high,mid;const Entry* returned;Work* work;const Entry* receiver; };
struct CompareCells { SV first,second;Z charge;bool result;Work* work;std::array<const core::Sha256Digest*,2> digests;
    std::array<const scenario::ScenarioDigest*,2> scenarios;std::array<const scenario::ResolvedScenarioPlanDigest*,2> resolved;
    std::array<const topology::TopologyFingerprints*,2> fingerprints;std::array<const build_manifest::ManifestDigest*,2> manifests;
    std::optional<Z> length,charged; };
struct GetterCells { Roots receivers;const authored_source::ResolutionRecord* record;const authored_source::SourceBundle* source;
    const scenario::ScenarioDigestedPlan* plan;const host_assembly::Owner* host;const run_creation::Owner* creation;
    const ps::BaseBuild* base;const topology_evidence::Owner* topology;const build_manifest::Owner* manifest;const journal::Timestamp* time;
    SV first,second;ProvenanceCells origin_returns;authored_source::Input source_return;ep::Usage child_return;
    decltype(std::declval<const run_creation::Owner&>().facts()) creation_return;
    std::span<const authored_source::SafeDiagnostic> diagnostics_return; };
struct WorkCall { Work* work;Z amount,next;bool result; };
struct WorkCells { Work work;WorkCall caller,callee; };
struct ArithmeticCall { Z first,second;std::optional<Z> result; };
struct ArithmeticCells { Z first,second;std::optional<Z> result;Z* destination;bool success;ArithmeticCall caller,callee; };
struct ReserveCells { std::vector<Entry>* entries;Z old,next,limit;Entry* destination;Record* record;ep::Owner* source;
    ep::Owner* child;References refs;ps::Usage* usage;std::optional<U> marker; };
struct ErrorCells { Error error;std::unexpected<Error> failure;std::expected<void,Error> result;bool predicate; };
struct LookupCaller { const ps::Session* receiver;std::optional<U> marker;U sequence;const Entry* entry;const Record* record;Work* work;std::span<const Entry> entries;const Limits* limits;const Usage* before;Usage* output; };
struct PlanningRecordCall { std::span<const Entry> entries;U marker;Work* work;const Entry* entry;const Record* returned; };
struct RecordEmplace { Record argument; };
struct DeclaredResult { std::expected<Membership,Error> caller; };
struct PairBorrow { const Record* record;const ep::Owner* receiver;std::span<const ep::Pair> returned,caller; };
using MatrixCell=typename std::remove_cvref_t<decltype(std::declval<scenario::ExpandedMatrixExperiment>().cells)>::value_type;
struct MapRows { const ep::Pair* pair;const preflight::Scope* scope;const scenario::ResolvedComparisonExperiment* comparison;
    const scenario::ExpandedMatrixExperiment* matrix;std::array<const scenario::ResolvedComparisonVariant*,2> variants;
    std::array<const MatrixCell*,2> cells;std::array<SV,3> ids;const pp::Owner* plan;
    const comparison_experiments_serialization::Owner* comparisons;const matrix_experiments_serialization::Owner* matrices;
    const std::vector<scenario::ResolvedComparisonExperiment>* comparison_rows;const std::vector<scenario::ExpandedMatrixExperiment>* matrix_rows;
    std::span<const scenario::ResolvedComparisonVariant> variant_span;std::span<const MatrixCell> cell_span; };
struct MapCall { const detail::MapPreparation* preparation;const Record* record;std::span<const preflight::Scope> scopes;const Limits* limits;Usage* usage;
    std::expected<Membership,Error> result;std::unexpected<Error> failure;Error error; };
struct MapLocals { Membership staging;std::span<const ep::Pair> pairs;Z count,cursor,pass,pair_cursor,backing,peak;U ordinal;bool match;
    std::expected<Membership,Error> returned;
    decltype(std::declval<std::span<const ep::Pair>>().begin()) current,end; };
struct MapParent { const Record* record;U sequence;References refs;Usage before,after;Limits limits;std::optional<Membership> staging;
    std::expected<Membership,Error> result;Z bytes,peak;Membership argument;
    detail::MapPreparation prepared,construction_argument;std::optional<std::expected<detail::MapPreparation,Error>> preparation_return;
    std::expected<detail::MapPreparation,Error> callee_return;
    preflight::References preflight_refs; };
struct DeclaredCall { const Record* record;const Entry* entry;U sequence;References refs;std::optional<U> ordinal,normalization;
    std::optional<Membership> result;Membership argument;Usage before,after;Z work,peak;Error error;
    std::unexpected<Error> failure;std::expected<Membership,Error> returned;bool success; };
struct AssociationCall { std::span<const Entry> entries,argument;SV run;ps::Domain domain;U sequence;const ep::Owner* input;Work* work;
    References local,returned;const Roots* roots_receiver; };
}
namespace detail {
auto checked_add(Z left,Z right) noexcept->std::optional<Z> { if(right>SIZE_MAX-left)return {};return left+right; }
auto checked_multiply(Z left,Z right) noexcept->std::optional<Z> { if(left && right>SIZE_MAX/left)return {};return left*right; }
auto append_fixed_stock() noexcept->Z {
    return sizeof(AppendCall)+sizeof(AppendLocals)+sizeof(RootCells)+sizeof(ProvenanceCells)+sizeof(LookupCells)+
        sizeof(CompareCells)+sizeof(GetterCells)+sizeof(WorkCells)+sizeof(ArithmeticCells)+sizeof(ReserveCells)+sizeof(ErrorCells)+sizeof(AssociationCall)+sizeof(PairBorrow)+sizeof(RecordEmplace);
}
auto map_fixed_stock() noexcept->Z {
    return sizeof(MapCall)+sizeof(MapLocals)+2*sizeof(MapRows)+sizeof(MapParent)+sizeof(LookupCaller)+sizeof(LookupCells)+
        sizeof(PairBorrow)+sizeof(CompareCells)+sizeof(GetterCells)+sizeof(RootCells)+sizeof(WorkCells)+sizeof(ArithmeticCells)+sizeof(ErrorCells)+sizeof(PlanningRecordCall);
}
auto declared_fixed_stock() noexcept->Z {
    return sizeof(DeclaredCall)+sizeof(LookupCaller)+sizeof(LookupCells)+sizeof(PairBorrow)+sizeof(WorkCells)+sizeof(ArithmeticCells)+sizeof(ErrorCells)+sizeof(DeclaredResult)+sizeof(PlanningRecordCall);
}
auto prepare_map(std::span<const Entry> entries,U marker,preflight::References refs,const Limits& limits,const Usage& usage)
    ->std::expected<MapPreparation,Error> {
    try {
        require(map_fixed_stock()<=limits.association_preparation_bytes,Error::capacity);
        Work work{usage,limits};const auto& record=planning_record(entries,marker,work);
        work.debit();require(refs.controller==record.references.controller && refs.normalization==record.references.normalization &&
            refs.base_build==record.references.base_build,Error::reference);
        work.debit();const auto pairs=record.plan.pairs();
        const auto bytes=work.multiply(pairs.size(),sizeof(U));const auto peak=work.add(map_fixed_stock(),bytes);
        work.bound(peak,limits.association_preparation_bytes);
        work.bound(work.add(usage.scope_links,pairs.size()),limits.scope_links);
        work.bound(work.add(usage.association_bytes,bytes),limits.association_bytes);
        work.usage.association_preparation_bytes=std::max(usage.association_preparation_bytes,peak);
        return MapPreparation{&record,marker,record.references,work.usage,bytes,peak};
    } catch(Error error){return std::unexpected{error};}
}
auto finish_map(const MapPreparation& prepared,std::span<const preflight::Scope> scopes,const Limits& limits,Usage& output)
    ->std::expected<Membership,Error> {
    try {
        require(prepared.peak<=limits.association_preparation_bytes,Error::capacity);
        Work work{prepared.usage,limits};work.debit();require(prepared.record,Error::state);
        work.debit();const auto pairs=prepared.record->plan.pairs();
        work.debit();require(scopes.size()==pairs.size(),Error::membership);
        Membership result{prepared.sequence,{},{}};work.bound(pairs.size(),result.scope_pairs.max_size());
        result.scope_pairs.reserve(pairs.size());
        const auto r=roots(prepared.record->plan,work);
        work.debit();require(r.plan->comparison_experiments() && r.plan->matrix_experiments(),Error::state);
        const auto* comparisons=r.plan->comparison_experiments()->experiments();const auto* matrices=r.plan->matrix_experiments()->experiments();
        work.debit();require(comparisons && matrices,Error::state);
        Z cursor=0;
        for(Z pass=0;pass<2;++pass){
            for(const auto& pair:pairs){
                work.debit();if(pair.kind!=(pass==0?ep::PairKind::comparison:ep::PairKind::matrix))continue;
                work.debit();require(cursor<scopes.size(),Error::membership);const auto& scope=scopes[cursor];
                work.debit();require(scope.matrix==(pass!=0) && scope.experiment_index==pair.experiment_index &&
                    scope.baseline_index==pair.baseline_index && scope.candidate_index==pair.candidate_index &&
                    pair.ordinal<pairs.size() && pairs[pair.ordinal].ordinal==pair.ordinal,Error::membership);
                if(pass==0){
                    work.debit();require(pair.child_experiment_index<comparisons->size(),Error::membership);
                    const auto& row=(*comparisons)[pair.child_experiment_index];
                    work.debit();require(row.experiment_index==pair.experiment_index && pair.baseline_index<row.variants.size() && pair.candidate_index<row.variants.size(),Error::membership);
                    require(work.equal(scope.experiment_id,row.experiment_id) && work.equal(scope.baseline_id,row.variants[pair.baseline_index].variant_id) &&
                        work.equal(scope.candidate_id,row.variants[pair.candidate_index].variant_id),Error::membership);
                }else{
                    work.debit();require(pair.child_experiment_index<matrices->size(),Error::membership);
                    const auto& row=(*matrices)[pair.child_experiment_index];
                    work.debit();require(row.experiment_index==pair.experiment_index && pair.baseline_index<row.cells.size() && pair.candidate_index<row.cells.size(),Error::membership);
                    require(work.equal(scope.experiment_id,row.experiment_id) && work.equal(scope.baseline_id,row.cells[pair.baseline_index].id.memory_node.value()) &&
                        work.equal(scope.candidate_id,row.cells[pair.candidate_index].id.memory_node.value()),Error::membership);
                }
                work.debit();require(std::in_range<U>(pair.ordinal),Error::capacity);
                result.scope_pairs.push_back(static_cast<U>(pair.ordinal));++cursor;
            }
        }
        work.debit();require(cursor==pairs.size(),Error::membership);
        work.usage.scope_links=work.add(work.usage.scope_links,pairs.size());work.bound(work.usage.scope_links,limits.scope_links);
        work.usage.association_bytes=work.add(work.usage.association_bytes,prepared.backing);work.bound(work.usage.association_bytes,limits.association_bytes);
        output=work.usage;return result;
    }catch(Error error){return std::unexpected{error};}
}
auto declared(std::span<const Entry> entries,U marker,std::optional<U> ordinal,std::optional<U> normalization,
    const Limits& limits,Usage& output)->std::expected<Membership,Error> {
    try {
        require(declared_fixed_stock()<=limits.association_preparation_bytes,Error::capacity);
        Work work{output,limits};const auto& record=planning_record(entries,marker,work);
        work.debit();require(normalization && *normalization==record.references.normalization,Error::reference);
        work.debit();const auto pairs=record.plan.pairs();
        if(ordinal){work.debit();require(*ordinal<pairs.size() && pairs[static_cast<Z>(*ordinal)].ordinal==*ordinal,Error::membership);}
        work.usage.association_preparation_bytes=std::max(work.usage.association_preparation_bytes,declared_fixed_stock());
        Membership result{marker,ordinal,{}};output=work.usage;return result;
    }catch(Error error){return std::unexpected{error};}
}
}
} // namespace epycbench::results::planning

namespace epycbench::results::process_subject {
auto Session::append_planning(std::uint64_t sequence,const journal::Timestamp& timestamp,execution_plan::Owner& incoming)
    ->std::expected<void,planning::Error> {
    using namespace planning;using namespace planning::detail;
    if(!std::exchange(live_,false))return std::unexpected{Error::state};
    try {
        require(append_fixed_stock()<=limits_.planning.planning_simultaneous_bytes,Error::capacity);
        Work work{usage_.planning,limits_.planning};
        work.debit();require(present_ && !planning_sequence_ && normalization_sequence_ && base_build_sequence_ && !source_outcome_sequence_ &&
            !preflight_sequence_ && !preflight_blocked_ && pairs_.empty() && !entries_.empty() && sequence>entries_.back().sequence && timestamp.text().size()==30U,Error::reference);
        const auto child=incoming.usage();
        work.debit();require(incoming.digest() && child.retained_bytes>=sizeof(execution_plan::Owner),Error::state);
        const auto old_capacity=entries_.capacity();
        const auto spare=work.multiply(old_capacity-entries_.size(),sizeof(Entry));
        auto peak=work.add(work.add(work.add(usage_.retained_bytes,spare),child.retained_bytes),append_fixed_stock());
        work.bound(peak,limits_.planning.planning_simultaneous_bytes);
        for(Z entry_cursor=0;entry_cursor<entries_.size();++entry_cursor){work.debit();require(!entries_[entry_cursor].lifecycle,Error::reference);}
        const auto refs=associate(entries_,run_,domain_,sequence,incoming,work);
        work.debit();require(refs.normalization==*normalization_sequence_ && refs.base_build==*base_build_sequence_,Error::reference);
        work.usage.records=work.add(work.usage.records,1);work.bound(work.usage.records,limits_.planning.records);
        work.usage.pairs=work.add(work.usage.pairs,incoming.pairs().size());work.bound(work.usage.pairs,limits_.planning.pairs);
        work.usage.child_retained_bytes=work.add(work.usage.child_retained_bytes,child.retained_bytes);work.bound(work.usage.child_retained_bytes,limits_.planning.child_retained_bytes);
        work.usage.canonical_bytes=work.add(work.usage.canonical_bytes,incoming.canonical().size());work.bound(work.usage.canonical_bytes,limits_.planning.canonical_bytes);
        auto next=usage_;
        next.entries=work.add(next.entries,1);work.bound(next.entries,limits_.entries);work.bound(next.entries,entries_.max_size());
        next.timestamp_bytes=work.add(next.timestamp_bytes,timestamp.text().size());work.bound(next.timestamp_bytes,limits_.timestamp_bytes);
        next.canonical_child_bytes=work.add(next.canonical_child_bytes,incoming.canonical().size());work.bound(next.canonical_child_bytes,limits_.canonical_child_bytes);
        next.retained_bytes=work.add(work.add(work.add(next.retained_bytes,sizeof(Entry)),timestamp.text().size()),child.retained_bytes-sizeof(execution_plan::Owner));
        work.bound(next.retained_bytes,limits_.retained_bytes);
        const bool growth=entries_.size()==old_capacity;
        const auto new_capacity=!growth?old_capacity:old_capacity==0?Z{1}:old_capacity>limits_.entries/2?limits_.entries:work.multiply(old_capacity,2);
        work.bound(new_capacity,entries_.max_size());
        peak=work.add(peak,timestamp.text().size());if(growth)peak=work.add(peak,work.multiply(new_capacity,sizeof(Entry)));
        work.bound(peak,limits_.planning.planning_simultaneous_bytes);
        work.usage.planning_simultaneous_bytes=std::max(work.usage.planning_simultaneous_bytes,peak);next.planning=work.usage;
        Entry prepared{sequence,timestamp.text(),Producer::controller,NoCreation{},{},{},{},{},{},{}};
        if(growth)entries_.reserve(new_capacity);
        static_assert(std::is_nothrow_move_constructible_v<Entry>);
        prepared.planning.emplace(Record{refs,std::move(incoming)});
        entries_.push_back(std::move(prepared));planning_sequence_=sequence;usage_=next;live_=true;return {};
    }catch(Error error){return std::unexpected{error};}
}
} // namespace epycbench::results::process_subject
