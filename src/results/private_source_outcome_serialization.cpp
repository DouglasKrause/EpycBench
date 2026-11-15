#include "results/private_source_outcome_serialization.hpp"
#include "results/authored_source_codes.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>
namespace epycbench::results::source_outcome_serialization {
namespace {
using Json=nlohmann::json;namespace a=authored_source;namespace s=source_serialization;namespace e=error_serialization;namespace p=process_subject;
constexpr std::array states{"not_attempted","source_unavailable","override_rejected","scenario_rejected","resolution_failed","resolved","expected_plan_mismatch"};
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::string_view digits="0123456789abcdef";
void require(bool value,Error error=Error::schema){if(!value)throw error;}
auto plus(std::size_t left,std::size_t right)->std::size_t{require(right<=SIZE_MAX-left,Error::capacity);return left+right;}
auto times(std::size_t left,std::size_t right)->std::size_t{require(!right||left<=SIZE_MAX/right,Error::capacity);return left*right;}
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
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,3> frames_{};
    void node(){if(level_&&!frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};
struct Diagnostics {std::span<const a::SafeDiagnostic> original;std::string_view wire;std::size_t count{},bytes{};};
struct Cursor {
    const Diagnostics& values;std::size_t index{};std::string_view remaining;
    explicit Cursor(const Diagnostics& d):values(d),remaining(d.wire){}
    auto next()->std::string_view{require(index<values.count);if(!values.original.empty())return values.original[index++].code;if(index++)remaining.remove_prefix(1);require(remaining.front()=='"');remaining.remove_prefix(1);const auto end=remaining.find('"');require(end!=std::string_view::npos);const auto value=remaining.substr(0,end);remaining.remove_prefix(end+1);return value;}
};
void safe(std::string_view code){require(a::detail::safe_code(code)==code,Error::schema);}
auto diagnostic_view(std::span<const a::SafeDiagnostic> values)->Diagnostics{Diagnostics out{values,{},values.size(),0};for(const auto& value:values){safe(value.code);out.bytes=plus(out.bytes,value.code.size());}return out;}
void valid_limits(const Limits& limits){for(auto n:{limits.diagnostics,limits.diagnostic_bytes,limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);require(limits.diagnostics<=std::vector<std::string>{}.max_size()&&limits.nodes<=Json::array_t{}.max_size()&&limits.depth<=3,Error::invalid_limits);}
void identity(const detail::View& view){require(view.sequence!=0,Error::identity);(void)name(view.domain,domains);auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_guard{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_guard{time};require(time.has_value(),Error::identity);}
void local(a::State state,const Diagnostics& diagnostics,const a::Input& input,const core::Error* upstream){
    (void)name(state,states);require(state!=a::State::resolved);const bool operational=state==a::State::resolution_failed;require(operational==(upstream!=nullptr),Error::child);
    if(state==a::State::not_attempted)require(input.availability==a::Availability::not_attempted);
    else if(state==a::State::source_unavailable)require(input.availability==a::Availability::not_obtained);
    else require(input.availability==a::Availability::available);
    require((state==a::State::override_rejected)==!input.rejected_overrides.empty());
    if(operational){require(diagnostics.count==1);Cursor cursor{diagnostics};require(cursor.next()==a::detail::safe_code(upstream->code.value()),Error::child);}
    else if(state!=a::State::scenario_rejected)require(diagnostics.count==0);
}
void source_match(const detail::View& view,const s::Owner& source){const auto* child=source.provenance();require(child&&source.source(),Error::child);require(child->kind==s::Kind::outcome_source&&child->run==view.run&&child->time==view.time&&child->domain==view.domain&&child->sequence==view.sequence,Error::child);}
void upstream_match(const detail::View& view,const e::Owner* upstream){if(!upstream)return;const auto* child=upstream->provenance();require(child&&upstream->error(),Error::child);require(child->kind==e::Kind::source_resolution&&child->run==view.run&&child->time==view.time&&child->domain==view.domain&&child->sequence==view.sequence&&child->entry_producer==p::Producer::controller&&!child->creation_ordinal&&!child->association,Error::child);}
auto primitive(const detail::View& view,const Diagnostics& diagnostics,const Limits& limits)->Usage{Usage out;out.diagnostics=diagnostics.count;out.diagnostic_bytes=diagnostics.bytes;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.diagnostics,limits.diagnostics);bound(out.diagnostic_bytes,limits.diagnostic_bytes);bound(out.provenance_bytes,limits.provenance_bytes);return out;}
void emit(Sink& sink,const detail::View& view,a::State state,const Diagnostics& diagnostics,std::string_view source,const std::optional<std::string_view>& upstream){sink.object();sink.array("diagnostics",diagnostics.count);Cursor cursor{diagnostics};for(std::size_t i=0;i<diagnostics.count;++i)sink.text({},cursor.next());sink.end();sink.text("domain",name(view.domain,domains));sink.text("entry_producer","controller");sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.octets("source_hex",source);sink.text("state",name(state,states));sink.text("timestamp",view.time);if(upstream)sink.octets("upstream_error_hex",*upstream);else sink.null("upstream_error_hex");sink.end();}
struct Parsed {detail::View view;Diagnostics diagnostics;Counts wire;a::State state{};std::string_view source,upstream;bool has_upstream{};};
auto parse(std::string_view body,const Limits& limits)->Parsed{Reader reader{body,limits};Parsed out;reader.object();reader.key("diagnostics");reader.array();const auto begin=reader.position();while(!reader.next(']')){const auto code=reader.text();safe(code);out.diagnostics.count=plus(out.diagnostics.count,1);out.diagnostics.bytes=plus(out.diagnostics.bytes,code.size());bound(out.diagnostics.count,limits.diagnostics);bound(out.diagnostics.bytes,limits.diagnostic_bytes);}reader.end_array();out.diagnostics.wire=reader.slice(begin);reader.key("domain");out.view.domain=enumerated<p::Domain>(reader.text(),domains);reader.key("entry_producer");require(reader.text()=="controller");reader.key("run_id");out.view.run=reader.text();reader.key("schema");require(reader.text()==profile);reader.key("sequence");out.view.sequence=reader.number();reader.key("source_hex");out.source=reader.octets();bound(out.source.size()/2,limits.source.canonical_bytes);reader.key("state");out.state=enumerated<a::State>(reader.text(),states);require(out.state!=a::State::resolved);reader.key("timestamp");out.view.time=reader.text();reader.key("upstream_error_hex");if(reader.next('n'))reader.null();else{out.has_upstream=true;out.upstream=reader.octets();bound(out.upstream.size()/2,limits.upstream.canonical_bytes);}reader.end_object();reader.finished();require(out.has_upstream==(out.state==a::State::resolution_failed),Error::child);if(out.state==a::State::resolution_failed)require(out.diagnostics.count==1);else if(out.state!=a::State::scenario_rejected)require(out.diagnostics.count==0);out.wire=reader.count;return out;}
auto source_phase(const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{const auto floor=plus(s::detail::fixed_stock(),upper?s::detail::session_fixed_stock():0);const auto envelope=std::max({limits.source.error_preparation_bytes,limits.source.source_preparation_bytes,limits.source.publication_bytes,limits.source.retained_bytes,floor});const auto stock=plus(plus(plus(detail::fixed_stock(),upper),scratch),envelope);bound(stock,limits.source_preparation_bytes);return stock;}
auto upstream_phase(const Limits& limits,bool needed,const s::Owner& source,std::size_t scratch,std::size_t upper)->std::size_t{if(!needed)return 0;const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();const auto envelope=std::max({limits.upstream.consumer_simultaneous_bytes,limits.upstream.retained_bytes,floor});const auto stock=plus(plus(plus(plus(detail::fixed_stock(),upper),source.usage().retained_bytes),scratch),envelope);bound(stock,limits.upstream_preparation_bytes);return stock;}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
auto publication(Usage out,const Counts& count,const s::Owner& source,const e::Owner* upstream,std::size_t upper,const Limits& limits)->Usage{out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);out.source=source.usage();require(out.source.retained_bytes>=sizeof(s::Owner),Error::child);out.retained_bytes=plus(plus(plus(plus(sizeof(Owner)+sizeof(detail::State),out.provenance_bytes),plus(times(out.diagnostics,sizeof(std::string)),out.diagnostic_bytes)),out.canonical_bytes),out.source.retained_bytes-sizeof(s::Owner));if(upstream){out.upstream=upstream->usage();require(out.upstream->retained_bytes>=sizeof(e::Owner),Error::child);out.retained_bytes=plus(out.retained_bytes,out.upstream->retained_bytes-sizeof(e::Owner));}const auto maximum=std::max(source.canonical().size(),upstream?upstream->canonical().size():0);out.publication_bytes=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),out.retained_bytes),dom_stock(count)),times(maximum,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;}
auto publish(std::optional<s::Owner>& source,std::optional<e::Owner>& upstream,const detail::View& view,a::State outcome,const Diagnostics& diagnostics,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->outcome=outcome;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.sequence};state->diagnostics.reserve(diagnostics.count);Cursor cursor{diagnostics};for(std::size_t i=0;i<diagnostics.count;++i)state->diagnostics.emplace_back(cursor.next());state->source=std::move(source);state->upstream=std::move(upstream);Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,outcome,diagnostics,state->source->canonical(),state->upstream?std::optional{state->upstream->canonical()}:std::nullopt);auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rendered_guard{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;}
// Conservative named union of operation cells, not a literal stack peak. Nested
// libraries retain their separately accepted logical/scratch contracts.
struct OperationCells {
    const detail::ResolvedInput *preflight_input,*upstream_input,*prepare_input;
    const Limits *preflight_limits,*upstream_limits;Limits prepare_limits,decode_limits;
    std::string_view decode_body;const detail::View* prepare_view;
    const a::SourceBundle* preflight_source;const private_error::Owner* preflight_error;
    const core::Error* preflight_original_error;a::Input preflight_source_input;
    Diagnostics preflight_diagnostics,prepared_diagnostics,diagnostic_out,diagnostic_return;
    std::span<const a::SafeDiagnostic> diagnostic_parameter;const a::SafeDiagnostic* diagnostic_value;
    Usage preflight_primitive,prepare_usage,decode_usage,primitive_out,primitive_return,publication_parameter,publication_return;
    const s::Owner* prepared_source;const e::Owner* prepared_upstream;
    a::Input prepared_input,decoded_input;Sink prepare_counter,publish_sink;
    Parsed decoded_parsed,parse_out,parse_return;std::optional<s::Owner> decoded_source;std::optional<e::Owner> decoded_upstream;
    std::string decoded_source_body,decoded_upstream_body;
    std::expected<s::Owner,s::Error> source_decoded;std::expected<e::Owner,e::Error> upstream_decoded;
    std::unique_ptr<detail::State> decode_state,publish_state,publish_return,owner_constructor_state;
    Owner* owner_constructor_this;Owner constructed_owner;std::expected<Owner,Error> owner_return;
    std::expected<void,Error> preflight_return;std::unexpected<Error> unexpected_return;Error caught;
    const Limits *validation_limits,*source_phase_limits,*upstream_phase_limits,*primitive_limits,*publication_limits;
    std::array<std::size_t,5> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;
    std::string limits_string;std::vector<std::string> limits_vector;Json::array_t limits_array;
    const detail::View *identity_view,*source_match_view,*upstream_match_view,*primitive_view,*publish_view;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<std::expected<journal::RunId,core::Error>> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<std::expected<journal::Timestamp,core::Error>> identity_time_guard;
    a::State local_state;const Diagnostics* local_diagnostics;const a::Input* local_input;const core::Error* local_upstream;bool local_operational;Cursor local_cursor;
    const s::Owner* match_source;const s::Provenance* source_provenance;
    const e::Owner* match_upstream;const e::Provenance* upstream_provenance;
    const Diagnostics* primitive_diagnostics;
    std::size_t source_scratch,source_upper,source_floor,source_envelope,source_stock;
    std::array<std::size_t,5> source_values;std::initializer_list<std::size_t> source_list;
    bool upstream_needed;const s::Owner* phase_source;
    std::size_t upstream_scratch,upstream_upper,upstream_floor,upstream_envelope,upstream_stock;
    std::array<std::size_t,3> upstream_values;std::initializer_list<std::size_t> upstream_list;
    const Counts *dom_count,*publication_count,*publish_count;const s::Owner* publication_source;const e::Owner* publication_upstream;
    std::size_t publication_upper,publication_maximum;
    std::optional<s::Owner>* publish_source;std::optional<e::Owner>* publish_upstream;
    a::State publish_outcome;const Diagnostics* publish_diagnostics;Usage publish_usage;Origin publish_origin;
    Provenance provenance_temporary;std::string run_temporary,time_temporary;Cursor publish_cursor;std::size_t publish_index;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_cleanup;std::optional<std::string_view> publish_optional,prepare_optional;
    core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<std::expected<std::string,core::Error>> rendered_guard;
    core::Sha256Digest digest_return;
    std::string_view parse_body;const Limits* parse_limits;Reader parse_reader;std::size_t parse_begin;std::string_view parse_code;
    Sink* emit_sink;const detail::View* emit_view;a::State emit_state;const Diagnostics* emit_diagnostics;
    std::string_view emit_source;const std::optional<std::string_view>* emit_upstream;Cursor emit_cursor;std::size_t emit_index;
    Cursor* cursor_constructor_this;const Diagnostics* cursor_constructor_values;Cursor* cursor_next_this;
    std::size_t cursor_end;std::string_view cursor_value,cursor_return;
    std::string_view safe_value,safe_code_parameter,safe_code_loop,safe_code_return;
    // Getter frames and actual by-value return cells remain live at callers.
    const a::ResolutionRecord *record_source_this,*record_state_this,*record_plan_this,*record_diagnostics_this,*record_error_this;
    const a::SourceBundle* source_getter_return; a::State state_getter_return;const scenario::ScenarioDigestedPlan* plan_getter_return;
    std::span<const a::SafeDiagnostic> diagnostics_getter_return;const private_error::Owner* upstream_getter_return;
    const private_error::Owner* error_getter_this;const core::Error* error_getter_return;
    const a::SourceBundle *source_input_this,*source_stock_this;a::Input source_input_return;std::size_t source_stock_return;
    const s::Owner *source_provenance_this,*source_owner_this,*source_usage_this,*source_canonical_this;
    const s::Provenance* source_provenance_return;const a::SourceBundle* source_owner_return;s::Usage source_usage_return;std::string_view source_canonical_return;
    const e::Owner *upstream_provenance_this,*upstream_error_this,*upstream_usage_this,*upstream_canonical_this;
    const e::Provenance* upstream_provenance_return;const core::Error* upstream_error_return;e::Usage upstream_usage_return;std::string_view upstream_canonical_return;
    // Renderer, forwarding frames and concrete append_object temporaries.
    Sink* sink_constructor_this;Json* sink_constructor_root;
    Sink* sink_container_this;std::string_view sink_container_key;bool sink_container_object;std::size_t sink_container_size;
    Json* sink_placed;Json sink_container_node;core::detail::JsonCleanup<Json> sink_container_cleanup;Frame sink_assigned;
    Sink* sink_object_this;std::string_view sink_object_key;
    Sink* sink_array_this;std::string_view sink_array_key;std::size_t sink_array_size;
    Sink* sink_end_this;Sink* sink_text_this;std::string_view sink_text_key,sink_text_value;Json sink_text_node;std::string sink_text_temporary;
    Sink* sink_octets_this;std::string_view sink_octets_key,sink_octets_value;std::string sink_encoded;
    Sink* sink_null_this;std::string_view sink_null_key;Json sink_null_node;
    Sink* sink_number_this;std::string_view sink_number_key;std::uint64_t sink_number_value;Decimal sink_decimal;
    Sink* sink_prefix_this;std::string_view sink_prefix_key;Frame* sink_prefix_frame;
    Sink* sink_insert_this;std::string_view sink_insert_key;Json* sink_insert_node;Frame* sink_insert_frame;Json* sink_insert_return;
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;std::string append_object_key;Json::iterator find_iterator;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;
    const Decimal* decimal_view_this;std::string_view decimal_view_return;
    Reader* reader_constructor_this;std::string_view reader_body;const Limits* reader_limits;
    const Reader* reader_next_this;char reader_next_char;std::size_t reader_next_at;
    const Reader* reader_position_this;const Reader* reader_slice_this;std::size_t reader_slice_start;std::string_view reader_slice_return;
    Reader* reader_expect_this;std::string_view reader_expected;
    Reader* reader_key_this;std::string_view reader_key;
    Reader* reader_object_this;Reader* reader_array_this;ParseFrame reader_assigned;
    Reader* reader_end_object_this;Reader* reader_end_array_this;
    Reader* reader_text_this;std::size_t reader_begin;unsigned char reader_character;std::string_view reader_text_result;
    Reader* reader_octets_this;std::string_view reader_octets_value;Reader* reader_number_this;
    Reader* reader_null_this;Reader* reader_finished_this;Reader* reader_node_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,hex_return,unhex_result,unhex_return;
    std::size_t hex_index,unhex_index;unsigned char hex_byte;std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value,name_return;std::size_t enum_index,name_index;
    const std::array<const char*,7>* enum_state_names;const std::array<const char*,2>* enum_domain_names;a::State name_state;p::Domain name_domain;
    std::size_t fixed_return,source_phase_return,upstream_phase_return,dom_return;
    std::size_t plus_left,plus_right,times_left,times_right,bound_value,bound_limit;bool require_value;Error require_error;
};
} // namespace

auto detail::fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
Owner::~Owner()=default;Owner::Owner(Owner&&)noexcept=default;auto Owner::operator=(Owner&&)noexcept->Owner& =default;
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::outcome()const noexcept->std::optional<a::State>{return state_?std::optional{state_->outcome}:std::nullopt;}
auto Owner::diagnostics()const noexcept->std::span<const std::string>{return state_?std::span<const std::string>{state_->diagnostics}:std::span<const std::string>{};}
auto Owner::source()const noexcept->const s::Owner*{return state_&&state_->source?&*state_->source:nullptr;}
auto Owner::upstream_error()const noexcept->const e::Owner*{return state_&&state_->upstream?&*state_->upstream:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::preflight(const detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{try{valid_limits(limits);require(input.record&&input.source&&input.upstream,Error::reference);const auto* source=input.record->source();require(source&&source->logical_bytes()&&!input.record->plan(),Error::reference);const auto diagnostics=diagnostic_view(input.record->diagnostics());const auto* error=input.record->upstream_error();local(input.record->state(),diagnostics,source->input(),error?error->source():nullptr);(void)primitive(input.view,diagnostics,limits);identity(input.view);(void)source_phase(limits,0,input.upper_fixed);return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::upstream_preflight(const detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{try{require(input.source&&*input.source,Error::child);source_match(input.view,**input.source);(void)upstream_phase(limits,input.record->state()==a::State::resolution_failed,**input.source,0,input.upper_fixed);return {};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::prepare(const detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{try{require(input.source&&*input.source&&input.upstream&&input.record,Error::reference);const auto& view=input.view;const auto diagnostics=diagnostic_view(input.record->diagnostics());const auto* source=&**input.source;const auto* upstream=*input.upstream?&**input.upstream:nullptr;source_match(view,*source);upstream_match(view,upstream);local(input.record->state(),diagnostics,source->source()->input(),upstream?upstream->error():nullptr);auto usage=primitive(view,diagnostics,limits);usage.source_preparation_bytes=source_phase(limits,0,input.upper_fixed);usage.upstream_preparation_bytes=upstream_phase(limits,upstream!=nullptr,*source,0,input.upper_fixed);Sink counter;emit(counter,view,input.record->state(),diagnostics,source->canonical(),upstream?std::optional{upstream->canonical()}:std::nullopt);usage=publication(usage,counter.count,*source,upstream,input.upper_fixed,limits);return Owner{publish(*input.source,*input.upstream,view,input.record->state(),diagnostics,counter.count,usage,Origin::session_selected)};}catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,parsed.diagnostics,limits);identity(parsed.view);usage.source_preparation_bytes=source_phase(limits,parsed.source.size()/2,0);std::optional<s::Owner> source;std::optional<e::Owner> upstream;
    {const auto bytes=unhex(parsed.source);auto decoded=s::Owner::decode(bytes,limits.source);if(!decoded)throw decoded.error()==s::Error::capacity?Error::capacity:Error::child;source=std::move(*decoded);}source_match(parsed.view,*source);
    usage.upstream_preparation_bytes=upstream_phase(limits,parsed.has_upstream,*source,parsed.upstream.size()/2,0);if(parsed.has_upstream){const auto bytes=unhex(parsed.upstream);auto decoded=e::Owner::decode(bytes,limits.upstream);if(!decoded)throw decoded.error()==e::Error::capacity?Error::capacity:Error::child;upstream=std::move(*decoded);}upstream_match(parsed.view,upstream?&*upstream:nullptr);local(parsed.state,parsed.diagnostics,source->source()->input(),upstream?upstream->error():nullptr);usage=publication(usage,parsed.wire,*source,upstream?&*upstream:nullptr,0,limits);auto state=publish(source,upstream,parsed.view,parsed.state,parsed.diagnostics,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}}
}
