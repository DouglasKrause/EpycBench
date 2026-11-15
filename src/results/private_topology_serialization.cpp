#include "results/private_topology_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::topology_serialization {
namespace {
using Json=nlohmann::json;
namespace b=topology_evidence;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array kinds{"ordinary","genesis"};
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array availabilities{"not_attempted","unavailable","failed","retained"};
constexpr std::array reporters{"controller","runner","results"};
constexpr std::array reasons{"not_supported","source_missing","access_denied"};
constexpr std::string_view topology_profile="epycbench.topology.v1";
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
    Json* root_;std::array<Frame,4> frames_{};std::size_t level_{};
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
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,4> frames_{};
    void node(){if(level_&&!frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};

struct DigestText {
    std::array<char,64> bytes{};
    explicit DigestText(const core::Sha256Digest& digest) {
        const auto raw=digest.bytes();
        for(std::size_t i=0;i<raw.size();++i){const auto value=std::to_integer<unsigned>(raw[i]);bytes[2*i]=digits[value>>4U];bytes[2*i+1]=digits[value&15U];}
    }
    auto view()const noexcept->std::string_view{return {bytes.data(),bytes.size()};}
};
struct FingerprintText {
    std::array<char,71> bytes{};
    explicit FingerprintText(const core::Sha256Digest& digest){const DigestText suffix{digest};std::copy_n("sha256:",7,bytes.begin());std::copy(suffix.bytes.begin(),suffix.bytes.end(),bytes.begin()+7);}
    auto view()const noexcept->std::string_view{return {bytes.data(),bytes.size()};}
};
void emit(Sink& sink,const detail::View& view,const b::Owner* topology,const e::Owner* child){
    sink.object();sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));
    if(child)sink.octets("failure_error_hex",child->canonical());else sink.null("failure_error_hex");
    if(view.failure){sink.object("failure_reference");if(view.failure->creation_ordinal)sink.number("creation_ordinal",*view.failure->creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",view.failure->sequence);sink.end();}else sink.null("failure_reference");
    sink.text("host_kind",name(view.kind,kinds));if(view.reason)sink.text("reason",name(*view.reason,reasons));else sink.null("reason");
    sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.text("state",name(view.slot_state,availabilities));sink.text("timestamp",view.time);
    if(topology){sink.object("topology");sink.octets("canonical_hex",topology->canonical());const DigestText digest{*topology->digest()};sink.text("canonical_sha256",digest.view());
        const FingerprintText effective{topology->fingerprints()->effective.digest()},physical{topology->fingerprints()->physical.digest()};
        sink.text("effective_fingerprint",effective.view());sink.text("physical_fingerprint",physical.view());sink.text("profile",topology_profile);sink.end();}else sink.null("topology");sink.end();
}
struct Parsed {detail::View view{};Counts wire;std::string_view topology,digest,effective,physical,child;bool has_topology{},has_child{};};
void fingerprint_syntax(std::string_view text){require(text.size()==71&&text.starts_with("sha256:"),Error::encoding);valid_hex(text.substr(7));}
auto parse(std::string_view body,const Limits& limits)->Parsed{
    Reader r{body,limits};Parsed out;r.object();r.key("domain");out.view.domain=enumerated<p::Domain>(r.text(),domains);r.key("entry_producer");out.view.reporter=enumerated<p::Producer>(r.text(),reporters);
    r.key("failure_error_hex");if(r.next('n'))r.null();else{out.has_child=true;out.child=r.octets();bound(out.child.size()/2,limits.child.canonical_bytes);}
    r.key("failure_reference");if(r.next('n'))r.null();else{r.object();r.key("creation_ordinal");const auto ordinal=r.optional();r.key("sequence");out.view.failure=FailureReference{r.number(),ordinal};r.end_object();}
    r.key("host_kind");out.view.kind=enumerated<Kind>(r.text(),kinds);r.key("reason");if(r.next('n'))r.null();else out.view.reason=enumerated<Reason>(r.text(),reasons);
    r.key("run_id");out.view.run=r.text();r.key("schema");require(r.text()==profile);r.key("sequence");out.view.sequence=r.number();r.key("state");out.view.slot_state=enumerated<SlotState>(r.text(),availabilities);r.key("timestamp");out.view.time=r.text();
    r.key("topology");if(r.next('n'))r.null();else{out.has_topology=true;r.object();r.key("canonical_hex");out.topology=r.octets();bound(out.topology.size()/2,limits.topology.bytes);bound(plus(sizeof(b::Owner),out.topology.size()/2),limits.topology.retained);
        r.key("canonical_sha256");out.digest=r.octets();require(out.digest.size()==64);r.key("effective_fingerprint");out.effective=r.text();fingerprint_syntax(out.effective);r.key("physical_fingerprint");out.physical=r.text();fingerprint_syntax(out.physical);r.key("profile");require(r.text()==topology_profile);r.end_object();}
    r.end_object();r.finished();out.wire=r.count;return out;
}
constexpr std::array topology_dimensions{&b::Limits::bytes,&b::Limits::tokens,&b::Limits::depth,&b::Limits::string_token,&b::Limits::scalar_token,&b::Limits::objects,&b::Limits::processing_units,&b::Limits::numa_nodes,&b::Limits::groups,&b::Limits::array_entries,&b::Limits::expanded_ranges,&b::Limits::ancestry,&b::Limits::derived_ids,&b::Limits::retained};
void valid_limits(const Limits& limits){
    for(auto n:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);
    require(limits.depth<=4&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
    const auto supported=b::supported_limits();for(auto field:topology_dimensions)require(limits.topology.*field<=supported.*field,Error::invalid_limits);
}
void local(const detail::View& view,bool topology,bool child){
    require(view.sequence!=0,Error::identity);(void)name(view.kind,kinds);(void)name(view.domain,domains);(void)name(view.reporter,reporters);(void)name(view.slot_state,availabilities);if(view.reason)(void)name(*view.reason,reasons);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    if(view.sequence==1)require(view.reporter==p::Producer::controller,Error::identity);
    require(topology==(view.slot_state==SlotState::retained));require(child==(view.slot_state==SlotState::failed));require(child==view.failure.has_value());require(view.reason.has_value()==(view.slot_state==SlotState::unavailable));
    if(view.failure){const auto& ref=*view.failure;require(ref.sequence!=0&&ref.sequence<=view.sequence,Error::reference);
        if(view.kind==Kind::genesis)require(ref.sequence==1&&ref.creation_ordinal.has_value(),Error::reference);
        if(ref.creation_ordinal)require(ref.sequence==1&&*ref.creation_ordinal<1024,Error::reference);
        if(view.kind==Kind::ordinary&&ref.sequence==view.sequence)require(!ref.creation_ordinal,Error::reference);}
}
auto primitive(const detail::View& view,const Limits& limits)->Usage{Usage out;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);return out;}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);}
void child_match(const detail::View& view,const e::Owner* child){
    require(view.failure.has_value()==(child!=nullptr),Error::child);if(!child)return;const auto* value=child->provenance();require(value&&child->error(),Error::child);const auto& ref=*view.failure;
    require(value->run==view.run&&value->domain==view.domain&&value->sequence==ref.sequence&&value->creation_ordinal==ref.creation_ordinal,Error::child);
    require(value->kind==(ref.creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure)&&value->association.has_value(),Error::child);const auto& fact=*value->association;
    require(!fact.process_sequence,Error::child);constexpr p::Target target{p::Category::topology};require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
    if(ref.sequence==view.sequence)require(value->time==view.time&&value->entry_producer==view.reporter,Error::child);
}
auto topology_phase(const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{const auto stock=plus(plus(plus(detail::fixed_stock(),upper),scratch),limits.topology.retained);bound(stock,limits.topology_preparation_bytes);return stock;}
auto error_phase(const Limits& limits,bool child,std::size_t decoded,std::size_t upper)->std::size_t{
    if(!child)return 0;
    const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();
    const auto allowance=std::max(limits.child.consumer_simultaneous_bytes,std::max(limits.child.retained_bytes,floor));const auto stock=plus(plus(plus(detail::fixed_stock(),upper),decoded),allowance);bound(stock,limits.error_preparation_bytes);return stock;
}
auto retain(std::string_view bytes,const Limits& limits,std::optional<epycbench::topology::TopologyFingerprints> expected={})->b::Owner{
    auto kept=b::retain(bytes,limits.topology,expected);core::detail::ExpectedErrorCleanup cleanup{kept};
    if(!kept)throw kept.error().code.value()=="results.topology.capacity"?Error::capacity:Error::source;
    return std::move(*kept);
}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
auto publication(Usage out,const Counts& count,const b::Owner* topology,const e::Owner* child,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.retained_bytes=plus(plus(sizeof(Owner)+sizeof(detail::State),out.provenance_bytes),out.canonical_bytes);std::size_t maximum=0;
    if(topology){out.topology=topology->usage();require(out.topology->retained>=sizeof(b::Owner),Error::source);out.retained_bytes=plus(out.retained_bytes,out.topology->retained-sizeof(b::Owner));maximum=topology->canonical().size();}
    if(child){out.child=child->usage();require(out.child->retained_bytes>=sizeof(e::Owner),Error::child);out.retained_bytes=plus(out.retained_bytes,out.child->retained_bytes-sizeof(e::Owner));maximum=child->canonical().size();}
    out.publication_bytes=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),out.retained_bytes),dom_stock(count)),times(maximum,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(std::optional<b::Owner>& topology,std::optional<e::Owner>& child,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->slot_state=view.slot_state;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.kind,view.sequence,view.failure,view.reason};state->topology=std::move(topology);state->child=std::move(child);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->topology?&*state->topology:nullptr,state->child?&*state->child:nullptr);
    auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}

// Named logical allowance including conservatively retired cells; not physical stack bytes.
// Named parse/unhex/publish locals may survive a nonelided move, but their returned
// prvalues initialize the caller/constructor cell directly. Provenance string
// prvalues are its aggregate members, not additional string-header cells.
struct OperationCells {
    const detail::ResolvedInput *preflight_input,*prepare_input;const Limits* preflight_limits;
    Limits prepare_limits,decode_limits;std::string_view decode_body;
    Usage preflight_usage,prepare_usage,decode_usage,primitive_out,publication_parameter,publication_return;
    std::expected<void,Error> preflight_return;
    const detail::View* prepare_view;const e::Owner* prepare_child;
    std::optional<b::Owner> prepare_topology,decode_topology;
    std::optional<e::Owner> decode_child;
    b::Owner retained_return;std::expected<b::Owner,core::Error> retained_result;
    core::detail::ExpectedErrorCleanup<decltype(retained_result)> retained_guard;
    std::string decoded_topology_body,decoded_child_body;
    std::expected<e::Owner,e::Error> decoded_child_result;
    Parsed decoded_parsed,parse_out;
    DigestText decoded_digest,emitted_digest;
    Sink prepare_counter,publish_sink;std::unique_ptr<detail::State> decoded_state,publish_state,constructor_state;
    Owner* constructor_this;Owner constructed_owner;std::expected<Owner,Error> owner_return;
    std::unexpected<Error> unexpected_return;Error caught;const std::length_error* caught_length;
    const Limits *valid_limits_parameter,*primitive_limits,*topology_phase_limits,*error_phase_limits,*retain_limits,*publication_limits;
    std::array<std::size_t,3> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;
    std::string limits_string;Json::array_t limits_array;
    const detail::View *local_view,*primitive_view,*identity_view,*child_view,*emit_view,*publish_view;
    bool local_topology,local_child;const FailureReference *local_reference,*child_reference;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<decltype(identity_run)> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<decltype(identity_time)> identity_time_guard;
    const e::Owner* child_parameter;const e::Provenance* child_provenance;const e::Association* child_fact;p::Target child_target;
    std::vector<p::Target>::const_iterator child_find_return;
    std::size_t topology_scratch,topology_upper,topology_stock,error_decoded,error_upper,error_floor,error_allowance,error_stock;
    bool error_present;std::size_t fixed_return,error_phase_return,topology_phase_return;
    std::string_view retain_bytes;b::Limits retain_limits_copy;
    std::optional<epycbench::topology::TopologyFingerprints> retain_expected,retain_expected_copy,prepare_expected,decode_default_expected;
    const Counts *publication_count,*dom_count,*publish_count;
    const b::Owner* publication_topology;const e::Owner* publication_child;
    std::size_t publication_upper,publication_maximum,dom_return;
    std::optional<b::Owner>* publish_topology;std::optional<e::Owner>* publish_child;
    Usage publish_usage;Origin publish_origin;Provenance provenance_temporary;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;
    core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_guard;
    core::Sha256Digest digest_return,initial_digest;std::string_view initial_digest_input;detail::State* state_constructor_this;
    const b::Owner *topology_usage_this,*topology_canonical_this,*topology_digest_this,*topology_fingerprints_this;
    b::Usage topology_usage_return;std::string_view topology_canonical_return;
    const core::Sha256Digest* topology_digest_return;const epycbench::topology::TopologyFingerprints* topology_fingerprints_return;
    const epycbench::topology::PhysicalTopologyFingerprint* physical_this;
    const epycbench::topology::EffectiveTopologyFingerprint* effective_this;
    const core::Sha256Digest *physical_return,*effective_return;
    const e::Owner *child_usage_this,*child_canonical_this,*child_provenance_this,*child_error_this;
    e::Usage child_usage_return;std::string_view child_canonical_return;const e::Provenance* child_provenance_return;const core::Error* child_error_return;
    DigestText* digest_text_this;const core::Sha256Digest* digest_text_parameter;
    std::array<std::byte,32> digest_raw;std::size_t digest_index;unsigned digest_value;
    const core::Sha256Digest* digest_bytes_this;const std::array<std::byte,32>* digest_bytes_return;
    const DigestText* digest_view_this;std::string_view digest_view_return;
    Sink* emit_sink;const b::Owner* emit_topology;const e::Owner* emit_child;
    Sink* sink_this;Json* sink_root;std::string_view sink_key,sink_value;std::size_t sink_size;bool sink_object;
    Frame* sink_frame;Frame sink_assigned_frame;Json* sink_placed;Json sink_node;core::detail::JsonCleanup<Json> sink_cleanup;
    std::string sink_encoded,sink_text_temporary;Decimal sink_decimal;Json* sink_insert_return;
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;
    std::string append_object_key;Json::iterator insert_find_result;
    Sink* object_this;std::string_view object_key;
    Sink* array_this;std::string_view array_key;std::size_t array_size;
    Sink* text_this;std::string_view text_key,text_value;
    Sink* octets_this;std::string_view octets_key,octets_value;
    Sink* null_this;std::string_view null_key;
    Sink* number_this;std::string_view number_key;std::uint64_t number_value;
    Sink* prefix_this;std::string_view prefix_key;Sink* insert_this;std::string_view insert_key;Json* insert_node;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;
    const Decimal* decimal_view_this;std::string_view decimal_view_return;
    std::string_view parse_body;const Limits* parse_limits;Reader parse_reader;std::optional<std::uint64_t> parse_ordinal;FailureReference parse_failure;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;char reader_next_char;
    std::size_t reader_next_at,reader_slice_start,reader_text_begin;std::string_view reader_expected,reader_key,reader_text_return,reader_text_result,reader_octets_value,reader_octets_return;
    unsigned char reader_character;ParseFrame reader_assigned_frame;
    Reader *reader_number_this,*reader_optional_this,*reader_octets_this,*reader_node_this,*reader_object_this,*reader_array_this,*reader_text_this,*reader_expect_this,*reader_key_this,*reader_null_this,*reader_finished_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,unhex_result;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value;std::size_t enum_index,name_index;
    const std::array<const char*,2>* enum_two_names;const std::array<const char*,3>* enum_three_names;const std::array<const char*,4>* enum_four_names;
    Kind name_kind;p::Domain name_domain;SlotState name_slot_state;p::Producer name_reporter;Reason name_reason;
    std::string_view name_return,reader_slice_return;
    b::Limits supported;std::size_t b::Limits::*dimension;
    FingerprintText emitted_effective,emitted_physical,decoded_effective,decoded_physical;
    FingerprintText* fingerprint_this;const core::Sha256Digest* fingerprint_parameter;
    DigestText fingerprint_suffix;const FingerprintText* fingerprint_view_this;std::string_view fingerprint_view_return;
    std::string_view fingerprint_syntax_parameter;
    std::array<char,71>::iterator fingerprint_destination,fingerprint_copy_return,fingerprint_copy_n_return;
    std::array<char,64>::const_iterator fingerprint_source_begin,fingerprint_source_end;
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
auto Owner::topology()const noexcept->const b::Owner*{return state_&&state_->topology?&*state_->topology:nullptr;}
auto Owner::failure_error()const noexcept->const e::Owner*{return state_&&state_->child?&*state_->child:nullptr;}
auto Owner::slot_state()const noexcept->std::optional<SlotState>{return state_?std::optional{state_->slot_state}:std::nullopt;}
auto Owner::reason()const noexcept->std::optional<Reason>{return state_?state_->provenance.reason:std::nullopt;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(const detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{valid_limits(limits);(void)primitive(input.view,limits);local(input.view,input.topology!=nullptr,input.view.failure.has_value());identity(input.view);
        if(input.topology){require(input.topology->digest()&&input.topology->fingerprints(),Error::source);(void)topology_phase(limits,0,input.upper_fixed);}
        (void)error_phase(limits,input.view.failure.has_value(),0,input.upper_fixed);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(const detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);require(input.child,Error::source);const auto& view=input.view;auto usage=primitive(view,limits);const auto* child=*input.child?&**input.child:nullptr;local(view,input.topology!=nullptr,child!=nullptr);identity(view);child_match(view,child);
        std::optional<b::Owner> topology;if(input.topology){usage.topology_preparation_bytes=topology_phase(limits,0,input.upper_fixed);require(input.topology->fingerprints()&&input.topology->digest(),Error::source);topology=retain(input.topology->canonical(),limits,*input.topology->fingerprints());require(topology->canonical()==input.topology->canonical()&&*topology->digest()==*input.topology->digest(),Error::source);}
        usage.error_preparation_bytes=error_phase(limits,child!=nullptr,0,input.upper_fixed);Sink count;emit(count,view,topology?&*topology:nullptr,child);usage=publication(usage,count.count,topology?&*topology:nullptr,child,input.upper_fixed,limits);
        return Owner{publish(topology,*input.child,view,count.count,usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits);local(parsed.view,parsed.has_topology,parsed.has_child);identity(parsed.view);
        std::optional<b::Owner> topology;std::optional<e::Owner> child;
        if(parsed.has_topology){usage.topology_decode_bytes=plus(detail::fixed_stock(),parsed.topology.size()/2);bound(usage.topology_decode_bytes,limits.topology_decode_bytes);
            usage.topology_preparation_bytes=topology_phase(limits,parsed.topology.size()/2,0);
            const auto canonical=unhex(parsed.topology);topology=retain(canonical,limits);const DigestText digest{*topology->digest()};const FingerprintText effective{topology->fingerprints()->effective.digest()},physical{topology->fingerprints()->physical.digest()};
            require(topology->canonical()==canonical&&digest.view()==parsed.digest&&effective.view()==parsed.effective&&physical.view()==parsed.physical,Error::encoding);
        }
        usage.error_preparation_bytes=error_phase(limits,parsed.has_child,parsed.child.size()/2,0);
        if(parsed.has_child){const auto canonical=unhex(parsed.child);auto decoded=e::Owner::decode(canonical,limits.child);require(decoded.has_value(),Error::child);child=std::move(*decoded);}
        child_match(parsed.view,child?&*child:nullptr);usage=publication(usage,parsed.wire,topology?&*topology:nullptr,child?&*child:nullptr,0,limits);
        auto state=publish(topology,child,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::topology_serialization
