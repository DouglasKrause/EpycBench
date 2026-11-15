#include "results/private_run_creation_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace epycbench::results::run_creation_serialization {
namespace {
using Json=nlohmann::json;
namespace t=topology_serialization;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array reporters{"controller","runner","results"};
constexpr std::string_view digits="0123456789abcdef";
void require(bool value,Error error=Error::schema){if(!value)throw error;}
auto plus(std::size_t left,std::size_t right)->std::size_t{require(detail::checked_add(left,right),Error::capacity);return left;}
auto times(std::size_t left,std::size_t right)->std::size_t{const auto result=detail::checked_multiply(left,right);require(result.has_value(),Error::capacity);return *result;}
void bound(std::size_t value,std::size_t limit){require(value<=limit,Error::capacity);}
template<class T,class Names> auto name(T value,const Names& names)->std::string_view{const auto i=static_cast<std::size_t>(value);require(i<names.size());return names[i];}
template<class T,class Names> auto enumerated(std::string_view value,const Names& names)->T{for(std::size_t i=0;i<names.size();++i)if(value==names[i])return static_cast<T>(i);throw Error::schema;}
struct Decimal {std::array<char,21> bytes{};std::size_t size{};explicit Decimal(std::uint64_t value){const auto converted=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);require(converted.ec==std::errc{},Error::source);size=static_cast<std::size_t>(converted.ptr-bytes.data());}auto view()const noexcept->std::string_view{return {bytes.data(),size};}};
auto integer(std::string_view value)->std::uint64_t{require(!value.empty()&&(value.size()==1||value.front()!='0'),Error::encoding);for(char ch:value)require(ch>='0'&&ch<='9',Error::encoding);std::uint64_t result{};const auto converted=std::from_chars(value.data(),value.data()+value.size(),result);require(converted.ec==std::errc{}&&converted.ptr==value.data()+value.size(),Error::encoding);return result;}
auto nibble(char ch)->unsigned{if(ch>='0'&&ch<='9')return static_cast<unsigned>(ch-'0');if(ch>='a'&&ch<='f')return static_cast<unsigned>(ch-'a')+10U;throw Error::encoding;}
void valid_hex(std::string_view value){require(value.size()%2U==0,Error::encoding);for(char ch:value)(void)nibble(ch);}
auto hex(std::string_view value)->std::string{std::string result(times(value.size(),2),'0');for(std::size_t i=0;i<value.size();++i){const auto byte=static_cast<unsigned char>(value[i]);result[2*i]=digits[byte>>4U];result[2*i+1]=digits[byte&15U];}return result;}
auto unhex(std::string_view value)->std::string{std::string result(value.size()/2,'\0');for(std::size_t i=0;i<result.size();++i)result[i]=static_cast<char>((nibble(value[2*i])<<4U)|nibble(value[2*i+1]));return result;}
struct Counts {std::size_t nodes{},depth{},text{},bytes{},objects{},arrays{},strings{},members{},elements{};};
struct Frame {Json* value{};std::size_t children{};bool object{};};
class Sink {
  public:
    Counts count;
    explicit Sink(Json* root=nullptr):root_(root){}
    void object(std::string_view key={}){container(key,true,0);}
    void array(std::string_view key,std::size_t size){container(key,false,size);}
    void end(){require(level_!=0);--level_;count.bytes=plus(count.bytes,1);}
    void text(std::string_view key,std::string_view value){prefix(key);++count.strings;count.text=plus(count.text,value.size());count.bytes=plus(count.bytes,plus(value.size(),2));if(root_){Json node=std::string{value};insert(key,node);}}
    void octets(std::string_view key,std::string_view value){if(root_){auto encoded=hex(value);text(key,encoded);}else{prefix(key);++count.strings;count.text=plus(count.text,times(value.size(),2));count.bytes=plus(count.bytes,plus(times(value.size(),2),2));}}
    void null(std::string_view key){prefix(key);count.bytes=plus(count.bytes,4);if(root_){Json node;insert(key,node);}}
    void number(std::string_view key,std::uint64_t value){const Decimal decimal{value};text(key,decimal.view());}
  private:
    Json* root_;std::array<Frame,5> frames_{};std::size_t level_{};
    void prefix(std::string_view key){count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);if(!level_)return;auto& f=frames_[level_-1];if(f.children++)count.bytes=plus(count.bytes,1);if(f.object){++count.members;count.text=plus(count.text,key.size());count.bytes=plus(count.bytes,plus(key.size(),3));}else ++count.elements;}
    auto insert(std::string_view key,Json& node)->Json*{if(!level_){root_->swap(node);return root_;}auto& f=frames_[level_-1];if(f.object){core::detail::append_object(*f.value,key,node);return &f.value->find(key).value();}f.value->push_back(std::move(node));return &f.value->back();}
    void container(std::string_view key,bool object,std::size_t size){prefix(key);require(level_<frames_.size());++count.bytes;if(object)++count.objects;else ++count.arrays;Json* placed=nullptr;if(root_){Json node=object?Json::object():Json::array();core::detail::JsonCleanup cleanup{node};if(!object)node.get_ref<Json::array_t&>().reserve(size);placed=insert(key,node);}frames_[level_++]={placed,0,object};}
};
struct ParseFrame {std::size_t children{};bool object{};};
class Reader {
  public:
    Counts count;
    Reader(std::string_view body,const Limits& limits):body_(body),limits_(limits){bound(body.size(),limits.canonical_bytes);count.bytes=body.size();}
    auto next(char ch)const noexcept->bool{const auto at=at_+(at_<body_.size()&&body_[at_]==','?1U:0U);return at<body_.size()&&body_[at]==ch;}
    void expect(std::string_view value){require(body_.substr(at_,value.size())==value,Error::encoding);at_+=value.size();}
    void key(std::string_view value){require(level_&&frames_[level_-1].object);if(frames_[level_-1].children++)expect(",");expect("\"");expect(value);expect("\":");++count.members;count.text=plus(count.text,value.size());bound(count.text,limits_.text_bytes);}
    void object(){node();expect("{");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,true};++count.objects;}
    void array(){node();expect("[");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,false};++count.arrays;}
    void end_object(){expect("}");require(level_!=0);--level_;}
    void end_array(){expect("]");require(level_!=0);--level_;}
    auto text()->std::string_view{node();expect("\"");const auto begin=at_;while(at_<body_.size()&&body_[at_]!='"'){const auto ch=static_cast<unsigned char>(body_[at_]);require(ch>=0x20&&ch<=0x7e&&ch!='\\',Error::encoding);++at_;}require(at_<body_.size(),Error::encoding);const auto result=body_.substr(begin,at_-begin);++at_;++count.strings;count.text=plus(count.text,result.size());bound(count.text,limits_.text_bytes);return result;}
    auto octets()->std::string_view{const auto value=text();valid_hex(value);return value;}
    auto number()->std::uint64_t{return integer(text());}
    auto boolean()->bool{node();if(next('t')){expect("true");return true;}expect("false");return false;}
    void null(){node();expect("null");}
    auto optional()->std::optional<std::uint64_t>{if(next('n')){null();return {};}return number();}
    void finished(){require(level_==0&&at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,5> frames_{};
    void node(){if(level_&&!frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};

constexpr std::array categories{"topology","system_controls","kernel_identity","pu_identity","frequency","anonymous_thp","process_thp","authored_source","base_build","preflight_temperature"};
constexpr std::array selectors{"owner_preparation","smt_control","smt_active","numa_mask","thp_enabled","thp_defrag","sysname","release","version","signature","microcode","uncovered_cpu","enumeration","page_size","process_observation","source_acquisition","base_build_acquisition","cpu_control_observation"};
constexpr std::array reasons{"none","read_failed","malformed_source","unsupported_value"};
struct Parsed {detail::View view{};Counts wire;std::array<std::string_view,3> children;std::size_t failures{},bindings{};};
auto parse(std::string_view body,const Limits& limits,detail::Staging* staging=nullptr)->Parsed{
    Parsed out;Reader reader{body,limits};reader.object();reader.key("artifacts");reader.object();
    reader.key("base_build");out.children[1]=reader.octets();reader.key("host");out.children[2]=reader.octets();reader.key("source");out.children[0]=reader.octets();reader.end_object();
    for(const auto value:out.children)require(!value.empty());
    reader.key("bindings");reader.array();while(!reader.next(']')){
        Binding binding{};reader.object();reader.key("failure_ordinal");binding.failure_ordinal=reader.number();
        require(binding.failure_ordinal<maximum_failures,Error::reference);reader.key("target");reader.object();
        reader.key("category");binding.target.category=enumerated<p::Category>(reader.text(),categories);
        reader.key("key");binding.target.key=reader.number();reader.key("ordinal");binding.target.ordinal=reader.number();
        reader.key("reason");binding.target.reason=enumerated<p::FailureReason>(reader.text(),reasons);
        reader.key("selector");binding.target.selector=enumerated<p::Selector>(reader.text(),selectors);reader.end_object();reader.end_object();
        out.bindings=plus(out.bindings,1);bound(out.bindings,maximum_bindings);bound(out.bindings,limits.bindings);
        if(staging)staging->children.bindings.push_back(binding);
    }reader.end_array();reader.key("domain");out.view.domain=enumerated<p::Domain>(reader.text(),domains);
    reader.key("entry_producer");out.view.reporter=enumerated<p::Producer>(reader.text(),reporters);
    reader.key("failures");reader.array();while(!reader.next(']')){const auto encoded=reader.octets();require(!encoded.empty());out.failures=plus(out.failures,1);bound(out.failures,maximum_failures);bound(out.failures,limits.failure_facts);if(staging)staging->wire_errors.push_back(encoded);}reader.end_array();
    reader.key("run_id");out.view.run=reader.text();reader.key("schema");require(reader.text()==profile);reader.key("sequence");out.view.sequence=reader.number();reader.key("timestamp");out.view.time=reader.text();reader.end_object();reader.finished();out.wire=reader.count;return out;
}
void emit(Sink& sink,const detail::View& view,const detail::Children& children){
    require(children.source&&children.base_build&&children.host,Error::child);sink.object();sink.object("artifacts");
    sink.octets("base_build",children.base_build->canonical());sink.octets("host",children.host->canonical());sink.octets("source",children.source->canonical());sink.end();
    sink.array("bindings",children.bindings.size());for(const auto& binding:children.bindings){sink.object();sink.number("failure_ordinal",binding.failure_ordinal);sink.object("target");sink.text("category",name(binding.target.category,categories));sink.number("key",binding.target.key);sink.number("ordinal",binding.target.ordinal);sink.text("reason",name(binding.target.reason,reasons));sink.text("selector",name(binding.target.selector,selectors));sink.end();sink.end();}sink.end();
    sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));sink.array("failures",children.failures.size());for(const auto& failure:children.failures)sink.octets({},failure.canonical());sink.end();
    sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.text("timestamp",view.time);sink.end();
}
void valid_limits(const Limits& limits){require(limits.depth<=5&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);for(const auto value:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes})require(value<=std::string{}.max_size(),Error::invalid_limits);}
auto base(std::size_t upper)->std::size_t{return plus(detail::fixed_stock(),upper);}
auto tables(std::size_t n,std::size_t b,bool decoded)->std::size_t{return plus(times(n,sizeof(e::Owner)+(decoded?sizeof(std::string_view):0)),times(b,sizeof(Binding)));}
auto primitive(const detail::View& view,std::size_t n,std::size_t b,const Limits& limits,std::size_t upper)->Usage{
    Usage out;out.artifact_slots=3;out.failure_facts=n;out.bindings=b;bound(n,maximum_failures);bound(b,maximum_bindings);bound(n,limits.failure_facts);bound(b,limits.bindings);bound(3,limits.artifact_slots);
    out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);bound(plus(base(upper),out.provenance_bytes),limits.publication_bytes);
    out.table_preparation_bytes=plus(plus(base(upper),tables(n,b,upper==0)),out.provenance_bytes);bound(out.table_preparation_bytes,limits.table_preparation_bytes);
    require(view.run.size()==36&&view.time.size()==30&&view.sequence==1&&view.reporter==p::Producer::controller,Error::identity);(void)name(view.domain,domains);return out;
}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);}
void reserve(detail::Staging& staging,std::size_t n,std::size_t b,bool decoded){require(n<=staging.children.failures.max_size()&&b<=staging.children.bindings.max_size()&&n<=staging.wire_errors.max_size(),Error::capacity);if(n){staging.children.failures.reserve(n);if(decoded)staging.wire_errors.reserve(n);}if(b)staging.children.bindings.reserve(b);}
template<class T>auto child_backing(const T& child)->std::size_t{const auto usage=child.usage();require(usage.retained_bytes>=sizeof(T),Error::child);return usage.retained_bytes-sizeof(T);}
auto backing(const detail::Children& children)->std::size_t{std::size_t out{};for(const auto& failure:children.failures)out=plus(out,child_backing(failure));if(children.source)out=plus(out,child_backing(*children.source));if(children.base_build)out=plus(out,child_backing(*children.base_build));if(children.host)out=plus(out,child_backing(*children.host));return out;}
auto phase_base(const detail::Staging& staging,const Usage& usage,std::size_t upper)->std::size_t{return plus(plus(base(upper),tables(usage.failure_facts,usage.bindings,upper==0)),backing(staging.children));}
auto error_stock(const detail::Staging& staging,const Limits& limits,const Usage& usage,std::size_t upper)->std::size_t{
    const auto floor=plus(upper?e::detail::fixed_stock():e::detail::decode_fixed_stock(),upper?e::detail::session_fixed_stock():0);
    const auto allowance=std::max({floor,limits.error.provenance_bytes,limits.error.retained_bytes,limits.error.consumer_simultaneous_bytes});
    const auto out=plus(plus(phase_base(staging,usage,upper),upper?0:limits.error.canonical_bytes),allowance);bound(out,limits.error_preparation_bytes);return out;
}
template<class T>void parent_match(const detail::View& view,const T& child){const auto* value=child.provenance();require(value&&child.digest()&&!child.canonical().empty(),Error::child);require(value->run==view.run&&value->time==view.time&&value->domain==view.domain&&value->sequence==1,Error::child);}
void root_match(const detail::View& view,const detail::Children& children,std::size_t index){
    const auto& child=children.failures[index];parent_match(view,child);const auto* value=child.provenance();
    require(value->kind==e::Kind::creation_failure&&value->entry_producer==p::Producer::controller&&value->creation_ordinal==index&&value->association,Error::child);
    const auto& association=*value->association;require(!association.process_sequence||*association.process_sequence==1,Error::reference);
    if(association.secondary_sequence){require(*association.secondary_sequence==1&&association.secondary_creation_ordinal&&*association.secondary_creation_ordinal<index,Error::reference);const auto* primary=children.failures[static_cast<std::size_t>(*association.secondary_creation_ordinal)].provenance();require(primary&&primary->association&&!primary->association->secondary_sequence&&primary->association->process_sequence==association.process_sequence,Error::reference);}
    else require(!association.secondary_creation_ordinal,Error::reference);
}
auto target_at(const detail::Children& children,detail::TargetPosition position)->const p::Target&{return children.failures[position.failure].provenance()->association->targets[position.target];}
struct TargetLess {const detail::Children& children;auto operator()(detail::TargetPosition left,detail::TargetPosition right)const noexcept->bool{const auto& a=target_at(children,left);const auto& b=target_at(children,right);return a<b||(a==b&&left.failure<right.failure);}};
struct BindingLess {const detail::Children& children;auto operator()(std::size_t left,std::size_t right)const noexcept->bool{return children.bindings[left].target<children.bindings[right].target;}};
template<class T,class Less>void sift(std::span<T> rows,std::size_t root,std::size_t size,Less less){while(root<size/2){auto child=2*root+1;if(child+1<size&&less(rows[child],rows[child+1]))++child;if(!less(rows[root],rows[child]))return;std::swap(rows[root],rows[child]);root=child;}}
template<class T,class Less>void sort(std::span<T> rows,Less less){for(auto root=rows.size()/2;root>0;--root)sift(rows,root-1,rows.size(),less);for(auto size=rows.size();size>1;--size){std::swap(rows[0],rows[size-1]);sift(rows,0,size-1,less);}}
void aggregate(e::Usage& total,const e::Usage& value){
#define SUM(M) total.M=plus(total.M,value.M)
    SUM(context_nodes);total.context_depth=std::max(total.context_depth,value.context_depth);SUM(string_bytes);SUM(binary_bytes);SUM(targets);SUM(nodes);total.depth=std::max(total.depth,value.depth);SUM(text_bytes);SUM(canonical_bytes);SUM(provenance_bytes);SUM(retained_bytes);SUM(consumer_simultaneous_bytes);
#undef SUM
}
void indices(const detail::View& view,detail::Staging& staging,const Limits& limits,std::size_t upper,Usage& usage){
    require(staging.children.failures.size()==usage.failure_facts&&staging.children.bindings.size()==usage.bindings,Error::source);usage.targets=0;usage.root_errors={};
    for(std::size_t i=0;i<staging.children.failures.size();++i){root_match(view,staging.children,i);usage.targets=plus(usage.targets,staging.children.failures[i].provenance()->association->targets.size());aggregate(usage.root_errors,staging.children.failures[i].usage());}
    bound(usage.targets,maximum_targets);bound(usage.targets,limits.targets);const auto bytes=plus(times(usage.targets,sizeof(detail::TargetPosition)),times(usage.bindings,sizeof(std::size_t)));
    usage.identity_index_bytes=plus(phase_base(staging,usage,upper),bytes);bound(usage.identity_index_bytes,limits.identity_index_bytes);
    require(usage.targets<=staging.targets.max_size()&&usage.bindings<=staging.binding_index.max_size(),Error::capacity);
    if(usage.targets)staging.targets.reserve(usage.targets);
    if(usage.bindings)staging.binding_index.reserve(usage.bindings);
    for(std::size_t i=0;i<staging.children.failures.size();++i){const auto* provenance=staging.children.failures[i].provenance();for(std::size_t j=0;j<provenance->association->targets.size();++j)staging.targets.push_back({i,j});}
    for(std::size_t i=0;i<usage.bindings;++i)staging.binding_index.push_back(i);
    sort(std::span{staging.targets},TargetLess{staging.children});sort(std::span{staging.binding_index},BindingLess{staging.children});
    for(std::size_t i=1;i<staging.targets.size();++i)require(TargetLess{staging.children}(staging.targets[i-1],staging.targets[i]),Error::reference);
    for(std::size_t i=0;i<staging.binding_index.size();++i){const auto& binding=staging.children.bindings[staging.binding_index[i]];require(binding.failure_ordinal<staging.children.failures.size(),Error::reference);if(i)require(staging.children.bindings[staging.binding_index[i-1]].target<binding.target,Error::reference);
        std::size_t low{},high=staging.targets.size();while(low<high){const auto mid=low+(high-low)/2;const auto position=staging.targets[mid];const auto& target=target_at(staging.children,position);if(target<binding.target||(target==binding.target&&position.failure<binding.failure_ordinal))low=mid+1;else high=mid;}
        require(low<staging.targets.size()&&target_at(staging.children,staging.targets[low])==binding.target&&staging.targets[low].failure==binding.failure_ordinal,Error::reference);
    }
}
auto child_allowance(const Limits& limits,std::size_t index,bool selected)->std::size_t{
    if(index==0){const auto& l=limits.source;return std::max({plus(source_serialization::detail::fixed_stock(),selected?source_serialization::detail::session_fixed_stock():0),l.provenance_bytes,l.retained_bytes,l.publication_bytes,l.source_preparation_bytes,l.error_preparation_bytes});}
    if(index==1){const auto& l=limits.base_build;return std::max({plus(base_build_serialization::detail::fixed_stock(),selected?base_build_serialization::detail::session_fixed_stock():0),l.provenance_bytes,l.retained_bytes,l.publication_bytes,l.manifest_decode_bytes,l.manifest_preparation_bytes,l.error_preparation_bytes});}
    require(index==2,Error::invalid_limits);const auto& l=limits.host;auto out=std::max({plus(host_serialization::detail::fixed_stock(),selected?host_serialization::detail::session_fixed_stock():0),l.provenance_bytes,l.retained_bytes,l.publication_bytes,l.identity_index_bytes});for(const auto value:l.child_preparation_bytes)out=std::max(out,value);return out;
}
auto child_stock(const detail::Staging& staging,const Limits& limits,const Usage& usage,std::size_t upper,std::size_t index,std::size_t decoded)->std::size_t{
    const auto index_bytes=plus(times(usage.targets,sizeof(detail::TargetPosition)),times(usage.bindings,sizeof(std::size_t)));
    const auto out=plus(plus(plus(phase_base(staging,usage,upper),index_bytes),decoded),child_allowance(limits,index,upper!=0));bound(out,limits.child_preparation_bytes[index]);return out;
}
struct Collector {
    const detail::Staging& staging;std::size_t count{};
    void add(const e::Owner* child,p::Target target){
        if(!child)return;
        count=plus(count,1);bound(count,maximum_consumed);const auto* provenance=child->provenance();require(provenance&&provenance->kind==e::Kind::creation_failure&&provenance->sequence==1&&provenance->creation_ordinal,Error::reference);
        const auto ordinal=*provenance->creation_ordinal;require(ordinal<staging.children.failures.size(),Error::reference);
        std::size_t low{},high=staging.binding_index.size();while(low<high){const auto mid=low+(high-low)/2;if(staging.children.bindings[staging.binding_index[mid]].target<target)low=mid+1;else high=mid;}
        require(low<staging.binding_index.size(),Error::reference);const auto& binding=staging.children.bindings[staging.binding_index[low]];
        require(binding.target==target&&binding.failure_ordinal==ordinal,Error::reference);
        const auto& root=staging.children.failures[static_cast<std::size_t>(ordinal)];require(child->digest()&&root.digest()&&child->canonical()==root.canonical()&&*child->digest()==*root.digest()&&*provenance==*root.provenance(),Error::child);
    }
};
template<class Reason>auto failure_reason(std::optional<Reason> reason)->p::FailureReason{require(reason.has_value(),Error::child);switch(*reason){case Reason::read_failed:return p::FailureReason::read_failed;case Reason::malformed_source:return p::FailureReason::malformed_source;case Reason::unsupported_value:return p::FailureReason::unsupported_value;default:throw Error::child;}}
template<class Source>auto ordinal(const Source* source)->std::uint64_t{require(source,Error::child);const auto value=source->ordinal();require(value.has_value(),Error::child);return *value;}
void consumed(const detail::View& view,const detail::Staging& staging,const Limits& limits,Usage& usage){
    const auto& c=staging.children;require(c.source&&c.base_build&&c.host,Error::child);parent_match(view,*c.source);parent_match(view,*c.base_build);parent_match(view,*c.host);
    require(c.source->provenance()->kind==source_serialization::Kind::creation_source&&c.base_build->provenance()->kind==base_build_serialization::Kind::genesis&&c.host->provenance()->kind==host_serialization::Kind::genesis&&c.host->provenance()->reporter==p::Producer::controller,Error::child);
    Collector collector{staging};collector.add(c.source->acquisition_error(),{p::Category::authored_source,p::Selector::source_acquisition});collector.add(c.base_build->acquisition_error(),{p::Category::base_build,p::Selector::base_build_acquisition});
    const auto& h=*c.host;
    collector.add(h.topology()->failure_error(),{p::Category::topology});collector.add(h.system_controls()->failure_error(),{p::Category::system_controls});collector.add(h.kernel_identity()->failure_error(),{p::Category::kernel_identity});collector.add(h.pu_identity()->failure_error(),{p::Category::pu_identity});collector.add(h.frequency()->failure_error(),{p::Category::frequency});collector.add(h.anonymous_thp()->failure_error(),{p::Category::anonymous_thp});collector.add(h.process_thp()->failure_error(),{p::Category::process_thp});
    if(const auto* source=h.system_controls()->controls()){
        const auto observations=source->observations();require(observations.size()==5,Error::child);
        constexpr std::array fields{p::Selector::smt_control,p::Selector::smt_active,p::Selector::numa_mask,p::Selector::thp_enabled,p::Selector::thp_defrag};
        for(std::size_t i=0;i<5;++i)if(const auto* child=h.system_controls()->observation_error(i))collector.add(child,{p::Category::system_controls,fields[i],ordinal(source),0,failure_reason(observations[i].reason)});
    }
    if(const auto* source=h.kernel_identity()->kernel_identity()){
        const auto observations=source->observations();require(observations.size()==3,Error::child);constexpr std::array fields{p::Selector::sysname,p::Selector::release,p::Selector::version};
        for(std::size_t i=0;i<3;++i)if(const auto* child=h.kernel_identity()->observation_error(i))collector.add(child,{p::Category::kernel_identity,fields[i],ordinal(source),0,failure_reason(observations[i].reason)});
    }
    if(const auto* source=h.pu_identity()->identity()){
        const auto rows=source->rows();const auto errors=h.pu_identity()->errors();require(rows.size()==errors.size(),Error::child);
        for(std::size_t i=0;i<rows.size();++i){require(rows[i].linux_cpu==errors[i].cpu,Error::child);for(std::size_t j=0;j<2;++j)if(errors[i].errors[j]){const auto& observation=j?rows[i].microcode:rows[i].signature;collector.add(&*errors[i].errors[j],{p::Category::pu_identity,j?p::Selector::microcode:p::Selector::signature,ordinal(source),rows[i].linux_cpu.value(),failure_reason(observation.reason)});}}
    }
    if(const auto* source=h.frequency()->source()){
        const auto rows=source->uncovered_cpus();const auto errors=h.frequency()->errors();require(rows.size()==errors.size(),Error::child);
        for(std::size_t i=0;i<rows.size();++i){require(rows[i].association.linux_cpu==errors[i].cpu,Error::child);if(errors[i].error)collector.add(&*errors[i].error,{p::Category::frequency,p::Selector::uncovered_cpu,ordinal(source),rows[i].association.linux_cpu.value(),failure_reason(rows[i].reason)});}
    }
    if(const auto* source=h.anonymous_thp()->source()){
        const auto* enumeration=h.anonymous_thp()->enumeration_error();if(enumeration&&enumeration->error){const auto* observed=source->enumeration();require(observed,Error::child);collector.add(&*enumeration->error,{p::Category::anonymous_thp,p::Selector::enumeration,ordinal(source),0,failure_reason(observed->reason)});}
        const auto rows=source->rows();const auto errors=h.anonymous_thp()->errors();require(rows.size()==errors.size(),Error::child);
        for(std::size_t i=0;i<rows.size();++i){require(rows[i].size==errors[i].size,Error::child);if(errors[i].error)collector.add(&*errors[i].error,{p::Category::anonymous_thp,p::Selector::page_size,ordinal(source),rows[i].size.value(),failure_reason(rows[i].reason)});}
    }
    if(const auto* source=h.process_thp()->source()){
        const auto* subject=h.process_thp()->subject();require(subject&&subject->role==process_thp_restriction::Role::controller&&subject->creation_sequence==1,Error::reference);
        const auto* observation=h.process_thp()->observation_error();if(observation&&observation->error){const auto* observed=source->observation();require(observed,Error::child);collector.add(&*observation->error,{p::Category::process_thp,p::Selector::process_observation,ordinal(source),subject->creation_sequence,failure_reason(observed->reason)});}
    }
    usage.consumed_references=collector.count;bound(collector.count,limits.consumed_references);
}
auto dom_stock(const Counts& count)->std::size_t{return plus(plus(plus(times(count.objects,sizeof(Json::object_t)),times(count.arrays,sizeof(Json::array_t))),plus(times(count.members,sizeof(Json::object_t::value_type)),times(count.elements,sizeof(Json)))),plus(times(count.strings,sizeof(Json::string_t)),count.text));}
auto publication(Usage out,const Counts& count,const detail::Staging& staging,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    const auto& c=staging.children;out.source=c.source->usage();out.base_build=c.base_build->usage();out.host=c.host->usage();
    out.retained_bytes=plus(plus(plus(plus(plus(sizeof(Owner),sizeof(detail::State)),tables(out.failure_facts,out.bindings,false)),backing(c)),out.provenance_bytes),count.bytes);
    std::size_t length=plus(plus(c.source->canonical().size(),c.base_build->canonical().size()),c.host->canonical().size());for(const auto& failure:c.failures)length=plus(length,failure.canonical().size());
    const auto index_bytes=plus(times(out.targets,sizeof(detail::TargetPosition)),times(out.bindings,sizeof(std::size_t)));
    out.publication_bytes=plus(plus(plus(plus(plus(phase_base(staging,out,upper),index_bytes),out.retained_bytes),dom_stock(count)),times(length,2)),plus(out.provenance_bytes,21));
    bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(detail::Children& children,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.sequence};state->children=std::move(children);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->children);auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup guard{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
template<class T,class L>void decode_child(std::optional<T>& target,std::string_view encoded,const L& limits){const auto bytes=unhex(encoded);auto child=T::decode(bytes,limits);using ChildError=typename decltype(child)::error_type;if(!child)throw child.error()==ChildError::capacity?Error::capacity:Error::child;target=std::move(*child);}
// Explicit logical fixed cells; independent test types mirror every executed new helper.
struct WireCells {
    Sink* emit_sink;const detail::View* emit_view;const detail::Children* emit_children;
    const Binding* emitted_binding;const e::Owner* emitted_failure;std::vector<Binding>::const_iterator bindings_begin,bindings_end;std::vector<e::Owner>::const_iterator failures_begin,failures_end;

    Sink* sink_this;Json* sink_root;std::string_view sink_key,sink_value;std::size_t sink_size;bool sink_object;
    Frame* sink_frame;Frame assigned_frame;Json* sink_placed;Json sink_node;core::detail::JsonCleanup<Json> sink_cleanup;
    std::string sink_encoded,sink_text;Decimal sink_decimal;Json* sink_insert_return;
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;std::string append_object_key;Json::iterator insert_find_result;
    Sink *array_this,*container_this,*object_this,*text_this,*octets_this,*number_this,*prefix_this,*insert_this,*end_this;
    std::string_view object_key,text_key,text_value,octets_key,octets_value,number_key,prefix_key,insert_key;Json* insert_node;
    std::uint64_t number_value;std::string_view array_key,container_key;std::size_t array_size,container_size;bool container_object;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;const Decimal* decimal_view_this;std::string_view decimal_view_return;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;std::size_t text_begin;unsigned char reader_character;
    std::string_view reader_expected,reader_key,reader_text_result,reader_text_return,reader_octets_value,reader_octets_return;ParseFrame assigned_parse_frame;
    const Reader* next_this;char next_character;std::size_t next_at;bool next_result;Reader *array_reader_this,*end_array_this,*reader_number_this,*reader_octets_this,*node_this,*object_reader_this,*text_reader_this,*expect_this,*key_this,*end_object_this,*finished_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,hex_return_view,unhex_value;std::string hex_result,unhex_result;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;unsigned nibble_return;
    std::string_view parsed_body;const Limits* parsed_limits;Parsed parsed_return;Reader parsed_reader;detail::Staging* parsed_staging;Binding parsed_binding;std::string_view parsed_child,parsed_encoded;const std::string_view *parsed_begin,*parsed_end;
    p::Domain name_domain,enumerated_domain;p::Producer name_producer,enumerated_producer;p::Category name_category,enumerated_category;p::Selector name_selector,enumerated_selector;p::FailureReason name_reason,enumerated_reason;
    const decltype(domains)* domain_names;const decltype(reporters)* producer_names;const decltype(categories)* category_names;const decltype(selectors)* selector_names;const decltype(reasons)* reason_names;
    std::size_t name_index,enumerated_index;std::string_view enumerated_value,name_return;
};
template<class T,class L,class U,class E>struct ChildCells {
    const T* backing_child;U backing_usage;std::size_t backing_return;
    const detail::View* parent_view;const T* parent_child;decltype(std::declval<const T&>().provenance()) parent_provenance;
    std::optional<T>* decode_target;std::string_view decode_encoded;const L* decode_limits;std::string decode_bytes;std::expected<T,E> decoded;L decode_limits_copy;
    const T *usage_this,*canonical_this,*digest_this,*provenance_this;U usage_return;std::string_view canonical_return;const core::Sha256Digest* digest_return;decltype(std::declval<const T&>().provenance()) provenance_return;
    T *move_target,*move_source;T conversion_temporary;
};
template<class T>struct HostChildCells {
    const host_serialization::Owner* child_this;const T* child_return;
    const T* failure_this;const e::Owner* failure_return;
};
template<class T,class Rows>struct SourceCells {
    const T *ordinal_source,*ordinal_this,*rows_this;std::optional<std::uint64_t> ordinal_value,ordinal_return;std::uint64_t returned_ordinal;
    Rows rows_return;
};
template<class R>struct ReasonCells {std::optional<R> reason; p::FailureReason returned;};
struct WalkCells {
    const detail::View* consumed_view;const detail::Staging* consumed_staging;const Limits* consumed_limits;Usage* consumed_usage;
    const detail::Children* children;const host_serialization::Owner* host;Collector collector;
    Collector* add_this;const e::Owner* add_child;p::Target add_target;const e::Provenance* add_provenance;std::uint64_t add_ordinal;std::size_t low,high,mid;const Binding* add_binding;const e::Owner* root;
    const e::Owner *canonical_child_this,*canonical_root_this,*digest_child_this,*digest_root_this,*provenance_root_this;
    std::string_view canonical_child,canonical_root;const core::Sha256Digest *digest_child,*digest_root;const e::Provenance* provenance_root;
    const source_serialization::Owner* acquisition_source_this;const base_build_serialization::Owner* acquisition_build_this;const e::Owner *source_error,*build_error;
    HostChildCells<topology_serialization::Owner> topology;HostChildCells<system_controls_serialization::Owner> controls;HostChildCells<kernel_identity_serialization::Owner> kernel;HostChildCells<pu_identity_serialization::Owner> pu;HostChildCells<frequency_policy_serialization::Owner> frequency;HostChildCells<anonymous_thp_policy_serialization::Owner> anonymous;HostChildCells<process_thp_restriction_serialization::Owner> process;
    const system_controls_serialization::Owner* controls_source_this;const host_controls::Owner *controls_source,*controls_source_return;
    SourceCells<host_controls::Owner,std::span<const host_controls::Observation>> controls_getters;
    std::span<const host_controls::Observation> control_observations;std::size_t controls_index;std::array<p::Selector,5> controls_fields;
    const system_controls_serialization::Owner* control_error_this;std::size_t control_error_index;const e::Owner *control_error,*control_error_return;
    const kernel_identity_serialization::Owner* kernel_source_this;const private_kernel_identity::Owner *kernel_source,*kernel_source_return;
    SourceCells<private_kernel_identity::Owner,std::span<const private_kernel_identity::Observation>> kernel_getters;
    std::span<const private_kernel_identity::Observation> kernel_observations;std::size_t kernel_index;std::array<p::Selector,3> kernel_fields;
    const kernel_identity_serialization::Owner* kernel_error_this;std::size_t kernel_error_index;const e::Owner *kernel_error,*kernel_error_return;
    const pu_identity_serialization::Owner *pu_source_this,*pu_errors_this;const pu_cpu_identity::Owner *pu_source,*pu_source_return;
    SourceCells<pu_cpu_identity::Owner,std::span<const pu_cpu_identity::OwnedRow>> pu_getters;
    std::span<const pu_cpu_identity::OwnedRow> pu_rows;std::span<const pu_identity_serialization::ErrorRow> pu_errors,pu_errors_return;
    std::size_t pu_index,pu_slot;const pu_cpu_identity::Observation* pu_observation;
    const frequency_policy_serialization::Owner *frequency_source_this,*frequency_errors_this;const frequency_policy_owner::Owner *frequency_source,*frequency_source_return;
    SourceCells<frequency_policy_owner::Owner,std::span<const frequency_policy_owner::Uncovered>> frequency_getters;
    std::span<const frequency_policy_owner::Uncovered> frequency_rows;std::span<const frequency_policy_serialization::ErrorRow> frequency_errors,frequency_errors_return;std::size_t frequency_index;
    const anonymous_thp_policy_serialization::Owner *anonymous_source_this,*anonymous_errors_this,*enumeration_error_this;
    const anonymous_thp_policy_owner::Owner *anonymous_source,*anonymous_source_return,*enumeration_this;
    SourceCells<anonymous_thp_policy_owner::Owner,std::span<const anonymous_thp_policy_owner::OwnedRow>> anonymous_getters;
    const anonymous_thp_policy_owner::Enumeration *enumeration_observed,*enumeration_return;
    const anonymous_thp_policy_serialization::ObservationError *enumeration_error,*enumeration_error_return;
    std::span<const anonymous_thp_policy_owner::OwnedRow> anonymous_rows;std::span<const anonymous_thp_policy_serialization::ErrorRow> anonymous_errors,anonymous_errors_return;std::size_t anonymous_index;
    const process_thp_restriction_serialization::Owner *process_source_this,*subject_this,*observation_error_this;
    const process_thp_restriction::Owner *process_source,*process_source_return,*observation_this;
    SourceCells<process_thp_restriction::Owner,const process_thp_restriction::Observation*> process_getters;
    const process_thp_restriction::Observation *observed,*observed_return;
    const process_thp_restriction_serialization::SubjectAssociation *subject,*subject_return;
    const process_thp_restriction_serialization::ObservationError *observation_error,*observation_error_return;
    ReasonCells<host_controls::Reason> control_reason;ReasonCells<private_kernel_identity::Reason> kernel_reason;ReasonCells<pu_cpu_identity::Reason> pu_reason;ReasonCells<frequency_policy_owner::Reason> frequency_reason;ReasonCells<anonymous_thp_policy_owner::Reason> anonymous_reason;ReasonCells<process_thp_restriction::Reason> process_reason;
    const core::LinuxCpuId* cpu_this;decltype(std::declval<core::LinuxCpuId>().value()) cpu_return;const core::ByteCount* size_this;std::uint64_t size_return;
};
template<class T,class Less>struct SortCells {
    std::span<T> sort_rows,sift_rows;Less sort_less,sift_less;std::size_t root,size,sift_root,sift_size,sift_child;
    T swap_temporary;T *swap_a,*swap_b;const Less* compare_this;T compare_left,compare_right;bool compared;
};
struct IndexCells {
    const detail::View *indices_view,*root_view;detail::Staging* staging;const Limits* limits;std::size_t upper;Usage* usage;
    const detail::Children* root_children;std::size_t root_index;const e::Owner* root_child;const e::Provenance *root_value,*primary;
    const e::Association* root_association;const e::Owner *root_provenance_this,*primary_provenance_this;const e::Provenance *root_provenance_return,*primary_provenance_return;
    std::size_t root_loop,target_loop,bytes,binding_loop,low,high,mid;const e::Provenance* loop_provenance;const Binding* binding;detail::TargetPosition position,inserted;const p::Target* target;
    const detail::Children* target_children;detail::TargetPosition target_position;const p::Target* target_return;const e::Owner* target_provenance_this;const e::Provenance* target_provenance_return;
    SortCells<detail::TargetPosition,TargetLess> target_sort;SortCells<std::size_t,BindingLess> binding_sort;TargetLess target_less;BindingLess binding_less;
    const p::Target *compare_a,*compare_b;std::strong_ordering target_order;bool target_equal;
    e::Usage *aggregate_total;const e::Usage* aggregate_value;e::Usage aggregated_return;
};
struct OperationCells {
    WireCells wire;WalkCells walk;IndexCells index;
    ChildCells<source_serialization::Owner,source_serialization::Limits,source_serialization::Usage,source_serialization::Error> source;
    ChildCells<base_build_serialization::Owner,base_build_serialization::Limits,base_build_serialization::Usage,base_build_serialization::Error> build;
    ChildCells<host_serialization::Owner,host_serialization::Limits,host_serialization::Usage,host_serialization::Error> host;
    ChildCells<e::Owner,e::Limits,e::Usage,e::Error> error;
    std::string_view decode_body;Limits decode_limits,prepare_limits;Parsed parsed,filled;Usage usage,primitive_return,publication_input,publication_return;
    detail::Staging staging;std::unique_ptr<detail::State> state,publish_return;Sink counter;std::string_view encoded_error;std::string error_bytes;std::expected<e::Owner,e::Error> decoded_error;std::size_t decode_index;
    std::vector<std::string_view>::iterator wire_errors_begin,wire_errors_end;
    detail::ResolvedInput *preflight_input,*error_phase_input,*index_phase_input,*child_phase_input,*prepare_input;
    const Limits *preflight_limits,*error_phase_limits,*index_phase_limits,*child_phase_limits;std::size_t selected_index;
    std::expected<void,Error> void_result;std::expected<Owner,Error> result;std::unexpected<Error> unexpected;Error caught;const std::length_error* caught_length;
    const Limits* valid_limits_parameter;std::initializer_list<std::size_t> limit_values;std::array<std::size_t,3> limit_array;std::size_t limit_value;Json::array_t array_maximum;std::string string_maximum;
    const detail::View* primitive_view;const Limits* primitive_limits;std::size_t primitive_n,primitive_b,primitive_upper;Usage primitive_usage;
    const detail::View* identity_view;std::expected<journal::RunId,core::Error> parsed_run;std::expected<journal::Timestamp,core::Error> parsed_time;core::detail::ExpectedErrorCleanup<decltype(parsed_run)> run_cleanup;core::detail::ExpectedErrorCleanup<decltype(parsed_time)> time_cleanup;
    detail::Staging* reserve_staging;std::size_t reserve_n,reserve_b;bool reserve_decoded;
    std::size_t tables_n,tables_b,tables_return;bool tables_decoded;
    const detail::Children* backing_children;std::size_t backing_out;const e::Owner* backing_failure;std::vector<e::Owner>::const_iterator backing_begin,backing_end;
    const detail::Staging* phase_staging;const Usage* phase_usage;std::size_t phase_upper,phase_return;
    const detail::Staging* error_staging;const Limits* error_limits;const Usage* error_usage;std::size_t error_upper,error_floor,error_allowance,error_out;std::array<std::size_t,4> error_values;std::initializer_list<std::size_t> error_value_list;
    const Limits* allowance_limits;std::size_t allowance_index;bool allowance_selected;const source_serialization::Limits* source_limits;const base_build_serialization::Limits* build_limits;const host_serialization::Limits* host_limits;std::size_t allowance_out,allowance_value,allowance_return;
    std::array<std::size_t,6> source_allowances;std::array<std::size_t,7> build_allowances;std::array<std::size_t,5> host_allowances;std::initializer_list<std::size_t> source_allowance_list,build_allowance_list,host_allowance_list;const std::size_t *host_allowance_begin,*host_allowance_end;
    const detail::Staging* child_staging;const Limits* child_limits;const Usage* child_usage;std::size_t child_upper,child_index,child_decoded,child_index_bytes,child_out;
    const Counts* dom_count;std::size_t dom_return;
    const Counts* publication_count;const detail::Staging* publication_staging;std::size_t publication_upper;const Limits* publication_limits;const detail::Children* publication_children;std::size_t publication_length,publication_index_bytes;const e::Owner* length_failure;std::vector<e::Owner>::const_iterator length_begin,length_end;
    detail::Children* publish_children;const detail::View* publish_view;const Counts* publish_count;Usage publish_usage;Origin publish_origin;std::unique_ptr<detail::State> publish_state;Provenance published_provenance;std::string published_run,published_time;
    Json dom;core::detail::JsonCleanup<Json> dom_cleanup;Sink emit_sink;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> render_guard;core::JcsLimits render_limits;core::Sha256Digest published_digest;std::string_view digest_input;
    bool require_value;Error require_error;std::size_t plus_left,plus_right,plus_return,times_left,times_right,times_return;std::optional<std::size_t> times_result;
    std::size_t bound_value,bound_limit,base_upper,base_return;std::size_t* add_value;std::size_t add_amount,multiply_a,multiply_b;std::optional<std::size_t> multiply_return;std::size_t fixed_return;
    detail::State* state_constructor_this;Owner* owner_constructor_this;std::unique_ptr<detail::State> owner_constructor_argument;Owner owner_conversion_temporary;Owner *owner_move_this,*owner_move_source;
    const Owner* usage_this;Usage usage_getter_return;const Owner* canonical_this;std::string_view canonical_getter_return;const Owner* provenance_this;const Provenance* provenance_getter_return;const Owner* digest_this;const core::Sha256Digest* digest_getter_return;const Owner* origin_this;std::optional<Origin> origin_getter_return;
    const Owner *source_this,*build_this,*host_this,*failures_this,*failure_at_this,*bindings_this;const source_serialization::Owner* source_return;const base_build_serialization::Owner* build_return;const host_serialization::Owner* host_return;std::span<const e::Owner> failures_return;std::uint64_t failure_at_ordinal;const e::Owner* failure_at_return;std::span<const Binding> bindings_return;
};
}
namespace detail {
auto checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto checked_multiply(std::size_t a,std::size_t b)noexcept->std::optional<std::size_t>{if(b&&a>SIZE_MAX/b)return {};return a*b;}
auto fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
}
Owner::~Owner()=default;Owner::Owner(Owner&&)noexcept=default;auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
#define CREATION_GETTER(M,NS) auto Owner::M()const noexcept->const NS::Owner*{return state_&&state_->children.M?&*state_->children.M:nullptr;}
CREATION_GETTER(source,source_serialization)
CREATION_GETTER(base_build,base_build_serialization)
CREATION_GETTER(host,host_serialization)
#undef CREATION_GETTER
auto Owner::failures()const noexcept->std::span<const e::Owner>{return state_?std::span<const e::Owner>{state_->children.failures}:std::span<const e::Owner>{};}
auto Owner::failure_at(std::uint64_t ordinal_value)const noexcept->const e::Owner*{return state_&&ordinal_value<state_->children.failures.size()?&state_->children.failures[static_cast<std::size_t>(ordinal_value)]:nullptr;}
auto Owner::bindings()const noexcept->std::span<const Binding>{return state_?std::span<const Binding>{state_->children.bindings}:std::span<const Binding>{};}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{try{valid_limits(limits);require(input.staging,Error::source);input.usage=primitive(input.view,input.failures,input.bindings,limits,input.upper_fixed);identity(input.view);reserve(*input.staging,input.failures,input.bindings,false);return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::error_phase(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{try{input.usage.error_preparation_bytes=std::max(input.usage.error_preparation_bytes,error_stock(*input.staging,limits,input.usage,input.upper_fixed));return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::index_phase(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{try{indices(input.view,*input.staging,limits,input.upper_fixed,input.usage);return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::child_phase(detail::ResolvedInput& input,const Limits& limits,std::size_t index)->std::expected<void,Error>{try{require(index<3,Error::invalid_limits);input.usage.child_preparation_bytes[index]=child_stock(*input.staging,limits,input.usage,input.upper_fixed,index,0);return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::prepare(detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{try{consumed(input.view,*input.staging,limits,input.usage);Sink counter;emit(counter,input.view,input.staging->children);input.usage=publication(input.usage,counter.count,*input.staging,input.upper_fixed,limits);return Owner{publish(input.staging->children,input.view,counter.count,input.usage,Origin::session_selected)};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{try{
    valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,parsed.failures,parsed.bindings,limits,0);identity(parsed.view);detail::Staging staging;reserve(staging,parsed.failures,parsed.bindings,true);const auto filled=parse(body,limits,&staging);require(filled.failures==parsed.failures&&filled.bindings==parsed.bindings,Error::source);
    for(const auto encoded:staging.wire_errors){usage.error_preparation_bytes=std::max(usage.error_preparation_bytes,error_stock(staging,limits,usage,0));bound(encoded.size()/2,limits.error.canonical_bytes);const auto bytes=unhex(encoded);auto child=e::Owner::decode(bytes,limits.error);if(!child)throw child.error()==e::Error::capacity?Error::capacity:Error::child;staging.children.failures.push_back(std::move(*child));}
    indices(parsed.view,staging,limits,0,usage);
#define CREATION_DECODE(N,M) usage.child_preparation_bytes[N]=child_stock(staging,limits,usage,0,N,parsed.children[N].size()/2);bound(parsed.children[N].size()/2,limits.M.canonical_bytes);decode_child(staging.children.M,parsed.children[N],limits.M)
    CREATION_DECODE(0,source);CREATION_DECODE(1,base_build);CREATION_DECODE(2,host);
#undef CREATION_DECODE
    consumed(parsed.view,staging,limits,usage);usage=publication(usage,parsed.wire,staging,0,limits);auto state=publish(staging.children,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
} // namespace epycbench::results::run_creation_serialization
