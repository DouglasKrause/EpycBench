#include "results/private_source_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::source_serialization {
namespace {
using Json=nlohmann::json;
namespace a=authored_source;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array kinds{"creation_source","normalization_source","outcome_source"};
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array availabilities{"not_attempted","not_obtained","available"};
constexpr std::string_view digits="0123456789abcdef";
void require(bool value,Error error=Error::schema){if(!value)throw error;}
auto plus(std::size_t left,std::size_t right)->std::size_t{require(detail::checked_add(left,right),Error::capacity);return left;}
auto times(std::size_t left,std::size_t right)->std::size_t{const auto result=detail::checked_multiply(left,right);require(result.has_value(),Error::capacity);return *result;}
void bound(std::size_t value,std::size_t limit){require(value<=limit,Error::capacity);}
template<class T,class Names> auto name(T value,const Names& names)->std::string_view{const auto i=static_cast<std::size_t>(value);require(i<names.size());return names[i];}
template<class T,class Names> auto enumerated(std::string_view value,const Names& names)->T{for(std::size_t i=0;i<names.size();++i)if(value==names[i])return static_cast<T>(i);throw Error::schema;}
auto format(scenario::ScenarioFormat value)->std::string_view{if(value==scenario::ScenarioFormat::json)return "json";require(value==scenario::ScenarioFormat::yaml);return "yaml";}
auto format(std::string_view value)->scenario::ScenarioFormat{if(value=="json")return scenario::ScenarioFormat::json;require(value=="yaml");return scenario::ScenarioFormat::yaml;}
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
    Json* root_;std::array<Frame,4> frames_{};std::size_t level_{};
    void prefix(std::string_view key){count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);if(!level_)return;auto& f=frames_[level_-1];if(f.children++)count.bytes=plus(count.bytes,1);if(f.object){++count.members;count.text=plus(count.text,key.size());count.bytes=plus(count.bytes,plus(key.size(),3));}else ++count.elements;}
    auto insert(std::string_view key,Json& node)->Json*{if(!level_){root_->swap(node);return root_;}auto& f=frames_[level_-1];if(f.object){core::detail::append_object(*f.value,key,node);return &f.value->find(key).value();}f.value->push_back(std::move(node));return &f.value->back();}
    void container(std::string_view key,bool object,std::size_t size){prefix(key);require(level_<frames_.size());++count.bytes;if(object)++count.objects;else ++count.arrays;Json* placed=nullptr;if(root_){Json node=object?Json::object():Json::array();core::detail::JsonCleanup cleanup{node};if(!object)node.get_ref<Json::array_t&>().reserve(size);placed=insert(key,node);}frames_[level_++]={placed,0,object};}
};
void emit(Sink& sink,const detail::View& view,const a::Input& source,const e::Owner* child){
    sink.object();if(child)sink.octets("acquisition_error_hex",child->canonical());else sink.null("acquisition_error_hex");
    sink.text("domain",name(view.domain,domains));sink.text("entry_producer","controller");sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);
    sink.object("source");if(view.acquisition){sink.object("acquisition_reference");if(view.acquisition->creation_ordinal)sink.number("creation_ordinal",*view.acquisition->creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",view.acquisition->sequence);sink.end();}else sink.null("acquisition_reference");
    sink.text("availability",name(source.availability,availabilities));sink.octets("bytes_hex",source.bytes);sink.text("format",format(source.format));sink.octets("name_hex",source.name);sink.array("rejected_overrides_hex",source.rejected_overrides.size());for(auto token:source.rejected_overrides)sink.octets({},token);sink.end();sink.end();sink.text("source_kind",name(view.kind,kinds));sink.text("timestamp",view.time);sink.end();
}
struct ParseFrame {std::size_t children{};bool object{};};
class Reader {
  public:
    Counts count;
    Reader(std::string_view body,const Limits& limits):body_(body),limits_(limits){bound(body.size(),limits.canonical_bytes);count.bytes=body.size();}
    auto next(char ch)const noexcept->bool{const auto at=at_+(at_<body_.size()&&body_[at_]==','?1U:0U);return at<body_.size()&&body_[at]==ch;}
    auto position()const noexcept->std::size_t{return at_;}
    auto slice(std::size_t start)const noexcept->std::string_view{return body_.substr(start,at_-start);}
    void expect(std::string_view value){require(body_.substr(at_,value.size())==value,Error::encoding);at_+=value.size();}
    void key(std::string_view value){require(level_&&frames_[level_-1].object);if(frames_[level_-1].children++)expect(",");expect("\"");expect(value);expect("\":");++count.members;count.text=plus(count.text,value.size());bound(count.text,limits_.text_bytes);}
    void object(){node();expect("{");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,true};++count.objects;}
    void array(){node();expect("[");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,false};++count.arrays;}
    void end_object(){expect("}");require(level_!=0);--level_;}
    void end_array(){expect("]");require(level_!=0);--level_;}
    auto text()->std::string_view{node();expect("\"");const auto begin=at_;while(at_<body_.size()&&body_[at_]!='"'){const auto ch=static_cast<unsigned char>(body_[at_]);require(ch>=0x20&&ch<=0x7e&&ch!='\\',Error::encoding);++at_;}require(at_<body_.size(),Error::encoding);const auto result=body_.substr(begin,at_-begin);++at_;++count.strings;count.text=plus(count.text,result.size());bound(count.text,limits_.text_bytes);return result;}
    auto octets()->std::string_view{const auto value=text();valid_hex(value);return value;}
    auto number()->std::uint64_t{return integer(text());}
    void null(){node();expect("null");}
    auto optional()->std::optional<std::uint64_t>{if(next('n')){null();return {};}return number();}
    void finished(){require(level_==0&&at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,4> frames_{};
    void node(){if(level_&&!frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};
struct Parsed {detail::View view;Counts wire;std::string_view bytes,name,overrides,child;bool has_child{};a::Availability availability{};scenario::ScenarioFormat format{};std::size_t override_count{},override_bytes{},maximum{};};
auto parse(std::string_view body,const Limits& limits)->Parsed{
    Reader r{body,limits};Parsed out;r.object();r.key("acquisition_error_hex");if(r.next('n'))r.null();else{out.has_child=true;out.child=r.octets();bound(out.child.size()/2,limits.child.canonical_bytes);}
    r.key("domain");out.view.domain=enumerated<p::Domain>(r.text(),domains);r.key("entry_producer");require(r.text()=="controller");r.key("run_id");out.view.run=r.text();r.key("schema");require(r.text()==profile);r.key("sequence");out.view.sequence=r.number();r.key("source");r.object();r.key("acquisition_reference");if(r.next('n'))r.null();else{r.object();r.key("creation_ordinal");const auto ordinal=r.optional();r.key("sequence");out.view.acquisition=Acquisition{r.number(),ordinal};r.end_object();}
    r.key("availability");out.availability=enumerated<a::Availability>(r.text(),availabilities);r.key("bytes_hex");out.bytes=r.octets();bound(out.bytes.size()/2,limits.source_bytes);r.key("format");out.format=format(r.text());r.key("name_hex");out.name=r.octets();bound(out.name.size()/2,limits.name_bytes);r.key("rejected_overrides_hex");r.array();const auto begin=r.position();while(!r.next(']')){const auto token=r.octets();out.override_count=plus(out.override_count,1);out.override_bytes=plus(out.override_bytes,token.size()/2);out.maximum=std::max(out.maximum,token.size()/2);bound(out.override_count,limits.override_count);bound(out.override_bytes,limits.override_bytes);}r.end_array();out.overrides=r.slice(begin);r.end_object();r.key("source_kind");out.view.kind=enumerated<Kind>(r.text(),kinds);r.key("timestamp");out.view.time=r.text();r.end_object();r.finished();out.wire=r.count;out.maximum=std::max({out.maximum,out.bytes.size()/2,out.name.size()/2,out.child.size()/2});return out;
}
void valid_limits(const Limits& limits){
    for(auto n:{limits.source_bytes,limits.name_bytes,limits.override_count,limits.override_bytes,limits.source_retained_bytes,limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);
    require(limits.override_count<=std::vector<std::string>{}.max_size()&&limits.override_count<=std::vector<std::string_view>{}.max_size(),Error::invalid_limits);
    require(limits.depth<=4&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
}
void local(const detail::View& view,a::Availability availability,std::size_t bytes,std::size_t name_bytes,std::size_t overrides){
    require(view.sequence!=0,Error::identity);(void)name(view.kind,kinds);(void)name(view.domain,domains);(void)name(availability,availabilities);
    if(view.kind==Kind::creation_source)require(view.sequence==1);
    if(view.kind==Kind::normalization_source)require(view.sequence>1&&availability==a::Availability::available&&overrides==0&&!view.acquisition);
    require((availability==a::Availability::not_obtained)==view.acquisition.has_value());
    if(availability!=a::Availability::available)require(bytes==0&&name_bytes==0&&overrides==0);
    if(view.acquisition){const auto& ref=*view.acquisition;require(ref.sequence!=0&&ref.sequence<=view.sequence,Error::reference);if(ref.creation_ordinal)require(ref.sequence==1&&*ref.creation_ordinal<1024,Error::reference);if(view.kind==Kind::creation_source)require(ref.creation_ordinal.has_value(),Error::reference);if(view.kind==Kind::outcome_source&&ref.sequence==view.sequence)require(!ref.creation_ordinal,Error::reference);}
}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);}
void child_match(const detail::View& view,const e::Owner* child){
    require(view.acquisition.has_value()==(child!=nullptr),Error::child);if(!child)return;const auto* value=child->provenance();require(value&&child->error(),Error::child);const auto& ref=*view.acquisition;
    require(value->run==view.run&&value->domain==view.domain&&value->sequence==ref.sequence&&value->creation_ordinal==ref.creation_ordinal,Error::child);
    require(value->kind==(ref.creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure)&&value->association.has_value(),Error::child);const auto& fact=*value->association;
    require(fact.producer==p::Producer::controller&&!fact.process_sequence,Error::child);constexpr p::Target target{p::Category::authored_source,p::Selector::source_acquisition};require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
    if(ref.sequence==view.sequence)require(value->time==view.time&&value->entry_producer==p::Producer::controller,Error::child);
}
auto primitive(const a::Input& source,const detail::View& view,const Limits& limits)->Usage{
    (void)format(source.format);Usage out;out.source_bytes=source.bytes.size();out.name_bytes=source.name.size();out.override_count=source.rejected_overrides.size();for(auto token:source.rejected_overrides)out.override_bytes=plus(out.override_bytes,token.size());out.provenance_bytes=plus(view.run.size(),view.time.size());
    bound(out.source_bytes,limits.source_bytes);bound(out.name_bytes,limits.name_bytes);bound(out.override_count,limits.override_count);bound(out.override_bytes,limits.override_bytes);bound(out.provenance_bytes,limits.provenance_bytes);
    local(view,source.availability,out.source_bytes,out.name_bytes,out.override_count);require(source.acquisition_failure.has_value()==view.acquisition.has_value(),Error::reference);if(source.acquisition_failure)require(source.acquisition_failure->value==view.acquisition->sequence,Error::reference);return out;
}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
auto outer_usage(Usage out,const Counts& count,const Limits& limits)->Usage{out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);return out;}
struct Scratch {std::string bytes,name;std::vector<std::string> overrides;std::vector<std::string_view> views;};
auto restore(const Parsed& parsed)->Scratch{Scratch out;out.bytes=unhex(parsed.bytes);out.name=unhex(parsed.name);out.overrides.reserve(parsed.override_count);out.views.reserve(parsed.override_count);auto cursor=parsed.overrides;for(std::size_t i=0;i<parsed.override_count;++i){if(i)cursor.remove_prefix(1);require(cursor.front()=='"');cursor.remove_prefix(1);const auto end=cursor.find('"');out.overrides.push_back(unhex(cursor.substr(0,end)));cursor.remove_prefix(end+1);}for(const auto& value:out.overrides)out.views.push_back(value);return out;}
auto child_stock(const e::Owner* child)->std::size_t{return child?child->usage().retained_bytes:0;}
auto error_phase(const Limits& limits,bool child,std::size_t decoded,std::size_t upper)->std::size_t{if(!child)return 0;const auto stock=plus(plus(plus(detail::fixed_stock(),upper),limits.child.consumer_simultaneous_bytes),decoded);bound(stock,limits.error_preparation_bytes);return stock;}
auto source_phase(const Limits& limits,const e::Owner* child,std::size_t scratch,std::size_t upper)->std::size_t{const auto stock=plus(plus(plus(plus(detail::fixed_stock(),upper),child_stock(child)),scratch),limits.source_retained_bytes);bound(stock,limits.source_preparation_bytes);return stock;}
auto source_limits(const Limits& limits)->a::Limits{return {limits.source_bytes,limits.name_bytes,limits.override_count,limits.override_bytes,0,0,0,limits.source_retained_bytes};}
auto retain(const a::Input& input,const Limits& limits)->a::SourceBundle{auto kept=a::retain(input,source_limits(limits));core::detail::ExpectedErrorCleanup cleanup{kept};require(kept.has_value(),Error::source);return std::move(*kept);}
auto publication(Usage out,const Counts& count,const a::SourceBundle& source,const e::Owner* child,std::size_t maximum,std::size_t upper,const Limits& limits)->Usage{
    out=outer_usage(out,count,limits);out.source_retained_bytes=source.logical_bytes();require(out.source_retained_bytes>=sizeof(a::SourceBundle),Error::source);bound(out.source_retained_bytes,limits.source_retained_bytes);
    out.retained_bytes=plus(plus(plus(sizeof(Owner)+sizeof(detail::State),out.provenance_bytes),out.source_retained_bytes-sizeof(a::SourceBundle)),out.canonical_bytes);
    if(child){out.child=child->usage();require(out.child->retained_bytes>=sizeof(e::Owner),Error::child);out.retained_bytes=plus(out.retained_bytes,out.child->retained_bytes-sizeof(e::Owner));}
    out.publication_bytes=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),out.retained_bytes),dom_stock(count)),times(maximum,2)),plus(out.provenance_bytes,21));
    bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(a::SourceBundle&& source,std::optional<e::Owner>& child,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.kind,view.sequence,view.acquisition};state->source=std::move(source);state->child=std::move(child);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};const auto input=state->source->input();emit(sink,view,input,state->child?&*state->child:nullptr);
    auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value(),Error::encoding);require(rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
// Conservative named operation-cell allowance, shared by all three phases and
// both entrypoints. Library implementation scratch remains separately bounded.
// A union of named cells deliberately charges retired earlier cells as well.
struct OperationCells {
    const detail::ResolvedInput* preflight_input;const Limits* preflight_limits;
    a::Input preflight_source;Usage preflight_primitive_return;std::expected<void,Error> preflight_return;
    const detail::ResolvedInput* prepare_input;Limits prepare_limits,decode_limits;
    std::string_view decode_body;const detail::View* prepare_view;
    a::Input prepare_source,decoded_input,publish_input;const e::Owner* prepared_child;
    Usage prepare_usage,decode_usage,primitive_out,outer_parameter,outer_return,publication_parameter,publication_return;
    Sink counter,publish_sink;std::size_t prepare_maximum;std::string_view prepare_token;
    std::array<std::size_t,3> prepare_maximum_values;std::initializer_list<std::size_t> prepare_maximum_list;
    a::SourceBundle prepared_source,retained_source_return;std::optional<a::SourceBundle> decoded_source;Parsed decoded_parsed,parse_out,parse_return;
    std::optional<e::Owner> decoded_child;std::string decoded_child_body;
    std::expected<e::Owner,e::Error> decoded_child_result;
    std::size_t decoded_error_phase,decoded_source_phase,decoded_scratch;
    Scratch restored,restore_out,restore_return;std::string_view restore_cursor;std::size_t restore_index,restore_end;const std::string* restore_value;
    std::string_view parse_body;const Limits* parse_limits;Reader parse_reader;std::optional<std::uint64_t> parse_ordinal;std::size_t parse_begin;std::string_view parse_token;
    std::array<std::size_t,4> parse_maximum_values;std::initializer_list<std::size_t> parse_maximum_list;
    const Parsed* restore_parameter;const Limits *validation_limits,*primitive_limits,*phase_limits,*retain_limits,*publication_limits;
    std::array<std::size_t,8> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;
    std::string limits_string;std::vector<std::string> limits_strings;std::vector<std::string_view> limits_views;Json::array_t limits_array;
    const detail::View *local_view,*identity_view,*child_view,*primitive_view,*publish_view;
    a::Availability local_availability;std::size_t local_bytes,local_name,local_overrides;const Acquisition* local_reference;
    std::expected<journal::RunId,core::Error> run_identity;core::detail::ExpectedErrorCleanup<journal::RunId> run_guard;
    std::expected<journal::Timestamp,core::Error> time_identity;core::detail::ExpectedErrorCleanup<journal::Timestamp> time_guard;
    const e::Owner* child_parameter;const e::Provenance* child_provenance;const Acquisition* child_reference;const e::Association* child_fact;p::Target child_target;
    e::Usage child_usage,child_usage_return;const e::Owner* child_usage_this;
    const a::Input *primitive_source,*retain_input;std::string_view primitive_token;
    const Counts *dom_count,*outer_count,*publication_count,*publish_count;
    std::size_t error_decoded,error_upper,error_stock,source_scratch,source_upper,source_stock;bool error_has_child;
    const e::Owner* phase_child;const a::SourceBundle* publication_source;const e::Owner* publication_child;
    std::size_t publication_maximum,publication_upper;
    a::Limits source_limits_return;std::expected<a::SourceBundle,core::Error> retained_result;core::detail::ExpectedErrorCleanup<a::SourceBundle> retained_guard;
    a::SourceBundle* publish_source;std::optional<e::Owner>* publish_child;Usage publish_usage;Origin publish_origin;
    std::unique_ptr<detail::State> decoded_state,publish_state,publish_return,owner_constructor_state;Owner* owner_constructor_this;
    Provenance publish_provenance_temporary;std::string publish_run_temporary,publish_time_temporary;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;
    core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<std::string> rendered_guard;
    core::Sha256Digest digest_return;Owner constructed_owner;std::expected<Owner,Error> owner_return;std::unexpected<Error> unexpected_return;Error caught;
    const a::SourceBundle* input_getter_this; a::Input input_getter_return;
    const a::SourceBundle* stock_getter_this;std::size_t stock_getter_return;
    const e::Owner *canonical_getter_this,*provenance_getter_this,*error_getter_this;
    std::string_view canonical_getter_return;const e::Provenance* provenance_getter_return;const core::Error* error_getter_return;
    // Renderer constructor/method/assignment cells, including actual Frame rvalue.
    Sink* sink_this;Json* sink_root;std::string_view sink_key,sink_value;std::size_t sink_size;bool sink_object;
    Frame* sink_frame;Frame sink_assigned_frame;Json* sink_placed;Json sink_node;core::detail::JsonCleanup<Json> sink_cleanup;
    std::string sink_encoded,sink_text_temporary;Decimal sink_decimal;Json* sink_insert_return;
    // append_object's concrete nlohmann::json branch; key backing moves into DOM.
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;
    std::string append_object_key;Json::iterator insert_find_result;
    Sink* emit_sink;const detail::View* emit_view;const a::Input* emit_source;const e::Owner* emit_child;std::string_view emit_token;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;
    const Decimal* decimal_view_this;std::string_view decimal_view_return;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;char reader_next_char;
    std::size_t reader_next_at,reader_slice_start,reader_text_begin;std::string_view reader_expected,reader_key,reader_text_return,reader_octets_value;
    unsigned char reader_character;ParseFrame reader_assigned_frame;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,unhex_result,unhex_return;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value;std::size_t enum_index,name_index;
    const std::array<const char*,3>* enum_kind_names;const std::array<const char*,2>* enum_domain_names;
    const std::array<const char*,3>* enum_availability_names;
    Kind name_kind;p::Domain name_domain;a::Availability name_availability;
    std::vector<p::Target>::const_iterator child_find_return;
    std::string_view name_return,format_return,reader_slice_return;
    Acquisition parsed_acquisition;std::optional<a::FailureReference> decoded_reference;
    std::size_t fixed_return,error_phase_return,source_phase_return,dom_return;
    const e::Owner* child_stock_parameter;std::size_t child_stock_return;
    const Limits* source_limits_parameter;std::expected<void,Error> preflight_expected;
    // Distinct forwarding frames remain live while their delegated helpers run.
    Sink* object_this;std::string_view object_key;
    Sink* array_this;std::string_view array_key;std::size_t array_size;
    Sink* text_this;std::string_view text_key,text_value;
    Sink* octets_this;std::string_view octets_key,octets_value;
    Sink* null_this;std::string_view null_key;
    Sink* number_this;std::string_view number_key;std::uint64_t number_value;
    Sink* prefix_this;std::string_view prefix_key;
    Sink* insert_this;std::string_view insert_key;Json* insert_node;
    Reader* reader_number_this;Reader* reader_optional_this;Reader* reader_octets_this;
    Reader* reader_node_this;Reader* reader_object_this;Reader* reader_array_this;
    Reader* reader_text_this;Reader* reader_expect_this;Reader* reader_key_this;
    Reader* reader_null_this;Reader* reader_finished_this;
    scenario::ScenarioFormat format_value;std::string_view format_text;
    std::size_t plus_left,plus_right,times_left,times_right;std::optional<std::size_t> times_result;
    std::size_t* checked_value;std::size_t checked_amount,multiply_left,multiply_right;
    std::size_t bound_value,bound_limit;bool require_value;Error require_error;
};
} // namespace

auto detail::checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto detail::checked_multiply(std::size_t left,std::size_t right)noexcept->std::optional<std::size_t>{if(right&&left>SIZE_MAX/right)return {};return left*right;}
auto detail::fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&)noexcept=default;
auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::~Owner()=default;
auto Owner::source()const noexcept->const a::SourceBundle*{return state_&&state_->source?&*state_->source:nullptr;}
auto Owner::acquisition_error()const noexcept->const e::Owner*{return state_&&state_->child?&*state_->child:nullptr;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(const detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{valid_limits(limits);require(input.source&&input.source->logical_bytes(),Error::source);const auto source=input.source->input();(void)primitive(source,input.view,limits);(void)error_phase(limits,input.view.acquisition.has_value(),0,input.upper_fixed);return {};}
    catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(const detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);require(input.source&&input.source->logical_bytes()&&input.child,Error::source);const auto& view=input.view;const auto source=input.source->input();const auto* child=*input.child?&**input.child:nullptr;auto usage=primitive(source,view,limits);identity(view);child_match(view,child);
        usage.error_preparation_bytes=error_phase(limits,child!=nullptr,0,input.upper_fixed);Sink count;emit(count,view,source,child);usage=outer_usage(usage,count.count,limits);
        std::size_t maximum=std::max({source.bytes.size(),source.name.size(),child?child->canonical().size():0});for(auto token:source.rejected_overrides)maximum=std::max(maximum,token.size());
        usage.source_preparation_bytes=source_phase(limits,child,0,input.upper_fixed);auto kept=retain(source,limits);usage=publication(usage,count.count,kept,child,maximum,input.upper_fixed,limits);
        return Owner{publish(std::move(kept),*input.child,view,count.count,usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);local(parsed.view,parsed.availability,parsed.bytes.size()/2,parsed.name.size()/2,parsed.override_count);bound(plus(parsed.view.run.size(),parsed.view.time.size()),limits.provenance_bytes);identity(parsed.view);require(parsed.has_child==parsed.view.acquisition.has_value(),Error::child);
        std::optional<e::Owner> child;const auto error_stock=error_phase(limits,parsed.has_child,parsed.child.size()/2,0);
        if(parsed.has_child){const auto child_body=unhex(parsed.child);auto decoded=e::Owner::decode(child_body,limits.child);require(decoded.has_value(),Error::child);child=std::move(*decoded);}child_match(parsed.view,child?&*child:nullptr);
        const auto scratch=plus(plus(plus(parsed.bytes.size()/2,parsed.name.size()/2),parsed.override_bytes),times(parsed.override_count,sizeof(std::string)+sizeof(std::string_view)));
        const auto source_stock=source_phase(limits,child?&*child:nullptr,scratch,0);
        std::optional<a::SourceBundle> source;Usage usage;
        {auto restored=restore(parsed);const a::Input input{parsed.availability,restored.bytes,restored.name,parsed.format,restored.views,parsed.view.acquisition?std::optional{a::FailureReference{parsed.view.acquisition->sequence}}:std::nullopt};usage=primitive(input,parsed.view,limits);source=retain(input,limits);}
        usage.error_preparation_bytes=error_stock;usage.source_preparation_bytes=source_stock;usage=publication(usage,parsed.wire,*source,child?&*child:nullptr,parsed.maximum,0,limits);
        auto state=publish(std::move(*source),child,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::source_serialization
