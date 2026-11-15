#include "results/process_subject_admission.hpp"
#include <algorithm>
#include <array>
#include <numeric>
#include <charconv>
#include <limits>
#include <type_traits>
#include <utility>
namespace epycbench::results::process_subject {
namespace {
constexpr std::size_t maximum_entries=1024;
auto valid(Domain value) noexcept -> bool {
    return value==Domain::supplied_observation || value==Domain::synthetic_execution;
}
auto add(std::size_t& value,std::size_t amount) noexcept -> bool {
    if(amount>std::numeric_limits<std::size_t>::max()-value)return false;
    value+=amount;return true;
}
auto failure(const char* suffix) -> core::Error {
    return {.category=core::ErrorCategory::results,
        .code=core::StableErrorCode{"results.process_subject."+std::string{suffix}},
        .message="Supplied process subject rejected",.operation="admit_process_subject",
        .os_error=std::nullopt,.context=nlohmann::json::object()};
}
auto reference(std::string_view text,std::string_view run) noexcept -> std::optional<std::uint64_t> {
    if(text.size()<=run.size()+1 || !text.starts_with(run) || text[run.size()]!=':')return {};
    text.remove_prefix(run.size()+1);if(text.front()=='0')return {};
    std::uint64_t value{};auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size())return {};
    return value;
}
auto role(const Creation& creation) noexcept -> std::optional<process_thp_restriction::Role> {
    if(std::holds_alternative<ControllerCreated>(creation))return process_thp_restriction::Role::controller;
    if(std::holds_alternative<RunnerCreated>(creation))return process_thp_restriction::Role::runner;
    return {};
}
template<class T> void reserve_next(std::vector<T>& values,std::size_t limit) {
    if(values.size()<values.capacity())return;
    const auto capacity=values.capacity();
    const auto next=capacity==0?std::size_t{1}:capacity>limit/2?limit:capacity*2;
    values.reserve(next);
}
auto valid(Producer p) noexcept -> bool { return p==Producer::controller || p==Producer::runner || p==Producer::results; }
auto valid_target_impl(const Target& t,const FailureFact& fact) noexcept -> bool {
    if(t.category==Category::preflight_temperature)
        return t.selector==Selector::cpu_control_observation && t.ordinal==0U &&
            t.reason>=FailureReason::read_failed && t.reason<=FailureReason::unsupported_value &&
            fact.producer==Producer::controller && !fact.process_sequence;
    if(t.category==Category::base_build)
        return t.selector==Selector::base_build_acquisition && t.ordinal==0 && t.key==0 && t.reason==FailureReason::none &&
            !fact.process_sequence && fact.producer==Producer::controller;
    if(t.category==Category::authored_source)
        return t.selector==Selector::source_acquisition && t.ordinal==0 && t.key==0 && t.reason==FailureReason::none &&
            !fact.process_sequence && fact.producer==Producer::controller;
    if(t.category<Category::topology || t.category>Category::process_thp)return false;
    if(t.selector==Selector::owner_preparation)
        return t.ordinal==0 && t.key==0 && t.reason==FailureReason::none && !fact.process_sequence;
    if(t.reason<FailureReason::read_failed || t.reason>FailureReason::unsupported_value)return false;
    if(t.category==Category::process_thp)
        return t.selector==Selector::process_observation && fact.process_sequence && t.key==*fact.process_sequence;
    if(fact.process_sequence)return false;
    switch(t.category){
    case Category::system_controls:return t.selector>=Selector::smt_control && t.selector<=Selector::thp_defrag && t.key==0;
    case Category::kernel_identity:return t.selector>=Selector::sysname && t.selector<=Selector::version && t.key==0;
    case Category::pu_identity:return (t.selector==Selector::signature || t.selector==Selector::microcode) && t.key<=UINT32_MAX;
    case Category::frequency:return t.selector==Selector::uncovered_cpu && t.key<=UINT32_MAX;
    case Category::anonymous_thp:return (t.selector==Selector::enumeration && t.key==0) ||
        (t.selector==Selector::page_size && t.key>=1024 && (t.key&(t.key-1))==0);
    default:return false;
    }
}
auto find_entry(std::span<const Entry> entries,std::uint64_t sequence) noexcept -> const Entry* {
    const auto found=std::lower_bound(entries.begin(),entries.end(),sequence,[](const Entry& e,std::uint64_t s){return e.sequence<s;});
    return found!=entries.end() && found->sequence==sequence?&*found:nullptr;
}
auto entry_host(const Entry* entry) noexcept -> const host_assembly::Owner* {
    if (!entry) return nullptr;
    if (entry->run_creation) {
        const auto* facts = entry->run_creation->facts();
        return !entry->host && facts && facts->host ? &*facts->host : nullptr;
    }
    return entry->host ? &*entry->host : nullptr;
}
auto entry_base_build(const Entry* entry) noexcept -> const BaseBuild* {
    if (!entry) return nullptr;
    if (entry->run_creation) {
        const auto* facts = entry->run_creation->facts();
        return !entry->base_build && facts && facts->build ? &*facts->build : nullptr;
    }
    return entry->base_build ? &*entry->base_build : nullptr;
}
auto addressed_failure(const Entry* entry, std::optional<std::uint64_t> ordinal) noexcept -> const FailureFact* {
    if (!entry) return nullptr;
    if (entry->run_creation) return ordinal ? entry->run_creation->failure_at(*ordinal) : nullptr;
    return !ordinal && entry->failure ? &*entry->failure : nullptr;
}
auto has_target(const FailureFact& fact,std::span<const std::size_t> index,const Target& target) noexcept -> bool {
    const auto found=std::lower_bound(index.begin(),index.end(),target,[&](std::size_t i,const Target& t){return fact.targets[i]<t;});
    return found!=index.end() && fact.targets[*found]==target;
}
auto targeted_failure(const Entry* entry, const Target& target) noexcept -> const FailureFact* {
    if (!entry) return nullptr;
    if (entry->run_creation) return entry->run_creation->failure_for(target);
    return entry->failure && has_target(*entry->failure, entry->target_index, target) ? &*entry->failure : nullptr;
}
template<class R> auto reason(R r) noexcept -> FailureReason {
    if(r==R::read_failed)return FailureReason::read_failed;
    if(r==R::malformed_source)return FailureReason::malformed_source;
    if(r==R::unsupported_value)return FailureReason::unsupported_value;
    return FailureReason::none;
}
template<class P> auto producer(P p) noexcept -> Producer {
    return p==P::controller?Producer::controller:Producer::runner;
}
auto host_failures(const host_assembly::Categories& categories,const Input& input,std::string_view run,
    std::span<const Entry> entries,std::span<const std::size_t> incoming_index,
    const run_creation::Input* genesis = nullptr, const run_creation::Owner* candidate = nullptr) noexcept -> bool {
    auto match=[&](std::uint64_t sequence,const Target& target,std::optional<Producer> source)->bool {
        const FailureFact* fact{};
        if (sequence == input.sequence) {
            if (genesis && candidate && candidate->facts())
                fact = run_creation::detail::bound_failure(genesis->failures, genesis->bindings,
                    candidate->facts()->binding_index, target);
            else if (input.failure && has_target(*input.failure, incoming_index, target)) fact = &*input.failure;
        } else if (sequence < input.sequence) fact = targeted_failure(find_entry(entries, sequence), target);
        return fact && (!source || fact->producer == *source);
    };
    auto outer=[&](const auto& slot,Category category)->bool {
        if(const auto* failed=std::get_if<host_assembly::Failed>(&slot))return match(failed->failure_event_sequence,{category},{ });
        return true;
    };
    if(!outer(categories.topology,Category::topology) || !outer(categories.system_controls,Category::system_controls) ||
       !outer(categories.kernel_identity,Category::kernel_identity) || !outer(categories.pu_identity,Category::pu_identity) ||
       !outer(categories.frequency,Category::frequency) || !outer(categories.anonymous_thp,Category::anonymous_thp) ||
       !outer(categories.process_thp,Category::process_thp))return false;
    auto observation=[&](const auto& owner,const auto& observed,Category category,Selector selector,std::uint64_t key)->bool {
        if(!observed.failure_event)return true;
        const auto seq=reference(*observed.failure_event,run);
        return seq && owner.producer() && owner.ordinal() && observed.reason &&
            match(*seq,{category,selector,*owner.ordinal(),key,reason(*observed.reason)},producer(*owner.producer()));
    };
    if(const auto* owner=std::get_if<host_controls::Owner>(&categories.system_controls)){
        constexpr std::array selectors{Selector::smt_control,Selector::smt_active,Selector::numa_mask,Selector::thp_enabled,Selector::thp_defrag};
        std::size_t i{};for(const auto& o:owner->observations())if(!observation(*owner,o,Category::system_controls,selectors[i++],0))return false;
    }
    if(const auto* owner=std::get_if<private_kernel_identity::Owner>(&categories.kernel_identity)){
        constexpr std::array selectors{Selector::sysname,Selector::release,Selector::version};
        std::size_t i{};for(const auto& o:owner->observations())if(!observation(*owner,o,Category::kernel_identity,selectors[i++],0))return false;
    }
    if(const auto* owner=std::get_if<pu_cpu_identity::Owner>(&categories.pu_identity))for(const auto& row:owner->rows())
        if(!observation(*owner,row.signature,Category::pu_identity,Selector::signature,row.linux_cpu.value()) ||
           !observation(*owner,row.microcode,Category::pu_identity,Selector::microcode,row.linux_cpu.value()))return false;
    if(const auto* owner=std::get_if<frequency_policy_owner::Owner>(&categories.frequency))for(const auto& row:owner->uncovered_cpus())
        if(!observation(*owner,row,Category::frequency,Selector::uncovered_cpu,row.association.linux_cpu.value()))return false;
    if(const auto* owner=std::get_if<anonymous_thp_policy_owner::Owner>(&categories.anonymous_thp)){
        if(!observation(*owner,*owner->enumeration(),Category::anonymous_thp,Selector::enumeration,0))return false;
        for(const auto& row:owner->rows())if(!observation(*owner,row,Category::anonymous_thp,Selector::page_size,row.size.value()))return false;
    }
    if(const auto* owner=std::get_if<process_thp_restriction::Owner>(&categories.process_thp)){
        const auto seq=reference(owner->subject()->event_id,run);
        if(!seq || !observation(*owner,*owner->observation(),Category::process_thp,Selector::process_observation,*seq))return false;
    }
    return true;
}

static_assert(std::is_nothrow_move_constructible_v<authored_source::ResolutionRecord>);
static_assert(std::is_nothrow_move_assignable_v<authored_source::ResolutionRecord>);
static_assert(std::is_nothrow_move_constructible_v<Entry>);
static_assert(std::is_nothrow_move_constructible_v<BaseBuild>);
static_assert(std::is_nothrow_move_assignable_v<BaseBuild>);
static_assert(std::is_nothrow_move_assignable_v<Entry>);
// Caller Usage and helper copy coexist; argument temporaries are inventoried below.
struct AddStockCells {
    Usage* usage_parameter;
    Usage next;
    std::size_t timestamp, canonical, host_stock, index_size;
    bool has_host, runner, returned;
    std::size_t* add_value_parameter;
    std::size_t add_amount;
    bool add_returned;
};
struct CreationSessionCells {
    run_creation::Input* input;
    const journal::Timestamp* timestamp;
    std::uint64_t sequence;
    run_creation::Limits remaining;
    Usage prior, base, next;
    AddStockCells add_stock;
    run_creation::Usage child, child_return;
    std::optional<std::expected<run_creation::Owner, run_creation::Error>> candidate, candidate_return;
    std::optional<Entry> prepared;
    std::optional<Input> association;
    std::optional<core::Error> error;
    std::expected<void, core::Error> result, result_return;
    const process_thp_restriction::Owner* process;
    std::size_t old_capacity, new_capacity, session_stock, owner_allowance;
};
struct CreationAppendCells {
    Input* input;
    Usage next;
    AddStockCells add_stock;
    host_assembly::Usage host_canonical_return, host_retained_return;
    std::vector<std::size_t> target_index;
    std::optional<Entry> prepared;
    std::optional<ResolvedSubject> resolved;
    const RunnerCreated* runner;
    const Normalization* normalization;
    const authored_source::ResolutionRecord* source;
    const BaseBuild* build;
    const FailureFact *fact, *primary, *selected;
    const Entry *entry, *lookup_entry;
    const host_assembly::Owner* host;
    const topology_evidence::Owner* topology;
    const run_creation::detail::State* genesis;
    const authored_source::SourceBundle* original;
    const authored_source::SourceBundle *comparison_left, *comparison_right;
    authored_source::Input supplied, left, right;
    authored_source::Usage source_stock, source_return;
    build_manifest::Usage build_stock, build_return;
    private_error::Usage error_stock, error_return;
    std::span<const std::size_t> index;
    std::span<const Entry> entries;
    Target target, target_parameter;
    std::optional<std::uint64_t> sequence, primary_ordinal;
    std::optional<Producer> source_producer;
    std::optional<core::Error> error;
    std::expected<void, core::Error> result, result_return;
    std::size_t pair_position, input_stock, index_stock, peak, old_capacity, new_capacity,
        pair_capacity, new_pair_capacity, target_position, override_position;
};
// Fixed helper frames are logical allowances, not a compiler stack measurement.
// The existing shared Host traversal is included when it is called by the new mode.
struct CreationReferenceCells {
    const host_assembly::Categories* categories;
    const Input* input;
    std::string_view run;
    std::span<const Entry> entries;
    std::span<const std::size_t> incoming_index;
    const run_creation::Input* genesis;
    const run_creation::Owner* candidate;
    struct MatchCapture {
        const Input* input; const std::string_view* run; const std::span<const Entry>* entries;
        const std::span<const std::size_t>* index; const run_creation::Input* const* genesis;
        const run_creation::Owner* const* candidate;
    } match;
    const MatchCapture *outer_capture, *observation_capture;
    const std::string_view* observation_run;
    const void *slot, *child, *observation, *row, *outer_failure;
    const host_controls::Owner* controls;
    const private_kernel_identity::Owner* kernel;
    const pu_cpu_identity::Owner* pu;
    const frequency_policy_owner::Owner* frequency;
    const anonymous_thp_policy_owner::Owner* anonymous;
    const process_thp_restriction::Owner* process;
    std::array<Selector,5> control_selectors;
    std::array<Selector,3> kernel_selectors;
    Category category;
    Selector selector;
    FailureReason reason;
    Producer producer;
    std::size_t index;
    std::uint64_t key, sequence, parsed_sequence;
    std::optional<std::uint64_t> parsed, parsed_return, ordinal;
    std::optional<Producer> source;
    Target target, target_parameter;
    const Target* borrowed_target;
    const Entry *entry, *entry_parameter;
    const FailureFact *fact, *fact_return;
    std::span<const FailureFact> facts;
    std::span<const run_creation::Binding> bindings;
    std::span<const std::size_t> lookup_index;
    std::span<const std::size_t>::iterator found;
    std::string_view text_parameter, run_parameter;
    std::from_chars_result parsed_chars;
};
struct CreationLookupCells {
    const Entry* entry;
    const run_creation::Owner* owner;
    const run_creation::detail::State* state;
    const FailureFact *selected, *returned;
    std::optional<std::uint64_t> ordinal;
    Target target;
    const Target* parameter;
    std::span<const FailureFact> failures;
    std::span<const run_creation::Binding> bindings;
    std::span<const std::size_t> index;
    const std::span<const run_creation::Binding>* comparator;
    std::span<const std::size_t>::iterator found;
};
auto next_capacity(std::size_t size, std::size_t capacity, std::size_t limit) noexcept -> std::size_t {
    return size < capacity ? capacity : capacity == 0U ? 1U : capacity > limit / 2U ? limit : capacity * 2U;
}
auto creation_failure(run_creation::Error error) -> core::Error {
    const char* suffix = "invalid_input";
    switch (error) {
    case run_creation::Error::state: suffix = "state"; break;
    case run_creation::Error::identity: suffix = "identity"; break;
    case run_creation::Error::reference: suffix = "reference"; break;
    case run_creation::Error::capacity: suffix = "capacity"; break;
    case run_creation::Error::invalid_input: break;
    }
    return {.category = core::ErrorCategory::results,
        .code = core::StableErrorCode{"results.run_creation." + std::string{suffix}},
        .message = "Supplied creation artifacts rejected", .operation = "admit_run_creation",
        .os_error = std::nullopt, .context = nullptr};
}
}
auto detail::creation_session_fixed_stock() noexcept -> std::size_t { return sizeof(CreationSessionCells) + sizeof(CreationReferenceCells); }
auto detail::creation_append_fixed_stock() noexcept -> std::size_t { return sizeof(CreationAppendCells) + sizeof(CreationReferenceCells); }
auto detail::creation_lookup_fixed_stock() noexcept -> std::size_t { return sizeof(CreationLookupCells); }
auto detail::valid_target(const Target& target, const FailureFact& fact) noexcept -> bool {
    return valid_target_impl(target, fact);
}
auto detail::add_stock(Usage& usage,std::size_t timestamp,std::size_t canonical,
    std::size_t host_stock,bool has_host,bool runner,std::size_t index_size) noexcept -> bool {
    auto next=usage;
    if(has_host && host_stock<sizeof(host_assembly::Owner))return false;
    if(!add(next.entries,1) || !add(next.timestamp_bytes,timestamp) || !add(next.canonical_child_bytes,canonical) ||
       !add(next.retained_bytes,sizeof(Entry)) || !add(next.retained_bytes,timestamp) ||
       (has_host && !add(next.retained_bytes,host_stock-sizeof(host_assembly::Owner))) ||
       (runner && (!add(next.runner_instances,1) || !add(next.retained_bytes,index_size))))return false;
    usage=next;return true;
}
auto Session::create(const journal::RunId& run,Domain domain,Limits limits) -> std::expected<Session,core::Error> {
    if(run.text().empty() || !valid(domain))return std::unexpected{failure("invalid_input")};
    if(limits.entries>maximum_entries || limits.runner_instances>limits.entries || limits.failure_facts>maximum_entries || limits.targets>65536 || limits.normalizations>1 || limits.source_outcomes>1 || limits.base_builds>1 || limits.preflight.records>1 || limits.planning.records>1 || limits.run_creation.records>1 || limits.run_creation.failure_facts>1024 || limits.run_creation.targets>65536 || limits.run_creation.bindings>65536)return std::unexpected{failure("capacity")};
    Usage usage{};usage.retained_bytes=sizeof(Session);
    if(!add(usage.retained_bytes,run.text().size()) || usage.retained_bytes>limits.retained_bytes)return std::unexpected{failure("capacity")};
    Session result;result.run_=run.text();result.domain_=domain;result.limits_=limits;result.usage_=usage;return result;
}
Session::Session(Session&& other) noexcept : present_(std::exchange(other.present_,false)),live_(std::exchange(other.live_,false)),
    run_(std::move(other.run_)),domain_(other.domain_),limits_(other.limits_),usage_(std::exchange(other.usage_,{})),
    entries_(std::move(other.entries_)),pairs_(std::move(other.pairs_)),normalization_sequence_(std::exchange(other.normalization_sequence_,{})),source_outcome_sequence_(std::exchange(other.source_outcome_sequence_,{})),base_build_sequence_(std::exchange(other.base_build_sequence_,{})),
    preflight_sequence_(std::exchange(other.preflight_sequence_,{})),planning_sequence_(std::exchange(other.planning_sequence_,{})),preflight_blocked_(std::exchange(other.preflight_blocked_,false)) {}
auto Session::operator=(Session&& other) noexcept -> Session& {
    if(this!=&other){present_=std::exchange(other.present_,false);live_=std::exchange(other.live_,false);
        run_=std::move(other.run_);domain_=other.domain_;limits_=other.limits_;usage_=std::exchange(other.usage_,{});
        entries_=std::move(other.entries_);pairs_=std::move(other.pairs_);normalization_sequence_=std::exchange(other.normalization_sequence_,{});source_outcome_sequence_=std::exchange(other.source_outcome_sequence_,{});base_build_sequence_=std::exchange(other.base_build_sequence_,{});
        preflight_sequence_=std::exchange(other.preflight_sequence_,{});planning_sequence_=std::exchange(other.planning_sequence_,{});preflight_blocked_=std::exchange(other.preflight_blocked_,false);}return *this;
}
auto Session::append_creation(std::uint64_t sequence, const journal::Timestamp& timestamp,
    run_creation::Input& input) -> std::expected<void, core::Error> {
    using E = run_creation::Error;
    if (!std::exchange(live_, false)) return std::unexpected{creation_failure(E::state)};
    if (!entries_.empty() || sequence != 1U) return std::unexpected{creation_failure(E::reference)};
    if (!valid(input.domain) || input.producer != Producer::controller || timestamp.text().empty())
        return std::unexpected{creation_failure(E::invalid_input)};
    if (input.domain != domain_ || input.run.text() != run_) return std::unexpected{creation_failure(E::identity)};
    auto base = usage_;
    if (!detail::add_stock(base, timestamp.text().size(), 0U, 0U, false, false, 0U) ||
        base.entries > limits_.entries || base.timestamp_bytes > limits_.timestamp_bytes ||
        base.retained_bytes > limits_.retained_bytes || limits_.base_builds == 0U)
        return std::unexpected{creation_failure(E::capacity)};
    auto remaining = limits_.run_creation;
    remaining.failure_facts = std::min(remaining.failure_facts, limits_.failure_facts);
    remaining.targets = std::min(remaining.targets, limits_.targets);
    remaining.error_context_nodes = std::min(remaining.error_context_nodes, limits_.context_nodes);
    remaining.error_string_bytes = std::min(remaining.error_string_bytes, limits_.private_string_bytes);
    remaining.error_binary_bytes = std::min(remaining.error_binary_bytes, limits_.private_binary_bytes);
    remaining.canonical_child_bytes = std::min(remaining.canonical_child_bytes, limits_.canonical_child_bytes);
    auto owner_allowance = limits_.retained_bytes - base.retained_bytes;
    if (!add(owner_allowance, sizeof(run_creation::Owner))) return std::unexpected{creation_failure(E::capacity)};
    remaining.retained_bytes = std::min(remaining.retained_bytes, owner_allowance);
    const auto old_capacity = entries_.capacity();
    const auto new_capacity = next_capacity(entries_.size(), old_capacity, limits_.entries);
    std::size_t session_stock = detail::creation_session_fixed_stock() + sizeof(journal::Timestamp);
    if (!add(session_stock, timestamp.text().size()) || !add(session_stock, timestamp.text().size()) ||
        !run_creation::detail::elements(session_stock, old_capacity, sizeof(Entry)) ||
        !run_creation::detail::elements(session_stock, new_capacity, sizeof(Entry)))
        return std::unexpected{creation_failure(E::capacity)};
    auto candidate = run_creation::Owner::prepare(input, remaining, usage_.retained_bytes, session_stock);
    if (!candidate) return std::unexpected{creation_failure(candidate.error())};
    Input association{domain_, sequence, timestamp, Producer::controller, ControllerCreated{}, {}};
    if (const auto* process = std::get_if<process_thp_restriction::Owner>(&input.host.categories()->process_thp))
        if (!process->subject() || process->subject()->role != process_thp_restriction::Role::controller ||
            reference(process->subject()->event_id, run_) != 1U) return std::unexpected{creation_failure(E::reference)};
    if (!host_failures(*input.host.categories(), association, run_, {}, {}, &input, &*candidate))
        return std::unexpected{creation_failure(E::reference)};
    const auto child = candidate->usage();
    auto next = base;
    if (child.retained_bytes < sizeof(run_creation::Owner) ||
        !add(next.retained_bytes, child.retained_bytes - sizeof(run_creation::Owner)) ||
        !add(next.canonical_child_bytes, child.canonical_child_bytes) ||
        !add(next.failure_facts, child.failure_facts) || !add(next.targets, child.targets) ||
        !add(next.context_nodes, child.error_context_nodes) ||
        !add(next.private_string_bytes, child.error_string_bytes) || !add(next.private_binary_bytes, child.error_binary_bytes))
        return std::unexpected{creation_failure(E::capacity)};
    next.base_builds = 1U;
    next.run_creation = child;
    Entry prepared{sequence, timestamp.text(), Producer::controller, ControllerCreated{}, {}, {}, {}, {}, {}, {}};
    if (std::holds_alternative<process_thp_restriction::Owner>(input.host.categories()->process_thp))
        prepared.resolved_subject = ResolvedSubject{process_thp_restriction::Role::controller, 1U};
    reserve_next(entries_, limits_.entries);
    candidate->commit(input);
    prepared.run_creation.emplace(std::move(*candidate));
    entries_.push_back(std::move(prepared));
    base_build_sequence_ = 1U;
    usage_ = next;
    live_ = true;
    return {};
}
auto Session::append(Input& input) -> std::expected<void,core::Error> {
    if(!std::exchange(live_,false))return std::unexpected{failure("invalid_state")};
    if(preflight_blocked_ && (!input.failure || input.host || input.normalization || input.source_outcome ||
        input.base_build || !std::holds_alternative<NoCreation>(input.creation)))
        return std::unexpected{failure("reference")};
    if(input.domain!=domain_ || !valid(input.domain) || input.sequence==0 || input.timestamp.text().empty() ||
       (input.producer!=Producer::controller && input.producer!=Producer::runner && input.producer!=Producer::results) || input.creation.valueless_by_exception())return std::unexpected{failure("invalid_input")};
    if(!std::holds_alternative<NoCreation>(input.creation) && input.producer!=Producer::controller)return std::unexpected{failure("invalid_input")};
    if((!entries_.empty() && input.sequence<=entries_.back().sequence) ||
       (entries_.empty() && (input.sequence!=1 || !std::holds_alternative<ControllerCreated>(input.creation))) ||
       (!entries_.empty() && std::holds_alternative<ControllerCreated>(input.creation)) ||
       (std::holds_alternative<NoCreation>(input.creation) && !input.host && !input.failure && !input.normalization && !input.source_outcome && !input.base_build))return std::unexpected{failure("reference")};
    const auto* runner=std::get_if<RunnerCreated>(&input.creation);
    std::size_t pair_position{};
    if(runner){
        if(base_build_sequence_ && (!entry_base_build(find_entry(entries_,*base_build_sequence_)) ||
            entry_base_build(find_entry(entries_,*base_build_sequence_))->state!=BuildState::available))
            return std::unexpected{failure("reference")};
        if(!entries_.empty() && entries_.front().run_creation && !normalization_sequence_)return std::unexpected{failure("reference")};
        if(source_outcome_sequence_)return std::unexpected{failure("reference")};
        if(entries_.empty() || runner->controller_sequence!=1 || !std::holds_alternative<ControllerCreated>(entries_.front().creation))
            return std::unexpected{failure("reference")};
        const auto found=std::lower_bound(pairs_.begin(),pairs_.end(),runner->pair_ordinal);
        if(found!=pairs_.end() && *found==runner->pair_ordinal)return std::unexpected{failure("reference")};
        pair_position=static_cast<std::size_t>(found-pairs_.begin());
    }
    if(input.host && (!input.host->categories() || input.host->run_id()!=run_))return std::unexpected{failure("reference")};
    auto next=usage_;
    if(!detail::add_stock(next,input.timestamp.text().size(),input.host?input.host->usage().canonical_bytes:0,
         input.host?input.host->usage().retained_bytes:0,input.host.has_value(),runner!=nullptr,sizeof(std::uint64_t)) ||
       next.entries>limits_.entries || next.runner_instances>limits_.runner_instances || next.timestamp_bytes>limits_.timestamp_bytes ||
       next.canonical_child_bytes>limits_.canonical_child_bytes || next.retained_bytes>limits_.retained_bytes)return std::unexpected{failure("capacity")};
    if(input.normalization){
        const auto& normalization=*input.normalization;
        const auto& source=normalization.source;
        if(entries_.empty() || normalization_sequence_ || source_outcome_sequence_ || input.source_outcome || !pairs_.empty() || input.producer!=Producer::controller ||
           !std::holds_alternative<NoCreation>(input.creation) || !source.source() || source.state()!=authored_source::State::resolved || !source.plan() || source.upstream_error())
            return std::unexpected{failure("reference")};
        const auto supplied=source.source()->input();
        if(supplied.availability!=authored_source::Availability::available || supplied.acquisition_failure || !supplied.rejected_overrides.empty())
            return std::unexpected{failure("reference")};
        const auto sequence=normalization.topology_event_sequence;
        const host_assembly::Owner* host{};
        if(sequence==input.sequence && input.host)host=&*input.host;
        else if(sequence<input.sequence)host=entry_host(find_entry(entries_,sequence));
        if(!sequence || !host || !host->categories() || host->run_id()!=run_)return std::unexpected{failure("reference")};
        const auto* topology=std::get_if<topology_evidence::Owner>(&host->categories()->topology);
        const auto& fingerprints=source.plan()->matrix_expanded().kernel_dispatched().memory_placement_resolved()
            .service_cpu_resolved().resource_resolved().topology_fingerprints();
        if(!topology || !topology->fingerprints() || *topology->fingerprints()!=fingerprints)return std::unexpected{failure("reference")};
        const auto stock=source.usage();
        if(stock.bytes<sizeof(authored_source::ResolutionRecord) || !add(next.normalizations,1) || !add(next.source_nodes,stock.nodes) ||
           !add(next.source_canonical_bytes,stock.canonical_bytes) || !add(next.retained_bytes,stock.bytes-sizeof(authored_source::ResolutionRecord)) ||
           next.normalizations>limits_.normalizations || next.source_nodes>limits_.source_nodes || next.source_canonical_bytes>limits_.source_canonical_bytes ||
           next.retained_bytes>limits_.retained_bytes)return std::unexpected{failure("capacity")};
    }
    if(input.source_outcome){
        const auto& source=input.source_outcome->source;
        if(normalization_sequence_ || source_outcome_sequence_ || !pairs_.empty() || input.producer!=Producer::controller ||
           runner || !source.source() || source.plan() || source.state()==authored_source::State::resolved)return std::unexpected{failure("reference")};
        const auto* upstream=source.upstream_error();
        if((source.state()==authored_source::State::resolution_failed)!=(upstream!=nullptr) ||
           (upstream && !upstream->source()))return std::unexpected{failure("reference")};
        const auto stock=source.usage();
        if(stock.bytes<sizeof(authored_source::ResolutionRecord) || !add(next.source_outcomes,1) || !add(next.source_nodes,stock.nodes) ||
           !add(next.source_canonical_bytes,stock.canonical_bytes) || !add(next.retained_bytes,stock.bytes-sizeof(authored_source::ResolutionRecord)) ||
           next.source_outcomes>limits_.source_outcomes || next.source_nodes>limits_.source_nodes || next.source_canonical_bytes>limits_.source_canonical_bytes ||
           next.retained_bytes>limits_.retained_bytes)return std::unexpected{failure("capacity")};
    }
    if(input.base_build){
        const auto& build=*input.base_build;
        if(base_build_sequence_ || !pairs_.empty() || runner || input.producer!=Producer::controller ||
           (build.state!=BuildState::not_attempted && build.state!=BuildState::not_obtained && build.state!=BuildState::available) ||
           ((build.state==BuildState::available)!=build.manifest.has_value()) ||
           ((build.state==BuildState::not_obtained)!=build.acquisition_failure.has_value()) ||
           (build.manifest && (!build.manifest->input() || !build.manifest->digest())))return std::unexpected{failure("reference")};
        if(!add(next.base_builds,1) || next.base_builds>limits_.base_builds)return std::unexpected{failure("capacity")};
        if(build.manifest){
            const auto stock=build.manifest->usage();
            if(stock.total_bytes<sizeof(build_manifest::Owner) ||
               !add(next.retained_bytes,stock.total_bytes-sizeof(build_manifest::Owner)) ||
               !add(next.canonical_child_bytes,stock.canonical_bytes) || next.retained_bytes>limits_.retained_bytes ||
               next.canonical_child_bytes>limits_.canonical_child_bytes)return std::unexpected{failure("capacity")};
        }
    }
    if (!entries_.empty() && entries_.front().run_creation && (input.normalization || input.source_outcome)) {
        const auto* genesis = entries_.front().run_creation->facts();
        const auto* source = input.normalization ? input.normalization->source.source() : input.source_outcome->source.source();
        if (!genesis || !genesis->source || !source ||
            !run_creation::detail::same_source(*genesis->source, *source)) return std::unexpected{failure("reference")};
    }
    std::vector<std::size_t> target_index;
    if(input.failure){
        const auto& fact=*input.failure;
        if(!fact.error.source() || !valid(fact.producer) || fact.targets.empty() ||
           (std::holds_alternative<NoCreation>(input.creation) && !input.host && !input.normalization && !input.source_outcome && !input.base_build && input.producer!=fact.producer))return std::unexpected{failure("reference")};
        if(fact.process_sequence){
            const auto sequence=*fact.process_sequence;
            const auto* earlier=find_entry(entries_,sequence);
            if(sequence>input.sequence || !(sequence==input.sequence?role(input.creation):earlier?role(earlier->creation):std::nullopt))return std::unexpected{failure("reference")};
        }
        if(fact.secondary_of){
            const auto* primary=find_entry(entries_,*fact.secondary_of);
            const auto* selected = addressed_failure(primary, fact.secondary_creation_ordinal);
            if(!selected || selected->secondary_of || selected->process_sequence!=fact.process_sequence)return std::unexpected{failure("reference")};
        } else if (fact.secondary_creation_ordinal) return std::unexpected{failure("reference")};
        if(fact.targets.size()>65536)return std::unexpected{failure("capacity")};
        for(const auto& target:fact.targets)if(!detail::valid_target(target,fact))return std::unexpected{failure("reference")};
        const auto stock=fact.error.usage();
        if(stock.retained_bytes<sizeof(private_error::Owner) ||
           !add(next.failure_facts,1) || !add(next.targets,fact.targets.size()) || !add(next.context_nodes,stock.nodes) ||
           !add(next.private_string_bytes,stock.string_bytes) || !add(next.private_binary_bytes,stock.binary_bytes) ||
           !add(next.retained_bytes,stock.retained_bytes-sizeof(private_error::Owner)) ||
           fact.targets.size()>65536 || !add(next.retained_bytes,fact.targets.size()*(sizeof(Target)+sizeof(std::size_t))) ||
           next.failure_facts>limits_.failure_facts || next.targets>limits_.targets || next.context_nodes>limits_.context_nodes ||
           next.private_string_bytes>limits_.private_string_bytes || next.private_binary_bytes>limits_.private_binary_bytes ||
           next.retained_bytes>limits_.retained_bytes)return std::unexpected{failure("capacity")};

    }
    if (!entries_.empty() && entries_.front().run_creation) {
        const auto index_stock = input.failure ? input.failure->targets.size() * sizeof(std::size_t) : 0U;
        std::size_t input_stock = sizeof(Input) + sizeof(journal::Timestamp);
        const auto added = next.retained_bytes - usage_.retained_bytes;
        const auto inline_added = sizeof(Entry) + input.timestamp.text().size() + (runner ? sizeof(std::uint64_t) : 0U);
        if (added < inline_added || added - inline_added < index_stock ||
            !add(input_stock, added - inline_added - index_stock) || !add(input_stock, input.timestamp.text().size()))
            return std::unexpected{failure("capacity")};
        auto peak = usage_.retained_bytes;
        const auto old_capacity = entries_.capacity();
        const auto new_capacity = next_capacity(entries_.size(), old_capacity, limits_.entries);
        const auto pair_capacity = pairs_.capacity();
        const auto new_pair_capacity = runner ? next_capacity(pairs_.size(), pair_capacity, limits_.runner_instances) : pair_capacity;
        if (!add(peak, detail::creation_append_fixed_stock()) || !add(peak, input_stock) ||
            !add(peak, index_stock) || !add(peak, input.timestamp.text().size()) ||
            !run_creation::detail::elements(peak, old_capacity - entries_.size(), sizeof(Entry)) ||
            (new_capacity != old_capacity && !run_creation::detail::elements(peak, new_capacity, sizeof(Entry))) ||
            !run_creation::detail::elements(peak, pair_capacity - pairs_.size(), sizeof(std::uint64_t)) ||
            (new_pair_capacity != pair_capacity && !run_creation::detail::elements(peak, new_pair_capacity, sizeof(std::uint64_t))) ||
            peak > limits_.run_creation.consumer_simultaneous_bytes) return std::unexpected{failure("capacity")};
        next.run_creation.consumer_simultaneous_bytes = std::max(next.run_creation.consumer_simultaneous_bytes, peak);
    }
    if (input.failure) {
        const auto& fact = *input.failure;
        target_index.resize(fact.targets.size());std::iota(target_index.begin(),target_index.end(),std::size_t{});
        std::sort(target_index.begin(),target_index.end(),[&](std::size_t a,std::size_t b){return fact.targets[a]<fact.targets[b];});
        for(std::size_t i=1;i<target_index.size();++i)if(fact.targets[target_index[i-1]]==fact.targets[target_index[i]])return std::unexpected{failure("reference")};
    }
    if(input.source_outcome && input.source_outcome->source.state()==authored_source::State::source_unavailable){
        const auto acquisition=input.source_outcome->source.source()->input().acquisition_failure;
        const Target target{Category::authored_source,Selector::source_acquisition};
        const FailureFact* fact{};
        if(acquisition && acquisition->value!=0){
            const auto sequence=acquisition->value;
            if(sequence==input.sequence && input.failure && has_target(*input.failure,target_index,target))fact=&*input.failure;
            else if(sequence<input.sequence)fact=targeted_failure(find_entry(entries_,sequence),target);
        }
        if(!fact || fact->producer!=Producer::controller || fact->process_sequence || !fact->error.source())return std::unexpected{failure("reference")};
    }
    if(input.base_build && input.base_build->state==BuildState::not_obtained){
        const auto sequence=*input.base_build->acquisition_failure;
        const Target target{Category::base_build,Selector::base_build_acquisition};
        const FailureFact* fact{};
        if(sequence!=0){
            if(sequence==input.sequence && input.failure && has_target(*input.failure,target_index,target))fact=&*input.failure;
            else if(sequence<input.sequence)fact=targeted_failure(find_entry(entries_,sequence),target);
        }
        if(!fact || fact->producer!=Producer::controller || fact->process_sequence || !fact->error.source())return std::unexpected{failure("reference")};
    }
    std::optional<ResolvedSubject> resolved;
    if(input.host){
        if(!input.host->categories() || input.host->run_id()!=run_)return std::unexpected{failure("reference")};
        if(const auto* process=std::get_if<process_thp_restriction::Owner>(&input.host->categories()->process_thp)){
            const auto* subject=process->subject();if(!subject)return std::unexpected{failure("reference")};
            const auto sequence=reference(subject->event_id,run_);if(!sequence || *sequence>input.sequence)return std::unexpected{failure("reference")};
            const Creation* creation=&input.creation;
            if(*sequence!=input.sequence){
                const auto found=std::lower_bound(entries_.begin(),entries_.end(),*sequence,[](const Entry& entry,std::uint64_t seq){return entry.sequence<seq;});
                if(found==entries_.end() || found->sequence!=*sequence)return std::unexpected{failure("reference")};
                creation=&found->creation;
            }
            const auto created_role=role(*creation);
            if(!created_role || *created_role!=subject->role)return std::unexpected{failure("reference")};
            resolved=ResolvedSubject{subject->role,*sequence};
        }
    }
    if(input.host && !host_failures(*input.host->categories(),input,run_,entries_,target_index))return std::unexpected{failure("reference")};
    Entry prepared{input.sequence,input.timestamp.text(),input.producer,input.creation,{},{},std::move(target_index),{},{},resolved};
    if(runner && planning_sequence_){
        auto associated=planning::detail::declared(entries_,*planning_sequence_,runner->pair_ordinal,normalization_sequence_,limits_.planning,next.planning);
        if(!associated)return std::unexpected{failure(associated.error()==planning::Error::capacity?"capacity":"reference")};
        prepared.planning_membership.emplace(std::move(*associated));
    }
    reserve_next(entries_,limits_.entries);
    if(runner)reserve_next(pairs_,limits_.runner_instances);
    prepared.base_build=std::move(input.base_build);
    prepared.source_outcome=std::move(input.source_outcome);
    prepared.normalization=std::move(input.normalization);
    prepared.failure=std::move(input.failure);
    prepared.host=std::move(input.host); // No throwing work remains after this transfer.
    entries_.push_back(std::move(prepared));
    if(runner)pairs_.insert(pairs_.begin()+static_cast<std::ptrdiff_t>(pair_position),runner->pair_ordinal);
    if(entries_.back().normalization)normalization_sequence_=input.sequence;
    if(entries_.back().source_outcome)source_outcome_sequence_=input.sequence;
    if(entries_.back().base_build)base_build_sequence_=input.sequence;
    usage_=next;live_=true;return {};
}
namespace {
auto preflight_failure(preflight::Error error) -> core::Error {
    const char* suffix="invalid_input";
    switch(error){
    case preflight::Error::state:suffix="state";break;
    case preflight::Error::reference:suffix="reference";break;
    case preflight::Error::capacity:suffix="capacity";break;
    case preflight::Error::invalid_input:break;
    case preflight::Error::topology:suffix="topology";break;
    case preflight::Error::policy:suffix="policy";break;
    }
    return {.category=core::ErrorCategory::results,.code=core::StableErrorCode{"results.preflight."+std::string{suffix}},
        .message="Supplied controller preflight rejected",.operation="admit_preflight",
        .os_error=std::nullopt,.context=nullptr};
}
struct PreflightSessionStaging {
    const Entry *controller,*normalization,*system,*build,*topology_entry;
    const topology_evidence::Owner *topology,*system_topology;
    const preflight::Input* input;
    const journal::Timestamp* timestamp;
    const host_assembly::Categories* categories;
    const FailureFact* selected;
    preflight::References references;
    preflight::Limits remaining;
    Usage base,next;
    preflight::Usage child,child_return;
    std::optional<std::expected<preflight::Owner,preflight::Error>> admitted;
    std::optional<Entry> prepared;
    std::optional<core::Error> error;
    std::expected<void,core::Error> result;
    std::size_t old_capacity,new_capacity,entry_stock,owner_allowance;
    std::uint64_t sequence;
    std::optional<preflight::Status> status;
    std::optional<std::expected<planning::detail::MapPreparation,planning::Error>> planning_preparation;
    std::optional<std::expected<planning::Membership,planning::Error>> planning_membership;
};
}
auto Session::append_preflight(std::uint64_t sequence,const journal::Timestamp& timestamp,preflight::Input& input)
    -> std::expected<void,core::Error> {
    using E=preflight::Error;
    if(!std::exchange(live_,false))return std::unexpected{preflight_failure(E::state)};
    if(preflight_sequence_ || preflight_blocked_ || entries_.empty() || !pairs_.empty() ||
        sequence<=entries_.back().sequence || timestamp.text().size()!=30U)
        return std::unexpected{preflight_failure(E::reference)};
    const auto refs=input.references;
    const auto* controller=find_entry(entries_,refs.controller);
    const auto* normalization=find_entry(entries_,refs.normalization);
    const auto* system=find_entry(entries_,refs.system_state);
    const auto* build=find_entry(entries_,refs.base_build);
    if(!controller || !std::holds_alternative<ControllerCreated>(controller->creation) ||
        !normalization || !normalization->normalization || !normalization->normalization->source.plan() ||
        !entry_host(system) || !entry_host(system)->categories() || system->producer!=Producer::controller ||
        !entry_base_build(build) || refs.controller>=sequence || refs.normalization>=sequence ||
        refs.system_state>=sequence || refs.base_build>=sequence)
        return std::unexpected{preflight_failure(E::reference)};
    const auto* topology_entry=find_entry(entries_,normalization->normalization->topology_event_sequence);
    const auto* topology=entry_host(topology_entry) && entry_host(topology_entry)->categories()?
        std::get_if<topology_evidence::Owner>(&entry_host(topology_entry)->categories()->topology):nullptr;
    const auto& categories=*entry_host(system)->categories();
    const auto* system_topology=std::get_if<topology_evidence::Owner>(&categories.topology);
    if(!topology || !system_topology || !topology->fingerprints() || !system_topology->fingerprints() ||
        topology->canonical()!=system_topology->canonical() || *topology->fingerprints()!=*system_topology->fingerprints())
        return std::unexpected{preflight_failure(E::reference)};
    const auto controller_fact=[&](const auto& slot){return std::visit([&](const auto& value){
        if constexpr(requires{value.producer();})return value.run_id()==run_ && value.producer() &&
            static_cast<unsigned>(*value.producer())==0U;
        else return true;},slot);};
    if(!controller_fact(categories.system_controls) || !controller_fact(categories.kernel_identity) ||
        !controller_fact(categories.pu_identity) || !controller_fact(categories.frequency) ||
        !controller_fact(categories.anonymous_thp) || !controller_fact(categories.process_thp))
        return std::unexpected{preflight_failure(E::reference)};
    if(const auto* process=std::get_if<process_thp_restriction::Owner>(&categories.process_thp))
        if(!process->subject() || process->subject()->role!=process_thp_restriction::Role::controller ||
            reference(process->subject()->event_id,run_)!=refs.controller)
            return std::unexpected{preflight_failure(E::reference)};
    for(std::size_t i=0;i<input.temperatures.size();++i){
        const auto& observed=input.temperatures[i];
        if(observed.state!=preflight::ThermalState::failed)continue;
        if(!observed.failure_event || !observed.reason || *observed.reason<preflight::ThermalReason::read_failed ||
            *observed.reason>preflight::ThermalReason::unsupported_value)
            return std::unexpected{preflight_failure(E::reference)};
        const auto* event=find_entry(entries_,*observed.failure_event);
        const auto reason_value=static_cast<FailureReason>(static_cast<unsigned>(*observed.reason)-
            static_cast<unsigned>(preflight::ThermalReason::read_failed)+static_cast<unsigned>(FailureReason::read_failed));
        const auto* selected=targeted_failure(event,{Category::preflight_temperature,
            Selector::cpu_control_observation,0U,static_cast<std::uint64_t>(i),reason_value});
        if(!selected || selected->producer!=Producer::controller || selected->process_sequence)
            return std::unexpected{preflight_failure(E::reference)};
    }
    auto base=usage_;
    if(!detail::add_stock(base,timestamp.text().size(),0U,0U,false,false,0U) || base.entries>limits_.entries ||
        base.timestamp_bytes>limits_.timestamp_bytes || base.retained_bytes>limits_.retained_bytes ||
        base.canonical_child_bytes>limits_.canonical_child_bytes)
        return std::unexpected{preflight_failure(E::capacity)};
    auto remaining=limits_.preflight;
    auto owner_allowance=limits_.retained_bytes-base.retained_bytes;
    if(!add(owner_allowance,sizeof(preflight::Owner)))return std::unexpected{preflight_failure(E::capacity)};
    remaining.retained_bytes=std::min(remaining.retained_bytes,owner_allowance);
    remaining.canonical_child_bytes=std::min(remaining.canonical_child_bytes,limits_.canonical_child_bytes-base.canonical_child_bytes);
    const auto old_capacity=entries_.capacity();
    const auto new_capacity=entries_.size()<old_capacity?old_capacity:
        old_capacity==0U?1U:old_capacity>limits_.entries/2U?limits_.entries:old_capacity*2U;
    std::size_t entry_stock=sizeof(PreflightSessionStaging)+detail::creation_lookup_fixed_stock();
    if(old_capacity>SIZE_MAX/sizeof(Entry) || new_capacity>SIZE_MAX/sizeof(Entry) ||
        !add(entry_stock,(old_capacity-entries_.size())*sizeof(Entry)) ||
        !add(entry_stock,new_capacity*sizeof(Entry)) || !add(entry_stock,timestamp.text().size()))
        return std::unexpected{preflight_failure(E::capacity)};
    std::optional<std::expected<planning::detail::MapPreparation,planning::Error>> planning_preparation;
    if(planning_sequence_){
        planning_preparation.emplace(planning::detail::prepare_map(entries_,*planning_sequence_,refs,limits_.planning,usage_.planning));
        if(!*planning_preparation)return std::unexpected{preflight_failure(planning_preparation->error()==planning::Error::capacity?E::capacity:E::reference)};
        if(!add(entry_stock,planning_preparation->value().peak) || planning_preparation->value().backing>limits_.retained_bytes-base.retained_bytes)
            return std::unexpected{preflight_failure(E::capacity)};
        remaining.retained_bytes=std::min(remaining.retained_bytes,owner_allowance-planning_preparation->value().backing);
    }
    auto admitted=preflight::Owner::prepare({run_,*topology,*entry_host(system),normalization->normalization->source,
        entry_base_build(build)->manifest?&*entry_base_build(build)->manifest:nullptr,
        entry_base_build(build)->state!=BuildState::not_attempted},input,remaining,usage_.retained_bytes,entry_stock);
    if(!admitted)return std::unexpected{preflight_failure(admitted.error())};
    const auto child=admitted->usage();auto next=base;
    if(child.retained_bytes<sizeof(preflight::Owner) || !add(next.retained_bytes,child.retained_bytes-sizeof(preflight::Owner)) ||
        !add(next.canonical_child_bytes,child.canonical_child_bytes) || next.retained_bytes>limits_.retained_bytes ||
        next.canonical_child_bytes>limits_.canonical_child_bytes)return std::unexpected{preflight_failure(E::capacity)};
    next.preflight=child;
    std::optional<std::expected<planning::Membership,planning::Error>> planning_membership;
    if(planning_preparation){
        planning_membership.emplace(planning::detail::finish_map(planning_preparation->value(),admitted->facts()->scopes,limits_.planning,next.planning));
        if(!*planning_membership)return std::unexpected{preflight_failure(planning_membership->error()==planning::Error::capacity?E::capacity:E::reference)};
        if(!add(next.retained_bytes,planning_preparation->value().backing) || next.retained_bytes>limits_.retained_bytes)
            return std::unexpected{preflight_failure(E::capacity)};
    }
    Entry prepared{sequence,timestamp.text(),Producer::controller,NoCreation{},{},{},{},{},{},{}};
    if(planning_membership)prepared.planning_membership.emplace(std::move(planning_membership->value()));
    prepared.preflight.emplace(std::move(*admitted));
    const auto status=prepared.preflight->status();
    reserve_next(entries_,limits_.entries);
    prepared.preflight->commit(input.monitoring);
    entries_.push_back(std::move(prepared));
    usage_=next;preflight_sequence_=sequence;
    preflight_blocked_=status==preflight::Status::rejected || status==preflight::Status::incomplete;
    live_=true;return {};
}
auto Session::append_lifecycle(std::uint64_t sequence, const journal::Timestamp& timestamp,
    std::uint64_t controller_sequence, std::optional<std::uint64_t> runner_sequence,
    runtime::detail::SyntheticLifecycleView& incoming, block_evidence::PairPrefix* prefix)
    -> std::expected<void, core::Error> {
    auto source = incoming.consume();
    if (!std::exchange(live_, false)) return std::unexpected{failure("invalid_state")};
    if (preflight_blocked_) return std::unexpected{failure("reference")};
    const auto* facts = source.facts();
    if (domain_ != Domain::synthetic_execution || !facts || entries_.empty() ||
        sequence == 0U || sequence <= entries_.back().sequence || timestamp.text().empty())
        return std::unexpected{failure("invalid_input")};
    const auto* controller = find_entry(entries_, controller_sequence);
    if (!controller || !std::holds_alternative<ControllerCreated>(controller->creation))
        return std::unexpected{failure("reference")};
    const bool spawned = facts->ordinal(runtime::detail::SyntheticLifecycleMark::child_created) != 0U;
    if (spawned != runner_sequence.has_value()) return std::unexpected{failure("reference")};
    pair_lifecycle::References references{controller_sequence, runner_sequence, {}, {}};
    if (runner_sequence) {
        const auto* runner = find_entry(entries_, *runner_sequence);
        const auto* created = runner ? std::get_if<RunnerCreated>(&runner->creation) : nullptr;
        if (!created || created->controller_sequence != controller_sequence)
            return std::unexpected{failure("reference")};
        references.pair_ordinal = created->pair_ordinal;
    }
    for (const auto& entry : entries_)
        if (entry.lifecycle && entry.lifecycle->references().runner_sequence == runner_sequence)
            return std::unexpected{failure("reference")};
    const scenario::ScenarioDigestedPlan* plan{};
    if (normalization_sequence_) {
        const auto* normalization = find_entry(entries_, *normalization_sequence_);
        if (!normalization || !normalization->normalization) return std::unexpected{failure("reference")};
        plan = normalization->normalization->source.plan();
        references.normalization_sequence = normalization_sequence_;
    }
    auto run = journal::RunId::parse(run_);
    if (!run) return std::unexpected{failure("invalid_state")};
    auto remaining = limits_.lifecycle;
    auto base_usage = usage_;
    if (!detail::add_stock(base_usage, timestamp.text().size(), 0U, 0U, false, false, 0U) ||
        base_usage.entries > limits_.entries || base_usage.timestamp_bytes > limits_.timestamp_bytes ||
        base_usage.retained_bytes > limits_.retained_bytes ||
        usage_.canonical_child_bytes > limits_.canonical_child_bytes)
        return std::unexpected{failure("capacity")};
    const auto subtract = [](std::size_t& bound, std::size_t used) noexcept {
        if (used > bound) return false;
        bound -= used;
        return true;
    };
    if (!subtract(remaining.records, usage_.lifecycle.records) ||
        !subtract(remaining.marks, usage_.lifecycle.marks) ||
        !subtract(remaining.request_bytes, usage_.lifecycle.request_bytes) ||
        !subtract(remaining.transport_bytes, usage_.lifecycle.transport_bytes) ||
        !subtract(remaining.canonical_bytes, usage_.lifecycle.canonical_bytes) ||
        !subtract(remaining.capacity_elements, usage_.lifecycle.capacity_elements) ||
        !subtract(remaining.retained_bytes, usage_.lifecycle.retained_bytes))
        return std::unexpected{failure("capacity")};
    remaining.canonical_bytes = std::min(remaining.canonical_bytes,
        limits_.canonical_child_bytes - usage_.canonical_child_bytes);
    auto owner_allowance = limits_.retained_bytes - base_usage.retained_bytes;
    if (!add(owner_allowance, sizeof(pair_lifecycle::Owner))) owner_allowance = SIZE_MAX;
    remaining.retained_bytes = std::min(remaining.retained_bytes, owner_allowance);
    // Bound old/new vector storage before reserve; the incoming Prefix is still
    // caller-owned throughout every allocation and all context comparisons.
    const auto old_capacity = entries_.capacity();
    const auto new_capacity = entries_.size() < old_capacity ? old_capacity :
        old_capacity == 0U ? 1U : old_capacity > limits_.entries / 2U ? limits_.entries : old_capacity * 2U;
    std::size_t entry_stock = sizeof(detail::LifecycleSessionStaging);
    if (planning_sequence_ && !add(entry_stock, planning::detail::declared_fixed_stock()))
        return std::unexpected{failure("capacity")};
    if (!add(entry_stock, timestamp.text().size()) ||
        (prefix && !add(entry_stock, sizeof(block_evidence::PairPrefix))))
        return std::unexpected{failure("capacity")};
    if (old_capacity > std::numeric_limits<std::size_t>::max() / sizeof(Entry) ||
        new_capacity > std::numeric_limits<std::size_t>::max() / sizeof(Entry) ||
        !add(entry_stock, old_capacity * sizeof(Entry)) ||
        !add(entry_stock, new_capacity * sizeof(Entry))) return std::unexpected{failure("capacity")};
    // Complete all borrowed planning/request comparisons before the lower copy
    // and before Entry growth can invalidate existing Record addresses.
    auto planning_candidate = usage_.planning;
    auto request_candidate = usage_.request_association;
    std::optional<std::expected<planning::Membership, planning::Error>> planning_result;
    std::optional<std::expected<request_association::Evidence, request_association::Error>> request_result;
    if (planning_sequence_) {
        const auto* actual = references.pair_ordinal ? source.admitted() : nullptr;
        if (actual && !add(entry_stock, request_association::detail::fixed_stock()))
            return std::unexpected{failure("capacity")};
        auto request_phase_peak = usage_.retained_bytes;
        if (!add(request_phase_peak, entry_stock) || request_phase_peak > remaining.simultaneous_bytes)
            return std::unexpected{failure("capacity")};
        planning_result.emplace(planning::detail::declared(entries_, *planning_sequence_,
            references.pair_ordinal, references.normalization_sequence, limits_.planning, planning_candidate));
        if (!*planning_result) return std::unexpected{failure(planning_result->error() == planning::Error::capacity ? "capacity" : "reference")};
        if (actual) {
            request_result.emplace(request_association::detail::prepare(entries_, *planning_sequence_,
                *references.pair_ordinal, actual->inputs(), facts->pair.first.value(), facts->pair.second.value(),
                limits_.request_association, request_candidate));
            if (!*request_result) return std::unexpected{failure(request_result->error() == request_association::Error::capacity ? "capacity" : "request_association")};
            planning_result->value().request.emplace(std::move(request_result->value()));
            planning_result->value().semantic = planning::SemanticMembership::admitted_request;
        }
    }
    auto admitted = pair_lifecycle::Owner::prepare(source, *run, plan, prefix, references,
        remaining, usage_.retained_bytes, entry_stock);
    if (!admitted) return std::unexpected{failure("lifecycle")};
    auto next = usage_;
    const auto child = admitted->usage();
    if (child.retained_bytes < sizeof(pair_lifecycle::Owner) ||
        !detail::add_stock(next, timestamp.text().size(), child.canonical_bytes, 0U, false, false, 0U) ||
        !add(next.retained_bytes, child.retained_bytes - sizeof(pair_lifecycle::Owner)) ||
        next.entries > limits_.entries || next.timestamp_bytes > limits_.timestamp_bytes ||
        next.canonical_child_bytes > limits_.canonical_child_bytes || next.retained_bytes > limits_.retained_bytes)
        return std::unexpected{failure("capacity")};
    if (!add(next.lifecycle.records, child.records) || !add(next.lifecycle.marks, child.marks) ||
        !add(next.lifecycle.request_bytes, child.request_bytes) ||
        !add(next.lifecycle.transport_bytes, child.transport_bytes) ||
        !add(next.lifecycle.canonical_bytes, child.canonical_bytes) ||
        !add(next.lifecycle.capacity_elements, child.capacity_elements) ||
        !add(next.lifecycle.retained_bytes, child.retained_bytes)) return std::unexpected{failure("capacity")};
    next.lifecycle.simultaneous_bytes = std::max(next.lifecycle.simultaneous_bytes, child.simultaneous_bytes);
    Entry prepared{sequence, timestamp.text(), Producer::controller, NoCreation{}, {}, {}, {}, {}, {}, {}};
    if (planning_result) prepared.planning_membership.emplace(std::move(planning_result->value()));
    next.planning = planning_candidate;
    next.request_association = request_candidate;
    prepared.lifecycle.emplace(std::move(*admitted));
    if (runner_sequence) prepared.resolved_subject = ResolvedSubject{process_thp_restriction::Role::runner, *runner_sequence};
    reserve_next(entries_, limits_.entries);
    static_assert(std::is_nothrow_move_constructible_v<block_evidence::PairPrefix>);
    static_assert(std::is_nothrow_move_constructible_v<pair_lifecycle::Owner>);
    prepared.lifecycle->commit_prefix(prefix);
    entries_.push_back(std::move(prepared));
    usage_ = next;
    live_ = true;
    return {};
}

auto Session::append_analysis(std::uint64_t sequence, const journal::Timestamp& timestamp,
    std::uint64_t lifecycle_sequence, std::string_view canonical_analysis)
    -> std::expected<void, core::Error> {
    if (!std::exchange(live_, false)) return std::unexpected{failure("invalid_state")};
    if (preflight_blocked_) return std::unexpected{failure("reference")};
    if (domain_ != Domain::synthetic_execution || entries_.empty() || timestamp.text().empty() ||
        entries_.back().sequence == UINT64_MAX || sequence != entries_.back().sequence + 1U)
        return std::unexpected{failure("invalid_input")};
    const auto* parent = find_entry(entries_, lifecycle_sequence);
    if (!parent || lifecycle_sequence >= sequence || !parent->lifecycle)
        return std::unexpected{failure("reference")};
    const auto& refs = parent->lifecycle->references();
    if (!refs.runner_sequence || !refs.pair_ordinal || !refs.normalization_sequence)
        return std::unexpected{failure("reference")};
    for (const auto& entry : entries_)
        if (entry.analysis && entry.analysis->references() &&
            entry.analysis->references()->lifecycle_sequence == lifecycle_sequence)
            return std::unexpected{failure("reference")};
    const pair_analysis::References references{lifecycle_sequence, refs.controller_sequence,
        *refs.runner_sequence, *refs.pair_ordinal, *refs.normalization_sequence};
    auto base = usage_;
    if (!detail::add_stock(base,timestamp.text().size(),0U,0U,false,false,0U) ||
        base.entries > limits_.entries || base.timestamp_bytes > limits_.timestamp_bytes ||
        base.retained_bytes > limits_.retained_bytes || usage_.canonical_child_bytes > limits_.canonical_child_bytes)
        return std::unexpected{failure("capacity")};
    auto remaining = limits_.analysis;
    const auto subtract = [](std::size_t& cap, std::size_t used) {
        if (used > cap) return false;
        cap -= used; return true;
    };
    if (!subtract(remaining.values.records,usage_.analysis.values.records) ||
        !subtract(remaining.values.canonical_bytes,usage_.analysis.values.canonical_bytes) ||
        !subtract(remaining.values.retained_bytes,usage_.analysis.values.retained_bytes))
        return std::unexpected{failure("capacity")};
    remaining.values.canonical_bytes = std::min(remaining.values.canonical_bytes,
        limits_.canonical_child_bytes - usage_.canonical_child_bytes);
    std::size_t owner_allowance = limits_.retained_bytes-base.retained_bytes;
    if (!add(owner_allowance,sizeof(pair_analysis::Owner))) return std::unexpected{failure("capacity")};
    remaining.values.retained_bytes=std::min(remaining.values.retained_bytes,owner_allowance);
    const auto old_capacity=entries_.capacity();
    const auto new_capacity=entries_.size()<old_capacity?old_capacity:
        old_capacity==0U?1U:old_capacity>limits_.entries/2U?limits_.entries:old_capacity*2U;
    std::size_t entry_stock=sizeof(detail::AnalysisSessionStaging);
    if (old_capacity>SIZE_MAX/sizeof(Entry) || new_capacity>SIZE_MAX/sizeof(Entry) ||
        !add(entry_stock,old_capacity*sizeof(Entry)) || !add(entry_stock,new_capacity*sizeof(Entry)) ||
        !add(entry_stock,timestamp.text().size())) return std::unexpected{failure("capacity")};
    auto admitted=pair_analysis::Owner::prepare(*parent->lifecycle,references,canonical_analysis,
        remaining,usage_.retained_bytes,entry_stock);
    if (!admitted) return std::unexpected{failure("analysis")};
    const auto child=admitted->usage();
    auto next=usage_;
    if (child.values.retained_bytes<sizeof(pair_analysis::Owner) ||
        !detail::add_stock(next,timestamp.text().size(),child.values.canonical_bytes,0U,false,false,0U) ||
        !add(next.retained_bytes,child.values.retained_bytes-sizeof(pair_analysis::Owner)) ||
        next.retained_bytes>limits_.retained_bytes || next.canonical_child_bytes>limits_.canonical_child_bytes)
        return std::unexpected{failure("capacity")};
    auto combined=pair_analysis::detail::combine(usage_.analysis,child);
    if (!combined) return std::unexpected{failure("capacity")};
    next.analysis=*combined;
    Entry prepared{sequence,timestamp.text(),Producer::results,NoCreation{},{},{},{},{},{},{}};
    prepared.analysis.emplace(std::move(*admitted));
    reserve_next(entries_,limits_.entries);
    static_assert(std::is_nothrow_move_constructible_v<pair_analysis::Owner>);
    entries_.push_back(std::move(prepared));
    usage_=next; live_=true;
    return {};
}

auto Session::finish() -> std::expected<Summary,core::Error> {
    if(!std::exchange(live_,false))return std::unexpected{failure("invalid_state")};
    return Summary{entries_.size(),entries_.empty()?std::nullopt:std::optional{entries_.back().sequence}};
}
} // namespace epycbench::results::process_subject

namespace epycbench::results::process_subject {
namespace {
namespace hp=host_projection;
struct ProjectionSessionCells {
    const Session* session;
    std::uint64_t sequence;
    hp::Limits limits;
    const Entry *entry, *failure_entry;
    const host_assembly::Owner* host;
    const host_assembly::Categories* categories;
    const FailureFact* fact;
    const run_creation::detail::State* genesis;
    std::optional<hp::ResolvedSubject> subject;
    std::vector<hp::ResolvedFailure> failures;
    hp::detail::ResolvedInput handoff;
    hp::ResolvedFailure resolved;
    hp::FactKey translated;
    Target target;
    std::optional<std::uint64_t> ordinal, parsed;
    std::optional<Producer> producer;
    std::span<const Entry> entries;
    std::span<const FailureFact> facts;
    std::size_t count, table_bytes, fixed, index, pass;
    std::uint64_t key, failure_sequence;
    std::array<Selector,5> control_selectors;
    std::array<Selector,3> kernel_selectors;
    const void *owner, *observation, *slot, *row;
    std::array<const void*,5> resolve_capture, outer_capture, observation_capture, visit_capture;
    const hp::Limits* lower_limits;
    const host_assembly::Owner* constructor_host;
    std::string_view constructor_run, constructor_time;
    hp::SourceDomain constructor_domain;
    hp::SourceOrigin constructor_origin;
    hp::Producer constructor_producer;
    std::uint64_t constructor_sequence;
    std::optional<hp::ResolvedSubject> constructor_subject;
    std::span<const hp::ResolvedFailure> constructor_failures;
    std::size_t constructor_fixed, constructor_table;
    std::expected<hp::Owner,hp::Error> returned;
    // Actual shared lookup/reference helpers, including their argument/return cells.
    CreationLookupCells lookup;
    std::string_view reference_text, reference_run;
    std::uint64_t parsed_value;
    std::from_chars_result parsed_chars;
    std::optional<std::uint64_t> parsed_return;
    const Entry* entry_host_parameter;
    const host_assembly::Owner* entry_host_return;
    std::vector<hp::ResolvedFailure>::iterator sort_first,sort_last,sort_cursor;
    const hp::ResolvedFailure *sort_left,*sort_right;
    const Target* translated_parameter;
    hp::FactKey translated_result;
    Producer producer_parameter;
    hp::Producer producer_result;
    hp::SourceDomain source_domain;
    std::optional<process_thp_restriction::Role> role_return, role_second_return;
    const Creation* role_parameter;
    const Entry* created;
    std::span<const Entry> find_entries;
    std::uint64_t find_sequence;
    std::span<const Entry>::iterator find_result;
    std::optional<host_controls::Producer> owner_producer_return;
    std::optional<std::uint64_t> owner_ordinal_return;
};
auto projection_producer(Producer value) -> hp::Producer {
    switch(value) {
    case Producer::controller:return hp::Producer::controller;
    case Producer::runner:return hp::Producer::runner;
    case Producer::results:return hp::Producer::results;
    }throw hp::Error::source;
}
auto projection_target(const Target& target) -> hp::FactKey {
    hp::FactKey result{};
#define MAP_CATEGORY(value) case Category::value: result.category=hp::Category::value; break
    switch(target.category) {
    MAP_CATEGORY(topology);MAP_CATEGORY(system_controls);MAP_CATEGORY(kernel_identity);MAP_CATEGORY(pu_identity);
    MAP_CATEGORY(frequency);MAP_CATEGORY(anonymous_thp);MAP_CATEGORY(process_thp);
    default:throw hp::Error::reference;
    }
#undef MAP_CATEGORY
#define MAP_SELECTOR(value) case Selector::value: result.selector=hp::Selector::value; break
    switch(target.selector) {
    MAP_SELECTOR(owner_preparation);MAP_SELECTOR(smt_control);MAP_SELECTOR(smt_active);MAP_SELECTOR(numa_mask);
    MAP_SELECTOR(thp_enabled);MAP_SELECTOR(thp_defrag);MAP_SELECTOR(sysname);MAP_SELECTOR(release);MAP_SELECTOR(version);
    MAP_SELECTOR(signature);MAP_SELECTOR(microcode);MAP_SELECTOR(uncovered_cpu);MAP_SELECTOR(enumeration);
    MAP_SELECTOR(page_size);MAP_SELECTOR(process_observation);
    default:throw hp::Error::reference;
    }
#undef MAP_SELECTOR
    switch(target.reason) {
    case FailureReason::none:result.reason=hp::FailureReason::none;break;
    case FailureReason::read_failed:result.reason=hp::FailureReason::read_failed;break;
    case FailureReason::malformed_source:result.reason=hp::FailureReason::malformed_source;break;
    case FailureReason::unsupported_value:result.reason=hp::FailureReason::unsupported_value;break;
    default:throw hp::Error::reference;
    }
    result.ordinal=target.ordinal;result.key=target.key;return result;
}
}
namespace {
namespace bp=build_projection;
struct BuildProjectionSessionCells {
    const Session* session;
    std::uint64_t sequence;
    bp::Limits limits;
    const Entry *entry,*failure_entry;
    const BaseBuild* build;
    const build_manifest::Owner* manifest;
    const build_manifest::ManifestDigest* digest_return;
    const FailureFact* fact;
    const run_creation::detail::State* genesis;
    bp::Disposition disposition;
    bp::Domain domain;
    std::optional<bp::FailureAddress> failure;
    bp::FailureAddress address_temporary;
    std::optional<std::uint64_t> ordinal;
    Target target;
    std::size_t index;
    bp::detail::ResolvedInput handoff;
    bp::detail::ResolvedInput* constructor_this;
    const build_manifest::Owner* constructor_manifest;
    bp::Disposition constructor_disposition;
    std::string_view constructor_run,constructor_time;
    bp::Domain constructor_domain;
    bp::Origin constructor_origin;
    std::uint64_t constructor_sequence;
    std::optional<bp::FailureAddress> constructor_failure;
    std::size_t constructor_fixed,fixed_return;
    CreationLookupCells lookup;
    const Entry* base_parameter;
    const BaseBuild* base_return;
    const run_creation::detail::State* base_facts;
    std::span<const Entry> find_entries;
    std::uint64_t find_sequence;
    std::span<const Entry>::iterator find_result;
    const Entry* find_comparator_entry;
    std::uint64_t find_comparator_sequence;
    const Target* valid_target;
    const FailureFact* valid_fact;
    bool valid_target_return,valid_limits_return;
    std::unexpected<bp::Error> unexpected_result;
    std::expected<bp::Owner,bp::Error> returned;
    const bp::Limits* lower_limits;
};
}
namespace {
namespace es=error_serialization;
struct ErrorSerializationSessionCells {
    const Session* session;
    es::Address address;
    es::Limits limits;
    const es::OrdinaryFailure* ordinary;
    const es::CreationFailure* creation;
    const es::SourceResolutionFailure* source;
    std::uint64_t sequence;
    es::Kind kind;
    std::optional<std::uint64_t> ordinal;
    const Entry* entry;
    const FailureFact* fact;
    const private_error::Owner* error;
    const authored_source::ResolutionRecord* record;
    std::span<const Entry> find_entries;
    std::uint64_t find_sequence;
    std::span<const Entry>::iterator find_result;
    const Entry* comparator_entry;
    std::uint64_t comparator_sequence;
    const run_creation::Owner* failure_at_this;
    std::uint64_t failure_at_ordinal;
    const FailureFact* failure_at_return;
    es::detail::View view,constructed_view;
    es::detail::ResolvedInput input;
    const private_error::Owner* constructor_error;
    es::detail::View constructor_view;
    std::size_t constructor_fixed,fixed_return;
    es::detail::ResolvedInput* constructor_this;
    std::expected<es::Owner,es::Error> returned;
    std::unexpected<es::Error> unexpected;
    const es::detail::ResolvedInput* prepare_input;
    es::Limits prepare_limits;
    const authored_source::ResolutionRecord *state_this,*source_this,*upstream_this;
    authored_source::State state_return;
    const authored_source::SourceBundle* source_return;
    const private_error::Owner* upstream_return;
    const private_error::Owner* error_source_this;
    const core::Error* error_source_return;
};
}
namespace {
namespace ss=source_serialization;
struct SourceSerializationSessionCells {
    const Session* session;ss::Address address;ss::Limits limits;
    const ss::CreationSource* creation;const ss::NormalizationSource* normalization;const ss::OutcomeSource* outcome;
    std::uint64_t sequence;ss::Kind kind;const Entry* entry;const authored_source::SourceBundle* source;
    const run_creation::detail::State* genesis;const authored_source::ResolutionRecord* record;
    authored_source::Input source_input;std::optional<ss::Acquisition> acquisition;
    std::optional<es::Owner> child;std::optional<es::Address> child_address;
    const Entry* failure_entry;const FailureFact* fact;Target target;
    std::uint64_t ordinal;const run_creation::detail::State* failure_state;
    ss::detail::View view,constructed_view;ss::detail::ResolvedInput input;
    const authored_source::SourceBundle* constructor_source;ss::detail::View constructor_view;
    std::optional<es::Owner>* constructor_child;std::size_t constructor_fixed,fixed_return;
    ss::detail::ResolvedInput* constructor_this;
    std::expected<void,ss::Error> preflight;std::expected<es::Owner,es::Error> prepared_child;
    std::expected<ss::Owner,ss::Error> returned;std::unexpected<ss::Error> unexpected;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;
    const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* targeted_return;
    const run_creation::Owner* facts_this;const run_creation::detail::State* facts_return;
    const authored_source::ResolutionRecord *source_this,*state_this,*plan_this;
    authored_source::State state_return;const authored_source::SourceBundle* source_return;
    const scenario::ScenarioDigestedPlan* plan_return;const authored_source::SourceBundle* source_input_this;
    authored_source::Input source_input_return;const private_error::Owner* error_source_this;const core::Error* error_source_return;
    const run_creation::Owner* failure_for_this;const Target* failure_for_target;const FailureFact* failure_for_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    ss::Acquisition acquisition_temporary;es::CreationFailure creation_address;es::OrdinaryFailure ordinary_address;
    ss::Limits prepare_limits;const ss::detail::ResolvedInput* prepare_input;
};
}
namespace {
namespace so=source_outcome_serialization;
struct SourceOutcomeSerializationSessionCells {
    const Session* session;std::uint64_t sequence;so::Limits limits;const Entry* entry;
    const authored_source::ResolutionRecord* record;
    std::optional<ss::Owner> source;std::optional<es::Owner> upstream;
    so::detail::View view,constructor_view;so::detail::ResolvedInput input;
    const authored_source::ResolutionRecord* constructor_record;
    std::optional<ss::Owner>* constructor_source;std::optional<es::Owner>* constructor_upstream;
    std::size_t constructor_fixed,fixed_return;so::detail::ResolvedInput* constructor_this;
    std::expected<void,so::Error> preflight,upstream_preflight;
    std::expected<ss::Owner,ss::Error> prepared_source;std::expected<es::Owner,es::Error> prepared_upstream;
    ss::OutcomeSource source_address;ss::Address source_address_variant;ss::Limits source_limits;
    es::SourceResolutionFailure upstream_address;es::Address upstream_address_variant;es::Limits upstream_limits;
    std::expected<so::Owner,so::Error> returned;std::unexpected<so::Error> unexpected;
    const so::detail::ResolvedInput* prepare_input;so::Limits prepare_limits;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;
    const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const authored_source::ResolutionRecord* state_this;authored_source::State state_return;
};
}
namespace {
namespace rc=run_creation_serialization;
template<class T,class L,class E>struct CreationChildSelectionCells {std::expected<T,E> prepared;L limits;const Session* session;std::uint64_t sequence;};
struct RunCreationSerializationSessionCells {
    const Session* session;std::uint64_t sequence;rc::Limits limits,prepare_limits;
    const Entry* entry;const run_creation::detail::State* facts;
    rc::detail::View view,constructor_view;rc::detail::Staging staging;rc::detail::ResolvedInput input;
    rc::detail::Staging* constructor_staging;std::size_t constructor_n,constructor_b,constructor_fixed,fixed_return;rc::detail::ResolvedInput* constructor_this;
    std::expected<void,rc::Error> preflight,error_phase,index_phase,source_phase,build_phase,host_phase;
    std::size_t ordinal;const run_creation::Binding* binding;rc::Binding binding_copy;std::vector<run_creation::Binding>::const_iterator binding_begin,binding_end;
    CreationChildSelectionCells<error_serialization::Owner,error_serialization::Limits,error_serialization::Error> error;
    CreationChildSelectionCells<source_serialization::Owner,source_serialization::Limits,source_serialization::Error> source;
    CreationChildSelectionCells<base_build_serialization::Owner,base_build_serialization::Limits,base_build_serialization::Error> build;
    CreationChildSelectionCells<host_serialization::Owner,host_serialization::Limits,host_serialization::Error> host;
    error_serialization::CreationFailure failure_address;error_serialization::Address failure_variant;
    source_serialization::CreationSource source_address;source_serialization::Address source_variant;
    std::expected<rc::Owner,rc::Error> returned;std::unexpected<rc::Error> unexpected;
    const run_creation::Owner* facts_this;const run_creation::detail::State* facts_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    rc::detail::ResolvedInput *preflight_input,*error_input,*index_input,*child_input,*prepare_input;const rc::Limits *preflight_limits,*error_limits,*index_limits,*child_limits;std::size_t child_index;
};
}
auto Session::serialize_run_creation(std::uint64_t sequence,rc::Limits limits)const->std::expected<rc::Owner,rc::Error>{
    if(!present_)return std::unexpected{rc::Error::state};
    if(sequence!=1||entries_.empty())return std::unexpected{rc::Error::reference};
    const auto* entry=&entries_.front();
    if(entry->sequence!=1||entry->producer!=Producer::controller||!std::holds_alternative<ControllerCreated>(entry->creation)||!entry->run_creation)return std::unexpected{rc::Error::reference};
    const auto* facts=entry->run_creation->facts();
    if(!facts||facts->run!=run_||facts->domain!=domain_||facts->sequence!=1||!facts->source||!facts->build||!facts->host)return std::unexpected{rc::Error::identity};
    const rc::detail::View view{run_,entry->timestamp,domain_,entry->producer,1};rc::detail::Staging staging;
    rc::detail::ResolvedInput input{view,facts->failures.size(),facts->bindings.size(),&staging,rc::detail::session_fixed_stock()};
    auto preflight=rc::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    for(const auto& binding:facts->bindings)staging.children.bindings.push_back({binding.target,binding.failure_ordinal});
    for(std::size_t ordinal=0;ordinal<facts->failures.size();++ordinal){
        if(entry->run_creation->failure_at(ordinal)!=&facts->failures[ordinal])return std::unexpected{rc::Error::reference};
        auto phase=rc::Owner::error_phase(input,limits);if(!phase)return std::unexpected{phase.error()};
        auto prepared=serialize_error(error_serialization::CreationFailure{1,ordinal},limits.error);if(!prepared)return std::unexpected{prepared.error()==error_serialization::Error::capacity?rc::Error::capacity:rc::Error::child};staging.children.failures.push_back(std::move(*prepared));
    }
    auto index_phase=rc::Owner::index_phase(input,limits);if(!index_phase)return std::unexpected{index_phase.error()};
    {auto phase=rc::Owner::child_phase(input,limits,0);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_source(source_serialization::CreationSource{1},limits.source);if(!prepared)return std::unexpected{prepared.error()==source_serialization::Error::capacity?rc::Error::capacity:rc::Error::child};staging.children.source=std::move(*prepared);}
    {auto phase=rc::Owner::child_phase(input,limits,1);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_base_build(1,limits.base_build);if(!prepared)return std::unexpected{prepared.error()==base_build_serialization::Error::capacity?rc::Error::capacity:rc::Error::child};staging.children.base_build=std::move(*prepared);}
    {auto phase=rc::Owner::child_phase(input,limits,2);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_host(1,limits.host);if(!prepared)return std::unexpected{prepared.error()==host_serialization::Error::capacity?rc::Error::capacity:rc::Error::child};staging.children.host=std::move(*prepared);}
    return rc::Owner::prepare(input,limits);
}
namespace {
namespace hs=host_serialization;
template<class T,class L,class E>struct HostChildSelectionCells {
    std::expected<T,E> prepared;L limits;const Session* session;std::uint64_t sequence;
};
struct HostSerializationSessionCells {
    const Session* session;std::uint64_t sequence;hs::Limits limits,prepare_limits;
    const Entry* entry;const host_assembly::Owner* host;
    hs::detail::View view,constructor_view;hs::detail::Children children;
    hs::detail::ResolvedInput input;hs::detail::Children* constructor_children;
    std::size_t constructor_fixed,fixed_return;hs::detail::ResolvedInput* constructor_this;
    std::expected<void,hs::Error> preflight;std::array<std::expected<void,hs::Error>,7> phases;
    HostChildSelectionCells<topology_serialization::Owner,topology_serialization::Limits,topology_serialization::Error> topology;
    HostChildSelectionCells<system_controls_serialization::Owner,system_controls_serialization::Limits,system_controls_serialization::Error> controls;
    HostChildSelectionCells<kernel_identity_serialization::Owner,kernel_identity_serialization::Limits,kernel_identity_serialization::Error> kernel;
    HostChildSelectionCells<pu_identity_serialization::Owner,pu_identity_serialization::Limits,pu_identity_serialization::Error> pu;
    HostChildSelectionCells<frequency_policy_serialization::Owner,frequency_policy_serialization::Limits,frequency_policy_serialization::Error> frequency;
    HostChildSelectionCells<anonymous_thp_policy_serialization::Owner,anonymous_thp_policy_serialization::Limits,anonymous_thp_policy_serialization::Error> anonymous;
    HostChildSelectionCells<process_thp_restriction_serialization::Owner,process_thp_restriction_serialization::Limits,process_thp_restriction_serialization::Error> process;
    std::expected<hs::Owner,hs::Error> returned;std::unexpected<hs::Error> unexpected;
    hs::detail::ResolvedInput* phase_input;const hs::Limits* phase_limits;std::size_t phase_index;
    hs::detail::ResolvedInput* prepare_input;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;
    const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_entry;const run_creation::detail::State* host_facts;const host_assembly::Owner* host_return;
};
}
auto Session::serialize_host(std::uint64_t sequence,hs::Limits limits)const->std::expected<hs::Owner,hs::Error>{
    if(!present_)return std::unexpected{hs::Error::state};
    if(!sequence)return std::unexpected{hs::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);
    if(!entry||!host)return std::unexpected{hs::Error::reference};
    const hs::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?hs::Kind::genesis:hs::Kind::ordinary,sequence};
    hs::detail::Children children;hs::detail::ResolvedInput input{view,&children,hs::detail::session_fixed_stock()};
    auto preflight=hs::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
#define HOST_SELECT(N,M,METHOD,NS) {auto phase=hs::Owner::child_phase(input,limits,N);if(!phase)return std::unexpected{phase.error()};auto prepared=METHOD(sequence,limits.M);if(!prepared)return std::unexpected{prepared.error()==NS::Error::capacity?hs::Error::capacity:hs::Error::child};children.M=std::move(*prepared);}
    HOST_SELECT(0,topology,serialize_topology,topology_serialization)
    HOST_SELECT(1,system_controls,serialize_system_controls,system_controls_serialization)
    HOST_SELECT(2,kernel_identity,serialize_kernel_identity,kernel_identity_serialization)
    HOST_SELECT(3,pu_identity,serialize_pu_identity,pu_identity_serialization)
    HOST_SELECT(4,frequency,serialize_frequency_policy,frequency_policy_serialization)
    HOST_SELECT(5,anonymous_thp,serialize_anonymous_thp_policy,anonymous_thp_policy_serialization)
    HOST_SELECT(6,process_thp,serialize_process_thp_restriction,process_thp_restriction_serialization)
#undef HOST_SELECT
    return hs::Owner::prepare(input,limits);
}
namespace {
namespace ti=process_thp_restriction_serialization;
auto process_thp_failure_binding(std::span<const Entry> entries,std::uint64_t sequence,Target target,std::optional<Producer> expected_producer,std::optional<std::uint64_t> process)->std::expected<ti::detail::Binding,ti::Error>{
    const auto* entry=find_entry(entries,sequence);const auto* fact=targeted_failure(entry,target);
    if(!entry||!fact||fact->process_sequence!=process||!fact->error.source()||(expected_producer&&fact->producer!=*expected_producer))return std::unexpected{ti::Error::reference};
    ti::detail::Binding out{target,ti::FailureReference{sequence,{}},{}};
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();if(!facts)return std::unexpected{ti::Error::reference};std::uint64_t ordinal{};while(ordinal<facts->failures.size()&&entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==facts->failures.size())return std::unexpected{ti::Error::reference};out.reference->creation_ordinal=ordinal;out.address=es::CreationFailure{sequence,ordinal};}
    else out.address=es::OrdinaryFailure{sequence};
    return out;
}
struct ProcessThpRestrictionSerializationSessionCells {
    const Session* session;std::uint64_t sequence;ti::Limits limits;
    const Entry* entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<process_thp_restriction::Owner>* slot;const process_thp_restriction::Owner* identity;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;
    ti::SlotState state;std::optional<ti::Reason> reason;ti::detail::Binding preparation;
    std::expected<ti::detail::Binding,ti::Error> preparation_resolved,observation_resolved;
    ti::detail::View view,constructor_view;std::expected<void,ti::Error> before;
    ti::detail::Binding observation_binding;std::optional<ti::SubjectAssociation> subject;
    const Entry* created;std::optional<process_thp_restriction::Role> created_role;const process_thp_restriction::Subject* source_subject;const process_thp_restriction::Observation* observation;
    std::optional<std::uint64_t> reference_return,subject_reference,observation_reference;
    Target observation_target;std::optional<Producer> observation_producer;
    ti::detail::Staging staging;ti::detail::ResolvedInput handoff;
    const process_thp_restriction::Owner* constructor_source;ti::detail::Staging* constructor_staging;std::optional<ti::SubjectAssociation> constructor_subject;const ti::detail::Binding* constructor_observation;std::size_t constructor_fixed;ti::detail::ResolvedInput* constructor_this;
    std::expected<void,ti::Error> preflight,topology_phase,preparation_phase,observation_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> preparation_prepared,observation_prepared;
    topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<ti::Owner,ti::Error> returned;std::unexpected<ti::Error> unexpected;
    std::span<const Entry> binding_entries;std::uint64_t binding_sequence;Target binding_target;std::optional<Producer> binding_expected;
    const Entry* binding_entry;const FailureFact* binding_fact;const run_creation::detail::State* binding_facts;std::uint64_t binding_ordinal;
    ti::detail::Binding binding_out,binding_return;ti::FailureReference binding_reference;es::CreationFailure binding_creation;es::OrdinaryFailure binding_ordinary;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const process_thp_restriction::Owner *observation_this,*subject_this,*ordinal_this,*producer_this,*digest_this,*topology_this;
    const process_thp_restriction::Observation* observation_return;const process_thp_restriction::Subject* subject_return;std::optional<std::uint64_t> ordinal_return;std::optional<process_thp_restriction::Producer> producer_return;const core::Sha256Digest* digest_return;const process_thp_restriction::TopologyBinding* topology_return;
    std::optional<std::uint64_t> binding_process;ti::SubjectAssociation subject_temporary;const Creation* role_parameter;std::optional<process_thp_restriction::Role> role_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    process_thp_restriction::Reason reason_parameter;process_thp_restriction::Producer producer_parameter;
    ti::detail::ResolvedInput* prepare_input;ti::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_process_thp_restriction(std::uint64_t sequence,ti::Limits limits)const -> std::expected<ti::Owner,ti::Error>{
    if(!present_)return std::unexpected{ti::Error::state};
    if(!sequence)return std::unexpected{ti::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);if(!entry||!host)return std::unexpected{ti::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{ti::Error::source};const auto& slot=categories->process_thp;const auto* source=std::get_if<process_thp_restriction::Owner>(&slot);
    auto state=ti::SlotState::not_attempted;std::optional<ti::Reason> missing_reason;ti::detail::Binding preparation{};
    if(source){state=ti::SlotState::retained;if(!source->digest()||!source->observation()||!source->subject()||!source->ordinal()||!source->producer())return std::unexpected{ti::Error::source};}
    else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=ti::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=ti::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=ti::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=ti::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=ti::SlotState::failed;auto resolved=process_thp_failure_binding(entries_,failed->failure_event_sequence,{Category::process_thp},{},{});if(!resolved)return std::unexpected{resolved.error()};preparation=std::move(*resolved);}
    const ti::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?ti::Kind::genesis:ti::Kind::ordinary,sequence,state,preparation.reference,missing_reason};
    auto before=ti::Owner::before_bindings(source,view,limits,ti::detail::session_fixed_stock());if(!before)return std::unexpected{before.error()};
    ti::detail::Binding observation_binding{};std::optional<ti::SubjectAssociation> subject;
    if(source){
        if(!entry->resolved_subject)return std::unexpected{ti::Error::reference};
        const auto* created=find_entry(entries_,entry->resolved_subject->creation_sequence);if(!created)return std::unexpected{ti::Error::reference};const auto created_role=role(created->creation);
        const auto* declared=source->subject();const auto subject_ref=reference(declared->event_id,run_);
        if(!created_role||*created_role!=entry->resolved_subject->role||declared->role!=*created_role||!subject_ref||*subject_ref!=created->sequence)return std::unexpected{ti::Error::reference};
        subject=ti::SubjectAssociation{*created_role,created->sequence};
        const auto* observed=source->observation();if(observed->failure_event){const auto ref=reference(*observed->failure_event,run_);if(!ref||!observed->reason)return std::unexpected{ti::Error::reference};auto resolved=process_thp_failure_binding(entries_,*ref,{Category::process_thp,Selector::process_observation,*source->ordinal(),subject->creation_sequence,reason(*observed->reason)},producer(*source->producer()),subject->creation_sequence);if(!resolved)return std::unexpected{resolved.error()};observation_binding=std::move(*resolved);}
    }
    ti::detail::Staging staging;ti::detail::ResolvedInput input{source,view,subject,&observation_binding,&staging,ti::detail::session_fixed_stock()};auto preflight=ti::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(source&&source->topology()){auto phase=ti::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?ti::Error::capacity:ti::Error::child};staging.children.topology=std::move(*prepared);}
    if(preparation.address){auto phase=ti::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*preparation.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ti::Error::capacity:ti::Error::child};staging.children.preparation_error=std::move(*prepared);}
    if(observation_binding.address){auto phase=ti::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*observation_binding.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ti::Error::capacity:ti::Error::child};staging.children.observation.error=std::move(*prepared);}
    return ti::Owner::prepare(input,limits);
}
namespace {
namespace ai=anonymous_thp_policy_serialization;
auto anonymous_thp_failure_binding(std::span<const Entry> entries,std::uint64_t sequence,Target target,std::optional<Producer> expected_producer)->std::expected<ai::detail::Binding,ai::Error>{
    const auto* entry=find_entry(entries,sequence);const auto* fact=targeted_failure(entry,target);
    if(!entry||!fact||fact->process_sequence||!fact->error.source()||(expected_producer&&fact->producer!=*expected_producer))return std::unexpected{ai::Error::reference};
    ai::detail::Binding out{target,ai::FailureReference{sequence,{}},{}};
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();if(!facts)return std::unexpected{ai::Error::reference};std::uint64_t ordinal{};while(ordinal<facts->failures.size()&&entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==facts->failures.size())return std::unexpected{ai::Error::reference};out.reference->creation_ordinal=ordinal;out.address=es::CreationFailure{sequence,ordinal};}
    else out.address=es::OrdinaryFailure{sequence};
    return out;
}
struct AnonymousThpPolicySerializationSessionCells {
    const Session* session;std::uint64_t sequence;ai::Limits limits;
    const Entry* entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<anonymous_thp_policy_owner::Owner>* slot;const anonymous_thp_policy_owner::Owner* identity;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;
    ai::SlotState state;std::optional<ai::Reason> reason;ai::detail::Binding preparation;
    std::expected<ai::detail::Binding,ai::Error> preparation_resolved,enumeration_resolved,observation_resolved;
    ai::detail::View view,constructor_view;std::expected<void,ai::Error> before;
    std::array<ai::detail::BindingRow,54> bindings;ai::detail::Binding enumeration_binding;std::size_t index,binding_count;const anonymous_thp_policy_owner::OwnedRow* row;std::span<const anonymous_thp_policy_owner::OwnedRow> rows;const anonymous_thp_policy_owner::Enumeration* enumeration;
    std::optional<std::uint64_t> reference_return;
    Target enumeration_target,observation_target;std::optional<Producer> enumeration_producer,observation_producer;std::optional<std::uint64_t> enumeration_reference;
    ai::detail::Staging staging;ai::detail::ResolvedInput handoff;
    const anonymous_thp_policy_owner::Owner* constructor_source;ai::detail::Staging* constructor_staging;std::span<const ai::detail::BindingRow> constructor_bindings;const ai::detail::Binding* constructor_enumeration;std::size_t constructor_fixed;ai::detail::ResolvedInput* constructor_this;
    std::expected<void,ai::Error> preflight,topology_phase,preparation_phase,enumeration_phase,observation_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> preparation_prepared,enumeration_prepared,observation_prepared;
    topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<ai::Owner,ai::Error> returned;std::unexpected<ai::Error> unexpected;
    std::span<const Entry> binding_entries;std::uint64_t binding_sequence;Target binding_target;std::optional<Producer> binding_expected;
    const Entry* binding_entry;const FailureFact* binding_fact;const run_creation::detail::State* binding_facts;std::uint64_t binding_ordinal;
    ai::detail::Binding binding_out,binding_return;ai::FailureReference binding_reference;es::CreationFailure binding_creation;es::OrdinaryFailure binding_ordinary;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const anonymous_thp_policy_owner::Owner *rows_this,*ordinal_this,*producer_this,*digest_this,*topology_this;
    std::span<const anonymous_thp_policy_owner::OwnedRow> rows_return;const anonymous_thp_policy_owner::Owner* enumeration_this;const anonymous_thp_policy_owner::Enumeration* enumeration_return;std::optional<std::uint64_t> ordinal_return;std::optional<anonymous_thp_policy_owner::Producer> producer_return;const core::Sha256Digest* digest_return;const anonymous_thp_policy_owner::TopologyBinding* topology_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    anonymous_thp_policy_owner::Reason reason_parameter;anonymous_thp_policy_owner::Producer producer_parameter;
    std::span<const ai::detail::BindingRow> handoff_span;core::ByteCount default_size_return;std::uint64_t default_size_value;const core::ByteCount* size_this;std::uint64_t size_return;
    ai::detail::ResolvedInput* prepare_input;ai::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_anonymous_thp_policy(std::uint64_t sequence,ai::Limits limits)const -> std::expected<ai::Owner,ai::Error>{
    if(!present_)return std::unexpected{ai::Error::state};
    if(!sequence)return std::unexpected{ai::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);if(!entry||!host)return std::unexpected{ai::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{ai::Error::source};const auto& slot=categories->anonymous_thp;const auto* source=std::get_if<anonymous_thp_policy_owner::Owner>(&slot);
    auto state=ai::SlotState::not_attempted;std::optional<ai::Reason> missing_reason;ai::detail::Binding preparation{};
    if(source){state=ai::SlotState::retained;if(!source->digest()||!source->enumeration()||!source->ordinal()||!source->producer())return std::unexpected{ai::Error::source};}
    else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=ai::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=ai::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=ai::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=ai::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=ai::SlotState::failed;auto resolved=anonymous_thp_failure_binding(entries_,failed->failure_event_sequence,{Category::anonymous_thp},{});if(!resolved)return std::unexpected{resolved.error()};preparation=std::move(*resolved);}
    const ai::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?ai::Kind::genesis:ai::Kind::ordinary,sequence,state,preparation.reference,missing_reason};
    auto before=ai::Owner::before_bindings(source,view,limits,ai::detail::session_fixed_stock());if(!before)return std::unexpected{before.error()};
    std::array<ai::detail::BindingRow,54> bindings{};ai::detail::Binding enumeration_binding{};std::size_t binding_count{};
    if(source){const auto* enumeration=source->enumeration();if(enumeration->failure_event){const auto ref=reference(*enumeration->failure_event,run_);if(!ref||!enumeration->reason)return std::unexpected{ai::Error::reference};auto resolved=anonymous_thp_failure_binding(entries_,*ref,{Category::anonymous_thp,Selector::enumeration,*source->ordinal(),0,reason(*enumeration->reason)},producer(*source->producer()));if(!resolved)return std::unexpected{resolved.error()};enumeration_binding=std::move(*resolved);}
        const auto rows=source->rows();binding_count=rows.size();if(binding_count>bindings.size())return std::unexpected{ai::Error::source};for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];bindings[i].size=row.size;if(row.failure_event){const auto ref=reference(*row.failure_event,run_);if(!ref||!row.reason)return std::unexpected{ai::Error::reference};auto resolved=anonymous_thp_failure_binding(entries_,*ref,{Category::anonymous_thp,Selector::page_size,*source->ordinal(),row.size.value(),reason(*row.reason)},producer(*source->producer()));if(!resolved)return std::unexpected{resolved.error()};bindings[i].failure=std::move(*resolved);}}}
    ai::detail::Staging staging;ai::detail::ResolvedInput input{source,view,std::span<const ai::detail::BindingRow>{bindings.data(),binding_count},&enumeration_binding,&staging,ai::detail::session_fixed_stock()};auto preflight=ai::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(source&&source->topology()){auto phase=ai::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?ai::Error::capacity:ai::Error::child};staging.children.topology=std::move(*prepared);}
    if(preparation.address){auto phase=ai::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*preparation.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ai::Error::capacity:ai::Error::child};staging.children.preparation_error=std::move(*prepared);}
    if(enumeration_binding.address){auto phase=ai::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*enumeration_binding.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ai::Error::capacity:ai::Error::child};staging.children.enumeration.error=std::move(*prepared);}
    for(std::size_t i=0;i<binding_count;++i)if(bindings[i].failure.address){auto phase=ai::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*bindings[i].failure.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ai::Error::capacity:ai::Error::child};staging.children.rows[i].error=std::move(*prepared);}
    return ai::Owner::prepare(input,limits);
}
namespace {
namespace fi=frequency_policy_serialization;
auto frequency_failure_binding(std::span<const Entry> entries,std::uint64_t sequence,Target target,std::optional<Producer> expected_producer)->std::expected<fi::detail::Binding,fi::Error>{
    const auto* entry=find_entry(entries,sequence);const auto* fact=targeted_failure(entry,target);
    if(!entry||!fact||fact->process_sequence||!fact->error.source()||(expected_producer&&fact->producer!=*expected_producer))return std::unexpected{fi::Error::reference};
    fi::detail::Binding out{target,fi::FailureReference{sequence,{}},{}};
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();if(!facts)return std::unexpected{fi::Error::reference};std::uint64_t ordinal{};while(ordinal<facts->failures.size()&&entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==facts->failures.size())return std::unexpected{fi::Error::reference};out.reference->creation_ordinal=ordinal;out.address=es::CreationFailure{sequence,ordinal};}
    else out.address=es::OrdinaryFailure{sequence};
    return out;
}
struct FrequencyPolicySerializationSessionCells {
    const Session* session;std::uint64_t sequence;fi::Limits limits;
    const Entry* entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<frequency_policy_owner::Owner>* slot;const frequency_policy_owner::Owner* identity;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;
    fi::SlotState state;std::optional<fi::Reason> reason;fi::detail::Binding preparation;
    std::expected<fi::detail::Binding,fi::Error> preparation_resolved,observation_resolved;
    fi::detail::View view,constructor_view;std::expected<std::size_t,fi::Error> dynamic;
    std::vector<fi::detail::BindingRow> bindings;std::size_t index;const frequency_policy_owner::Uncovered* row;std::span<const frequency_policy_owner::Uncovered> rows;
    std::optional<std::uint64_t> reference_return;
    Target observation_target;std::optional<Producer> observation_producer;
    fi::detail::Staging staging;fi::detail::ResolvedInput handoff;
    const frequency_policy_owner::Owner* constructor_source;fi::detail::Staging* constructor_staging;std::span<const fi::detail::BindingRow> constructor_bindings;std::size_t constructor_fixed,constructor_dynamic;fi::detail::ResolvedInput* constructor_this;
    std::expected<void,fi::Error> preflight,topology_phase,preparation_phase,observation_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> preparation_prepared,observation_prepared;
    topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<fi::Owner,fi::Error> returned;std::unexpected<fi::Error> unexpected;
    std::span<const Entry> binding_entries;std::uint64_t binding_sequence;Target binding_target;std::optional<Producer> binding_expected;
    const Entry* binding_entry;const FailureFact* binding_fact;const run_creation::detail::State* binding_facts;std::uint64_t binding_ordinal;
    fi::detail::Binding binding_out,binding_return;fi::FailureReference binding_reference;es::CreationFailure binding_creation;es::OrdinaryFailure binding_ordinary;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const frequency_policy_owner::Owner *rows_this,*ordinal_this,*producer_this,*digest_this,*topology_this;
    std::span<const frequency_policy_owner::Uncovered> rows_return;std::optional<std::uint64_t> ordinal_return;std::optional<frequency_policy_owner::Producer> producer_return;const core::Sha256Digest* digest_return;const frequency_policy_owner::TopologyBinding* topology_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    frequency_policy_owner::Reason reason_parameter;frequency_policy_owner::Producer producer_parameter;
    fi::detail::ResolvedInput* prepare_input;fi::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_frequency_policy(std::uint64_t sequence,fi::Limits limits)const -> std::expected<fi::Owner,fi::Error>{
    if(!present_)return std::unexpected{fi::Error::state};
    if(!sequence)return std::unexpected{fi::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);if(!entry||!host)return std::unexpected{fi::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{fi::Error::source};const auto& slot=categories->frequency;const auto* source=std::get_if<frequency_policy_owner::Owner>(&slot);
    auto state=fi::SlotState::not_attempted;std::optional<fi::Reason> missing_reason;fi::detail::Binding preparation{};
    if(source){state=fi::SlotState::retained;if(!source->digest()||!source->topology()||!source->ordinal()||!source->producer())return std::unexpected{fi::Error::source};}
    else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=fi::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=fi::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=fi::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=fi::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=fi::SlotState::failed;auto resolved=frequency_failure_binding(entries_,failed->failure_event_sequence,{Category::frequency},{});if(!resolved)return std::unexpected{resolved.error()};preparation=std::move(*resolved);}
    const fi::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?fi::Kind::genesis:fi::Kind::ordinary,sequence,state,preparation.reference,missing_reason};
    auto dynamic=fi::Owner::before_bindings(source,view,limits,fi::detail::session_fixed_stock());if(!dynamic)return std::unexpected{dynamic.error()};
    std::vector<fi::detail::BindingRow> bindings;
    if(source){const auto rows=source->uncovered_cpus();bindings.resize(rows.size());for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];bindings[i].cpu=row.association.linux_cpu;if(row.failure_event){const auto ref=reference(*row.failure_event,run_);if(!ref||!row.reason)return std::unexpected{fi::Error::reference};auto resolved=frequency_failure_binding(entries_,*ref,{Category::frequency,Selector::uncovered_cpu,*source->ordinal(),row.association.linux_cpu.value(),reason(*row.reason)},producer(*source->producer()));if(!resolved)return std::unexpected{resolved.error()};bindings[i].failure=std::move(*resolved);}}}
    fi::detail::Staging staging;fi::detail::ResolvedInput input{source,view,bindings,&staging,fi::detail::session_fixed_stock(),*dynamic};auto preflight=fi::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(source){auto phase=fi::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?fi::Error::capacity:fi::Error::child};staging.children.topology=std::move(*prepared);}
    if(preparation.address){auto phase=fi::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*preparation.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?fi::Error::capacity:fi::Error::child};staging.children.preparation_error=std::move(*prepared);}
    for(std::size_t i=0;i<bindings.size();++i)if(bindings[i].failure.address){auto phase=fi::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*bindings[i].failure.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?fi::Error::capacity:fi::Error::child};staging.children.rows[i].error=std::move(*prepared);}
    return fi::Owner::prepare(input,limits);
}
namespace {
namespace pi=pu_identity_serialization;
auto pu_failure_binding(std::span<const Entry> entries,std::uint64_t sequence,Target target,std::optional<Producer> expected_producer)->std::expected<pi::detail::Binding,pi::Error>{
    const auto* entry=find_entry(entries,sequence);const auto* fact=targeted_failure(entry,target);
    if(!entry||!fact||fact->process_sequence||!fact->error.source()||(expected_producer&&fact->producer!=*expected_producer))return std::unexpected{pi::Error::reference};
    pi::detail::Binding out{target,pi::FailureReference{sequence,{}},{}};
    if(entry->run_creation){const auto* facts=entry->run_creation->facts();if(!facts)return std::unexpected{pi::Error::reference};std::uint64_t ordinal{};while(ordinal<facts->failures.size()&&entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==facts->failures.size())return std::unexpected{pi::Error::reference};out.reference->creation_ordinal=ordinal;out.address=es::CreationFailure{sequence,ordinal};}
    else out.address=es::OrdinaryFailure{sequence};
    return out;
}
struct PuIdentitySerializationSessionCells {
    const Session* session;std::uint64_t sequence;pi::Limits limits;
    const Entry* entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<pu_cpu_identity::Owner>* slot;const pu_cpu_identity::Owner* identity;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;
    pi::SlotState state;std::optional<pi::Reason> reason;pi::detail::Binding preparation;
    std::expected<pi::detail::Binding,pi::Error> preparation_resolved,observation_resolved;
    pi::detail::View view,constructor_view;std::expected<std::size_t,pi::Error> dynamic;
    std::vector<pi::detail::BindingRow> bindings;std::size_t index,observation_index;const pu_cpu_identity::OwnedRow* row;
    std::array<const pu_cpu_identity::Observation*,2> observations;std::array<Selector,2> selectors;
    const pu_cpu_identity::Observation* observation;std::optional<std::uint64_t> reference_return;
    Target observation_target;std::optional<Producer> observation_producer;
    pi::detail::Staging staging;pi::detail::ResolvedInput handoff;
    const pu_cpu_identity::Owner* constructor_source;pi::detail::Staging* constructor_staging;std::span<const pi::detail::BindingRow> constructor_bindings;std::size_t constructor_fixed,constructor_dynamic;pi::detail::ResolvedInput* constructor_this;
    std::expected<void,pi::Error> preflight,topology_phase,preparation_phase,observation_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> preparation_prepared,observation_prepared;
    topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<pi::Owner,pi::Error> returned;std::unexpected<pi::Error> unexpected;
    std::span<const Entry> binding_entries;std::uint64_t binding_sequence;Target binding_target;std::optional<Producer> binding_expected;
    const Entry* binding_entry;const FailureFact* binding_fact;const run_creation::detail::State* binding_facts;std::uint64_t binding_ordinal;
    pi::detail::Binding binding_out,binding_return;pi::FailureReference binding_reference;es::CreationFailure binding_creation;es::OrdinaryFailure binding_ordinary;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const pu_cpu_identity::Owner *rows_this,*ordinal_this,*producer_this,*digest_this,*topology_this;
    std::span<const pu_cpu_identity::OwnedRow> rows_return;std::optional<std::uint64_t> ordinal_return;std::optional<pu_cpu_identity::Producer> producer_return;const core::Sha256Digest* digest_return;const pu_cpu_identity::TopologyBinding* topology_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    pu_cpu_identity::Reason reason_parameter;pu_cpu_identity::Producer producer_parameter;
    pi::detail::ResolvedInput* prepare_input;pi::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_pu_identity(std::uint64_t sequence,pi::Limits limits)const -> std::expected<pi::Owner,pi::Error>{
    if(!present_)return std::unexpected{pi::Error::state};
    if(!sequence)return std::unexpected{pi::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);if(!entry||!host)return std::unexpected{pi::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{pi::Error::source};const auto& slot=categories->pu_identity;const auto* source=std::get_if<pu_cpu_identity::Owner>(&slot);
    auto state=pi::SlotState::not_attempted;std::optional<pi::Reason> missing_reason;pi::detail::Binding preparation{};
    if(source){state=pi::SlotState::retained;if(!source->digest()||!source->topology()||!source->ordinal()||!source->producer())return std::unexpected{pi::Error::source};}
    else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=pi::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=pi::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=pi::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=pi::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=pi::SlotState::failed;auto resolved=pu_failure_binding(entries_,failed->failure_event_sequence,{Category::pu_identity},{});if(!resolved)return std::unexpected{resolved.error()};preparation=std::move(*resolved);}
    const pi::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?pi::Kind::genesis:pi::Kind::ordinary,sequence,state,preparation.reference,missing_reason};
    auto dynamic=pi::Owner::before_bindings(source,view,limits,pi::detail::session_fixed_stock());if(!dynamic)return std::unexpected{dynamic.error()};
    std::vector<pi::detail::BindingRow> bindings;
    if(source){bindings.resize(source->rows().size());constexpr std::array selectors{Selector::signature,Selector::microcode};for(std::size_t i=0;i<bindings.size();++i){const auto& row=source->rows()[i];bindings[i].cpu=row.linux_cpu;const std::array<const pu_cpu_identity::Observation*,2> observations{&row.signature,&row.microcode};for(std::size_t j=0;j<2;++j){const auto& observed=*observations[j];if(observed.failure_event){const auto ref=reference(*observed.failure_event,run_);if(!ref||!observed.reason)return std::unexpected{pi::Error::reference};auto resolved=pu_failure_binding(entries_,*ref,{Category::pu_identity,selectors[j],*source->ordinal(),row.linux_cpu.value(),reason(*observed.reason)},producer(*source->producer()));if(!resolved)return std::unexpected{resolved.error()};bindings[i].slots[j]=std::move(*resolved);}}}}
    pi::detail::Staging staging;pi::detail::ResolvedInput input{source,view,bindings,&staging,pi::detail::session_fixed_stock(),*dynamic};auto preflight=pi::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(source){auto phase=pi::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?pi::Error::capacity:pi::Error::child};staging.children.topology=std::move(*prepared);}
    if(preparation.address){auto phase=pi::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*preparation.address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?pi::Error::capacity:pi::Error::child};staging.children.preparation_error=std::move(*prepared);}
    for(std::size_t i=0;i<bindings.size();++i)for(std::size_t j=0;j<2;++j)if(bindings[i].slots[j].address){auto phase=pi::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*bindings[i].slots[j].address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?pi::Error::capacity:pi::Error::child};staging.children.rows[i].errors[j]=std::move(*prepared);}
    return pi::Owner::prepare(input,limits);
}
namespace {
namespace ki=kernel_identity_serialization;
struct KernelIdentitySerializationSessionCells {
    const Session* session;std::uint64_t sequence;ki::Limits limits;
    const Entry *entry,*failure_entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<private_kernel_identity::Owner>* slot;const private_kernel_identity::Owner* kernel_identity;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;ki::SlotState state;std::optional<ki::Reason> reason;
    std::array<std::optional<ki::FailureReference>,4> failures;std::array<std::optional<es::Address>,4> addresses;
    std::array<Target,4> targets;std::array<std::optional<std::uint64_t>,4> sequences;
    std::array<Selector,3> observation_selectors;
    std::size_t index;const private_kernel_identity::Observation* observation;std::optional<std::uint64_t> reference_return;
    const FailureFact* fact;const run_creation::detail::State* failure_state;std::uint64_t ordinal;
    ki::FailureReference reference_temporary;es::CreationFailure creation_address;es::OrdinaryFailure ordinary_address;
    ki::detail::Children children;ki::detail::View view,constructor_view;ki::detail::ResolvedInput handoff;
    const private_kernel_identity::Owner* constructor_kernel_identity;ki::detail::Children* constructor_children;std::size_t constructor_fixed;ki::detail::ResolvedInput* constructor_this;
    std::expected<void,ki::Error> preflight,topology_phase,error_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> error_prepared;topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<ki::Owner,ki::Error> returned;std::unexpected<ki::Error> unexpected;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const private_kernel_identity::Owner *observations_this,*ordinal_this,*producer_this,*topology_this;
    std::span<const private_kernel_identity::Observation> observations_return;std::optional<std::uint64_t> ordinal_return;std::optional<private_kernel_identity::Producer> producer_return;const private_kernel_identity::TopologyBinding* topology_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    private_kernel_identity::Reason reason_parameter;private_kernel_identity::Producer producer_parameter;
    Target target_temporary;ki::detail::ResolvedInput* prepare_input;ki::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_kernel_identity(std::uint64_t sequence,ki::Limits limits)const -> std::expected<ki::Owner,ki::Error>{
    if(!present_)return std::unexpected{ki::Error::state};
    if(!sequence)return std::unexpected{ki::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);
    if(!entry||!host)return std::unexpected{ki::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{ki::Error::source};
    const auto& slot=categories->kernel_identity;const auto* kernel_identity=std::get_if<private_kernel_identity::Owner>(&slot);
    auto state=ki::SlotState::not_attempted;std::optional<ki::Reason> missing_reason;
    std::array<std::optional<ki::FailureReference>,4> failures;std::array<std::optional<es::Address>,4> addresses;
    std::array<Target,4> targets{};std::array<std::optional<std::uint64_t>,4> sequences;
    if(kernel_identity){state=ki::SlotState::retained;if(!kernel_identity->ordinal()||!kernel_identity->producer()||kernel_identity->observations().size()!=3)return std::unexpected{ki::Error::source};
        constexpr std::array selectors{Selector::sysname,Selector::release,Selector::version};
        for(std::size_t i=0;i<3;++i){const auto& observed=kernel_identity->observations()[i];if(observed.failure_event){sequences[i+1]=reference(*observed.failure_event,run_);if(!sequences[i+1]||!observed.reason)return std::unexpected{ki::Error::reference};targets[i+1]={Category::kernel_identity,selectors[i],*kernel_identity->ordinal(),0,reason(*observed.reason)};}}
    }else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=ki::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=ki::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=ki::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=ki::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=ki::SlotState::failed;sequences[0]=failed->failure_event_sequence;targets[0]={Category::kernel_identity};}
    for(std::size_t i=0;i<4;++i)if(sequences[i]){
        const auto* failure_entry=find_entry(entries_,*sequences[i]);const auto* fact=targeted_failure(failure_entry,targets[i]);
        if(!failure_entry||!fact||fact->process_sequence||!fact->error.source()||(i&&fact->producer!=producer(*kernel_identity->producer())))return std::unexpected{ki::Error::reference};
        failures[i]=ki::FailureReference{*sequences[i],{}};
        if(failure_entry->run_creation){const auto* failure_state=failure_entry->run_creation->facts();std::uint64_t ordinal{};if(!failure_state)return std::unexpected{ki::Error::reference};while(ordinal<failure_state->failures.size()&&failure_entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==failure_state->failures.size())return std::unexpected{ki::Error::reference};failures[i]->creation_ordinal=ordinal;addresses[i]=es::CreationFailure{failure_entry->sequence,ordinal};}
        else addresses[i]=es::OrdinaryFailure{failure_entry->sequence};
    }
    ki::detail::Children children;const ki::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?ki::Kind::genesis:ki::Kind::ordinary,sequence,state,failures,missing_reason};
    ki::detail::ResolvedInput input{kernel_identity,view,&children,ki::detail::session_fixed_stock()};
    auto preflight=ki::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(kernel_identity&&kernel_identity->topology()){auto phase=ki::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?ki::Error::capacity:ki::Error::child};children.topology=std::move(*prepared);}
    for(std::size_t i=0;i<4;++i)if(addresses[i]){auto phase=ki::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*addresses[i],limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ki::Error::capacity:ki::Error::child};children.errors[i]=std::move(*prepared);}
    return ki::Owner::prepare(input,limits);
}
namespace {
namespace cs=system_controls_serialization;
struct SystemControlsSerializationSessionCells {
    const Session* session;std::uint64_t sequence;cs::Limits limits;
    const Entry *entry,*failure_entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<host_controls::Owner>* slot;const host_controls::Owner* controls;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;cs::SlotState state;std::optional<cs::Reason> reason;
    std::array<std::optional<cs::FailureReference>,6> failures;std::array<std::optional<es::Address>,6> addresses;
    std::array<Target,6> targets;std::array<std::optional<std::uint64_t>,6> sequences;
    std::array<Selector,5> observation_selectors;
    std::size_t index;const host_controls::Observation* observation;std::optional<std::uint64_t> reference_return;
    const FailureFact* fact;const run_creation::detail::State* failure_state;std::uint64_t ordinal;
    cs::FailureReference reference_temporary;es::CreationFailure creation_address;es::OrdinaryFailure ordinary_address;
    cs::detail::Children children;cs::detail::View view,constructor_view;cs::detail::ResolvedInput handoff;
    const host_controls::Owner* constructor_controls;cs::detail::Children* constructor_children;std::size_t constructor_fixed;cs::detail::ResolvedInput* constructor_this;
    std::expected<void,cs::Error> preflight,topology_phase,error_phase;
    std::expected<topology_serialization::Owner,topology_serialization::Error> topology_prepared;
    std::expected<es::Owner,es::Error> error_prepared;topology_serialization::Limits topology_limits_copy;es::Limits error_limits_copy;es::Address error_address_copy;
    std::expected<cs::Owner,cs::Error> returned;std::unexpected<cs::Error> unexpected;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;const Entry* comparator_entry;std::uint64_t comparator_sequence;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const Entry* targeted_entry;const Target* targeted_target;const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const host_controls::Owner *observations_this,*ordinal_this,*producer_this,*topology_this;
    std::span<const host_controls::Observation> observations_return;std::optional<std::uint64_t> ordinal_return;std::optional<host_controls::Producer> producer_return;const host_controls::TopologyBinding* topology_return;
    std::string_view reference_text,reference_run;std::uint64_t reference_value;std::from_chars_result reference_converted;
    host_controls::Reason reason_parameter;host_controls::Producer producer_parameter;
    Target target_temporary;cs::detail::ResolvedInput* prepare_input;cs::Limits prepare_limits_copy;std::size_t fixed_return;
};
}
auto Session::serialize_system_controls(std::uint64_t sequence,cs::Limits limits)const -> std::expected<cs::Owner,cs::Error>{
    if(!present_)return std::unexpected{cs::Error::state};
    if(!sequence)return std::unexpected{cs::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);
    if(!entry||!host)return std::unexpected{cs::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{cs::Error::source};
    const auto& slot=categories->system_controls;const auto* controls=std::get_if<host_controls::Owner>(&slot);
    auto state=cs::SlotState::not_attempted;std::optional<cs::Reason> missing_reason;
    std::array<std::optional<cs::FailureReference>,6> failures;std::array<std::optional<es::Address>,6> addresses;
    std::array<Target,6> targets{};std::array<std::optional<std::uint64_t>,6> sequences;
    if(controls){state=cs::SlotState::retained;if(!controls->ordinal()||!controls->producer()||controls->observations().size()!=5)return std::unexpected{cs::Error::source};
        constexpr std::array selectors{Selector::smt_control,Selector::smt_active,Selector::numa_mask,Selector::thp_enabled,Selector::thp_defrag};
        for(std::size_t i=0;i<5;++i){const auto& observed=controls->observations()[i];if(observed.failure_event){sequences[i+1]=reference(*observed.failure_event,run_);if(!sequences[i+1]||!observed.reason)return std::unexpected{cs::Error::reference};targets[i+1]={Category::system_controls,selectors[i],*controls->ordinal(),0,reason(*observed.reason)};}}
    }else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){state=cs::SlotState::unavailable;switch(unavailable->reason){case host_assembly::Reason::not_supported:missing_reason=cs::Reason::not_supported;break;case host_assembly::Reason::source_missing:missing_reason=cs::Reason::source_missing;break;case host_assembly::Reason::access_denied:missing_reason=cs::Reason::access_denied;break;}}
    else if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){state=cs::SlotState::failed;sequences[0]=failed->failure_event_sequence;targets[0]={Category::system_controls};}
    for(std::size_t i=0;i<6;++i)if(sequences[i]){
        const auto* failure_entry=find_entry(entries_,*sequences[i]);const auto* fact=targeted_failure(failure_entry,targets[i]);
        if(!failure_entry||!fact||fact->process_sequence||!fact->error.source()||(i&&fact->producer!=producer(*controls->producer())))return std::unexpected{cs::Error::reference};
        failures[i]=cs::FailureReference{*sequences[i],{}};
        if(failure_entry->run_creation){const auto* failure_state=failure_entry->run_creation->facts();std::uint64_t ordinal{};if(!failure_state)return std::unexpected{cs::Error::reference};while(ordinal<failure_state->failures.size()&&failure_entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==failure_state->failures.size())return std::unexpected{cs::Error::reference};failures[i]->creation_ordinal=ordinal;addresses[i]=es::CreationFailure{failure_entry->sequence,ordinal};}
        else addresses[i]=es::OrdinaryFailure{failure_entry->sequence};
    }
    cs::detail::Children children;const cs::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?cs::Kind::genesis:cs::Kind::ordinary,sequence,state,failures,missing_reason};
    cs::detail::ResolvedInput input{controls,view,&children,cs::detail::session_fixed_stock()};
    auto preflight=cs::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(controls&&controls->topology()){auto phase=cs::Owner::child_phase(input,limits,true);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_topology(sequence,limits.topology);if(!prepared)return std::unexpected{prepared.error()==topology_serialization::Error::capacity?cs::Error::capacity:cs::Error::child};children.topology=std::move(*prepared);}
    for(std::size_t i=0;i<6;++i)if(addresses[i]){auto phase=cs::Owner::child_phase(input,limits,false);if(!phase)return std::unexpected{phase.error()};auto prepared=serialize_error(*addresses[i],limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?cs::Error::capacity:cs::Error::child};children.errors[i]=std::move(*prepared);}
    return cs::Owner::prepare(input,limits);
}
namespace {
namespace ts=topology_serialization;
struct TopologySerializationSessionCells {
    const Session* session;std::uint64_t sequence;ts::Limits limits;
    const Entry *entry,*failure_entry;const host_assembly::Owner* host;const host_assembly::Categories* categories;
    const host_assembly::Slot<topology_evidence::Owner>* slot;ts::SlotState state;std::optional<ts::Reason> reason;
    const host_assembly::Unavailable* unavailable;const host_assembly::Failed* failed;const topology_evidence::Owner* topology;
    const FailureFact* fact;const run_creation::detail::State* failure_state;
    std::optional<ts::FailureReference> failure;std::optional<es::Owner> child;std::optional<es::Address> child_address;
    Target target;std::uint64_t ordinal;
    ts::detail::View view,constructor_view;ts::detail::ResolvedInput handoff;
    const topology_evidence::Owner* constructor_topology;std::optional<es::Owner>* constructor_child;
    std::size_t constructor_fixed,fixed_return;ts::detail::ResolvedInput* constructor_this;
    std::expected<void,ts::Error> preflight;std::expected<es::Owner,es::Error> prepared;
    es::CreationFailure creation_address;es::OrdinaryFailure ordinary_address;es::Address child_address_copy;es::Limits child_limits;
    ts::FailureReference failure_temporary;std::expected<ts::Owner,ts::Error> returned;std::unexpected<ts::Error> unexpected;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;
    const Entry* find_comparator_entry;std::uint64_t find_comparator_sequence;
    const Entry* targeted_entry;const Target* targeted_target;
    const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;
    std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const Entry* host_parameter;const host_assembly::Owner* host_return;const run_creation::detail::State* host_facts;
    const host_assembly::Owner* categories_this;const host_assembly::Categories* categories_return;
    const private_error::Owner* error_this;const core::Error* error_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const ts::detail::ResolvedInput* prepare_input;ts::Limits prepare_limits;
};
}
auto Session::serialize_topology(std::uint64_t sequence,ts::Limits limits)const -> std::expected<ts::Owner,ts::Error>{
    if(!present_)return std::unexpected{ts::Error::state};
    if(!sequence)return std::unexpected{ts::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);
    if(!entry||!host)return std::unexpected{ts::Error::reference};
    const auto* categories=host->categories();if(!categories)return std::unexpected{ts::Error::source};
    const auto& slot=categories->topology;const auto* topology=std::get_if<topology_evidence::Owner>(&slot);
    auto state=ts::SlotState::not_attempted;std::optional<ts::Reason> reason;
    if(topology)state=ts::SlotState::retained;
    else if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&slot)){
        state=ts::SlotState::unavailable;
        switch(unavailable->reason){case host_assembly::Reason::not_supported:reason=ts::Reason::not_supported;break;case host_assembly::Reason::source_missing:reason=ts::Reason::source_missing;break;case host_assembly::Reason::access_denied:reason=ts::Reason::access_denied;break;}
    }
    std::optional<ts::FailureReference> failure;std::optional<es::Owner> child;std::optional<es::Address> child_address;
    if(const auto* failed=std::get_if<host_assembly::Failed>(&slot)){
        state=ts::SlotState::failed;constexpr Target target{Category::topology};
        const auto* failure_entry=find_entry(entries_,failed->failure_event_sequence);const auto* fact=targeted_failure(failure_entry,target);
        if(!failure_entry||!fact||fact->process_sequence||!fact->error.source())return std::unexpected{ts::Error::reference};
        failure=ts::FailureReference{failed->failure_event_sequence,{}};
        if(failure_entry->run_creation){const auto* failure_state=failure_entry->run_creation->facts();std::uint64_t ordinal{};if(!failure_state)return std::unexpected{ts::Error::reference};while(ordinal<failure_state->failures.size()&&failure_entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==failure_state->failures.size())return std::unexpected{ts::Error::reference};failure->creation_ordinal=ordinal;child_address=es::CreationFailure{failure_entry->sequence,ordinal};}
        else child_address=es::OrdinaryFailure{failure_entry->sequence};
    }
    const ts::detail::View view{run_,entry->timestamp,domain_,entry->producer,entry->run_creation?ts::Kind::genesis:ts::Kind::ordinary,sequence,state,failure,reason};
    const ts::detail::ResolvedInput input{topology,view,&child,ts::detail::session_fixed_stock()};
    auto preflight=ts::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(child_address){auto prepared=serialize_error(*child_address,limits.child);if(!prepared)return std::unexpected{prepared.error()==es::Error::capacity?ts::Error::capacity:ts::Error::child};child=std::move(*prepared);}
    return ts::Owner::prepare(input,limits);
}
namespace {
namespace bs=base_build_serialization;
struct BaseBuildSerializationSessionCells {
    const Session* session;std::uint64_t sequence;bs::Limits limits;
    const Entry *entry,*failure_entry;const BaseBuild* build;const build_manifest::Owner* manifest;
    const FailureFact* fact;const run_creation::detail::State* failure_state;
    std::optional<bs::Acquisition> acquisition;std::optional<es::Owner> child;std::optional<es::Address> child_address;
    Target target;std::uint64_t ordinal;
    bs::detail::View view,constructor_view;bs::detail::ResolvedInput handoff;
    const build_manifest::Owner* constructor_manifest;std::optional<es::Owner>* constructor_child;
    std::size_t constructor_fixed,fixed_return;bs::detail::ResolvedInput* constructor_this;
    std::expected<void,bs::Error> preflight;std::expected<es::Owner,es::Error> prepared;
    es::CreationFailure creation_address;es::OrdinaryFailure ordinary_address;es::Address child_address_copy;es::Limits child_limits;
    bs::Acquisition acquisition_temporary;std::expected<bs::Owner,bs::Error> returned;std::unexpected<bs::Error> unexpected;
    CreationLookupCells lookup;
    std::span<const Entry> find_entries;std::uint64_t find_sequence;std::span<const Entry>::iterator find_result;
    const Entry* find_comparator_entry;std::uint64_t find_comparator_sequence;
    const Entry* targeted_entry;const Target* targeted_target;
    const FailureFact* has_target_fact;std::span<const std::size_t> has_target_index;const Target* has_target_key;
    std::span<const std::size_t>::iterator has_target_found;
    std::size_t has_target_comparator_index;const Target* has_target_comparator_key;
    const Entry* base_parameter;const BaseBuild* base_return;const run_creation::detail::State* base_facts;
    const private_error::Owner* error_this;const core::Error* error_return;
    const run_creation::Owner* failure_at_this;std::uint64_t failure_at_ordinal;const FailureFact* failure_at_return;
    const bs::detail::ResolvedInput* prepare_input;bs::Limits prepare_limits;
};
}
auto Session::serialize_base_build(std::uint64_t sequence,bs::Limits limits)const -> std::expected<bs::Owner,bs::Error>{
    if(!present_)return std::unexpected{bs::Error::state};
    if(!sequence)return std::unexpected{bs::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* build=entry_base_build(entry);
    if(!entry||entry->producer!=Producer::controller||!build)return std::unexpected{bs::Error::reference};
    const auto* manifest=build->manifest?&*build->manifest:nullptr;
    std::optional<bs::Acquisition> acquisition;std::optional<es::Owner> child;std::optional<es::Address> child_address;
    if(build->acquisition_failure){
        constexpr Target target{Category::base_build,Selector::base_build_acquisition};
        const auto* failure_entry=find_entry(entries_,*build->acquisition_failure);const auto* fact=targeted_failure(failure_entry,target);
        if(!failure_entry||!fact||fact->producer!=Producer::controller||fact->process_sequence||!fact->error.source())return std::unexpected{bs::Error::reference};
        acquisition=bs::Acquisition{*build->acquisition_failure,{}};
        if(failure_entry->run_creation){const auto* failure_state=failure_entry->run_creation->facts();std::uint64_t ordinal{};if(!failure_state)return std::unexpected{bs::Error::reference};while(ordinal<failure_state->failures.size()&&failure_entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==failure_state->failures.size())return std::unexpected{bs::Error::reference};acquisition->creation_ordinal=ordinal;child_address=es::CreationFailure{failure_entry->sequence,ordinal};}
        else child_address=es::OrdinaryFailure{failure_entry->sequence};
    }
    const bs::detail::View view{run_,entry->timestamp,domain_,entry->run_creation?bs::Kind::genesis:bs::Kind::ordinary,sequence,build->state,acquisition};
    const bs::detail::ResolvedInput input{manifest,view,&child,bs::detail::session_fixed_stock()};
    auto preflight=bs::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(child_address){auto prepared=serialize_error(*child_address,limits.child);if(!prepared)return std::unexpected{bs::Error::child};child=std::move(*prepared);}
    return bs::Owner::prepare(input,limits);
}
namespace {
namespace ns = normalization_serialization;
struct NormalizationSerializationSessionCells {
    const Session* session; std::uint64_t sequence; ns::Limits limits;
    const Entry* entry; const authored_source::ResolutionRecord* record;
    std::uint64_t topology_sequence; ns::detail::View view;
    std::optional<ss::Owner> source; std::optional<private_plan_composition::Owner> plan;
    std::optional<ts::Owner> topology; ns::detail::ResolvedInput input;
    ss::NormalizationSource source_address; ss::Address address;
    ss::Limits source_limits; private_plan_composition::Limits plan_limits; ts::Limits topology_limits;
    std::expected<ss::Owner, ss::Error> prepared_source;
    std::expected<private_plan_composition::Owner, private_plan_composition::Error> prepared_plan;
    std::expected<ts::Owner, ts::Error> prepared_topology;
    std::expected<void, ns::Error> preflight;
    std::expected<ns::Owner, ns::Error> returned; std::unexpected<ns::Error> unexpected;
    ns::Error mapped; std::size_t fixed_return;
};
struct NormalizationFindEntryCells {
    std::span<const Entry> find_entries; std::uint64_t find_sequence;
    std::span<const Entry>::iterator find_result;
    const Entry* find_comparator_entry; std::uint64_t find_comparator_sequence;
};
}
auto Session::serialize_normalization(std::uint64_t sequence, ns::Limits limits) const
    -> std::expected<ns::Owner, ns::Error> {
    if (!present_) return std::unexpected{ns::Error::state};
    if (sequence <= 1) return std::unexpected{ns::Error::reference};
    const auto* entry = find_entry(entries_, sequence);
    if (!entry || entry->producer != Producer::controller || !entry->normalization)
        return std::unexpected{ns::Error::reference};
    const auto* record = &entry->normalization->source;
    std::optional<ss::Owner> source; std::optional<private_plan_composition::Owner> plan;
    std::optional<ts::Owner> topology;
    const ns::detail::View view{run_, entry->timestamp, domain_, sequence, entry->normalization->topology_event_sequence};
    const ns::detail::ResolvedInput input{record, view, &source, &plan, &topology, ns::detail::session_fixed_stock()};
    auto preflight = ns::Owner::preflight(input, limits);
    if (!preflight) return std::unexpected{preflight.error()};
    auto prepared_source = serialize_source(ss::NormalizationSource{sequence}, limits.source);
    if (!prepared_source) return std::unexpected{prepared_source.error() == ss::Error::capacity ? ns::Error::capacity :
        prepared_source.error() == ss::Error::invalid_limits ? ns::Error::invalid_limits : ns::Error::child};
    source = std::move(*prepared_source);
    preflight = ns::Owner::preflight(input, limits);
    if (!preflight) return std::unexpected{preflight.error()};
    auto prepared_plan = private_plan_composition::Owner::capture(*record->plan(), limits.plan);
    if (!prepared_plan) return std::unexpected{prepared_plan.error() == private_plan_composition::Error::capacity ? ns::Error::capacity :
        prepared_plan.error() == private_plan_composition::Error::invalid_limits ? ns::Error::invalid_limits : ns::Error::child};
    plan = std::move(*prepared_plan);
    preflight = ns::Owner::preflight(input, limits);
    if (!preflight) return std::unexpected{preflight.error()};
    auto prepared_topology = serialize_topology(view.topology_sequence, limits.topology);
    if (!prepared_topology) return std::unexpected{prepared_topology.error() == ts::Error::capacity ? ns::Error::capacity :
        prepared_topology.error() == ts::Error::invalid_limits ? ns::Error::invalid_limits : ns::Error::child};
    topology = std::move(*prepared_topology);
    return ns::Owner::prepare(input, limits);
}
auto Session::serialize_source_outcome(std::uint64_t sequence,so::Limits limits)const -> std::expected<so::Owner,so::Error>{
    if(!present_)return std::unexpected{so::Error::state};
    if(!sequence)return std::unexpected{so::Error::reference};
    const auto* entry=find_entry(entries_,sequence);
    if(!entry||entry->producer!=Producer::controller||!entry->source_outcome)return std::unexpected{so::Error::reference};
    const auto* record=&entry->source_outcome->source;
    std::optional<ss::Owner> source;std::optional<es::Owner> upstream;
    const so::detail::View view{run_,entry->timestamp,domain_,sequence};
    const so::detail::ResolvedInput input{record,view,&source,&upstream,so::detail::session_fixed_stock()};
    auto preflight=so::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    auto prepared_source=serialize_source(ss::OutcomeSource{sequence},limits.source);
    if(!prepared_source)return std::unexpected{prepared_source.error()==ss::Error::capacity?so::Error::capacity:so::Error::child};
    source=std::move(*prepared_source);
    auto upstream_preflight=so::Owner::upstream_preflight(input,limits);if(!upstream_preflight)return std::unexpected{upstream_preflight.error()};
    if(record->state()==authored_source::State::resolution_failed){auto prepared_upstream=serialize_error(es::SourceResolutionFailure{sequence},limits.upstream);if(!prepared_upstream)return std::unexpected{prepared_upstream.error()==es::Error::capacity?so::Error::capacity:so::Error::child};upstream=std::move(*prepared_upstream);}
    return so::Owner::prepare(input,limits);
}
auto Session::serialize_source(ss::Address address,ss::Limits limits)const -> std::expected<ss::Owner,ss::Error>{
    if(!present_)return std::unexpected{ss::Error::state};
    if(address.valueless_by_exception())return std::unexpected{ss::Error::reference};
    std::uint64_t sequence{};ss::Kind kind{};
    if(const auto* creation=std::get_if<ss::CreationSource>(&address)){sequence=creation->sequence;kind=ss::Kind::creation_source;}
    else if(const auto* normalization=std::get_if<ss::NormalizationSource>(&address)){sequence=normalization->sequence;kind=ss::Kind::normalization_source;}
    else if(const auto* outcome=std::get_if<ss::OutcomeSource>(&address)){sequence=outcome->sequence;kind=ss::Kind::outcome_source;}
    if(!sequence)return std::unexpected{ss::Error::reference};
    const auto* entry=find_entry(entries_,sequence);if(!entry||entry->producer!=Producer::controller)return std::unexpected{ss::Error::reference};
    const authored_source::SourceBundle* source=nullptr;
    if(kind==ss::Kind::creation_source){if(sequence!=1||!entry->run_creation)return std::unexpected{ss::Error::reference};const auto* genesis=entry->run_creation->facts();if(genesis&&genesis->source)source=&*genesis->source;}
    else if(kind==ss::Kind::normalization_source){if(!entry->normalization)return std::unexpected{ss::Error::reference};const auto* record=&entry->normalization->source;if(record->state()==authored_source::State::resolved&&record->plan())source=record->source();}
    else{if(!entry->source_outcome)return std::unexpected{ss::Error::reference};const auto* record=&entry->source_outcome->source;if(record->state()!=authored_source::State::resolved&&!record->plan())source=record->source();}
    if(!source||!source->logical_bytes())return std::unexpected{ss::Error::reference};
    const auto source_input=source->input();std::optional<ss::Acquisition> acquisition;std::optional<es::Owner> child;std::optional<es::Address> child_address;
    if(source_input.acquisition_failure){
        constexpr Target target{Category::authored_source,Selector::source_acquisition};
        const auto* failure_entry=find_entry(entries_,source_input.acquisition_failure->value);const auto* fact=targeted_failure(failure_entry,target);
        if(!failure_entry||!fact||fact->producer!=Producer::controller||fact->process_sequence||!fact->error.source())return std::unexpected{ss::Error::reference};
        acquisition=ss::Acquisition{source_input.acquisition_failure->value,{}};
        if(failure_entry->run_creation){const auto* failure_state=failure_entry->run_creation->facts();std::uint64_t ordinal{};if(!failure_state)return std::unexpected{ss::Error::reference};while(ordinal<failure_state->failures.size()&&failure_entry->run_creation->failure_at(ordinal)!=fact)++ordinal;if(ordinal==failure_state->failures.size())return std::unexpected{ss::Error::reference};acquisition->creation_ordinal=ordinal;child_address=es::CreationFailure{failure_entry->sequence,ordinal};}
        else child_address=es::OrdinaryFailure{failure_entry->sequence};
    }
    const ss::detail::View view{run_,entry->timestamp,domain_,kind,sequence,acquisition};
    const ss::detail::ResolvedInput input{source,view,&child,ss::detail::session_fixed_stock()};
    auto preflight=ss::Owner::preflight(input,limits);if(!preflight)return std::unexpected{preflight.error()};
    if(child_address){auto prepared=serialize_error(*child_address,limits.child);if(!prepared)return std::unexpected{ss::Error::child};child=std::move(*prepared);}
    return ss::Owner::prepare(input,limits);
}
auto Session::serialize_error(es::Address address,es::Limits limits) const
    -> std::expected<es::Owner,es::Error> {
    if(!present_)return std::unexpected{es::Error::state};
    if(address.valueless_by_exception())return std::unexpected{es::Error::reference};
    std::uint64_t sequence{};es::Kind kind{};std::optional<std::uint64_t> ordinal;
    if(const auto* ordinary=std::get_if<es::OrdinaryFailure>(&address)){sequence=ordinary->sequence;kind=es::Kind::ordinary_failure;}
    else if(const auto* creation=std::get_if<es::CreationFailure>(&address)){sequence=creation->sequence;ordinal=creation->ordinal;kind=es::Kind::creation_failure;}
    else if(const auto* source=std::get_if<es::SourceResolutionFailure>(&address)){sequence=source->sequence;kind=es::Kind::source_resolution;}
    if(!sequence)return std::unexpected{es::Error::reference};
    const auto* entry=find_entry(entries_,sequence);
    if(!entry)return std::unexpected{es::Error::reference};
    const FailureFact* fact=nullptr;const private_error::Owner* error=nullptr;
    if(kind==es::Kind::ordinary_failure){if(entry->run_creation || !entry->failure)return std::unexpected{es::Error::reference};fact=&*entry->failure;error=&fact->error;}
    else if(kind==es::Kind::creation_failure){if(sequence!=1 || !entry->run_creation)return std::unexpected{es::Error::reference};fact=entry->run_creation->failure_at(*ordinal);if(!fact)return std::unexpected{es::Error::reference};error=&fact->error;}
    else{if(!entry->source_outcome)return std::unexpected{es::Error::reference};const auto* record=&entry->source_outcome->source;if(record->state()!=authored_source::State::resolution_failed || !record->source())return std::unexpected{es::Error::reference};error=record->upstream_error();}
    if(!error || !error->source())return std::unexpected{es::Error::reference};
    const es::detail::View view{run_,entry->timestamp,domain_,entry->producer,kind,sequence,ordinal,fact};
    const es::detail::ResolvedInput input{error,view,es::detail::session_fixed_stock()};
    return es::Owner::prepare(input,limits);
}
auto Session::project_base_build(std::uint64_t sequence,bp::Limits limits) const -> std::expected<bp::Owner,bp::Error> {
    if(!present_)return std::unexpected{bp::Error::state};
    if(!sequence)return std::unexpected{bp::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* build=entry_base_build(entry);
    if(!entry || !build)return std::unexpected{bp::Error::reference};
    const auto* manifest=build->manifest?&*build->manifest:nullptr;
    bp::Disposition disposition;
    switch(build->state) {
    case BuildState::not_attempted:
        if(manifest || build->acquisition_failure)return std::unexpected{bp::Error::source};
        disposition=bp::Disposition::not_attempted;break;
    case BuildState::not_obtained:
        if(manifest || !build->acquisition_failure)return std::unexpected{bp::Error::source};
        disposition=bp::Disposition::not_obtained;break;
    case BuildState::available:
        if(!manifest || !manifest->digest() || build->acquisition_failure)return std::unexpected{bp::Error::source};
        disposition=bp::Disposition::available;break;
    default:return std::unexpected{bp::Error::source};
    }
    if(!bp::detail::valid_limits(limits))return std::unexpected{bp::Error::invalid_limits};
    std::optional<bp::FailureAddress> failure;
    if(build->acquisition_failure) {
        if(!*build->acquisition_failure || *build->acquisition_failure>sequence)return std::unexpected{bp::Error::reference};
        const auto* failure_entry=find_entry(entries_,*build->acquisition_failure);
        const Target target{Category::base_build,Selector::base_build_acquisition};
        const auto* fact=targeted_failure(failure_entry,target);
        if(!fact || fact->producer!=Producer::controller || fact->process_sequence ||
            !detail::valid_target(target,*fact))return std::unexpected{bp::Error::reference};
        std::optional<std::uint64_t> ordinal;
        if(failure_entry->run_creation) {
            const auto* genesis=failure_entry->run_creation->facts();
            if(!genesis)return std::unexpected{bp::Error::reference};
            for(std::size_t i=0;i<genesis->failures.size();++i)if(&genesis->failures[i]==fact)ordinal=i;
            if(!ordinal || failure_entry->run_creation->failure_at(*ordinal)!=fact)return std::unexpected{bp::Error::reference};
        }
        failure=bp::FailureAddress{*build->acquisition_failure,ordinal};
    }
    bp::Domain domain;
    switch(domain_) {
    case Domain::supplied_observation:domain=bp::Domain::supplied_observation;break;
    case Domain::synthetic_execution:domain=bp::Domain::synthetic_execution;break;
    default:return std::unexpected{bp::Error::source};
    }
    const bp::detail::ResolvedInput input{manifest,disposition,run_,entry->timestamp,domain,
        entry->run_creation?bp::Origin::genesis:bp::Origin::ordinary,sequence,failure,bp::detail::session_fixed_stock()};
    return bp::Owner::prepare(input,limits);
}
auto Session::project_host(std::uint64_t sequence,hp::Limits limits) const -> std::expected<hp::Owner,hp::Error> {
    if(!present_)return std::unexpected{hp::Error::state};
    if(!sequence)return std::unexpected{hp::Error::reference};
    const auto* entry=find_entry(entries_,sequence);const auto* host=entry_host(entry);
    if(!entry || !host || !host->categories())return std::unexpected{hp::Error::reference};
    if(!hp::detail::valid_limits(limits))return std::unexpected{hp::Error::invalid_limits};
    try {
        std::optional<hp::ResolvedSubject> subject;
        if(entry->resolved_subject) {
            switch(entry->resolved_subject->role) {
            case process_thp_restriction::Role::controller:subject=hp::ResolvedSubject{hp::Role::controller,entry->resolved_subject->creation_sequence};break;
            case process_thp_restriction::Role::runner:subject=hp::ResolvedSubject{hp::Role::runner,entry->resolved_subject->creation_sequence};break;
            default:throw hp::Error::reference;
            }
            const auto* created=find_entry(entries_,subject->creation_sequence);
            if(!created || !role(created->creation) || *role(created->creation)!=entry->resolved_subject->role)throw hp::Error::reference;
        }
        const auto& categories=*host->categories();std::size_t count=0;
        std::vector<hp::ResolvedFailure> failures;
        const auto visit=[&](bool fill) {
            const auto resolve=[&](std::uint64_t failure_sequence,const Target& target,std::optional<Producer> source) {
                if(failure_sequence>sequence)throw hp::Error::reference;
                const auto* failure_entry=find_entry(entries_,failure_sequence);
                const auto* fact=targeted_failure(failure_entry,target);
                if(!fact || (source && fact->producer!=*source) || !detail::valid_target(target,*fact))throw hp::Error::reference;
                std::optional<std::uint64_t> ordinal;
                if(failure_entry->run_creation) {
                    const auto* genesis=failure_entry->run_creation->facts();if(!genesis)throw hp::Error::reference;
                    for(std::size_t i=0;i<genesis->failures.size();++i)if(&genesis->failures[i]==fact)ordinal=i;
                    if(!ordinal || failure_entry->run_creation->failure_at(*ordinal)!=fact)throw hp::Error::reference;
                }
                hp::ResolvedFailure resolved{projection_target(target),failure_sequence,ordinal,projection_producer(fact->producer),fact->process_sequence};
                if(fill)failures.push_back(resolved);
                else {if(count==SIZE_MAX)throw hp::Error::capacity;++count;}
            };
            const auto outer=[&](const auto& slot,Category category) {
                if(const auto* failed=std::get_if<host_assembly::Failed>(&slot))resolve(failed->failure_event_sequence,{category},{});
            };
            outer(categories.topology,Category::topology);outer(categories.system_controls,Category::system_controls);
            outer(categories.kernel_identity,Category::kernel_identity);outer(categories.pu_identity,Category::pu_identity);
            outer(categories.frequency,Category::frequency);outer(categories.anonymous_thp,Category::anonymous_thp);outer(categories.process_thp,Category::process_thp);
            const auto observed=[&](const auto& owner,const auto& observation,Category category,Selector selector,std::uint64_t key) {
                if(!observation.failure_event)return;
                const auto parsed=reference(*observation.failure_event,run_);
                if(!parsed || !owner.ordinal() || !owner.producer() || !observation.reason)throw hp::Error::reference;
                resolve(*parsed,{category,selector,*owner.ordinal(),key,reason(*observation.reason)},producer(*owner.producer()));
            };
            if(const auto* owner=std::get_if<host_controls::Owner>(&categories.system_controls)) {
                constexpr std::array selectors{Selector::smt_control,Selector::smt_active,Selector::numa_mask,Selector::thp_enabled,Selector::thp_defrag};
                std::size_t index=0;for(const auto& o:owner->observations())observed(*owner,o,Category::system_controls,selectors[index++],0);
            }
            if(const auto* owner=std::get_if<private_kernel_identity::Owner>(&categories.kernel_identity)) {
                constexpr std::array selectors{Selector::sysname,Selector::release,Selector::version};
                std::size_t index=0;for(const auto& o:owner->observations())observed(*owner,o,Category::kernel_identity,selectors[index++],0);
            }
            if(const auto* owner=std::get_if<pu_cpu_identity::Owner>(&categories.pu_identity))for(const auto& row:owner->rows()) {
                observed(*owner,row.signature,Category::pu_identity,Selector::signature,row.linux_cpu.value());
                observed(*owner,row.microcode,Category::pu_identity,Selector::microcode,row.linux_cpu.value());
            }
            if(const auto* owner=std::get_if<frequency_policy_owner::Owner>(&categories.frequency))for(const auto& row:owner->uncovered_cpus())
                observed(*owner,row,Category::frequency,Selector::uncovered_cpu,row.association.linux_cpu.value());
            if(const auto* owner=std::get_if<anonymous_thp_policy_owner::Owner>(&categories.anonymous_thp)) {
                if(!owner->enumeration())throw hp::Error::reference;
                observed(*owner,*owner->enumeration(),Category::anonymous_thp,Selector::enumeration,0);
                for(const auto& row:owner->rows())observed(*owner,row,Category::anonymous_thp,Selector::page_size,row.size.value());
            }
            if(const auto* owner=std::get_if<process_thp_restriction::Owner>(&categories.process_thp)) {
                if(!subject || !owner->observation())throw hp::Error::reference;
                observed(*owner,*owner->observation(),Category::process_thp,Selector::process_observation,subject->creation_sequence);
            }
        };
        visit(false);
        if(count>limits.failure_references || count>failures.max_size() || count>SIZE_MAX/sizeof(hp::ResolvedFailure))throw hp::Error::capacity;
        const auto table_bytes=count*sizeof(hp::ResolvedFailure);const auto fixed=hp::detail::session_fixed_stock();
        if(fixed>limits.consumer_simultaneous_bytes || table_bytes>limits.consumer_simultaneous_bytes-fixed)throw hp::Error::capacity;
        failures.reserve(count);visit(true);
        std::sort(failures.begin(),failures.end(),[](const auto& a,const auto& b){return a.target<b.target;});
        for(std::size_t i=1;i<failures.size();++i)if(failures[i-1].target==failures[i].target)throw hp::Error::reference;
        hp::SourceDomain domain;
        switch(domain_) {
        case Domain::supplied_observation:domain=hp::SourceDomain::supplied_observation;break;
        case Domain::synthetic_execution:domain=hp::SourceDomain::synthetic_execution;break;
        default:throw hp::Error::source;
        }
        const hp::detail::ResolvedInput input{*host,run_,entry->timestamp,domain,
            entry->run_creation?hp::SourceOrigin::genesis:hp::SourceOrigin::ordinary,projection_producer(entry->producer),sequence,
            subject,failures,fixed,table_bytes};
        return hp::Owner::prepare(input,limits);
    }catch(hp::Error error){return std::unexpected{error};}
    catch(const std::length_error&){return std::unexpected{hp::Error::capacity};}
}
} // namespace epycbench::results::process_subject
namespace epycbench::results::host_projection::detail {
auto session_fixed_stock() noexcept -> std::size_t {return sizeof(process_subject::ProjectionSessionCells);}
}
namespace epycbench::results::build_projection::detail {
auto session_fixed_stock() noexcept -> std::size_t {return sizeof(process_subject::BuildProjectionSessionCells);}
}

namespace epycbench::results::error_serialization::detail {
auto session_fixed_stock() noexcept -> std::size_t {return sizeof(process_subject::ErrorSerializationSessionCells);}
}

namespace epycbench::results::source_serialization::detail {
auto session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::SourceSerializationSessionCells)+process_subject::detail::creation_lookup_fixed_stock();}
}

namespace epycbench::results::source_outcome_serialization::detail {
auto session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::SourceOutcomeSerializationSessionCells);}
}

namespace epycbench::results::base_build_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::BaseBuildSerializationSessionCells);}
}

namespace epycbench::results::topology_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::TopologySerializationSessionCells);}
}

namespace epycbench::results::system_controls_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::SystemControlsSerializationSessionCells);}
}

namespace epycbench::results::kernel_identity_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::KernelIdentitySerializationSessionCells);}
}

namespace epycbench::results::pu_identity_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::PuIdentitySerializationSessionCells);}
}

namespace epycbench::results::frequency_policy_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::FrequencyPolicySerializationSessionCells);}
}

namespace epycbench::results::anonymous_thp_policy_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::AnonymousThpPolicySerializationSessionCells);}
}

namespace epycbench::results::process_thp_restriction_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::ProcessThpRestrictionSerializationSessionCells);}
}

namespace epycbench::results::host_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::HostSerializationSessionCells);}
}

namespace epycbench::results::run_creation_serialization {
auto detail::session_fixed_stock()noexcept->std::size_t{return sizeof(process_subject::RunCreationSerializationSessionCells);}
}

namespace epycbench::results::normalization_serialization::detail {
auto session_fixed_stock() noexcept -> std::size_t {
    return sizeof(process_subject::NormalizationSerializationSessionCells) + sizeof(process_subject::NormalizationFindEntryCells);
}
}
