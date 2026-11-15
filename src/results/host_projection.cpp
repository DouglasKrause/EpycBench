#include "results/host_projection.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/serialization.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <tuple>
#include <type_traits>

namespace epycbench::results::host_projection {
namespace {
using Json = nlohmann::json;
struct Rejected { Error error; };
void require(bool condition, Error error = Error::source) { if (!condition) throw Rejected{error}; }
void bound(std::size_t value, std::size_t maximum) { require(value <= maximum, Error::capacity); }
auto plus(std::size_t a, std::size_t b) -> std::size_t {
    bound(b, SIZE_MAX - a); return a + b;
}
auto times(std::size_t a, std::size_t b) -> std::size_t {
    if (a) bound(b, SIZE_MAX / a);
    return a * b;
}
constexpr std::array category_names{"topology", "system_controls", "kernel_identity", "pu_identity",
    "frequency", "anonymous_thp", "process_thp"};
constexpr std::array selector_names{"owner_preparation", "smt_control", "smt_active", "numa_mask", "thp_enabled",
    "thp_defrag", "sysname", "release", "version", "signature", "microcode", "uncovered_cpu", "enumeration",
    "page_size", "process_observation"};
constexpr std::array state_names{"not_attempted", "available", "unavailable", "failed"};
constexpr std::array reason_names{"not_supported", "source_missing", "access_denied", "read_failed",
    "malformed_source", "unsupported_value"};
template<class E, std::size_t N> auto name(E value, const std::array<const char*, N>& names) -> std::string_view {
    const auto index = static_cast<std::size_t>(value); require(index < names.size()); return names[index];
}
struct Number {
    std::array<char, 20> bytes{};
    std::size_t size{};
    explicit Number(std::uint64_t value) { const auto result = std::to_chars(bytes.data(), bytes.data()+bytes.size(), value);
        require(result.ec == std::errc{}); size = static_cast<std::size_t>(result.ptr-bytes.data()); }
    auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};
// The count/build passes execute the same typed visitor. No private DOM is parsed.
struct Counts { std::size_t nodes{}, depth{}, text{}, bytes{}, objects{}, arrays{}, strings{}, members{}, elements{}, sort_entries{}; };
struct Frame { Json* json{}; std::size_t children{}; bool object{}; };
class Sink {
  public:
    Counts count;
    explicit Sink(Json* output = nullptr, std::vector<std::size_t>* scratch = nullptr) : output_(output), scratch_(scratch) {}
    void object(std::string_view key = {}) { open(key, true, 0); }
    void array(std::string_view key, std::size_t size) { open(key, false, size); }
    void end() { require(level_ != 0); --level_; count.bytes=plus(count.bytes,1); }
    void text(std::string_view key, std::string_view value) {
        prefix(key); count.strings=plus(count.strings,1); count.text=plus(count.text,value.size());
        count.bytes=plus(count.bytes,quoted(value));
        if(output_) { Json item=std::string{value}; insert(key,item); }
    }
    void number(std::string_view key, std::uint64_t value) { const Number number{value}; text(key,number.view()); }
    void boolean(std::string_view key, bool value) {
        prefix(key); count.bytes=plus(count.bytes,value?4U:5U);
        if(output_) { Json item=value; insert(key,item); }
    }
    void null(std::string_view key) {
        prefix(key); count.bytes=plus(count.bytes,4); if(output_) { Json item; insert(key,item); }
    }
    void optional_text(std::string_view key, const std::optional<std::string>& value) {
        if(value)text(key,*value);else null(key);
    }
    void omitted(std::string_view key) { object(key); text("omitted","opaque_text"); end(); }
    auto sort_enter(std::size_t size) -> std::size_t {
        const auto base=sort_depth_;sort_depth_=plus(sort_depth_,size);
        count.sort_entries=std::max(count.sort_entries,sort_depth_);
        if(scratch_)scratch_->resize(sort_depth_);
        return base;
    }
    void sort_leave(std::size_t base) { sort_depth_=base;if(scratch_)scratch_->resize(base); }
    auto scratch() noexcept -> std::vector<std::size_t>* { return scratch_; }
  private:
    Json* output_{};
    std::vector<std::size_t>* scratch_{};
    std::array<Frame,64> stack_{};
    std::size_t level_{}, sort_depth_{};
    static auto quoted(std::string_view text) -> std::size_t {
        std::size_t size=2;
        for(char raw:text) { const auto c=static_cast<unsigned char>(raw); size=plus(size,c=='"'||c=='\\'||c=='\b'||c=='\f'||c=='\n'||c=='\r'||c=='\t'?2U:c<0x20?6U:1U); }
        return size;
    }
    void prefix(std::string_view key) {
        count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);
        if(!level_)return;
        auto& parent=stack_[level_-1];
        if(parent.children++)count.bytes=plus(count.bytes,1);
        if(parent.object) { count.members=plus(count.members,1);count.text=plus(count.text,key.size());
            count.bytes=plus(count.bytes,plus(quoted(key),1)); }
        else count.elements=plus(count.elements,1);
    }
    auto insert(std::string_view key,Json& item) -> Json* {
        core::detail::JsonCleanup cleanup{item};
        if(!level_) { output_->swap(item); return output_; }
        auto* parent=stack_[level_-1].json;
        if(parent->is_array()) { parent->push_back(std::move(item));return &parent->back(); }
        core::detail::append_object(*parent,key,item);return &parent->get_ref<Json::object_t&>().find(key)->second;
    }
    void open(std::string_view key,bool object,std::size_t size) {
        prefix(key);bound(level_+1,stack_.size());count.bytes=plus(count.bytes,1);
        if(object)count.objects=plus(count.objects,1);else count.arrays=plus(count.arrays,1);
        Json* value=nullptr;
        if(output_) { Json item=object?Json::object():Json::array();core::detail::JsonCleanup cleanup{item};
            if(!object) { bound(size,item.get_ref<Json::array_t&>().max_size());item.get_ref<Json::array_t&>().reserve(size); }
            value=insert(key,item); }
        stack_[level_++]={value,0,object};
    }
};
// Count pass needs only populations; the build pass sorts exact-reserved index
// segments. Nested segments coexist, and the single backing never reallocates.
template<class Range,class Less,class Emit> void ordered(Sink& sink,const Range& range,Less less,Emit emit) {
    const auto base=sink.sort_enter(range.size());
    if(auto* scratch=sink.scratch()) {
        auto first=scratch->begin()+static_cast<std::ptrdiff_t>(base);
        for(std::size_t i=0;i<range.size();++i)first[static_cast<std::ptrdiff_t>(i)]=i;
        std::sort(first,scratch->end(),[&](std::size_t a,std::size_t b){return less(range[a],range[b]);});
        for(std::size_t i=0;i<range.size();++i)emit(range[(*scratch)[base+i]]);
    }else for(const auto& value:range)emit(value);
    sink.sort_leave(base);
}
void evidence(Sink& sink,std::string_view key,const topology::Evidence& value) {
    constexpr std::array sources{"hwloc","linux_sysfs","linux_procfs","numa_api","architecture_specific","fixture","derived","user"};
    constexpr std::array confidences{"reported","derived","inferred","user_supplied","unknown"};
    sink.object(key);sink.text("source",name(value.source,sources));sink.text("confidence",name(value.confidence,confidences));sink.end();
}
auto evidence_less(const topology::Evidence& a,const topology::Evidence& b) noexcept -> bool {
    return std::tie(a.source,a.confidence)<std::tie(b.source,b.confidence);
}
void evidences(Sink& sink,std::string_view key,const std::vector<topology::Evidence>& values) {
    sink.array(key,values.size());ordered(sink,values,evidence_less,[&](const auto& value){evidence(sink,{},value);});sink.end();
}
void failure(Sink& sink,std::string_view key,const FactKey& target,std::span<const ResolvedFailure> table) {
    const auto found=std::lower_bound(table.begin(),table.end(),target,[](const auto& a,const auto& b){return a.target<b;});
    require(found!=table.end() && found->target==target,Error::reference);
    sink.object(key);sink.number("sequence",found->sequence);
    if(found->creation_ordinal)sink.number("creation_ordinal",*found->creation_ordinal);else sink.null("creation_ordinal");
    sink.object("target");sink.text("category",name(target.category,category_names));sink.text("selector",name(target.selector,selector_names));
    sink.number("ordinal",target.ordinal);sink.number("key",target.key);
    constexpr std::array reasons{"none","read_failed","malformed_source","unsupported_value"};
    sink.text("reason",name(target.reason,reasons));sink.end();sink.end();
}
template<class R> auto failure_reason(R value) -> FailureReason {
    if(value==R::read_failed)return FailureReason::read_failed;
    if(value==R::malformed_source)return FailureReason::malformed_source;
    if(value==R::unsupported_value)return FailureReason::unsupported_value;
    throw Rejected{Error::source};
}
template<class O> void observation_fields(Sink& sink,const O& value,FactKey key,std::span<const ResolvedFailure> table) {
    sink.optional_text("observed_at",value.observed_at);
    if(value.reason)sink.text("reason",name(*value.reason,reason_names));else sink.null("reason");
    if(value.failure_event) { require(value.reason.has_value());key.reason=failure_reason(*value.reason);failure(sink,"failure",key,table); }
    else sink.null("failure");
}
template<class O,class Value> void observation(Sink& sink,std::string_view label,const O& observed,FactKey key,
    std::span<const ResolvedFailure> table,Value value) {
    sink.object(label);sink.text("state",name(observed.state,state_names));
    if(observed.state==decltype(observed.state)::available)value();else sink.null("value");
    observation_fields(sink,observed,key,table);sink.end();
}
template<class O> void common(Sink& sink,const O& owner) {
    require(owner.producer().has_value() && owner.ordinal().has_value());
    sink.text("producer",*owner.producer()==std::remove_cvref_t<decltype(*owner.producer())>::controller?"controller":"runner");
    sink.number("ordinal",*owner.ordinal());sink.text("topology_association",owner.topology()?"retained_topology":"none");
}
void payload(Sink& sink,const host_controls::Owner& owner,std::span<const ResolvedFailure> table) {
    common(sink,owner);
    constexpr std::array slots{"smt_control","smt_active","numa_balancing","thp_top_level_enabled","thp_defrag"};
    constexpr std::array selectors{Selector::smt_control,Selector::smt_active,Selector::numa_mask,Selector::thp_enabled,Selector::thp_defrag};
    sink.array("observations",5);std::size_t index=0;
    for(const auto& observed:owner.observations()) {
        sink.object();sink.text("slot",slots[index]);
        observation(sink,"observation",observed,{Category::system_controls,selectors[index],*owner.ordinal()},table,[&]{
            std::visit([&](const auto& value){
                using T=std::remove_cvref_t<decltype(value)>;
                if constexpr(std::is_same_v<T,host_controls::SmtControl>) {
                    constexpr std::array modes{"on","off","forceoff","notsupported","notimplemented","threads"};
                    sink.object("value");sink.text("mode",name(value.mode,modes));
                    if(value.threads_per_core)sink.number("threads_per_core",*value.threads_per_core);else sink.null("threads_per_core");sink.end();
                } else if constexpr(std::is_same_v<T,bool>)sink.boolean("value",value);
                else if constexpr(std::is_same_v<T,std::uint32_t>)sink.number("value",value);
                else if constexpr(std::is_same_v<T,host_controls::ThpEnabled>) {
                    constexpr std::array modes{"always","madvise","never"};sink.text("value",name(value,modes));
                } else if constexpr(std::is_same_v<T,host_controls::ThpDefrag>) {
                    constexpr std::array modes{"always","defer","defer_madvise","madvise","never"};sink.text("value",name(value,modes));
                } else throw Rejected{Error::source};
            },observed.value);
        });sink.end();++index;
    }sink.end();
}
void payload(Sink& sink,const private_kernel_identity::Owner& owner,std::span<const ResolvedFailure> table) {
    common(sink,owner);constexpr std::array slots{"sysname","release","version"};
    constexpr std::array selectors{Selector::sysname,Selector::release,Selector::version};
    sink.array("observations",3);std::size_t index=0;
    for(const auto& observed:owner.observations()) {
        sink.object();sink.text("slot",slots[index]);
        observation(sink,"observation",observed,{Category::kernel_identity,selectors[index],*owner.ordinal()},table,[&]{
            if(index==0)sink.text("value","Linux");else sink.omitted("value");
        });sink.end();++index;
    }sink.end();
}
void payload(Sink& sink,const pu_cpu_identity::Owner& owner,std::span<const ResolvedFailure> table) {
    common(sink,owner);sink.array("rows",owner.rows().size());
    for(const auto& row:owner.rows()) {
        sink.object();sink.number("linux_cpu",row.linux_cpu.value());sink.text("stable_pu_id",row.stable_pu_id);
        sink.boolean("topology_online",row.topology_online);
        observation(sink,"signature",row.signature,{Category::pu_identity,Selector::signature,*owner.ordinal(),row.linux_cpu.value()},table,[&]{
            const auto& value=std::get<pu_cpu_identity::DecodedSignature>(row.signature.value);
            sink.object("value");sink.number("eax",value.eax);sink.number("family",value.family);
            sink.number("model",value.model);sink.number("stepping",value.stepping);sink.end();
        });
        observation(sink,"microcode",row.microcode,{Category::pu_identity,Selector::microcode,*owner.ordinal(),row.linux_cpu.value()},table,[&]{
            sink.number("value",std::get<pu_cpu_identity::Microcode>(row.microcode.value).revision);
        });sink.end();
    }sink.end();sink.text("unrepresented_present_cpus",owner.unrepresented_present_cpus());
}
void association(Sink& sink,const frequency_policy_owner::Association& value) {
    sink.number("linux_cpu",value.linux_cpu.value());
    if(value.stable_pu_id)sink.text("stable_pu_id",*value.stable_pu_id);else sink.null("stable_pu_id");
    sink.boolean("topology_online",value.topology_online);
}
void payload(Sink& sink,const frequency_policy_owner::Owner& owner,std::span<const ResolvedFailure> table) {
    common(sink,owner);sink.array("policies",owner.policies().size());std::size_t index=0;
    for(const auto& row:owner.policies()) {
        sink.object();sink.number("index",index++);sink.text("observed_at",row.observed_at);
        constexpr std::array sources{"hardware_current","scaling_current"};sink.text("source_kind",name(row.source_kind,sources));
        sink.omitted("scaling_driver");sink.omitted("scaling_governor");
        sink.number("scaling_minimum_hz",row.scaling_minimum.value());sink.number("scaling_maximum_hz",row.scaling_maximum.value());
        sink.array("related_cpus",row.related_cpus.size());for(const auto& cpu:row.related_cpus){sink.object();association(sink,cpu);sink.end();}sink.end();
        sink.array("topology_online_cpus",row.topology_online_cpus.size());for(auto cpu:row.topology_online_cpus)sink.number({},cpu.value());sink.end();sink.end();
    }sink.end();sink.array("uncovered_cpus",owner.uncovered_cpus().size());
    for(const auto& row:owner.uncovered_cpus()) {
        sink.object();association(sink,row.association);
        constexpr std::array states{"not_attempted","unavailable","failed"};sink.text("state",name(row.state,states));
        observation_fields(sink,row,{Category::frequency,Selector::uncovered_cpu,*owner.ordinal(),row.association.linux_cpu.value()},table);sink.end();
    }sink.end();
}
void payload(Sink& sink,const anonymous_thp_policy_owner::Owner& owner,std::span<const ResolvedFailure> table) {
    common(sink,owner);require(owner.enumeration());const auto& enumeration=*owner.enumeration();
    constexpr std::array states{"not_attempted","complete","unavailable","failed"};
    sink.object("enumeration");sink.text("state",name(enumeration.state,states));
    observation_fields(sink,enumeration,{Category::anonymous_thp,Selector::enumeration,*owner.ordinal()},table);sink.end();
    sink.array("sizes",owner.rows().size());for(const auto& row:owner.rows()) {
        sink.object();sink.number("size_bytes",row.size.value());sink.text("state",name(row.state,state_names));
        constexpr std::array modes{"always","madvise","never","inherit"};
        if(row.mode)sink.text("mode",name(*row.mode,modes));else sink.null("mode");
        observation_fields(sink,row,{Category::anonymous_thp,Selector::page_size,*owner.ordinal(),row.size.value()},table);sink.end();
    }sink.end();
}
void payload(Sink& sink,const process_thp_restriction::Owner& owner,std::span<const ResolvedFailure> table,
    const std::optional<ResolvedSubject>& subject) {
    common(sink,owner);require(subject.has_value(),Error::reference);require(owner.observation());
    sink.object("subject");sink.text("role",subject->role==Role::controller?"controller":"runner");
    sink.number("creation_sequence",subject->creation_sequence);sink.end();
    observation(sink,"observation",*owner.observation(),{Category::process_thp,Selector::process_observation,*owner.ordinal(),subject->creation_sequence},table,[&]{
        require(owner.observation()->value.has_value());
        sink.text("value",*owner.observation()->value==process_thp_restriction::Restriction::disabled?"disabled":"not_disabled");
    });
}
class Graph {
  public:
    explicit Graph(const topology::LoadedTopology& value): graph_(value.topology),ids_(value.stable_ids) {}
    void emit(Sink& sink) const {
        const auto& machine=graph_.machine();sink.object("machine");sink.text("id","machine");sink.omitted("architecture");
        attribute(sink,"cpu_vendor",machine.cpu_vendor);attribute(sink,"cpu_model",machine.cpu_model);
        attribute(sink,"total_memory_bytes",machine.total_memory);numbers(sink,"page_sizes_bytes",machine.page_sizes);
        value(sink,"present_cpus",machine.present_cpus);value(sink,"online_cpus",machine.online_cpus);
        attribute(sink,"allowed_cpus",machine.allowed_cpus);attribute(sink,"effective_cpus",machine.effective_cpus);
        value(sink,"present_nodes",machine.present_nodes);value(sink,"online_nodes",machine.online_nodes);
        attribute(sink,"allowed_nodes",machine.allowed_nodes);attribute(sink,"effective_nodes",machine.effective_nodes);sink.end();
        objects(sink,"packages",graph_.packages(),[&](const auto& row){attribute(sink,"os_id",row.os_id);});
        objects(sink,"dies",graph_.dies(),[&](const auto& row){sink.text("package",id(row.package_id));attribute(sink,"os_id",row.os_id);});
        objects(sink,"cores",graph_.cores(),[&](const auto& row){sink.text("package",id(row.package_id));
            if(row.die_id)sink.text("die",id(*row.die_id));else sink.null("die");attribute(sink,"os_id",row.os_id);});
        objects(sink,"processing_units",graph_.processing_units(),[&](const auto& row){sink.number("linux_cpu",row.linux_cpu_id.value());
            sink.text("core",id(row.core_id));attribute(sink,"isolated",row.isolated);});
        objects(sink,"caches",graph_.caches(),[&](const auto& row){sink.number("level",row.level);
            constexpr std::array kinds{"data","instruction","unified","unknown"};sink.text("kind",name(row.kind,kinds));
            sink.number("size_bytes",row.size.value());sink.number("line_bytes",row.line_size.value());
            attribute(sink,"associativity",row.associativity);value(sink,"processing_units",row.member_pus);evidences(sink,"evidence",row.provenance);});
        objects(sink,"numa_nodes",graph_.numa_nodes(),[&](const auto& row){attribute(sink,"linux_node",row.linux_node_id);
            attribute(sink,"installed_memory_bytes",row.installed_memory);attribute(sink,"available_memory_bytes",row.available_memory);
            value(sink,"processing_units",row.member_pus);numbers(sink,"page_sizes_bytes",row.page_sizes);});
        const auto& distances=graph_.numa_distances().directed_distances;sink.array("numa_distances",distances.size());
        ordered(sink,distances,[&](const auto& a,const auto& b){return std::pair{id(a.from),id(a.to)}<std::pair{id(b.from),id(b.to)};},[&](const auto& row){
            sink.object();sink.text("from",id(row.from));sink.text("to",id(row.to));sink.number("value",row.value);sink.end();});sink.end();
        objects(sink,"pci_devices",graph_.pci_devices(),[&](const auto& row){sink.number("domain",row.address.domain());sink.number("bus",row.address.bus());
            sink.number("device",row.address.device());sink.number("function",row.address.function());sink.number("vendor_id",row.vendor_id);
            sink.number("device_id",row.device_id);sink.number("class_code",row.class_code);attribute(sink,"numa_locality",row.numa_locality);
            attribute(sink,"local_processing_units",row.local_pus);});
    }
  private:
    const topology::TopologySnapshot& graph_;
    const topology::StableTopologyIds& ids_;
    template<class T> auto id(T handle) const -> std::string_view {
        if constexpr(std::is_same_v<T,core::PackageId>)return ids_.package_id(handle)->get().value();
        else if constexpr(std::is_same_v<T,core::DieId>)return ids_.die_id(handle)->get().value();
        else if constexpr(std::is_same_v<T,core::CoreId>)return ids_.core_id(handle)->get().value();
        else if constexpr(std::is_same_v<T,core::ProcessingUnitId>)return ids_.processing_unit_id(handle)->get().value();
        else if constexpr(std::is_same_v<T,core::CacheId>)return ids_.cache_id(handle)->get().value();
        else if constexpr(std::is_same_v<T,core::NumaNodeId>)return ids_.numa_node_id(handle)->get().value();
        else return ids_.pci_device_id(handle)->get().value();
    }
    template<class T> auto object_id(const T& value) const -> std::string_view {
        if constexpr(requires{value.id;})return id(value.id);else return id(value.address);
    }
    template<class Range,class Emit> void objects(Sink& sink,std::string_view key,const Range& rows,Emit emit) const {
        sink.array(key,rows.size());ordered(sink,rows,[&](const auto& a,const auto& b){return object_id(a)<object_id(b);},[&](const auto& row){
            sink.object();sink.text("id",object_id(row));emit(row);sink.end();});sink.end();
    }
    template<class Range> void numbers(Sink& sink,std::string_view key,const Range& values) const {
        sink.array(key,values.size());for(const auto& item:values)sink.number({},item.value());sink.end();
    }
    template<class T> void value(Sink& sink,std::string_view key,const T& item) const {
        if constexpr(std::is_same_v<T,std::string>)sink.omitted(key);
        else if constexpr(std::is_same_v<T,bool>)sink.boolean(key,item);
        else if constexpr(std::is_same_v<T,topology::ProcessingUnitSet>) {
            sink.array(key,static_cast<std::size_t>(item.cardinality()));
            // Iterate stable IDs to sort without copying the source set or IDs.
            for(const auto& entry:ids_.processing_units())
                if(item.contains(entry.handle))sink.text({},entry.semantic_id.value());
            sink.end();
        } else if constexpr(requires{item.cardinality();}) {
            sink.array(key,static_cast<std::size_t>(item.cardinality()));for(auto member:item)sink.number({},member.value());sink.end();
        } else if constexpr(std::is_same_v<T,core::NumaNodeId>)sink.text(key,id(item));
        else if constexpr(requires{item.value();})sink.number(key,item.value());
        else sink.number(key,item);
    }
    template<class T> auto value_less(const T& a,const T& b) const -> bool {
        if constexpr(std::is_same_v<T,std::string>)return false;
        else if constexpr(std::is_same_v<T,bool>)return !a && b;
        else if constexpr(std::is_same_v<T,core::NumaNodeId>)return id(a)<id(b);
        else if constexpr(std::is_same_v<T,topology::ProcessingUnitSet>) {
            const auto entries=ids_.processing_units();auto ai=entries.begin(),bi=entries.begin();
            for(;;) {
                while(ai!=entries.end() && !a.contains(ai->handle))++ai;
                while(bi!=entries.end() && !b.contains(bi->handle))++bi;
                if(ai==entries.end() || bi==entries.end())return ai!=entries.end();
                if(ai->semantic_id.value()!=bi->semantic_id.value())return ai->semantic_id.value()<bi->semantic_id.value();
                ++ai;++bi;
            }
        } else if constexpr(requires{a.cardinality();}) {
            auto ai=a.begin(),bi=b.begin();
            while(ai!=a.end() && bi!=b.end()) {
                const Number an{(*ai).value()},bn{(*bi).value()};if(an.view()!=bn.view())return an.view()<bn.view();
                ++ai;++bi;
            }
            // In canonical JSON a comma sorts before the closing bracket.
            return ai!=a.end();
        } else if constexpr(requires{a.value();}) {const Number an{a.value()},bn{b.value()};return an.view()<bn.view();}
        else {const Number an{a},bn{b};return an.view()<bn.view();}
    }
    template<class T> void attribute(Sink& sink,std::string_view key,const topology::Attribute<T>& item) const {
        sink.object(key);constexpr std::array availability{"available","unavailable","unknown","conflicted"};
        sink.text("availability",name(item.availability(),availability));
        if(item.value())value(sink,"value",*item.value());else sink.null("value");
        evidences(sink,"evidence",item.evidence());sink.array("conflicts",item.conflicts().size());
        ordered(sink,item.conflicts(),[&](const auto& a,const auto& b){return value_less(a.value,b.value) ||
            (!value_less(b.value,a.value) && evidence_less(a.evidence,b.evidence));},[&](const auto& conflict){
                sink.object();value(sink,"value",conflict.value);evidence(sink,"evidence",conflict.evidence);sink.end();});sink.end();sink.end();
    }
};
template<class T> void slot(Sink& sink,std::string_view key,const host_assembly::Slot<T>& source,Category category,
    std::span<const ResolvedFailure> table,const topology::LoadedTopology* graph,const std::optional<ResolvedSubject>& subject) {
    sink.object(key);constexpr std::array states{"not_attempted","unavailable","failed","retained"};
    require(!source.valueless_by_exception());sink.text("state",states[source.index()]);
    if(const auto* unavailable=std::get_if<host_assembly::Unavailable>(&source)) {
        constexpr std::array reasons{"not_supported","source_missing","access_denied"};sink.text("reason",name(unavailable->reason,reasons));
    }else sink.null("reason");
    if(std::holds_alternative<host_assembly::Failed>(source))failure(sink,"failure",{category},table);else sink.null("failure");
    if(const auto* child=std::get_if<T>(&source)) {
        sink.object("payload");
        if constexpr(std::is_same_v<T,topology_evidence::Owner>) {require(graph);Graph{*graph}.emit(sink);}
        else if constexpr(std::is_same_v<T,process_thp_restriction::Owner>)payload(sink,*child,table,subject);
        else payload(sink,*child,table);
        sink.end();
    }else sink.null("payload");sink.end();
}
void project(Sink& sink,const host_assembly::Categories& source,std::span<const ResolvedFailure> table,
    const topology::LoadedTopology* graph,const std::optional<ResolvedSubject>& subject) {
    sink.object();sink.text("schema",profile);sink.object("categories");
    slot(sink,"topology",source.topology,Category::topology,table,graph,subject);
    slot(sink,"system_controls",source.system_controls,Category::system_controls,table,graph,subject);
    slot(sink,"kernel_identity",source.kernel_identity,Category::kernel_identity,table,graph,subject);
    slot(sink,"pu_identity",source.pu_identity,Category::pu_identity,table,graph,subject);
    slot(sink,"frequency",source.frequency,Category::frequency,table,graph,subject);
    slot(sink,"anonymous_thp",source.anonymous_thp,Category::anonymous_thp,table,graph,subject);
    slot(sink,"process_thp",source.process_thp,Category::process_thp,table,graph,subject);sink.end();
    constexpr std::array<std::pair<std::string_view,std::string_view>,9> omissions{{
        {"source_identity","root"},{"private_error_payload","failures"},{"original_child_bytes_and_digests","categories"},
        {"topology_fingerprints","topology"},{"machine_text","topology"},{"provenance_locators","topology"},
        {"architecture_group_graph","topology"},{"kernel_release_version","kernel_identity"},{"frequency_labels","frequency"}}};
    sink.array("omissions",omissions.size());for(const auto& [kind,scope]:omissions) {
        sink.object();sink.text("kind",kind);sink.text("scope",scope);sink.end();
    }sink.end();sink.end();
}
auto dom_stock(const Counts& c) -> std::size_t {
    std::size_t total=sizeof(Json);
    total=plus(total,times(c.objects,sizeof(Json::object_t)));total=plus(total,times(c.arrays,sizeof(Json::array_t)));
    total=plus(total,times(c.strings,sizeof(Json::string_t)));total=plus(total,times(c.members,sizeof(Json::object_t::value_type)));
    total=plus(total,times(c.elements,sizeof(Json)));return plus(total,c.text);
}
constexpr std::array reload_limits{&topology_evidence::Limits::bytes,&topology_evidence::Limits::tokens,
    &topology_evidence::Limits::depth,&topology_evidence::Limits::string_token,&topology_evidence::Limits::scalar_token,
    &topology_evidence::Limits::objects,&topology_evidence::Limits::processing_units,&topology_evidence::Limits::numa_nodes,
    &topology_evidence::Limits::groups,&topology_evidence::Limits::array_entries,&topology_evidence::Limits::expanded_ranges,
    &topology_evidence::Limits::ancestry,&topology_evidence::Limits::derived_ids,&topology_evidence::Limits::retained};
constexpr std::array reload_usage{&topology_evidence::Usage::bytes,&topology_evidence::Usage::tokens,
    &topology_evidence::Usage::depth,&topology_evidence::Usage::string_token,&topology_evidence::Usage::scalar_token,
    &topology_evidence::Usage::objects,&topology_evidence::Usage::processing_units,&topology_evidence::Usage::numa_nodes,
    &topology_evidence::Usage::groups,&topology_evidence::Usage::array_entries,&topology_evidence::Usage::expanded_ranges,
    &topology_evidence::Usage::ancestry,&topology_evidence::Usage::derived_ids,&topology_evidence::Usage::retained};
// Named conservative logical frames. Alternative nonrecursive payload paths are
// simultaneously charged; library reload/rendering internals remain separately
// structurally bounded. No compiler stack or allocator-overhead claim.
struct VisitorCells {
    const host_assembly::Categories* categories;
    std::span<const ResolvedFailure> table;
    const topology::LoadedTopology* graph;
    const std::optional<ResolvedSubject>* subject;
    Sink* sink;
    const void *slot, *child, *observation, *row, *value, *attribute, *conflict;
    std::string_view key, name, id, left_id, right_id;
    Category category;
    FactKey target, failure_parameter;
    std::span<const ResolvedFailure>::iterator found;
    const topology::Evidence *evidence, *left_evidence, *right_evidence;
    std::size_t index, emitted, base, candidate, sort_a, sort_b;
    std::vector<std::size_t>* scratch;
    std::vector<std::size_t>::iterator first, last;
    std::span<const topology::StableProcessingUnitEntry> stable_entries;
    std::span<const topology::StableProcessingUnitEntry>::iterator stable_left, stable_right;
    Number number, comparison_left, comparison_right;
    Graph graph_view;
    std::optional<std::reference_wrapper<const topology::StableProcessingUnitId>> pu_id;
    std::optional<std::reference_wrapper<const topology::StableNumaNodeId>> node_id;
    core::CpuSet::const_iterator cpu_begin, cpu_end, cpu_other;
    core::NodeSet::const_iterator node_begin, node_end, node_other;
    topology::ProcessingUnitSet::const_iterator pu_begin, pu_end, pu_other;
    // Closure references for nested object/attribute/evidence/ordering emitters.
    std::array<const void*,4> object_capture, attribute_capture, evidence_capture, ordering_capture;
    std::array<const char*,5> control_names, defrag_names;
    std::array<const char*,3> kernel_names, enabled_names;
    std::array<const char*,6> smt_names;
    std::array<const char*,4> state_names, mode_names, availability_names;
    std::array<const char*,2> frequency_names;
    std::array<const char*,8> evidence_sources;
    std::array<const char*,5> evidence_confidences;
    std::array<Selector,5> control_selectors;
    std::array<Selector,3> kernel_selectors;
    std::array<std::pair<std::string_view,std::string_view>,9> omission_declarations;
    std::optional<std::uint64_t> ordinal, ordinal_return;
    std::optional<host_controls::Producer> producer, producer_return;
    std::pair<std::string_view,std::string_view> distance_left, distance_right;
};
struct SinkCallCells {
    Sink* text_this;
    std::string_view text_key, text_value;
    Json text_item;
    Sink* number_this;
    std::string_view number_key;
    std::uint64_t number_value;
    Number number_buffer;
    std::to_chars_result number_conversion;
    Sink* open_this;
    std::string_view open_key;
    bool open_object;
    std::size_t open_size;
    Json open_item;
    Json* open_value;
    core::detail::JsonCleanup<Json> open_cleanup;
    Sink* prefix_this;
    std::string_view prefix_key;
    Frame* prefix_parent;
    Sink* insert_this;
    std::string_view insert_key;
    Json *insert_item, *insert_parent, *insert_result;
    core::detail::JsonCleanup<Json> insert_cleanup;
    std::string append_key;
    Json *append_parent, *append_child;
    std::string_view append_name;
    std::string_view quote_input;
    std::size_t quote_size, quote_result;
    char quote_raw;
    unsigned char quote_byte;
    std::size_t bound_value, bound_maximum;
    bool require_condition;
    Error require_error;
    std::underlying_type_t<Category> name_value;
    const void* name_array;
    std::size_t name_index;
    std::string_view name_result;
};
struct FixedCells {
    const detail::ResolvedInput* input;
    const Limits* limits;
    SinkCallCells sink_calls;
    Usage usage;
    const host_assembly::Categories* categories;
    const topology_evidence::Owner* topology;
    topology_evidence::Usage source_return;
    topology_evidence::Limits supported;
    std::optional<topology::LoadedTopology> loaded;
    std::expected<topology::LoadedTopology,core::Error> loaded_result;
    core::detail::ExpectedErrorCleanup<decltype(loaded_result)> loaded_cleanup;
    Sink counter, builder;
        Json dom, insertion_value;
    core::detail::JsonCleanup<Json> dom_cleanup, insertion_cleanup;
    std::string insertion_key;
    std::unique_ptr<detail::State> state, constructor_parameter;
    PrivateProvenance* provenance;
    std::size_t fixed, provenance_bytes, dom_bytes, retained, construction, rendering, publication, index, scratch_bytes;
    std::vector<std::size_t> scratch;
    std::expected<std::string,core::Error> rendered;
    core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_cleanup;
    core::JcsLimits renderer_limits;
    core::Sha256Digest digest_return;
    std::expected<Owner,Error> returned;
    Owner constructed;
    std::size_t sum_a, sum_b, sum_return, multiply_a, multiply_b, multiply_return;
    std::string_view quoted_text, prefix_key, insert_key, text_key, text_value;
    std::size_t quoted_size;
    unsigned char quoted_char;
    Json* insert_return;
    Number number;
    std::to_chars_result converted;
    // Maximum nested typed paths: category -> graph object -> Attribute ->
    // conflict -> reference set/evidence; each has an explicit frame allowance.
    VisitorCells category_frame, graph_frame, attribute_frame, conflict_frame, value_frame;
};
} // namespace
namespace detail {
auto fixed_stock() noexcept -> std::size_t { return sizeof(FixedCells); }
auto valid_limits(const Limits& limits) noexcept -> bool {
    if(limits.depth>64 || limits.canonical_bytes>std::string{}.max_size())return false;
    const auto maximum=topology_evidence::supported_limits();
    for(auto member:reload_limits)if(limits.reload.*member>maximum.*member)return false;
    return true;
}
}
Owner::Owner(std::unique_ptr<detail::State> state) noexcept:state_(std::move(state)) {}
Owner::~Owner()=default;
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
auto Owner::reduced_canonical() const noexcept -> std::string_view {return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::reduced_digest() const noexcept -> const core::Sha256Digest* {return state_?&state_->digest:nullptr;}
auto Owner::private_provenance() const noexcept -> const PrivateProvenance* {return state_?&state_->provenance:nullptr;}
auto Owner::usage() const noexcept -> Usage {return state_?state_->usage:Usage{};}
auto Owner::prepare(const detail::ResolvedInput& input,const Limits& limits) -> std::expected<Owner,Error> {
    try {
        require(detail::valid_limits(limits),Error::invalid_limits);
        const auto* categories=input.host.categories();require(categories && input.host.run_id()==input.run);
        Usage usage{};usage.failure_references=input.failures.size();bound(usage.failure_references,limits.failure_references);
        std::optional<topology::LoadedTopology> loaded;
        if(const auto* topology=std::get_if<topology_evidence::Owner>(&categories->topology)) {
            require(topology->digest() && topology->fingerprints());usage.reload=topology->usage();
            for(std::size_t i=0;i<reload_limits.size();++i)bound(usage.reload.*reload_usage[i],limits.reload.*reload_limits[i]);
            auto result=topology::deserialize_topology(topology->canonical(),topology::TopologyJsonInputPolicy::require_canonical);
            core::detail::ExpectedErrorCleanup cleanup{result};require(result.has_value());
            require(result->fingerprints==*topology->fingerprints() && core::sha256(result->canonical_json)==*topology->digest());
            loaded.emplace(std::move(*result));
        }
        Sink counter;project(counter,*categories,input.failures,loaded?&*loaded:nullptr,input.subject);
        usage.nodes=counter.count.nodes;usage.depth=counter.count.depth;usage.text_bytes=counter.count.text;
        usage.canonical_bytes=counter.count.bytes;usage.sort_entries=counter.count.sort_entries;
        bound(usage.nodes,limits.nodes);bound(usage.depth,limits.depth);bound(usage.text_bytes,limits.text_bytes);
        bound(usage.canonical_bytes,limits.canonical_bytes);bound(usage.sort_entries,limits.sort_entries);
        std::size_t provenance=plus(sizeof(detail::State),sizeof(Owner));
        provenance=plus(provenance,plus(input.run.size(),input.handoff_time.size()));
        provenance=plus(provenance,times(input.failures.size(),sizeof(ResolvedFailure)));
        const auto fixed=plus(input.session_fixed_bytes,detail::fixed_stock());
        const auto dom_bytes=dom_stock(counter.count);
        usage.retained_bytes=plus(provenance,usage.canonical_bytes);bound(usage.retained_bytes,limits.retained_bytes);
        // One exact-reserved scratch backing, released before rendering.
        const auto scratch_bytes=times(usage.sort_entries,sizeof(std::size_t));
        const auto construction=plus(input.resolved_table_bytes,plus(provenance,plus(dom_bytes,plus(scratch_bytes,fixed))));
        const auto rendering=plus(input.resolved_table_bytes,plus(provenance,plus(dom_bytes,plus(usage.canonical_bytes,fixed))));
        const auto publication=plus(input.resolved_table_bytes,plus(usage.retained_bytes,fixed));
        usage.consumer_simultaneous_bytes=std::max(construction,std::max(rendering,publication));
        bound(usage.consumer_simultaneous_bytes,limits.consumer_simultaneous_bytes);
        auto state=std::make_unique<detail::State>();auto& retained=state->provenance;
        retained.run=input.run;retained.handoff_time=input.handoff_time;retained.domain=input.domain;
        retained.origin=input.origin;retained.producer=input.producer;retained.sequence=input.sequence;retained.subject=input.subject;
        retained.failures.reserve(input.failures.size());retained.failures.assign(input.failures.begin(),input.failures.end());
        std::size_t index=0;
        const auto retain_slot=[&](const auto& source) {
            retained.slots[index]=static_cast<Disposition>(source.index());
            if(const auto* child=std::get_if<3>(&source)){require(child->digest());retained.child_digests[index]=*child->digest();}
            ++index;
        };
        retain_slot(categories->topology);retain_slot(categories->system_controls);retain_slot(categories->kernel_identity);
        retain_slot(categories->pu_identity);retain_slot(categories->frequency);retain_slot(categories->anonymous_thp);retain_slot(categories->process_thp);
        if(loaded)retained.topology=loaded->fingerprints;
        Json dom;core::detail::JsonCleanup cleanup{dom};
        {
            std::vector<std::size_t> scratch;bound(usage.sort_entries,scratch.max_size());scratch.reserve(usage.sort_entries);
            Sink builder{&dom,&scratch};project(builder,*categories,input.failures,loaded?&*loaded:nullptr,input.subject);
            require(builder.count.bytes==counter.count.bytes);
        }
        loaded.reset(); // No loaded graph coexists with canonical rendering.
        auto rendered=core::canonicalize_json_jcs(dom,{usage.canonical_bytes,usage.depth,usage.nodes});
        core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value(),Error::rendering);
        require(rendered->size()==usage.canonical_bytes,Error::rendering);
        state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);state->usage=usage;
        core::detail::retire_json(dom); // Publication retains only provenance and canonical bytes.
        return Owner{std::move(state)};
    }catch(const Rejected& rejected){return std::unexpected{rejected.error};}
    catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::host_projection
