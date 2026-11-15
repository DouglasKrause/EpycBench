#include "private_resource_service_serialization.hpp"
#include "../core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace epycbench::results::resource_service_serialization {
namespace detail {
struct State {
    WorkerMap workers;
    std::vector<scenario::ExperimentWorkerReference> bindings;
    std::vector<scenario::MatrixWorkerSelectorTemplate> templates;
    PoolMap pools;
    core::CpuSet cpus;
    std::vector<topology::StableCoreId> cores;
    std::optional<topology::TopologyFingerprints> fingerprints;
    bool dedicated{};
    std::string canonical;
    std::optional<core::Sha256Digest> digest;
    Origin origin{};
    Usage usage{};
};
auto checked_add(std::size_t& a, std::size_t b) noexcept -> bool {
    if (b > std::numeric_limits<std::size_t>::max() - a) return false;
    a += b;
    return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return std::nullopt;
    return a * b;
}
} // namespace detail
namespace {
using detail::State;
constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::array<std::string_view, 2> units{"pu", "core"};
constexpr std::array<std::string_view, 3> smts{"one-per-core", "all", "explicit-indices"};
constexpr std::array<std::string_view, 7> orders{"ascending-pu", "descending-pu", "compact-cache",
    "spread-cache", "compact-numa", "spread-numa", "random"};
void need(bool condition, Error error = Error::schema) { if (!condition) throw error; }
void add(std::size_t& value, std::size_t increment) {
    need(detail::checked_add(value, increment), Error::capacity);
}
auto multiply(std::size_t a, std::size_t b) -> std::size_t {
    const auto result = detail::checked_multiply(a, b);
    need(result.has_value(), Error::capacity);
    return *result;
}
struct Decimal {
    std::array<char, 20> buffer{};
    std::size_t size{};
    explicit Decimal(std::uint64_t value) {
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        need(result.ec == std::errc{}, Error::capacity);
        size = static_cast<std::size_t>(result.ptr - buffer.data());
    }
    auto view() const noexcept -> std::string_view { return {buffer.data(), size}; }
};
auto nibble(char value) -> unsigned {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a') + 10U;
    throw Error::encoding;
}
struct Text {
    std::string_view bytes;
    bool encoded{};
    auto size() const noexcept -> std::size_t { return encoded ? bytes.size()/2 : bytes.size(); }
    auto at(std::size_t index) const -> char {
        return encoded ? static_cast<char>((nibble(bytes[index*2]) << 4U) | nibble(bytes[index*2+1]))
                       : bytes[index];
    }
    auto owning() const -> std::string {
        std::string result;
        result.reserve(size());
        for (std::size_t i=0; i<size(); ++i) result.push_back(at(i));
        return result;
    }
};
constexpr std::array<std::size_t Quantities::*,19> quantities{
    &Quantities::worker_sets,&Quantities::worker_pus,&Quantities::bindings,&Quantities::templates,
    &Quantities::selector_numbers,&Quantities::selector_names,&Quantities::pools,&Quantities::service_pus,
    &Quantities::reserved_cores,&Quantities::cpu_members,&Quantities::text_bytes,&Quantities::maximum_text,
    &Quantities::range_components,&Quantities::cpu_list_bytes,&Quantities::identity_entries,&Quantities::smt_entries,
    &Quantities::canonical_bytes,&Quantities::retained_bytes,&Quantities::consumer_simultaneous_bytes};
struct Counts {
    Usage q{};
    std::size_t dynamic{};
    const Limits* limits{};
    void check() const {
        if (limits) for (const auto member:quantities) need(q.*member<=(*limits).*member,Error::capacity);
    }
    void row(std::size_t& counter, std::size_t bytes) { add(counter, 1); add(dynamic, bytes); check(); }
    void text(Text value) {
        add(q.text_bytes, value.size()); add(dynamic, value.size());
        q.maximum_text = std::max(q.maximum_text, value.size()); check();
    }
    void core(Text value) {
        text(value);
        std::size_t components=1;
        for (std::size_t i=0; i<value.size(); ++i) {
            if (value.at(i)==',') add(components, 1);
            if (i+3<value.size() && value.at(i)=='-' && value.at(i+1)=='s' &&
                value.at(i+2)=='e' && value.at(i+3)=='t') {
                q.range_components=std::max(q.range_components,components); components=1;
            }
        }
        q.range_components=std::max(q.range_components, components); check();
    }
};
struct Sink {
    std::string* output{};
    std::size_t size{};
    std::size_t ceiling{SIZE_MAX};
    void raw(std::string_view value) { add(size, value.size()); need(size<=ceiling,Error::capacity); if (output) output->append(value); }
    void character(char value) { add(size, 1); need(size<=ceiling,Error::capacity); if (output) output->push_back(value); }
    void quoted(std::string_view value) { character('"'); raw(value); character('"'); }
    void text(std::string_view value) {
        need(size<=ceiling && ceiling-size>=2 && value.size()<=(ceiling-size-2)/2,Error::capacity);
        character('"');
        for (const char octet : value) {
            const auto byte=static_cast<unsigned char>(octet);
            character(hex_digits[byte >> 4U]); character(hex_digits[byte & 15U]);
        }
        character('"');
    }
    void number(std::uint64_t value) { const Decimal decimal{value}; quoted(decimal.view()); }
    void boolean(bool value) { raw(value ? "true" : "false"); }
    void optional(std::optional<bool> value) { if (value) boolean(*value); else raw("null"); }
    void fingerprint(const core::Sha256Digest& value) {
        raw("\"sha256:");
        for (const auto byte : value.bytes()) {
            const auto v=std::to_integer<unsigned>(byte);
            character(hex_digits[v>>4U]); character(hex_digits[v&15U]);
        }
        character('"');
    }
};
template<class Range, class F> void array(Sink& out, const Range& values, F emit) {
    out.character('['); bool first=true;
    for (const auto& value : values) { if (!first) out.character(','); first=false; emit(value); }
    out.character(']');
}
auto emit_cpu_callback(Sink& out, std::size_t& text) {
    return [&out,&text](auto cpu) {
        const Decimal decimal{cpu.value()}; if (text) add(text, 1); add(text, decimal.size);
        out.number(cpu.value());
    };
}
void cpu_array(Sink& out, const core::CpuSet& cpus, Counts& count) {
    need(cpus.cardinality() <= std::numeric_limits<std::size_t>::max(), Error::capacity);
    const auto cardinality=static_cast<std::size_t>(cpus.cardinality());
    add(count.q.cpu_members, cardinality); count.check();
    add(count.dynamic, multiply(cardinality, 2*sizeof(std::uint32_t)));
    count.q.range_components=std::max(count.q.range_components, cardinality);
    std::size_t text=0;
    array(out, cpus, emit_cpu_callback(out,text));
    count.q.cpu_list_bytes=std::max(count.q.cpu_list_bytes, text); count.check();
}
auto emit_core_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& core) {
        count.row(count.q.reserved_cores, sizeof(topology::StableCoreId));
        count.core({core.value(),false}); out.text(core.value());
    };
}
void cores(Sink& out, const std::vector<topology::StableCoreId>& values, Counts& count) {
    array(out, values, emit_core_callback(out,count));
}
auto emit_number_callback(Sink& out, Counts& count) {
    return [&out,&count](auto value) { count.row(count.q.selector_numbers, sizeof(value)); out.number(value); };
}
void numbers(Sink& out, const std::vector<std::uint32_t>& values, Counts& count) {
    array(out, values, emit_number_callback(out,count));
}
auto emit_excluded_cpu_callback(Sink& out, Counts& count) {
    return [&out,&count](auto cpu) {
        count.row(count.q.selector_numbers,sizeof(cpu)); out.number(cpu.value());
    };
}
auto emit_name_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& name) {
        count.row(count.q.selector_names,sizeof(name)); count.text({name,false}); out.text(name);
    };
}
auto emit_smt_callback(Sink& out, Counts& count) {
    return [&out,&count](auto index) {
        count.row(count.q.selector_numbers,sizeof(index)); out.number(index);
    };
}
void template_row(Sink& out, const scenario::MatrixWorkerSelectorTemplate& value, Counts& count) {
    count.row(count.q.templates, sizeof(value));
    out.character('['); out.number(value.experiment_index); out.character(',');
    out.quoted(units.at(static_cast<std::size_t>(value.unit))); out.raw(",[");
    numbers(out,value.filters.packages,count); out.character(',');
    numbers(out,value.filters.numa_nodes,count); out.character(',');
    numbers(out,value.filters.dies,count); out.character(',');
    numbers(out,value.filters.cache_domains,count); out.character(',');
    out.optional(value.filters.allowed); out.character(','); out.optional(value.filters.online);
    out.raw("],[");
    array(out,value.exclusions.processing_units,emit_excluded_cpu_callback(out,count)); out.character(','); numbers(out,value.exclusions.cores,count); out.character(',');
    array(out,value.exclusions.worker_sets,emit_name_callback(out,count)); out.raw("],"); out.quoted(smts.at(static_cast<std::size_t>(value.smt))); out.character(',');
    count.q.smt_entries=std::max(count.q.smt_entries,value.smt_indices.size());
    array(out,value.smt_indices,emit_smt_callback(out,count)); out.character(','); if (value.count) out.number(*value.count); else out.raw("null");
    out.character(','); out.quoted(orders.at(static_cast<std::size_t>(value.ordering))); out.character(',');
    count.text({value.source_pointer,false}); out.text(value.source_pointer); out.character(',');
    count.text({value.profile,false}); out.quoted(value.profile); out.character(']');
}
struct View {
    const WorkerMap& workers;
    const std::vector<scenario::ExperimentWorkerReference>& bindings;
    const std::vector<scenario::MatrixWorkerSelectorTemplate>& templates;
    const PoolMap& pools;
    const core::CpuSet& cpus;
    const std::vector<topology::StableCoreId>& cores;
    const topology::TopologyFingerprints& fingerprints;
    bool dedicated;
};
auto view(const State& state) -> View {
    return {state.workers,state.bindings,state.templates,state.pools,state.cpus,state.cores,
            *state.fingerprints,state.dedicated};
}
auto emit_binding_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& binding) {
        count.row(count.q.bindings,sizeof(binding)); out.character('['); out.number(binding.experiment_index);
        out.character(','); count.text({binding.worker_set_name,false}); out.text(binding.worker_set_name);
        out.character(','); count.text({binding.source_pointer,false}); out.text(binding.source_pointer); out.character(']');
    };
}
auto emit_template_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& item) { template_row(out,item,count); };
}
auto emit_service_pu_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& pu) {
            count.row(count.q.service_pus,sizeof(pu)); count.text({pu.stable_id.value(),false});
            count.core({pu.core_id.value(),false}); out.character('['); out.text(pu.stable_id.value());
            out.character(','); out.text(pu.core_id.value()); out.character(','); out.number(pu.linux_cpu.value()); out.character(']');
        };
}
auto emit_pool_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& entry) {
        count.row(count.q.pools,sizeof(PoolMap::value_type)); count.text({entry.first,false});
        const auto& pool=entry.second; count.text({pool.name,false});
        out.character('['); out.text(entry.first); out.character(','); out.text(pool.name); out.character(',');
        array(out,pool.processing_units,emit_service_pu_callback(out,count)); out.character(','); cpu_array(out,pool.linux_cpus,count); out.character(',');
        cores(out,pool.reserved_cores,count); out.character(','); count.text({pool.source_pointer,false});
        out.text(pool.source_pointer); out.character(']');
    };
}
auto emit_worker_pu_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& pu) {
            count.row(count.q.worker_pus,sizeof(pu)); count.text({pu.stable_id.value(),false});
            count.core({pu.core_id.value(),false}); out.character('['); out.text(pu.stable_id.value());
            out.character(','); out.number(pu.linux_cpu.value()); out.character(','); out.text(pu.core_id.value()); out.character(']');
        };
}
auto emit_worker_callback(Sink& out, Counts& count) {
    return [&out,&count](const auto& entry) {
        count.row(count.q.worker_sets,sizeof(WorkerMap::value_type)); count.text({entry.first,false});
        const auto& set=entry.second; count.text({set.name,false});
        out.character('['); out.text(entry.first); out.character(','); out.text(set.name); out.character(',');
        array(out,set.processing_units,emit_worker_pu_callback(out,count)); out.character(','); cpu_array(out,set.linux_cpus,count); out.character(',');
        count.text({set.source_pointer,false}); out.text(set.source_pointer); out.character(']');
    };
}
void emit(Sink& out, View value, Counts& count) {
    if (count.limits) {
        need(value.workers.size()<=count.limits->worker_sets && value.bindings.size()<=count.limits->bindings &&
             value.templates.size()<=count.limits->templates && value.pools.size()<=count.limits->pools &&
             value.cores.size()<=count.limits->reserved_cores,Error::capacity);
    }
    out.raw("{\"dedicated_service_placement\":"); out.boolean(value.dedicated);
    out.raw(",\"experiment_workers\":");
    array(out,value.bindings,emit_binding_callback(out,count));
    out.raw(",\"matrix_worker_templates\":");
    array(out,value.templates,emit_template_callback(out,count));
    out.raw(",\"resource_profile\":"); out.quoted(scenario::resource_resolution_profile);
    out.raw(",\"schema\":"); out.quoted(profile); out.raw(",\"service_pools\":");
    array(out,value.pools,emit_pool_callback(out,count));
    out.raw(",\"service_profile\":"); out.quoted(scenario::service_cpu_resolution_profile);
    out.raw(",\"service_reserved_cores\":"); cores(out,value.cores,count);
    out.raw(",\"service_reserved_cpus\":"); cpu_array(out,value.cpus,count);
    out.raw(",\"topology_fingerprints\":["); out.fingerprint(value.fingerprints.physical.digest());
    out.character(','); out.fingerprint(value.fingerprints.effective.digest()); out.raw("],\"worker_sets\":");
    array(out,value.workers,emit_worker_callback(out,count)); out.character('}'); count.q.canonical_bytes=out.size;
}
template<class T> auto factory(std::string_view text) -> T {
    auto parsed=T::parse(text);
    core::detail::ExpectedErrorCleanup cleanup{parsed};
    need(parsed.has_value(),Error::typed_factory_failure);
    return std::move(*parsed);
}
struct Parser {
    std::string_view input;
    std::size_t pos{};
    Counts count{};
    State* state{};
    void literal(std::string_view value) {
        need(input.substr(pos,value.size())==value); pos+=value.size();
    }
    auto consume(char value) -> bool {
        if (pos<input.size() && input[pos]==value) { ++pos; return true; }
        return false;
    }
    auto quoted() -> std::string_view {
        literal("\""); const auto begin=pos;
        while (pos<input.size() && input[pos]!='"') {
            need(input[pos]!='\\' && static_cast<unsigned char>(input[pos])>=32U); ++pos;
        }
        need(pos<input.size()); const auto result=input.substr(begin,pos-begin); ++pos; return result;
    }
    auto text(bool core=false) -> Text {
        const Text value{quoted(),true}; need(value.bytes.size()%2==0,Error::encoding);
        for (const auto c:value.bytes) (void)nibble(c);
        if (core) count.core(value); else count.text(value);
        return value;
    }
    void string(std::string* target) { const auto value=text(); if (target) *target=value.owning(); }
    auto number() -> std::uint64_t {
        const auto value=quoted(); need(!value.empty() && (value.size()==1 || value.front()!='0'),Error::encoding);
        for (const auto c:value) need(c>='0' && c<='9',Error::encoding);
        std::uint64_t result{};
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
        need(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size(),Error::encoding); return result;
    }
    auto u32() -> std::uint32_t {
        const auto value=number(); need(value<=std::numeric_limits<std::uint32_t>::max(),Error::encoding);
        return static_cast<std::uint32_t>(value);
    }
    auto index() -> std::size_t {
        const auto value=number(); need(value<=std::numeric_limits<std::size_t>::max(),Error::capacity);
        return static_cast<std::size_t>(value);
    }
    auto boolean() -> bool {
        if (input.substr(pos,4)=="true") { pos+=4; return true; }
        literal("false"); return false;
    }
    auto optional_bool() -> std::optional<bool> {
        if (input.substr(pos,4)=="null") { pos+=4; return std::nullopt; } return boolean();
    }
    template<class F> void list(F element) {
        literal("["); if (consume(']')) return;
        do { element(); } while (consume(',')); literal("]");
    }
    template<std::size_t N> auto token(const std::array<std::string_view,N>& values) -> std::size_t {
        const auto value=quoted();
        for (std::size_t i=0;i<N;++i) if (value==values[i]) return i;
        throw Error::schema;
    }
    auto cpu_callback(core::CpuSet* target, std::string& spelling, std::optional<std::uint32_t>& previous, std::size_t& components, std::size_t& bytes) {
        return [this,target,&spelling,&previous,&components,&bytes] {
            const auto value=u32(); need(!previous || value>*previous,Error::reference); previous=value;
            count.row(count.q.cpu_members,2*sizeof(std::uint32_t));
            const Decimal decimal{value}; if (components) add(bytes,1); add(bytes,decimal.size);
            if (target) { if (components) spelling.push_back(','); spelling.append(decimal.view()); }
            add(components,1);
        };
    }
    void cpus(core::CpuSet* target) {
        std::string spelling;
        std::optional<std::uint32_t> previous;
        std::size_t components=0, bytes=0;
        list(cpu_callback(target,spelling,previous,components,bytes));
        count.q.range_components=std::max(count.q.range_components,components);
        count.q.cpu_list_bytes=std::max(count.q.cpu_list_bytes,bytes); count.check();
        if (target && components) {
            auto parsed=core::CpuSet::parse(spelling); need(parsed.has_value(),Error::typed_factory_failure);
            *target=std::move(*parsed);
        }
    }
    auto core_callback(std::vector<topology::StableCoreId>* target) {
        return [this,target] {
            count.row(count.q.reserved_cores,sizeof(topology::StableCoreId)); const auto value=text(true);
            if (target) { const auto decoded=value.owning(); target->push_back(factory<topology::StableCoreId>(decoded)); }
        };
    }
    void core_list(std::vector<topology::StableCoreId>* target) {
        list(core_callback(target));
    }
    auto number_callback(std::vector<std::uint32_t>* target) {
        return [this,target] { count.row(count.q.selector_numbers,sizeof(std::uint32_t)); const auto value=u32(); if (target) target->push_back(value); };
    }
    void numeric_list(std::vector<std::uint32_t>* target) {
        list(number_callback(target));
    }
    auto excluded_cpu_callback(std::optional<scenario::MatrixWorkerSelectorTemplate>& item) {
        return [this,&item] {
                count.row(count.q.selector_numbers,sizeof(core::LinuxCpuId)); const auto value=u32();
                if (item) item->exclusions.processing_units.push_back(core::LinuxCpuId{value});
            };
    }
    auto name_callback(std::optional<scenario::MatrixWorkerSelectorTemplate>& item) {
        return [this,&item] {
                count.row(count.q.selector_names,sizeof(std::string)); const auto value=text();
                if (item) item->exclusions.worker_sets.push_back(value.owning());
            };
    }
    auto smt_callback(std::optional<scenario::MatrixWorkerSelectorTemplate>& item, std::size_t& indices) {
        return [this,&item,&indices] {
                count.row(count.q.selector_numbers,sizeof(std::uint64_t)); const auto value=number(); add(indices,1);
                if (item) item->smt_indices.push_back(value);
            };
    }
    auto template_callback() {
        return [this] {
            count.row(count.q.templates,sizeof(scenario::MatrixWorkerSelectorTemplate));
            std::optional<scenario::MatrixWorkerSelectorTemplate> item;
            if (state) item.emplace();
            literal("["); const auto id=index(); literal(","); const auto unit=token(units); literal(",[");
            numeric_list(item ? &item->filters.packages:nullptr); literal(",");
            numeric_list(item ? &item->filters.numa_nodes:nullptr); literal(",");
            numeric_list(item ? &item->filters.dies:nullptr); literal(",");
            numeric_list(item ? &item->filters.cache_domains:nullptr); literal(",");
            const auto allowed=optional_bool(); literal(","); const auto online=optional_bool(); literal("],[");
            list(excluded_cpu_callback(item)); literal(","); numeric_list(item ? &item->exclusions.cores:nullptr); literal(",");
            list(name_callback(item)); literal("],"); const auto smt=token(smts); literal(",");
            std::size_t indices=0;
            list(smt_callback(item,indices)); count.q.smt_entries=std::max(count.q.smt_entries,indices); literal(",");
            std::optional<std::uint64_t> limit;
            if (input.substr(pos,4)=="null") pos+=4; else limit=number();
            literal(","); const auto order=token(orders); literal(",");
            string(item ? &item->source_pointer:nullptr); literal(","); const auto version=quoted();
            need(version==scenario::resource_resolution_profile); count.text({version,false}); literal("]");
            if (item) {
                item->experiment_index=id; item->unit=static_cast<scenario::SelectorUnit>(unit);
                item->filters.allowed=allowed; item->filters.online=online;
                item->smt=static_cast<scenario::SelectorSmtMode>(smt); item->count=limit;
                item->ordering=static_cast<scenario::SelectorOrdering>(order);
                state->templates.push_back(std::move(*item));
            }
        };
    }
    void templates() {
        list(template_callback());
    }
    auto worker_pu_callback(std::optional<scenario::ResolvedWorkerSet>& set) {
        return [this,&set] {
                count.row(count.q.worker_pus,sizeof(scenario::ResolvedProcessingUnit));
                literal("["); const auto pu=text(); literal(","); const auto cpu=u32(); literal(",");
                const auto core=text(true); literal("]");
                if (set) {
                    auto pu_id=factory<topology::StableProcessingUnitId>(pu.owning());
                    auto core_id=factory<topology::StableCoreId>(core.owning());
                    set->processing_units.push_back({std::move(pu_id),core::LinuxCpuId{cpu},std::move(core_id)});
                }
            };
    }
    auto worker_callback() {
        return [this] {
            count.row(count.q.worker_sets,sizeof(WorkerMap::value_type));
            std::optional<scenario::ResolvedWorkerSet> set;
            if (state) set.emplace();
            literal("["); const auto key=text(); literal(","); string(set ? &set->name:nullptr); literal(",");
            list(worker_pu_callback(set)); literal(","); cpus(set ? &set->linux_cpus:nullptr); literal(",");
            string(set ? &set->source_pointer:nullptr); literal("]");
            if (set) need(state->workers.emplace(key.owning(),std::move(*set)).second,Error::reference);
        };
    }
    void workers() {
        list(worker_callback());
    }
    auto service_pu_callback(std::optional<scenario::ResolvedServiceCpuPool>& pool) {
        return [this,&pool] {
                count.row(count.q.service_pus,sizeof(scenario::ResolvedServiceCpu));
                literal("["); const auto pu=text(); literal(","); const auto core=text(true); literal(",");
                const auto cpu=u32(); literal("]");
                if (pool) {
                    auto pu_id=factory<topology::StableProcessingUnitId>(pu.owning());
                    auto core_id=factory<topology::StableCoreId>(core.owning());
                    pool->processing_units.push_back({std::move(pu_id),std::move(core_id),core::LinuxCpuId{cpu}});
                }
            };
    }
    auto pool_callback() {
        return [this] {
            count.row(count.q.pools,sizeof(PoolMap::value_type));
            std::optional<scenario::ResolvedServiceCpuPool> pool;
            if (state) pool.emplace();
            literal("["); const auto key=text(); literal(","); string(pool ? &pool->name:nullptr); literal(",");
            list(service_pu_callback(pool)); literal(","); cpus(pool ? &pool->linux_cpus:nullptr); literal(",");
            core_list(pool ? &pool->reserved_cores:nullptr); literal(",");
            string(pool ? &pool->source_pointer:nullptr); literal("]");
            if (pool) need(state->pools.emplace(key.owning(),std::move(*pool)).second,Error::reference);
        };
    }
    void pools() {
        list(pool_callback());
    }
    auto fingerprint() -> std::string_view {
        const auto value=quoted(); need(value.size()==71 && value.starts_with("sha256:"),Error::encoding);
        for (const auto c:value.substr(7)) (void)nibble(c);
        return value;
    }
    auto binding_callback() {
        return [this] {
            count.row(count.q.bindings,sizeof(scenario::ExperimentWorkerReference));
            literal("["); const auto id=index(); literal(","); const auto name=text(); literal(",");
            const auto pointer=text(); literal("]");
            if (state) state->bindings.push_back({id,name.owning(),pointer.owning()});
        };
    }
    void run() {
        literal("{\"dedicated_service_placement\":"); const auto dedicated=boolean();
        literal(",\"experiment_workers\":");
        list(binding_callback()); literal(",\"matrix_worker_templates\":"); templates();
        literal(",\"resource_profile\":"); need(quoted()==scenario::resource_resolution_profile);
        literal(",\"schema\":"); need(quoted()==profile); literal(",\"service_pools\":"); pools();
        literal(",\"service_profile\":"); need(quoted()==scenario::service_cpu_resolution_profile);
        literal(",\"service_reserved_cores\":"); core_list(state ? &state->cores:nullptr);
        literal(",\"service_reserved_cpus\":"); cpus(state ? &state->cpus:nullptr);
        literal(",\"topology_fingerprints\":["); const auto physical=fingerprint(); literal(",");
        const auto effective=fingerprint(); literal("],\"worker_sets\":"); workers(); literal("}");
        need(pos==input.size()); count.q.canonical_bytes=input.size();
        if (state) {
            state->fingerprints.emplace(factory<topology::PhysicalTopologyFingerprint>(physical),
                factory<topology::EffectiveTopologyFingerprint>(effective));
            state->dedicated=dedicated;
        }
    }
};
auto name_valid(std::string_view value) -> bool {
    if (value.empty() || value.size()>64 || value.front()<'a' || value.front()>'z') return false;
    return std::ranges::all_of(value,[](char c) { return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-'; });
}
auto pointer_equal(std::string_view value, std::string_view prefix, std::string_view tail) -> bool {
    return value.starts_with(prefix) && value.substr(prefix.size())==tail;
}
auto experiment_pointer(std::string_view value, std::size_t index, std::string_view suffix) -> bool {
    const Decimal decimal{index};
    if (!value.starts_with("/experiments/")) return false;
    value.remove_prefix(13);
    return value.starts_with(decimal.view()) && value.substr(decimal.size)==suffix;
}
struct Identity {
    std::string_view pu, core, container;
    std::uint32_t cpu{};
    bool service{}, active{};
};
auto selection_callback(std::vector<Identity>& index) {
    return [&index](const auto& entry, bool service) {
        const auto& set=entry.second;
        need(name_valid(entry.first) && entry.first==set.name,Error::reference);
        need(pointer_equal(set.source_pointer,service ? "/resources/service_cpus/":"/resources/worker_sets/",entry.first),Error::reference);
        need(!set.processing_units.empty() && set.linux_cpus.cardinality()==set.processing_units.size(),Error::reference);
        for (const auto& pu:set.processing_units) {
            const Decimal decimal{pu.linux_cpu.value()};
            need(pu.stable_id.value().starts_with("pu/") && pu.stable_id.value().substr(3)==decimal.view(),Error::reference);
            need(set.linux_cpus.contains(pu.linux_cpu),Error::reference);
            index.push_back({pu.stable_id.value(),pu.core_id.value(),entry.first,pu.linux_cpu.value(),service,false});
        }
    };
}
void validate(View value, Usage usage) {
    need(value.dedicated==!value.pools.empty(),Error::reference);
    std::vector<Identity> index;
    index.reserve(usage.identity_entries);
    const auto selection=selection_callback(index);
    for (const auto& entry:value.workers) selection(entry,false);
    for (const auto& entry:value.pools) selection(entry,true);
    // Group lookup is logarithmic; repeated experiment references never cause a full rescan.
    std::ranges::sort(index,{},[](const Identity& item) { return std::tuple{item.service,item.container,item.cpu}; });
    for (std::size_t i=1;i<index.size();++i)
        need(std::tuple{index[i-1].service,index[i-1].container,index[i-1].cpu}!=
             std::tuple{index[i].service,index[i].container,index[i].cpu},Error::reference);
    std::vector<std::uint64_t> smt;
    smt.reserve(usage.smt_entries);
    std::size_t binding=0, matrix=0, expected=0;
    while (binding<value.bindings.size() || matrix<value.templates.size()) {
        const bool use_binding=matrix==value.templates.size() ||
            (binding<value.bindings.size() && value.bindings[binding].experiment_index<value.templates[matrix].experiment_index);
        if (use_binding) {
            const auto& item=value.bindings[binding++];
            need(item.experiment_index==expected && name_valid(item.worker_set_name) &&
                 value.workers.contains(item.worker_set_name) &&
                 experiment_pointer(item.source_pointer,expected,"/workers"),Error::reference);
            const auto first=std::ranges::lower_bound(index,std::pair{false,std::string_view{item.worker_set_name}}, {},
                [](const Identity& id) { return std::pair{id.service,id.container}; });
            // A repeated reference finds an already marked group and does no group work.
            if (first!=index.end() && !first->active)
                for (auto it=first;it!=index.end() && !it->service && it->container==item.worker_set_name;++it) it->active=true;
        } else {
            const auto& item=value.templates[matrix++];
            need(item.experiment_index==expected && experiment_pointer(item.source_pointer,expected,"/workers_per_node/select") &&
                 item.profile==scenario::resource_resolution_profile,Error::reference);
            need(!item.count || *item.count>0,Error::reference);
            need((!item.filters.allowed || *item.filters.allowed) && (!item.filters.online || *item.filters.online),Error::reference);
            need((item.smt==scenario::SelectorSmtMode::explicit_indices)==!item.smt_indices.empty(),Error::reference);
            smt.assign(item.smt_indices.begin(),item.smt_indices.end()); std::ranges::sort(smt);
            need(std::ranges::adjacent_find(smt)==smt.end(),Error::reference);
            for (const auto& name:item.exclusions.worker_sets)
                need(name_valid(name) && value.workers.contains(name),Error::reference);
        }
        add(expected,1);
    }
    need(expected>0,Error::reference);
    std::ranges::sort(index,{},[](const Identity& item) { return std::tuple{item.cpu,item.service}; });
    auto cpu=value.cpus.begin();
    for (std::size_t begin=0;begin<index.size();) {
        std::size_t end=begin+1; bool service=index[begin].service;
        while (end<index.size() && index[end].cpu==index[begin].cpu) {
            need(index[end].core==index[begin].core,Error::reference); service=service || index[end].service; ++end;
        }
        if (service) { need(cpu!=value.cpus.end() && (*cpu).value()==index[begin].cpu,Error::reference); ++cpu; }
        begin=end;
    }
    need(cpu==value.cpus.end(),Error::reference);
    std::ranges::sort(index,{},[](const Identity& item) { return std::tuple{item.service,item.container,item.core}; });
    std::size_t pool_begin=0;
    while (pool_begin<index.size() && !index[pool_begin].service) ++pool_begin;
    for (const auto& entry:value.pools) {
        const auto& reserved=entry.second.reserved_cores;
        std::size_t core=0;
        while (pool_begin<index.size() && index[pool_begin].container==entry.first) {
            const auto spelling=index[pool_begin].core;
            need(core<reserved.size() && reserved[core].value()==spelling,Error::reference); ++core;
            do { ++pool_begin; } while (pool_begin<index.size() && index[pool_begin].container==entry.first && index[pool_begin].core==spelling);
        }
        need(core==reserved.size(),Error::reference);
    }
    std::ranges::sort(index,{},[](const Identity& item) { return std::tuple{item.core,item.service,item.container}; });
    std::size_t core=0;
    for (std::size_t begin=0;begin<index.size();) {
        std::size_t end=begin; bool active=false, service=false; std::string_view pool;
        while (end<index.size() && index[end].core==index[begin].core) {
            active=active || index[end].active;
            if (index[end].service) {
                need(!service || pool==index[end].container,Error::reference);
                service=true; pool=index[end].container;
            }
            ++end;
        }
        need(!(active && service),Error::reference);
        if (service) { need(core<value.cores.size() && value.cores[core].value()==index[begin].core,Error::reference); ++core; }
        begin=end;
    }
    need(core==value.cores.size(),Error::reference);
}
void limits_check(Usage usage, Limits limits) {
    for (const auto member:quantities) need(usage.*member<=limits.*member,Error::capacity);
}
void representable(Usage value) {
    need(value.worker_sets<=WorkerMap{}.max_size() && value.pools<=PoolMap{}.max_size(),Error::capacity);
    need(value.worker_pus<=std::vector<scenario::ResolvedProcessingUnit>{}.max_size() &&
         value.service_pus<=std::vector<scenario::ResolvedServiceCpu>{}.max_size(),Error::capacity);
    need(value.bindings<=std::vector<scenario::ExperimentWorkerReference>{}.max_size() &&
         value.templates<=std::vector<scenario::MatrixWorkerSelectorTemplate>{}.max_size(),Error::capacity);
    need(value.selector_numbers<=std::vector<std::uint64_t>{}.max_size() &&
         value.selector_names<=std::vector<std::string>{}.max_size(),Error::capacity);
    need(value.reserved_cores<=std::vector<topology::StableCoreId>{}.max_size() &&
         value.identity_entries<=std::vector<Identity>{}.max_size() &&
         value.smt_entries<=std::vector<std::uint64_t>{}.max_size(),Error::capacity);
}
auto admit(Counts count, Limits limits, bool decoding) -> Usage {
    count.q.identity_entries=count.q.worker_pus; add(count.q.identity_entries,count.q.service_pus);
    representable(count.q);
    count.q.retained_bytes=sizeof(Owner)+sizeof(State);
    add(count.q.retained_bytes,count.dynamic); add(count.q.retained_bytes,count.q.canonical_bytes);
    auto& total=count.q.consumer_simultaneous_bytes;
    total=decoding ? detail::decode_fixed_stock():detail::capture_fixed_stock();
    add(total,count.q.retained_bytes);
    add(total,multiply(count.q.identity_entries,sizeof(Identity)));
    add(total,multiply(count.q.smt_entries,sizeof(std::uint64_t)));
    if (decoding) {
        add(total,multiply(count.q.maximum_text,2)); add(total,count.q.cpu_list_bytes);
        add(total,multiply(count.q.range_components,4*sizeof(std::uint32_t)));
        add(total,detail::factory_error_stock());
    }
    // Standard containers cannot represent more than PTRDIFF_MAX logical bytes.
    need(count.dynamic<=static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()),Error::capacity);
    need(count.q.canonical_bytes<=std::string{}.max_size(),Error::capacity);
    limits_check(count.q,limits); return count.q;
}
// Explicit logical call-cell stocks. Library allocator metadata, vector slack and
// implementation-internal sort frames are deliberately not a whole-heap promise.
struct PrimitiveCells {
    bool need_condition; Error need_error;
    std::size_t *add_value; std::size_t add_increment;
    std::size_t *checked_add_value; std::size_t checked_add_increment;
    std::size_t multiply_a,multiply_b,checked_multiply_a,checked_multiply_b;
    std::optional<std::size_t> multiply_result,checked_multiply_result;
    Decimal decimal; std::uint64_t decimal_value; std::to_chars_result decimal_result;
    const Decimal* decimal_this; std::string_view decimal_view;
    char nibble_value; unsigned nibble_result;
    Text text; const Text* text_this; std::size_t text_index,text_size,text_i;
    char text_at; std::string text_owned,text_return;
    Counts *counts_this; const Counts *check_this; std::size_t Quantities::*check_member;
    std::size_t *row_counter; std::size_t row_bytes;
    Text counted_text,counted_core; std::size_t core_components,core_i;
    std::size_t maximum_result;
};
struct EmitCells {
    Sink sink; Sink *sink_this; std::string_view raw,quoted,text;
    char character,text_octet; unsigned char text_byte; std::uint64_t number; Decimal number_decimal;
    bool boolean; std::optional<bool> optional;
    const core::Sha256Digest* fingerprint; const std::array<std::byte,32>* fingerprint_bytes;
    std::byte fingerprint_byte; unsigned fingerprint_value;
    const std::byte *fingerprint_begin,*fingerprint_end;
    WorkerMap::const_iterator worker_begin,worker_end;
    PoolMap::const_iterator pool_begin,pool_end;
    const WorkerMap::value_type* worker_entry; const PoolMap::value_type* pool_entry;
    const scenario::ResolvedWorkerSet* worker; const scenario::ResolvedServiceCpuPool* pool;
    const scenario::ResolvedProcessingUnit* worker_pu; std::vector<scenario::ResolvedProcessingUnit>::const_iterator worker_pu_begin,worker_pu_end;
    const scenario::ResolvedServiceCpu* service_pu; std::vector<scenario::ResolvedServiceCpu>::const_iterator service_pu_begin,service_pu_end;
    const scenario::ExperimentWorkerReference* binding; std::vector<scenario::ExperimentWorkerReference>::const_iterator binding_begin,binding_end;
    const scenario::MatrixWorkerSelectorTemplate* item; std::vector<scenario::MatrixWorkerSelectorTemplate>::const_iterator template_begin,template_end;
    const topology::StableCoreId* core; std::vector<topology::StableCoreId>::const_iterator core_begin,core_end;
    const std::string* name; std::vector<std::string>::const_iterator name_begin,name_end;
    std::vector<std::uint32_t>::const_iterator numeric_begin,numeric_end; std::uint32_t numeric_value;
    std::vector<std::uint64_t>::const_iterator smt_begin,smt_end; std::uint64_t smt_value;
    std::vector<core::LinuxCpuId>::const_iterator excluded_begin,excluded_end; core::LinuxCpuId excluded_cpu;
    core::CpuSet::const_iterator cpu_begin,cpu_end; core::LinuxCpuId cpu;
    const core::CpuSet* cpus; std::uint64_t cardinality_result; std::size_t cardinality,cpu_text;
    Decimal cpu_decimal; Counts *count; View emit_view,view_return; const State* view_state;
    std::string_view stable_id_return,core_id_return; const topology::StableProcessingUnitId* stable_id_this;
    const topology::StableCoreId* core_id_this; core::LinuxCpuId linux_this; std::uint32_t linux_return;
    const topology::PhysicalTopologyFingerprint* physical_this;
    const topology::EffectiveTopologyFingerprint* effective_this;
    const core::Sha256Digest *physical_return,*effective_return;
    std::string_view numeric_profile,enum_return; std::size_t enum_index;
    std::optional<bool> allowed_copy,online_copy; std::optional<std::uint64_t> count_copy;
};
struct ValidationCells {
    View value; Usage usage; std::vector<Identity> index; std::vector<std::uint64_t> smt;
    Identity identity_return; const Identity *sort_argument,*sort_other;
    std::tuple<bool,std::string_view,std::uint32_t> local_sort_key,local_sort_other;
    std::tuple<std::uint32_t,bool> cpu_sort_key;
    std::tuple<bool,std::string_view,std::string_view> pool_sort_key;
    std::tuple<std::string_view,bool,std::string_view> core_sort_key;
    std::pair<bool,std::string_view> lookup_key,lookup_result;
    std::vector<Identity>::iterator first,it,index_end;
    std::vector<std::uint64_t>::iterator smt_begin,smt_end,smt_duplicate;
    std::vector<std::uint64_t>::const_iterator assign_begin,assign_end;
    WorkerMap::const_iterator worker_begin,worker_end;
    PoolMap::const_iterator pool_begin_iterator,pool_end;
    const WorkerMap::value_type *worker_entry; const PoolMap::value_type *pool_entry;
    const scenario::ResolvedWorkerSet* worker_set; const scenario::ResolvedServiceCpuPool* pool_set;
    const scenario::ResolvedProcessingUnit* worker_pu; std::vector<scenario::ResolvedProcessingUnit>::const_iterator worker_pu_begin,worker_pu_end;
    const scenario::ResolvedServiceCpu* service_pu; std::vector<scenario::ResolvedServiceCpu>::const_iterator service_pu_begin,service_pu_end;
    bool selection_service; Decimal pu_decimal;
    std::size_t i,binding,matrix,expected,pool_begin,core,begin,end;
    bool use_binding,active,service; std::string_view pool,spelling;
    const scenario::ExperimentWorkerReference* binding_item;
    const scenario::MatrixWorkerSelectorTemplate* template_item;
    const std::string* name; std::vector<std::string>::const_iterator name_begin,name_end;
    const std::vector<topology::StableCoreId>* reserved;
    core::CpuSet::const_iterator cpu,cpu_end; core::LinuxCpuId cpu_result;
    std::string_view name_input; char name_character; bool name_result;
    std::string_view pointer_value,pointer_prefix,pointer_tail; bool pointer_result;
    std::string_view experiment_value,experiment_suffix; std::size_t experiment_index;
    Decimal experiment_decimal; bool experiment_result;
    std::uint64_t cardinality_return; bool contains_result,map_contains_result;
    const core::CpuSet* set_this; core::LinuxCpuId contains_argument;
};
struct ParserCells {
    Parser parser,preflight; Parser* parser_this; State* state;
    std::string_view literal_value,quoted_result,number_text,token_text,fingerprint_text;
    std::size_t quoted_begin,token_i; char consume_value; bool consume_result;
    std::uint64_t number_result,u32_value,index_value; std::from_chars_result parsed_number;
    std::uint32_t u32_result; std::size_t index_result;
    char quoted_character,number_character,hex_character; unsigned hex_result;
    bool core_text,boolean_result; Text text_result,string_value;
    std::string* string_target; std::string string_temporary;
    std::optional<bool> optional_result;
    core::CpuSet *cpu_target; std::string cpu_spelling;
    std::optional<std::uint32_t> cpu_previous; std::size_t cpu_components,cpu_bytes;
    std::uint32_t cpu_value; Decimal cpu_decimal;
    std::expected<core::CpuSet,core::IdSetParseError> cpu_parsed;
    std::vector<topology::StableCoreId>* cores_target; Text core_value; std::string core_decoded;
    topology::StableCoreId core_push_value;
    std::vector<std::uint32_t>* numeric_target; std::uint32_t numeric_value;
    std::optional<scenario::MatrixWorkerSelectorTemplate> template_item;
    std::size_t template_id,unit,smt,indices,order;
    std::optional<bool> allowed,online; std::optional<std::uint64_t> limit;
    std::uint32_t excluded_cpu; core::LinuxCpuId excluded_push;
    Text excluded_name; std::string excluded_push_name;
    std::uint64_t smt_index; std::string_view template_version;
    std::optional<scenario::ResolvedWorkerSet> worker;
    std::optional<scenario::ResolvedServiceCpuPool> pool;
    Text worker_key,worker_pu,worker_core,pool_key,pool_pu,pool_core;
    std::uint32_t worker_cpu,pool_cpu;
    topology::StableProcessingUnitId worker_pu_id,pool_pu_id;
    topology::StableCoreId worker_core_id,pool_core_id;
    scenario::ResolvedProcessingUnit worker_push;
    scenario::ResolvedServiceCpu service_push;
    std::string worker_key_owned,pool_key_owned;
    std::pair<WorkerMap::iterator,bool> worker_insertion;
    std::pair<PoolMap::iterator,bool> pool_insertion;
    std::size_t binding_id; Text binding_name,binding_pointer;
    scenario::ExperimentWorkerReference binding_push;
    bool dedicated; std::string_view physical,effective;
    topology::PhysicalTopologyFingerprint physical_result;
    topology::EffectiveTopologyFingerprint effective_result;
};
// Public typed parsers are inherited code. Their explicit owning/error cells are
// charged here in addition to input-derived transient vectors/text in Usage.
// Logical reference cells for the inherited parse_error callback; not its ABI layout.
struct ParseErrorCaptureCells {
    topology::detail::StableIdKind* kind;
    std::string* classification;
};
struct FactoryCells {
    ParseErrorCaptureCells parse_error_captures;
    ParseErrorCaptureCells* json_object_fill_reference;
    const ParseErrorCaptureCells* parse_error_callback_this;
    std::string_view pu_input,core_input,physical_input,effective_input;
    std::expected<topology::StableProcessingUnitId,core::Error> pu;
    std::expected<topology::StableCoreId,core::Error> core;
    std::expected<topology::PhysicalTopologyFingerprint,core::Error> physical;
    std::expected<topology::EffectiveTopologyFingerprint,core::Error> effective;
    core::detail::ExpectedErrorCleanup<decltype(pu)> pu_cleanup;
    core::detail::ExpectedErrorCleanup<decltype(core)> core_cleanup;
    core::detail::ExpectedErrorCleanup<decltype(physical)> physical_cleanup;
    core::detail::ExpectedErrorCleanup<decltype(effective)> effective_cleanup;
    topology::StableProcessingUnitId pu_return; topology::StableCoreId core_return;
    topology::PhysicalTopologyFingerprint physical_return; topology::EffectiveTopologyFingerprint effective_return;
    std::string pu_input_temporary,core_input_temporary,stable_result;
    std::expected<std::string,core::Error> stable_parsed;
    std::unexpected<core::Error> stable_unexpected,wrapper_unexpected;
    topology::detail::StableIdKind stable_kind; std::string_view stable_text;
    std::expected<core::CpuSet,core::IdSetParseError> range_result;
    std::string range_canonical; std::string_view range_text,package_text,member_text;
    std::size_t delimiter,range_begin,range_end,range_dash; std::uint32_t range_first,range_last;
    std::expected<std::uint32_t,core::IdSetParseError> range_first_result,range_last_result;
    core::CpuSet range_owner,range_return; std::vector<std::array<std::uint32_t,2>> input_ranges,normalized_ranges;
    std::array<std::uint32_t,2> range_push; const std::array<std::uint32_t,2>* current_range;
    std::string_view error_kind,error_classification;
    core::Error error_result; nlohmann::json error_object,error_child,error_return,json_assignment_parameter;
    core::detail::JsonCleanup<nlohmann::json> error_guard,child_guard;
    std::string error_key,error_classification_owned;
    nlohmann::json::object_t* error_entries;
    std::pair<nlohmann::json::object_t::iterator,bool> error_insertion;
    decltype(pu_cleanup)* pu_cleanup_this; decltype(core_cleanup)* core_cleanup_this;
    decltype(physical_cleanup)* physical_cleanup_this; decltype(effective_cleanup)* effective_cleanup_this;
    nlohmann::json *retire_value,*retire_child;
    nlohmann::json::iterator retire_begin,retire_end,set_lookup;
    core::detail::JsonCleanup<nlohmann::json> *json_cleanup_this;
    nlohmann::json *set_object,*set_value,*append_object,*append_child,*json_fill_argument;
    std::string_view set_key,append_name,kind_name_result;
    topology::detail::StableIdKind kind_name_argument,parse_error_kind,valid_kind;
    std::string_view valid_text,decimal_text,package_argument;
    std::size_t core_separator; std::uint32_t decimal_value; std::from_chars_result decimal_result;
    bool valid_result,decimal_valid,range_valid,package_valid;
    std::string stable_constructor_value,core_constructor_value;
    std::string_view fingerprint_text,fingerprint_kind,fingerprint_prefix,fingerprint_hex;
    core::detail::ExpectedErrorCleanup<std::expected<core::Sha256Digest,core::Error>> digest_cleanup;
    decltype(digest_cleanup)* digest_cleanup_this;
    std::expected<core::Sha256Digest,core::Error> physical_parsed,effective_parsed;
    core::Sha256Digest physical_constructor_value,effective_constructor_value;
    std::array<std::byte,32> digest_bytes;
    std::size_t digest_i,digest_byte_index; std::optional<std::uint8_t> digest_value;
    std::expected<core::Sha256Digest,core::Error> parsed_digest;
    core::Sha256Digest digest_return; std::string_view digest_text;
    char digest_character,nibble_argument; std::uint8_t digest_shifted;
    std::optional<std::uint8_t> nibble_return;
};
struct OperationCells {
    Counts measured,rendered,admit_count; Sink measuring,rendering;
    Usage usage,admitted,limits_usage; Limits limits,admit_limits,limits_copy;
    bool decoding; std::size_t *total; std::size_t member_result;
    std::size_t Quantities::*member;
    std::string max_size_probe; std::unique_ptr<State> state,constructor_parameter;
    Owner owner_temporary; std::expected<Owner,Error> returned;
    std::unexpected<Error> error_conversion,capacity_conversion; Error caught;
    const std::length_error* caught_length;
    State *state_this; Owner *owner_this,*move_other;
    std::string_view input,canonical_view; core::Sha256Digest hash_result;
    View facts; const State* view_argument;
};
struct RepresentationCells {
    Usage argument;
    WorkerMap workers; PoolMap pools;
    std::vector<scenario::ResolvedProcessingUnit> worker_pus;
    std::vector<scenario::ResolvedServiceCpu> service_pus;
    std::vector<scenario::ExperimentWorkerReference> bindings;
    std::vector<scenario::MatrixWorkerSelectorTemplate> templates;
    std::vector<std::uint64_t> numbers,smt;
    std::vector<std::string> names;
    std::vector<topology::StableCoreId> cores;
    std::vector<Identity> identities;
    std::size_t max_size_result;
};
struct ProbeCells {
    const scenario::ScenarioDigestedPlan* plan;
    std::string_view input;
    Limits limits;
    std::expected<Usage,Error> result;
    const Usage* result_value;
    Error result_error;
    std::unexpected<Error> error_return;
};
struct CaptureCells {
    const scenario::ScenarioDigestedPlan *plan,*plan_this;
    const scenario::MatrixExpandedScenario *matrix_this;
    const scenario::KernelDispatchedScenario *kernel_this;
    const scenario::MemoryPlacementResolvedScenario *memory_this;
    const scenario::MatrixExpandedScenario *matrix;
    const scenario::KernelDispatchedScenario *kernel;
    const scenario::MemoryPlacementResolvedScenario *memory;
    const scenario::ServiceCpuResolvedScenario *service;
    const scenario::ResourceResolvedScenario *resource;
    const scenario::ResourceResolvedScenario *workers_this,*bindings_this,*templates_this,*fingerprints_this;
    const scenario::ServiceCpuResolvedScenario *pools_this,*cpus_this,*cores_this,*dedicated_this,*resource_this;
    const WorkerMap *workers_return; const PoolMap *pools_return;
    const std::vector<scenario::ExperimentWorkerReference>* bindings_return;
    const std::vector<scenario::MatrixWorkerSelectorTemplate>* templates_return;
    const core::CpuSet* cpus_return; const std::vector<topology::StableCoreId>* cores_return;
    const topology::TopologyFingerprints* fingerprints_return;
    bool dedicated_return; View view;
};
// Each callback factory returns a prvalue directly into the by-value helper
// parameter (or the selection local). Guaranteed elision means one closure object,
// not a guessed second return copy. Explicit captures make its real type nameable.
template<class Range,class Callback,class Second=Counts> struct ArrayCallCells {
    Sink* out_argument;
    bool first;
    const Range* range_argument;
    Callback callback_parameter;
    const Callback* callback_this;
    Sink* factory_out_argument;
    Second* factory_second_argument;
    const std::ranges::range_value_t<Range>* loop_reference;
};
using EmitCpu=decltype(emit_cpu_callback(std::declval<Sink&>(),std::declval<std::size_t&>()));
using EmitCore=decltype(emit_core_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitNumber=decltype(emit_number_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitExcludedCpu=decltype(emit_excluded_cpu_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitName=decltype(emit_name_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitSmt=decltype(emit_smt_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitBinding=decltype(emit_binding_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitTemplate=decltype(emit_template_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitServicePu=decltype(emit_service_pu_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitPool=decltype(emit_pool_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitWorkerPu=decltype(emit_worker_pu_callback(std::declval<Sink&>(),std::declval<Counts&>()));
using EmitWorker=decltype(emit_worker_callback(std::declval<Sink&>(),std::declval<Counts&>()));
struct ArrayCalls {
    ArrayCallCells<core::CpuSet,EmitCpu,std::size_t> cpu;
    ArrayCallCells<std::vector<topology::StableCoreId>,EmitCore> core;
    ArrayCallCells<std::vector<std::uint32_t>,EmitNumber> number;
    ArrayCallCells<std::vector<core::LinuxCpuId>,EmitExcludedCpu> excludedcpu;
    ArrayCallCells<std::vector<std::string>,EmitName> name;
    ArrayCallCells<std::vector<std::uint64_t>,EmitSmt> smt;
    ArrayCallCells<std::vector<scenario::ExperimentWorkerReference>,EmitBinding> binding;
    ArrayCallCells<std::vector<scenario::MatrixWorkerSelectorTemplate>,EmitTemplate> matrix;
    ArrayCallCells<std::vector<scenario::ResolvedServiceCpu>,EmitServicePu> servicepu;
    ArrayCallCells<PoolMap,EmitPool> pool;
    ArrayCallCells<std::vector<scenario::ResolvedProcessingUnit>,EmitWorkerPu> workerpu;
    ArrayCallCells<WorkerMap,EmitWorker> worker;
    core::LinuxCpuId materialized_set_dereference;
};
template<class Callback> struct ListCallCells {
    Parser* list_this;
    Callback callback_parameter;
    const Callback* callback_this;
    Parser* factory_this;
};
using ParseCpu=decltype(std::declval<Parser&>().cpu_callback(nullptr,std::declval<std::string&>(),std::declval<std::optional<std::uint32_t>&>(),std::declval<std::size_t&>(),std::declval<std::size_t&>()));
using ParseCore=decltype(std::declval<Parser&>().core_callback(nullptr));
using ParseNumber=decltype(std::declval<Parser&>().number_callback(nullptr));
using ParseExcludedCpu=decltype(std::declval<Parser&>().excluded_cpu_callback(std::declval<std::optional<scenario::MatrixWorkerSelectorTemplate>&>()));
using ParseName=decltype(std::declval<Parser&>().name_callback(std::declval<std::optional<scenario::MatrixWorkerSelectorTemplate>&>()));
using ParseSmt=decltype(std::declval<Parser&>().smt_callback(std::declval<std::optional<scenario::MatrixWorkerSelectorTemplate>&>(),std::declval<std::size_t&>()));
using ParseWorkerPu=decltype(std::declval<Parser&>().worker_pu_callback(std::declval<std::optional<scenario::ResolvedWorkerSet>&>()));
using ParseServicePu=decltype(std::declval<Parser&>().service_pu_callback(std::declval<std::optional<scenario::ResolvedServiceCpuPool>&>()));
using ParseTemplate=decltype(std::declval<Parser&>().template_callback());
using ParseWorker=decltype(std::declval<Parser&>().worker_callback());
using ParsePool=decltype(std::declval<Parser&>().pool_callback());
using ParseBinding=decltype(std::declval<Parser&>().binding_callback());
struct ListCalls {
    ListCallCells<ParseCpu> cpu;
    ListCallCells<ParseCore> core;
    ListCallCells<ParseNumber> number;
    ListCallCells<ParseExcludedCpu> excludedcpu;
    ListCallCells<ParseName> name;
    ListCallCells<ParseSmt> smt;
    ListCallCells<ParseWorkerPu> workerpu;
    ListCallCells<ParseServicePu> servicepu;
    ListCallCells<ParseTemplate> matrix;
    ListCallCells<ParseWorker> worker;
    ListCallCells<ParsePool> pool;
    ListCallCells<ParseBinding> binding;
    core::CpuSet* cpu_target;
    std::string* cpu_spelling;
    std::optional<std::uint32_t>* cpu_previous;
    std::size_t *cpu_components,*cpu_bytes;
    std::vector<topology::StableCoreId>* core_target;
    std::vector<std::uint32_t>* number_target;
    std::optional<scenario::MatrixWorkerSelectorTemplate> *excluded_item,*name_item,*smt_item;
    std::size_t* smt_indices;
    std::optional<scenario::ResolvedWorkerSet>* worker_set;
    std::optional<scenario::ResolvedServiceCpuPool>* service_pool;
};
using Selection=decltype(selection_callback(std::declval<std::vector<Identity>&>()));
struct SelectionCells {
    Selection selection_local;
    const Selection *worker_this,*service_this;
    std::vector<Identity>* factory_index;
};
} // namespace
namespace detail {
auto capture_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ValidationCells)+sizeof(OperationCells)+sizeof(RepresentationCells)+sizeof(CaptureCells)+sizeof(ArrayCalls)+sizeof(SelectionCells);
}
auto decode_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ValidationCells)+sizeof(OperationCells)+sizeof(RepresentationCells)+sizeof(ParserCells)+sizeof(FactoryCells)+sizeof(ArrayCalls)+sizeof(ListCalls)+sizeof(SelectionCells);
}
auto factory_error_stock() noexcept -> std::size_t {
    return sizeof(nlohmann::json::object_t)+2*sizeof(nlohmann::json::object_t::value_type)+
        2*sizeof(nlohmann::json::string_t)+
        std::string_view{"topology.stable_id.invalid_canonical_component"}.size()+
        std::string_view{"Stable topology identity text is malformed or noncanonical"}.size()+
        std::string_view{"parse_stable_topology_id"}.size()+std::string_view{"object_type"}.size()+
        std::string_view{"classification"}.size()+std::string_view{"processing_unit"}.size()+
        std::string_view{"malformed_or_noncanonical"}.size();
}
auto identity_element_stock() noexcept -> std::size_t { return sizeof(Identity); }
auto state_stock() noexcept -> std::size_t { return sizeof(State); }
} // namespace detail
Owner::Owner(std::unique_ptr<State> state) noexcept :state_(std::move(state)) {}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this!=&other) state_=std::move(other.state_);
    return *this;
}
Owner::~Owner()=default;
auto Owner::worker_sets() const noexcept -> const WorkerMap* { return state_ ? &state_->workers:nullptr; }
auto Owner::experiment_workers() const noexcept -> const std::vector<scenario::ExperimentWorkerReference>* { return state_ ? &state_->bindings:nullptr; }
auto Owner::matrix_worker_templates() const noexcept -> const std::vector<scenario::MatrixWorkerSelectorTemplate>* { return state_ ? &state_->templates:nullptr; }
auto Owner::service_cpu_pools() const noexcept -> const PoolMap* { return state_ ? &state_->pools:nullptr; }
auto Owner::reserved_service_cpus() const noexcept -> const core::CpuSet* { return state_ ? &state_->cpus:nullptr; }
auto Owner::reserved_service_cores() const noexcept -> const std::vector<topology::StableCoreId>* { return state_ ? &state_->cores:nullptr; }
auto Owner::topology_fingerprints() const noexcept -> const topology::TopologyFingerprints* { return state_ ? &*state_->fingerprints:nullptr; }
auto Owner::has_dedicated_service_placement() const noexcept -> std::optional<bool> { return state_ ? std::optional{state_->dedicated}:std::nullopt; }
auto Owner::resource_profile() const noexcept -> std::string_view { return state_ ? scenario::resource_resolution_profile:std::string_view{}; }
auto Owner::service_profile() const noexcept -> std::string_view { return state_ ? scenario::service_cpu_resolution_profile:std::string_view{}; }
auto Owner::canonical() const noexcept -> std::string_view { return state_ ? std::string_view{state_->canonical}:std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_ ? &*state_->digest:nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_ ? std::optional{state_->origin}:std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage:Usage{}; }
auto measure_capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& service=plan.matrix_expanded().kernel_dispatched().memory_placement_resolved().service_cpu_resolved();
        const auto& resource=service.resource_resolved();
        const View facts{resource.worker_sets(),resource.experiment_workers(),resource.matrix_worker_templates(),
            service.service_cpu_pools(),service.reserved_service_cpus(),service.reserved_service_cores(),
            resource.topology_fingerprints(),service.has_dedicated_service_placement()};
        Counts measured; measured.limits=&limits; Sink measuring{nullptr,0,limits.canonical_bytes}; emit(measuring,facts,measured);
        const auto usage=admit(measured,limits,false);
        return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto measure_decode(std::string_view input,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(input.size()<=limits.canonical_bytes,Error::capacity);
        Parser preflight{input}; preflight.count.limits=&limits; preflight.run(); const auto usage=admit(preflight.count,limits,true);
        return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Owner,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& service=plan.matrix_expanded().kernel_dispatched().memory_placement_resolved().service_cpu_resolved();
        const auto& resource=service.resource_resolved();
        const View facts{resource.worker_sets(),resource.experiment_workers(),resource.matrix_worker_templates(),
            service.service_cpu_pools(),service.reserved_service_cpus(),service.reserved_service_cores(),
            resource.topology_fingerprints(),service.has_dedicated_service_placement()};
        const auto probe=measure_capture(plan,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe;
        auto state=std::make_unique<State>();
        state->workers=facts.workers; state->bindings=facts.bindings; state->templates=facts.templates;
        state->pools=facts.pools; state->cpus=facts.cpus; state->cores=facts.cores;
        state->fingerprints=facts.fingerprints; state->dedicated=facts.dedicated;
        validate(view(*state),usage);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink rendering{&state->canonical};
        emit(rendering,view(*state),rendered);
        state->digest=core::sha256(state->canonical); state->origin=Origin::plan_captured; state->usage=usage;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view input,Limits limits) -> std::expected<Owner,Error> {
    try {
        const auto probe=measure_decode(input,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe;
        auto state=std::make_unique<State>(); Parser parser{input,0,{},state.get()}; parser.run();
        validate(view(*state),usage);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink rendering{&state->canonical};
        emit(rendering,view(*state),rendered); need(state->canonical==input,Error::encoding);
        state->digest=core::sha256(state->canonical); state->origin=Origin::decoded_unassociated; state->usage=usage;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
} // namespace epycbench::results::resource_service_serialization
