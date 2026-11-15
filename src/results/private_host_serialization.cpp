#include "results/private_host_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace epycbench::results::host_serialization {
namespace {
using Json=nlohmann::json;
namespace t=topology_serialization;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array kinds{"ordinary","genesis"};
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
    Json* root_;std::array<Frame,3> frames_{};std::size_t level_{};
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
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,3> frames_{};
    void node(){if(level_&&!frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};

constexpr std::array category_names{"topology","system_controls","kernel_identity","pu_identity","frequency","anonymous_thp","process_thp"};
constexpr std::array<std::size_t,7> canonical_order{5,4,2,6,3,1,0};
struct Parsed {detail::View view{};Counts wire;std::array<std::string_view,7> children;};
auto parse(std::string_view body,const Limits& limits)->Parsed{
    Parsed out;Reader reader{body,limits};reader.object();reader.key("categories");reader.object();
    for(const auto index:canonical_order){reader.key(category_names[index]);out.children[index]=reader.octets();require(!out.children[index].empty());}
    reader.end_object();reader.key("domain");out.view.domain=enumerated<p::Domain>(reader.text(),domains);
    reader.key("entry_producer");out.view.reporter=enumerated<p::Producer>(reader.text(),reporters);
    reader.key("host_kind");out.view.kind=enumerated<Kind>(reader.text(),kinds);
    reader.key("run_id");out.view.run=reader.text();reader.key("schema");require(reader.text()==profile);
    reader.key("sequence");out.view.sequence=reader.number();reader.key("timestamp");out.view.time=reader.text();
    reader.end_object();reader.finished();out.wire=reader.count;return out;
}
auto views(const detail::Children& children)->std::array<std::string_view,7>{
    require(children.topology&&children.system_controls&&children.kernel_identity&&children.pu_identity&&children.frequency&&children.anonymous_thp&&children.process_thp,Error::child);
    return {children.topology->canonical(),children.system_controls->canonical(),children.kernel_identity->canonical(),children.pu_identity->canonical(),children.frequency->canonical(),children.anonymous_thp->canonical(),children.process_thp->canonical()};
}
void emit(Sink& sink,const detail::View& view,const detail::Children& children){
    const auto bytes=views(children);sink.object();sink.object("categories");
    for(const auto index:canonical_order)sink.octets(category_names[index],bytes[index]);
    sink.end();sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));
    sink.text("host_kind",name(view.kind,kinds));sink.text("run_id",view.run);sink.text("schema",profile);
    sink.number("sequence",view.sequence);sink.text("timestamp",view.time);sink.end();
}
void valid_limits(const Limits& limits){
    require(limits.depth<=3&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
    for(const auto value:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes})require(value<=std::string{}.max_size(),Error::invalid_limits);
}
auto base(std::size_t upper)->std::size_t{return plus(detail::fixed_stock(),upper);}
auto primitive(const detail::View& view,const Limits& limits,std::size_t upper)->Usage{
    Usage out;out.categories=7;bound(out.categories,limits.categories);out.provenance_bytes=plus(view.run.size(),view.time.size());
    bound(out.provenance_bytes,limits.provenance_bytes);bound(plus(base(upper),out.provenance_bytes),limits.publication_bytes);
    require(view.run.size()==36&&view.time.size()==30&&view.sequence,Error::identity);
    (void)name(view.domain,domains);(void)name(view.reporter,reporters);(void)name(view.kind,kinds);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    if(view.sequence==1)require(view.reporter==p::Producer::controller,Error::identity);
    return out;
}
void identity(const detail::View& view){
    auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);
    auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);
}
template<class T>auto child_backing(const std::optional<T>& child)->std::size_t{
    if(!child)return 0;
    const auto usage=child->usage();require(usage.retained_bytes>=sizeof(T),Error::child);return usage.retained_bytes-sizeof(T);
}
auto backing(const detail::Children& c)->std::size_t{
    auto out=child_backing(c.topology);out=plus(out,child_backing(c.system_controls));out=plus(out,child_backing(c.kernel_identity));
    out=plus(out,child_backing(c.pu_identity));out=plus(out,child_backing(c.frequency));out=plus(out,child_backing(c.anonymous_thp));return plus(out,child_backing(c.process_thp));
}
template<class T>auto allowance(const T& limits,std::size_t floor)->std::size_t{
    auto out=std::max({floor,limits.provenance_bytes,limits.retained_bytes,limits.publication_bytes,limits.topology_preparation_bytes,limits.error_preparation_bytes});
    if constexpr(requires{limits.topology_decode_bytes;})out=std::max(out,limits.topology_decode_bytes);
    if constexpr(requires{limits.controls_input_bytes;})out=std::max({out,limits.controls_input_bytes,limits.controls_preparation_bytes});
    if constexpr(requires{limits.identity_input_bytes;})out=std::max({out,limits.identity_input_bytes,limits.identity_preparation_bytes});
    if constexpr(requires{limits.source_input_bytes;})out=std::max({out,limits.source_input_bytes,limits.source_preparation_bytes});
    if constexpr(requires{limits.binding_bytes;})out=std::max({out,limits.binding_bytes,limits.row_staging_bytes});
    if constexpr(requires{limits.row_staging_bytes;})out=std::max(out,limits.row_staging_bytes);
    if constexpr(requires{limits.identity_index_bytes;})out=std::max(out,limits.identity_index_bytes);
    return out;
}
struct ChildBound {std::size_t canonical,allowance;};
auto child_bound(const Limits& limits,std::size_t index,bool selected)->ChildBound{
#define HOST_BOUND(N,M,NS) case N:return {limits.M.canonical_bytes,allowance(limits.M,plus(NS::detail::fixed_stock(),selected?NS::detail::session_fixed_stock():0))}
    switch(index){
        HOST_BOUND(0,topology,topology_serialization);
        HOST_BOUND(1,system_controls,system_controls_serialization);
        HOST_BOUND(2,kernel_identity,kernel_identity_serialization);
        HOST_BOUND(3,pu_identity,pu_identity_serialization);
        HOST_BOUND(4,frequency,frequency_policy_serialization);
        HOST_BOUND(5,anonymous_thp,anonymous_thp_policy_serialization);
        HOST_BOUND(6,process_thp,process_thp_restriction_serialization);
    }
#undef HOST_BOUND
    throw Error::invalid_limits;
}
auto child_phase_stock(const detail::Children& children,const Limits& limits,std::size_t index,std::size_t decoded,std::size_t upper)->std::size_t{
    require(index<7,Error::invalid_limits);const auto child=child_bound(limits,index,upper!=0);bound(decoded,child.canonical);
    const auto out=plus(plus(plus(base(upper),backing(children)),decoded),child.allowance);bound(out,limits.child_preparation_bytes[index]);return out;
}
template<class T>void parent_match(const detail::View& view,const std::optional<T>& child){
    require(child&&child->digest()&&child->provenance()&&child->slot_state(),Error::child);const auto* value=child->provenance();
    require(value->run==view.run&&value->time==view.time&&value->domain==view.domain&&value->reporter==view.reporter&&value->kind==view.kind&&value->sequence==view.sequence,Error::child);
}
void topology_match(const t::Owner& root,const t::Owner* nested){
    if(!nested)return;
    const auto* original=root.topology();const auto* candidate=nested->topology();
    require(original&&candidate&&root.slot_state()==t::SlotState::retained&&nested->slot_state()==t::SlotState::retained&&!root.failure_error()&&!nested->failure_error(),Error::child);
    require(root.canonical()==nested->canonical()&&*root.digest()==*nested->digest()&&*root.provenance()==*nested->provenance(),Error::child);
    require(original->canonical()==candidate->canonical()&&*original->digest()==*candidate->digest()&&*original->fingerprints()==*candidate->fingerprints(),Error::child);
}
void parents_match(const detail::View& view,const detail::Children& c){
    parent_match(view,c.topology);parent_match(view,c.system_controls);parent_match(view,c.kernel_identity);parent_match(view,c.pu_identity);
    parent_match(view,c.frequency);parent_match(view,c.anonymous_thp);parent_match(view,c.process_thp);
    topology_match(*c.topology,c.system_controls->topology());topology_match(*c.topology,c.kernel_identity->topology());
    topology_match(*c.topology,c.pu_identity->topology());topology_match(*c.topology,c.frequency->topology());
    topology_match(*c.topology,c.anonymous_thp->topology());topology_match(*c.topology,c.process_thp->topology());
}
struct ErrorCollector {
    const detail::View& view;std::vector<detail::ErrorIndexRow>* rows{};std::size_t count{};
    void add(const e::Owner* child){
        if(!child)return;
        const auto* value=child->provenance();require(value&&child->error()&&child->digest(),Error::child);
        require(value->run==view.run&&value->domain==view.domain&&value->kind!=e::Kind::source_resolution,Error::child);
        count=plus(count,1);bound(count,maximum_error_references);
        if(rows)rows->push_back({value->kind,value->sequence,value->creation_ordinal,child});
    }
    void nested(const t::Owner* child){if(child){require(!child->failure_error(),Error::child);add(child->failure_error());}}
};
void collect_errors(ErrorCollector& collector,const detail::Children& c){
    collector.add(c.topology->failure_error());collector.add(c.system_controls->failure_error());collector.nested(c.system_controls->topology());
    for(std::size_t i=0;i<5;++i)collector.add(c.system_controls->observation_error(i));
    collector.add(c.kernel_identity->failure_error());collector.nested(c.kernel_identity->topology());
    for(std::size_t i=0;i<3;++i)collector.add(c.kernel_identity->observation_error(i));
    collector.add(c.pu_identity->failure_error());collector.nested(c.pu_identity->topology());
    const auto pu_rows=c.pu_identity->errors();for(const auto& row:pu_rows)for(const auto& child:row.errors)collector.add(child?&*child:nullptr);
    collector.add(c.frequency->failure_error());collector.nested(c.frequency->topology());
    const auto frequency_rows=c.frequency->errors();for(const auto& row:frequency_rows)collector.add(row.error?&*row.error:nullptr);
    collector.add(c.anonymous_thp->failure_error());collector.nested(c.anonymous_thp->topology());
    if(const auto* enumeration=c.anonymous_thp->enumeration_error())collector.add(enumeration->error?&*enumeration->error:nullptr);
    const auto anonymous_rows=c.anonymous_thp->errors();for(const auto& row:anonymous_rows)collector.add(row.error?&*row.error:nullptr);
    collector.add(c.process_thp->failure_error());collector.nested(c.process_thp->topology());
    if(const auto* observation=c.process_thp->observation_error())collector.add(observation->error?&*observation->error:nullptr);
}
auto less(const detail::ErrorIndexRow& a,const detail::ErrorIndexRow& b)noexcept->bool{
    if(a.kind!=b.kind)return a.kind<b.kind;
    if(a.sequence!=b.sequence)return a.sequence<b.sequence;
    return a.ordinal<b.ordinal;
}
void sift(std::span<detail::ErrorIndexRow> rows,std::size_t root,std::size_t size)noexcept{
    while(root<size/2){auto child=2*root+1;if(child+1<size&&less(rows[child],rows[child+1]))++child;
        if(!less(rows[root],rows[child]))return;
        std::swap(rows[root],rows[child]);root=child;}
}
void errors_match(const detail::View& view,const detail::Children& children,const Limits& limits,std::size_t upper,Usage& usage){
    ErrorCollector counter{view};collect_errors(counter,children);usage.error_references=counter.count;bound(counter.count,limits.error_references);
    const auto bytes=times(counter.count,sizeof(detail::ErrorIndexRow));usage.identity_index_bytes=plus(plus(base(upper),backing(children)),bytes);bound(usage.identity_index_bytes,limits.identity_index_bytes);
    std::vector<detail::ErrorIndexRow> rows;require(counter.count<=rows.max_size(),Error::capacity);if(counter.count)rows.reserve(counter.count);
    ErrorCollector collector{view,&rows};collect_errors(collector,children);require(collector.count==counter.count,Error::source);detail::sort_error_index(rows);
    for(std::size_t i=1;i<rows.size();++i){const auto& previous=rows[i-1];const auto& current=rows[i];if(less(previous,current))continue;
        require(previous.error->canonical()==current.error->canonical()&&*previous.error->digest()==*current.error->digest()&&*previous.error->provenance()==*current.error->provenance(),Error::child);}
}
auto dom_stock(const Counts& count)->std::size_t{return plus(plus(plus(times(count.objects,sizeof(Json::object_t)),times(count.arrays,sizeof(Json::array_t))),plus(times(count.members,sizeof(Json::object_t::value_type)),times(count.elements,sizeof(Json)))),plus(times(count.strings,sizeof(Json::string_t)),count.text));}
auto publication(Usage out,const Counts& count,const detail::Children& children,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;
    bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.topology=children.topology->usage();out.system_controls=children.system_controls->usage();out.kernel_identity=children.kernel_identity->usage();out.pu_identity=children.pu_identity->usage();out.frequency=children.frequency->usage();out.anonymous_thp=children.anonymous_thp->usage();out.process_thp=children.process_thp->usage();
    out.retained_bytes=plus(plus(plus(plus(sizeof(Owner),sizeof(detail::State)),out.provenance_bytes),count.bytes),backing(children));
    const auto child_bytes=views(children);std::size_t length{};for(const auto value:child_bytes)length=plus(length,value.size());
    out.publication_bytes=plus(plus(plus(plus(base(upper),out.retained_bytes),dom_stock(count)),times(length,2)),plus(out.provenance_bytes,21));
    bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(detail::Children& children,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;
    state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.kind,view.sequence};
    state->children=std::move(children);Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->children);
    auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup guard{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);
    state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
template<class T,class L>void decode_child(std::optional<T>& target,std::string_view encoded,const L& limits){
    const auto bytes=unhex(encoded);auto child=T::decode(bytes,limits);using ChildError=typename decltype(child)::error_type;if(!child)throw child.error()==ChildError::capacity?Error::capacity:Error::child;
    target=std::move(*child);
}
// Logical fixed-cell inventory. Independent tests mirror actual types and cells.
struct WireCells {
    Sink* emit_sink;const detail::View* emit_view;const detail::Children* emit_children;
    std::array<std::string_view,7> emitted_views,returned_views;std::size_t emitted_index;
    const detail::Children* views_children;
    Sink* sink_this;Json* sink_root;std::string_view sink_key,sink_value;std::size_t sink_size;bool sink_object;
    Frame* sink_frame;Frame assigned_frame;Json* sink_placed;Json sink_node;core::detail::JsonCleanup<Json> sink_cleanup;
    std::string sink_encoded,sink_text;Decimal sink_decimal;Json* sink_insert_return;
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;std::string append_object_key;Json::iterator insert_find_result;
    Sink *object_this,*text_this,*octets_this,*number_this,*prefix_this,*insert_this,*end_this;
    std::string_view object_key,text_key,text_value,octets_key,octets_value,number_key,prefix_key,insert_key;Json* insert_node;
    std::uint64_t number_value;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;const Decimal* decimal_view_this;std::string_view decimal_view_return;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;std::size_t text_begin;unsigned char reader_character;
    std::string_view reader_expected,reader_key,reader_text_result,reader_text_return,reader_octets_value,reader_octets_return;ParseFrame assigned_parse_frame;
    Reader *reader_number_this,*reader_octets_this,*node_this,*object_reader_this,*text_reader_this,*expect_this,*key_this,*end_object_this,*finished_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,hex_return_view,unhex_value;std::string hex_result,unhex_result;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;unsigned nibble_return;
    std::string_view parsed_body;const Limits* parsed_limits;Parsed parsed_return;Reader parsed_reader;std::size_t parsed_index;
    p::Domain name_domain,enumerated_domain;p::Producer name_producer,enumerated_producer;Kind name_kind,enumerated_kind;
    const decltype(domains)* domain_names;const decltype(reporters)* producer_names;const decltype(kinds)* kind_names;
    std::size_t name_index,enumerated_index;std::string_view enumerated_value,name_return;
};
struct IndexCells {
    const detail::View* match_view;const detail::Children* match_children;const Limits* match_limits;std::size_t match_upper;Usage* match_usage;
    ErrorCollector counter,collector;std::vector<detail::ErrorIndexRow> rows;std::size_t bytes,index;
    const detail::ErrorIndexRow *previous,*current;
    ErrorCollector* collect_collector;const detail::Children* collect_children;std::size_t controls_index,kernel_index;
    std::span<const pu_identity_serialization::ErrorRow> pu_rows;const pu_identity_serialization::ErrorRow* pu_row;
    std::span<const frequency_policy_serialization::ErrorRow> frequency_rows;const frequency_policy_serialization::ErrorRow* frequency_row;
    std::span<const anonymous_thp_policy_serialization::ErrorRow> anonymous_rows;const anonymous_thp_policy_serialization::ErrorRow* anonymous_row;
    const std::optional<e::Owner>* pu_error;
    const anonymous_thp_policy_serialization::ObservationError* enumeration;const process_thp_restriction_serialization::ObservationError* observation;
    ErrorCollector* add_this;const e::Owner* add_child;const e::Provenance* add_provenance;detail::ErrorIndexRow added_row;
    ErrorCollector* nested_this;const t::Owner* nested_child;
    const system_controls_serialization::Owner* controls_observation_this;std::size_t controls_observation_index;const e::Owner* controls_observation_return;
    const kernel_identity_serialization::Owner* kernel_observation_this;std::size_t kernel_observation_index;const e::Owner* kernel_observation_return;
    const pu_identity_serialization::Owner* pu_errors_this;std::span<const pu_identity_serialization::ErrorRow> pu_errors_return;
    const frequency_policy_serialization::Owner* frequency_errors_this;std::span<const frequency_policy_serialization::ErrorRow> frequency_errors_return;
    const anonymous_thp_policy_serialization::Owner* anonymous_errors_this;std::span<const anonymous_thp_policy_serialization::ErrorRow> anonymous_errors_return;
    const anonymous_thp_policy_serialization::Owner* enumeration_this;const anonymous_thp_policy_serialization::ObservationError* enumeration_return;
    const process_thp_restriction_serialization::Owner* observation_this;const process_thp_restriction_serialization::ObservationError* observation_return;
    std::span<detail::ErrorIndexRow> sort_rows,sift_rows;std::size_t sort_root,sort_size,sift_root,sift_size,sift_child;
    const detail::ErrorIndexRow *less_a,*less_b;bool less_return;detail::ErrorIndexRow swap_temporary;detail::ErrorIndexRow *swap_a,*swap_b;
    std::span<detail::ErrorIndexRow>::iterator span_begin,span_end;
    const e::Owner *canonical_this_a,*canonical_this_b,*digest_this_a,*digest_this_b,*provenance_this_a,*provenance_this_b,*error_this;
    std::string_view canonical_a,canonical_b;const core::Sha256Digest *digest_a,*digest_b;const e::Provenance *provenance_a,*provenance_b;const core::Error* error_return;
};
template<class T,class L,class U,class E>struct ChildCells {
    const std::optional<T>* backing_child;U backing_usage;std::size_t backing_return;
    const L* allowance_limits;std::size_t allowance_floor,allowance_out,allowance_return;
    const detail::View* parent_view;const std::optional<T>* parent_child;decltype(std::declval<const T&>().provenance()) parent_provenance;
    std::optional<T>* decode_target;std::string_view decode_encoded;const L* decode_limits;
    std::string decode_bytes;std::expected<T,E> decoded;L decode_limits_copy;
    const T *usage_this,*canonical_this,*digest_this,*provenance_this,*slot_this;
    U usage_return;std::string_view canonical_return;const core::Sha256Digest* digest_return;
    decltype(std::declval<const T&>().provenance()) provenance_return;std::optional<t::SlotState> slot_return;
    T* move_target;T* move_source;
    const T* topology_this;decltype(std::declval<const T&>().topology()) topology_return;
    const T* failure_error_this;const e::Owner* failure_error_return;
    std::array<std::size_t,6> allowance_base_values;std::initializer_list<std::size_t> allowance_base_list;

};
struct OperationCells {
    WireCells wire;IndexCells index;
    ChildCells<t::Owner,t::Limits,t::Usage,t::Error> topology;
    ChildCells<system_controls_serialization::Owner,system_controls_serialization::Limits,system_controls_serialization::Usage,system_controls_serialization::Error> controls;
    ChildCells<kernel_identity_serialization::Owner,kernel_identity_serialization::Limits,kernel_identity_serialization::Usage,kernel_identity_serialization::Error> kernel;
    ChildCells<pu_identity_serialization::Owner,pu_identity_serialization::Limits,pu_identity_serialization::Usage,pu_identity_serialization::Error> pu;
    ChildCells<frequency_policy_serialization::Owner,frequency_policy_serialization::Limits,frequency_policy_serialization::Usage,frequency_policy_serialization::Error> frequency;
    ChildCells<anonymous_thp_policy_serialization::Owner,anonymous_thp_policy_serialization::Limits,anonymous_thp_policy_serialization::Usage,anonymous_thp_policy_serialization::Error> anonymous;
    ChildCells<process_thp_restriction_serialization::Owner,process_thp_restriction_serialization::Limits,process_thp_restriction_serialization::Usage,process_thp_restriction_serialization::Error> process;
    std::string_view decode_body;Limits decode_limits,prepare_limits;Parsed parsed;Usage usage,usage_return,primitive_return,publication_input;
    detail::Children children;std::unique_ptr<detail::State> state,publish_return;Sink counter;
    detail::ResolvedInput *prepare_input,*preflight_input,*child_phase_input;const Limits *preflight_limits,*child_phase_limits;std::size_t phase_index;
    std::expected<void,Error> void_result;std::expected<Owner,Error> result;std::unexpected<Error> unexpected;Error caught;const std::length_error* caught_length;
    const Limits* valid_limits_parameter;std::initializer_list<std::size_t> limit_values;std::size_t limit_value;Json::array_t array_maximum;std::string string_maximum;
    const detail::View* primitive_view;const Limits* primitive_limits;std::size_t primitive_upper;Usage primitive_usage;
    const detail::View* identity_view;std::expected<journal::RunId,core::Error> parsed_run;std::expected<journal::Timestamp,core::Error> parsed_time;
    core::detail::ExpectedErrorCleanup<decltype(parsed_run)> run_cleanup;core::detail::ExpectedErrorCleanup<decltype(parsed_time)> time_cleanup;
    const detail::Children* backing_children;std::size_t backing_out;
    const Limits* bound_limits;std::size_t bound_index;bool bound_selected;ChildBound bound_return,phase_bound;
    const detail::Children* phase_children;const Limits* phase_limits;std::size_t phase_child_index,phase_decoded,phase_upper,phase_out;
    const detail::View* parents_view;const detail::Children* parents_children;
    const t::Owner *root,*nested;const topology_evidence::Owner *original,*candidate;
    const topology_evidence::Owner *topology_canonical_this_a,*topology_canonical_this_b,*topology_digest_this_a,*topology_digest_this_b,*fingerprints_this_a,*fingerprints_this_b;
    std::string_view topology_canonical_a,topology_canonical_b;const core::Sha256Digest *topology_digest_a,*topology_digest_b;const topology::TopologyFingerprints *fingerprints_a,*fingerprints_b;
    const Counts* dom_count;std::size_t dom_return;
    const Counts* publication_count;const detail::Children* publication_children;std::size_t publication_upper;const Limits* publication_limits;
    std::array<std::string_view,7> child_bytes;std::size_t canonical_length;std::string_view child_bytes_value;
    detail::Children* publish_children;const detail::View* publish_view;const Counts* publish_count;Usage publish_usage;Origin publish_origin;
    std::unique_ptr<detail::State> publish_state;Provenance published_provenance;std::string published_run,published_time;
    Json dom;core::detail::JsonCleanup<Json> dom_cleanup;Sink emit_sink;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> render_guard;
    core::JcsLimits render_limits;core::Sha256Digest published_digest;std::string_view digest_input;
    bool require_value;Error require_error;std::size_t plus_left,plus_right,plus_return,times_left,times_right,times_return;std::optional<std::size_t> times_result;
    std::size_t bound_value,bound_limit,base_upper,base_return;std::size_t* add_value;std::size_t add_amount,multiply_a,multiply_b;std::optional<std::size_t> multiply_return;
    std::size_t fixed_return,preflight_phase_return;
    std::array<std::size_t,3> valid_limits_values;
    detail::State* state_constructor_this;Owner* owner_constructor_this;std::unique_ptr<detail::State> owner_constructor_argument;
    Owner owner_conversion_temporary;Owner *owner_move_this,*owner_move_source;
    const Owner* usage_this;Usage usage_getter_return;const Owner* canonical_this;std::string_view canonical_getter_return;
    const Owner* provenance_this;const Provenance* provenance_getter_return;const Owner* digest_this;const core::Sha256Digest* digest_getter_return;
    const Owner* origin_this;std::optional<Origin> origin_getter_return;
    const std::size_t *emitted_begin,*emitted_end,*parsed_begin,*parsed_end;
    const std::string_view *length_begin,*length_end;
    decltype(std::declval<std::vector<detail::ErrorIndexRow>&>().max_size()) vector_maximum;
    std::span<const pu_identity_serialization::ErrorRow>::iterator pu_begin,pu_end;
    std::span<const frequency_policy_serialization::ErrorRow>::iterator frequency_begin,frequency_end;
    std::span<const anonymous_thp_policy_serialization::ErrorRow>::iterator anonymous_begin,anonymous_end;
    const std::optional<e::Owner> *pu_error_begin,*pu_error_end;
    std::array<std::size_t,3> controls_maxima,kernel_maxima,pu_input_maxima,pu_binding_maxima,frequency_input_maxima,frequency_binding_maxima,anonymous_maxima,process_maxima;
    std::initializer_list<std::size_t> controls_maxima_list,kernel_maxima_list,pu_input_maxima_list,pu_binding_maxima_list,frequency_input_maxima_list,frequency_binding_maxima_list,anonymous_maxima_list,process_maxima_list;

};
}
namespace detail {
auto checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto checked_multiply(std::size_t a,std::size_t b)noexcept->std::optional<std::size_t>{if(b&&a>SIZE_MAX/b)return {};return a*b;}
auto fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
void sort_error_index(std::span<ErrorIndexRow> rows)noexcept{
    for(auto root=rows.size()/2;root>0;--root)sift(rows,root-1,rows.size());
    for(auto size=rows.size();size>1;--size){std::swap(rows[0],rows[size-1]);sift(rows,0,size-1);}
}
}
Owner::~Owner()=default;Owner::Owner(Owner&&)noexcept=default;auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
#define HOST_GETTER(M,NS) auto Owner::M()const noexcept->const NS::Owner*{return state_&&state_->children.M?&*state_->children.M:nullptr;}
HOST_GETTER(topology,topology_serialization)
HOST_GETTER(system_controls,system_controls_serialization)
HOST_GETTER(kernel_identity,kernel_identity_serialization)
HOST_GETTER(pu_identity,pu_identity_serialization)
HOST_GETTER(frequency,frequency_policy_serialization)
HOST_GETTER(anonymous_thp,anonymous_thp_policy_serialization)
HOST_GETTER(process_thp,process_thp_restriction_serialization)
#undef HOST_GETTER
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{valid_limits(limits);require(input.children,Error::source);input.usage=primitive(input.view,limits,input.upper_fixed);identity(input.view);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::child_phase(detail::ResolvedInput& input,const Limits& limits,std::size_t index)->std::expected<void,Error>{
    try{input.usage.child_preparation_bytes.at(index)=child_phase_stock(*input.children,limits,index,0,input.upper_fixed);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{parents_match(input.view,*input.children);errors_match(input.view,*input.children,limits,input.upper_fixed,input.usage);
        Sink counter;emit(counter,input.view,*input.children);input.usage=publication(input.usage,counter.count,*input.children,input.upper_fixed,limits);
        return Owner{publish(*input.children,input.view,counter.count,input.usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits,0);identity(parsed.view);detail::Children children;
#define HOST_DECODE(N,M) usage.child_preparation_bytes[N]=child_phase_stock(children,limits,N,parsed.children[N].size()/2,0);decode_child(children.M,parsed.children[N],limits.M)
        HOST_DECODE(0,topology);HOST_DECODE(1,system_controls);HOST_DECODE(2,kernel_identity);HOST_DECODE(3,pu_identity);
        HOST_DECODE(4,frequency);HOST_DECODE(5,anonymous_thp);HOST_DECODE(6,process_thp);
#undef HOST_DECODE
        parents_match(parsed.view,children);errors_match(parsed.view,children,limits,0,usage);usage=publication(usage,parsed.wire,children,0,limits);
        auto state=publish(children,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::host_serialization
