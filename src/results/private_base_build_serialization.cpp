#include "results/private_base_build_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::base_build_serialization {
namespace {
using Json=nlohmann::json;
namespace b=build_manifest;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array kinds{"ordinary","genesis"};
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array availabilities{"not_attempted","not_obtained","available"};
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
void emit(Sink& sink,const detail::View& view,const b::Owner* manifest,const e::Owner* child){
    sink.object();if(child)sink.octets("acquisition_error_hex",child->canonical());else sink.null("acquisition_error_hex");
    if(view.acquisition){sink.object("acquisition_reference");if(view.acquisition->creation_ordinal)sink.number("creation_ordinal",*view.acquisition->creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",view.acquisition->sequence);sink.end();}else sink.null("acquisition_reference");
    sink.text("base_build_kind",name(view.kind,kinds));sink.text("disposition",name(view.disposition,availabilities));sink.text("domain",name(view.domain,domains));sink.text("entry_producer","controller");
    if(manifest){sink.object("manifest");sink.octets("canonical_hex",manifest->canonical());const DigestText digest{manifest->digest()->value()};sink.text("digest",digest.view());sink.text("profile",b::profile);sink.end();}else sink.null("manifest");
    sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.text("timestamp",view.time);sink.end();
}
struct Parsed {detail::View view{};Counts wire;std::string_view manifest,digest,child;bool has_manifest{},has_child{};};
auto parse(std::string_view body,const Limits& limits)->Parsed{
    Reader r{body,limits};Parsed out;r.object();r.key("acquisition_error_hex");if(r.next('n'))r.null();else{out.has_child=true;out.child=r.octets();bound(out.child.size()/2,limits.child.canonical_bytes);}
    r.key("acquisition_reference");if(r.next('n'))r.null();else{r.object();r.key("creation_ordinal");const auto ordinal=r.optional();r.key("sequence");out.view.acquisition=Acquisition{r.number(),ordinal};r.end_object();}
    r.key("base_build_kind");out.view.kind=enumerated<Kind>(r.text(),kinds);r.key("disposition");out.view.disposition=enumerated<p::BuildState>(r.text(),availabilities);r.key("domain");out.view.domain=enumerated<p::Domain>(r.text(),domains);r.key("entry_producer");require(r.text()=="controller");
    r.key("manifest");if(r.next('n'))r.null();else{out.has_manifest=true;r.object();r.key("canonical_hex");out.manifest=r.octets();bound(out.manifest.size()/2,limits.manifest.canonical_bytes);r.key("digest");out.digest=r.octets();require(out.digest.size()==64);r.key("profile");require(r.text()==b::profile);r.end_object();}
    r.key("run_id");out.view.run=r.text();r.key("schema");require(r.text()==profile);r.key("sequence");out.view.sequence=r.number();r.key("timestamp");out.view.time=r.text();r.end_object();r.finished();out.wire=r.count;return out;
}
void valid_limits(const Limits& limits){
    for(auto n:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes,limits.manifest.features,limits.manifest.field_bytes,limits.manifest.string_bytes,limits.manifest.preimage_bytes,limits.manifest.canonical_bytes,limits.manifest.total_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);
    require(limits.depth<=4&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
}
void local(const detail::View& view,bool manifest,bool child){
    require(view.sequence!=0,Error::identity);(void)name(view.kind,kinds);(void)name(view.domain,domains);(void)name(view.disposition,availabilities);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    require(manifest==(view.disposition==p::BuildState::available));require(child==(view.disposition==p::BuildState::not_obtained));require(child==view.acquisition.has_value());
    if(view.acquisition){const auto& ref=*view.acquisition;require(ref.sequence!=0&&ref.sequence<=view.sequence,Error::reference);if(view.kind==Kind::genesis)require(ref.sequence==1&&ref.creation_ordinal.has_value(),Error::reference);else require(!ref.creation_ordinal,Error::reference);if(ref.creation_ordinal)require(*ref.creation_ordinal<1024,Error::reference);}
}
auto primitive(const detail::View& view,const Limits& limits)->Usage{Usage out;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);return out;}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);}
void child_match(const detail::View& view,const e::Owner* child){
    require(view.acquisition.has_value()==(child!=nullptr),Error::child);if(!child)return;const auto* value=child->provenance();require(value&&child->error(),Error::child);const auto& ref=*view.acquisition;
    require(value->run==view.run&&value->domain==view.domain&&value->sequence==ref.sequence&&value->creation_ordinal==ref.creation_ordinal,Error::child);
    require(value->kind==(ref.creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure)&&value->association.has_value(),Error::child);const auto& fact=*value->association;
    require(fact.producer==p::Producer::controller&&!fact.process_sequence,Error::child);constexpr p::Target target{p::Category::base_build,p::Selector::base_build_acquisition};require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
    if(ref.sequence==view.sequence)require(value->time==view.time&&value->entry_producer==p::Producer::controller,Error::child);
}
constexpr std::array feature_names{"linux","cxx23","testing","asan_ubsan","tsan","native_reference"};
struct Inner {
    std::array<std::string_view,18> fields{};std::array<std::size_t,18> lengths{};
    std::array<b::Feature,6> features{};std::size_t count{},bytes{};bool dirty{};
};
// This reader borrows already bounded canonical bytes. It never builds a DOM.
class ManifestReader {
  public:
    ManifestReader(std::string_view body,const b::Limits& limits):body_(body),limits_(limits){}
    void expect(std::string_view value){require(body_.substr(at_,value.size())==value,Error::encoding);at_+=value.size();}
    auto next(char value)const noexcept->bool{return at_<body_.size()&&body_[at_]==value;}
    auto text(std::size_t& length)->std::string_view{
        expect("\"");const auto begin=at_;length=0;
        while(at_<body_.size()&&body_[at_]!='"'){
            const auto ch=static_cast<unsigned char>(body_[at_]);require(ch>=0x20,Error::encoding);
            if(ch=='\\'){
                ++at_;require(at_<body_.size(),Error::encoding);const char escaped=body_[at_++];
                if(escaped=='u'){expect("00");require(at_+2<=body_.size(),Error::encoding);const auto value=nibble(body_[at_])*16U+nibble(body_[at_+1]);at_+=2;require(value>0&&value<32&&value!=8&&value!=9&&value!=10&&value!=12&&value!=13,Error::encoding);}
                else require(escaped=='"'||escaped=='\\'||escaped=='b'||escaped=='f'||escaped=='n'||escaped=='r'||escaped=='t',Error::encoding);
                length=plus(length,1);
            }else{
                std::size_t width=1;
                if(ch>=0x80){width=ch>=0xc2&&ch<=0xdf?2U:ch>=0xe0&&ch<=0xef?3U:ch>=0xf0&&ch<=0xf4?4U:0U;require(width&&width<=body_.size()-at_,Error::encoding);for(std::size_t i=1;i<width;++i){const auto continuation=static_cast<unsigned char>(body_[at_+i]);require(continuation>=0x80&&continuation<=0xbf,Error::encoding);}const auto second=static_cast<unsigned char>(body_[at_+1]);require(!(ch==0xe0&&second<0xa0)&&!(ch==0xed&&second>=0xa0)&&!(ch==0xf0&&second<0x90)&&!(ch==0xf4&&second>=0x90),Error::encoding);}
                at_+=width;length=plus(length,width);
            }
            bound(length,limits_.field_bytes);
        }
        require(at_<body_.size(),Error::encoding);const auto value=body_.substr(begin,at_-begin);++at_;return value;
    }
    void field(Inner& out,std::size_t index){out.fields[index]=text(out.lengths[index]);out.bytes=plus(out.bytes,out.lengths[index]);bound(out.bytes,limits_.string_bytes);}
    void finished(){require(at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const b::Limits& limits_;std::size_t at_{};
};
auto parse_manifest(std::string_view body,const b::Limits& limits)->Inner{
    ManifestReader r{body,limits};Inner out;r.expect("{\"build_identity\":");r.field(out,17);r.expect(",\"compiler\":{\"cxx_standard\":");r.field(out,5);r.expect(",\"flags\":");r.field(out,6);r.expect(",\"id\":");r.field(out,2);r.expect(",\"target\":");r.field(out,4);r.expect(",\"version\":");r.field(out,3);
    r.expect("},\"dependencies\":{\"CLI11\":");r.field(out,13);r.expect(",\"Catch2\":");r.field(out,16);r.expect(",\"fmt\":");r.field(out,15);r.expect(",\"hwloc\":");r.field(out,10);r.expect(",\"libfyaml\":");r.field(out,12);r.expect(",\"libnuma\":");r.field(out,11);r.expect(",\"nlohmann-json\":");r.field(out,14);r.expect("},\"enabled_features\":[");
    while(!r.next(']')){if(out.count)r.expect(",");bound(plus(out.count,1),std::min(limits.features,out.features.size()));std::size_t size{};const auto token=r.text(size);const auto feature=enumerated<b::Feature>(token,feature_names);require(out.count==0||feature>out.features[out.count-1]);out.features[out.count++]=feature;}
    r.expect("],\"git\":{\"dirty\":");if(r.next('t')){r.expect("true");out.dirty=true;}else r.expect("false");r.expect(",\"revision\":");r.field(out,1);r.expect("},\"linker\":{\"id\":");r.field(out,7);r.expect(",\"version\":");r.field(out,8);r.expect("},\"preset\":");r.field(out,9);r.expect(",\"project_version\":");r.field(out,0);r.expect("}");r.finished();return out;
}
auto restore_text(std::string_view text,std::size_t size)->std::string{
    std::string out;out.reserve(size);
    for(std::size_t at=0;at<text.size();++at){char ch=text[at];if(ch=='\\'){ch=text[++at];switch(ch){case 'b':ch='\b';break;case 'f':ch='\f';break;case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;case 'u':at+=3;ch=static_cast<char>(nibble(text[at])*16U+nibble(text[at+1]));++at;break;default:break;}}out.push_back(ch);}require(out.size()==size,Error::encoding);return out;
}
struct Scratch {
    std::array<std::string,18> fields;
    explicit Scratch(const Inner& inner){for(std::size_t i=0;i<fields.size();++i)fields[i]=restore_text(inner.fields[i],inner.lengths[i]);}
    auto input(const Inner& inner)const->b::Input{return {fields[0],{fields[1],inner.dirty},{fields[2],fields[3],fields[4],fields[5],fields[6]},{fields[7],fields[8]},fields[9],std::span{inner.features}.first(inner.count),{fields[10],fields[11],fields[12],fields[13],fields[14],fields[15],fields[16]},fields[17]};}
};
auto manifest_phase(const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{const auto stock=plus(plus(plus(detail::fixed_stock(),upper),scratch),limits.manifest.total_bytes);bound(stock,limits.manifest_preparation_bytes);return stock;}
auto error_phase(const Limits& limits,bool child,std::size_t decoded,std::size_t upper)->std::size_t{
    if(!child)return 0;
    const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();
    const auto allowance=std::max(limits.child.consumer_simultaneous_bytes,std::max(limits.child.retained_bytes,floor));const auto stock=plus(plus(plus(detail::fixed_stock(),upper),decoded),allowance);bound(stock,limits.error_preparation_bytes);return stock;
}
auto retain(const b::Input& input,const Limits& limits)->b::Owner{auto kept=b::retain(input,limits.manifest);core::detail::ExpectedErrorCleanup cleanup{kept};require(kept.has_value(),Error::source);return std::move(*kept);}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
auto publication(Usage out,const Counts& count,const b::Owner* manifest,const e::Owner* child,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.retained_bytes=plus(plus(sizeof(Owner)+sizeof(detail::State),out.provenance_bytes),out.canonical_bytes);std::size_t maximum=0;
    if(manifest){out.manifest=manifest->usage();require(out.manifest->total_bytes>=sizeof(b::Owner),Error::source);out.retained_bytes=plus(out.retained_bytes,out.manifest->total_bytes-sizeof(b::Owner));maximum=manifest->canonical().size();}
    if(child){out.child=child->usage();require(out.child->retained_bytes>=sizeof(e::Owner),Error::child);out.retained_bytes=plus(out.retained_bytes,out.child->retained_bytes-sizeof(e::Owner));maximum=child->canonical().size();}
    out.publication_bytes=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),out.retained_bytes),dom_stock(count)),times(maximum,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(std::optional<b::Owner>& manifest,std::optional<e::Owner>& child,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->disposition=view.disposition;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.kind,view.sequence,view.acquisition};state->manifest=std::move(manifest);state->child=std::move(child);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->manifest?&*state->manifest:nullptr,state->child?&*state->child:nullptr);
    auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}

// Logical named allowance; includes retired cells conservatively, not stack bytes.
struct OperationCells {
    const detail::ResolvedInput *preflight_input,*prepare_input;const Limits* preflight_limits;
    Limits prepare_limits,decode_limits;std::string_view decode_body;
    Usage preflight_usage,prepare_usage,decode_usage,primitive_out,publication_parameter,publication_return;
    std::expected<void,Error> preflight_return;
    const detail::View* prepare_view;const e::Owner* prepare_child;
    std::optional<b::Owner> prepare_manifest,decode_manifest;
    std::optional<e::Owner> decode_child;
    std::optional<b::Input> prepare_input_return;
    b::Owner retained_return;std::expected<b::Owner,core::Error> retained_result;
    core::detail::ExpectedErrorCleanup<decltype(retained_result)> retained_guard;
    std::string decoded_manifest_body,decoded_child_body;
    std::expected<e::Owner,e::Error> decoded_child_result;
    Parsed decoded_parsed,parse_out,parse_return;Inner decoded_inner,inner_out,inner_return;
    Scratch decoded_scratch;b::Input decoded_input;
    DigestText decoded_digest,emitted_digest;
    Sink prepare_counter,publish_sink;std::unique_ptr<detail::State> decoded_state,publish_state,publish_return,constructor_state;
    Owner* constructor_this;Owner constructed_owner;std::expected<Owner,Error> owner_return;
    std::unexpected<Error> unexpected_return;Error caught;
    const Limits *valid_limits_parameter,*primitive_limits,*manifest_phase_limits,*error_phase_limits,*retain_limits,*publication_limits;
    std::array<std::size_t,9> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;
    std::string limits_string;Json::array_t limits_array;
    const detail::View *local_view,*primitive_view,*identity_view,*child_view,*emit_view,*publish_view;
    bool local_manifest,local_child;const Acquisition *local_reference,*child_reference;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<decltype(identity_run)> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<decltype(identity_time)> identity_time_guard;
    const e::Owner* child_parameter;const e::Provenance* child_provenance;const e::Association* child_fact;p::Target child_target;
    std::vector<p::Target>::const_iterator child_find_return;
    std::size_t manifest_scratch,manifest_upper,manifest_stock,error_decoded,error_upper,error_floor,error_allowance,error_stock;
    bool error_present;std::size_t fixed_return,error_phase_return,manifest_phase_return;
    const b::Input* retain_input;b::Input factory_input;b::Limits retain_limits_copy;
    const Counts *publication_count,*dom_count,*publish_count;
    const b::Owner* publication_manifest;const e::Owner* publication_child;
    std::size_t publication_upper,publication_maximum,dom_return;
    std::optional<b::Owner>* publish_manifest;std::optional<e::Owner>* publish_child;
    Usage publish_usage;Origin publish_origin;Provenance provenance_temporary;
    std::string run_temporary,time_temporary;Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;
    core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_guard;
    core::Sha256Digest digest_return;
    const b::Owner *manifest_input_this,*manifest_usage_this,*manifest_canonical_this,*manifest_digest_this;
    std::optional<b::Input> manifest_input_return;b::Usage manifest_usage_return;std::string_view manifest_canonical_return;
    const b::ManifestDigest* manifest_digest_return;const b::ManifestDigest* digest_value_this;const core::Sha256Digest* digest_value_return;
    const e::Owner *child_usage_this,*child_canonical_this,*child_provenance_this,*child_error_this;
    e::Usage child_usage_return;std::string_view child_canonical_return;const e::Provenance* child_provenance_return;const core::Error* child_error_return;
    DigestText* digest_text_this;const core::Sha256Digest* digest_text_parameter;
    std::array<std::byte,32> digest_raw;std::size_t digest_index;unsigned digest_value;
    const core::Sha256Digest* digest_bytes_this;const std::array<std::byte,32>* digest_bytes_return;
    const DigestText* digest_view_this;std::string_view digest_view_return;
    Sink* emit_sink;const b::Owner* emit_manifest;const e::Owner* emit_child;
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
    std::string_view parse_body;const Limits* parse_limits;Reader parse_reader;std::optional<std::uint64_t> parse_ordinal;Acquisition parse_acquisition;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;char reader_next_char;
    std::size_t reader_next_at,reader_slice_start,reader_text_begin;std::string_view reader_expected,reader_key,reader_text_return,reader_text_result,reader_octets_value,reader_octets_return;
    unsigned char reader_character;ParseFrame reader_assigned_frame;
    Reader *reader_number_this,*reader_optional_this,*reader_octets_this,*reader_node_this,*reader_object_this,*reader_array_this,*reader_text_this,*reader_expect_this,*reader_key_this,*reader_null_this,*reader_finished_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,unhex_result,unhex_return;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value;std::size_t enum_index,name_index;
    const std::array<const char*,2>* enum_two_names;const std::array<const char*,3>* enum_three_names;const std::array<const char*,6>* enum_feature_names;
    Kind name_kind;p::Domain name_domain;p::BuildState name_disposition;b::Feature enum_feature;
    std::string_view name_return,reader_slice_return;
    std::string_view inner_body;const b::Limits* inner_limits;ManifestReader inner_reader;
    std::size_t inner_feature_size,inner_feature_capacity;std::string_view inner_feature_token;b::Feature inner_feature;
    ManifestReader* manifest_reader_this;std::string_view manifest_reader_body;const b::Limits* manifest_reader_limits;
    ManifestReader *manifest_expect_this,*manifest_next_this,*manifest_text_this,*manifest_field_this,*manifest_finished_this;
    std::string_view manifest_expected;char manifest_next_value;std::size_t* manifest_length;
    std::size_t manifest_begin,manifest_width,manifest_utf8_index;unsigned char manifest_character,manifest_continuation,manifest_second;char manifest_escaped;unsigned manifest_control;
    std::string_view manifest_text_return,manifest_text_value;Inner* manifest_field_output;std::size_t manifest_field_index;
    Scratch* scratch_this;const Inner* scratch_inner;std::size_t scratch_index;
    const Scratch* scratch_input_this;const Inner* scratch_input_inner;b::Input scratch_input_return;
    std::span<const b::Feature,6> scratch_span;std::span<const b::Feature> scratch_span_return;
    std::string_view restore_text_parameter;std::size_t restore_size,restore_at;std::string restore_out,restore_return;char restore_character;
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
auto Owner::manifest()const noexcept->const b::Owner*{return state_&&state_->manifest?&*state_->manifest:nullptr;}
auto Owner::acquisition_error()const noexcept->const e::Owner*{return state_&&state_->child?&*state_->child:nullptr;}
auto Owner::disposition()const noexcept->std::optional<p::BuildState>{return state_?std::optional{state_->disposition}:std::nullopt;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(const detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{valid_limits(limits);(void)primitive(input.view,limits);local(input.view,input.manifest!=nullptr,input.view.acquisition.has_value());identity(input.view);
        if(input.manifest){require(input.manifest->digest(),Error::source);(void)manifest_phase(limits,0,input.upper_fixed);}
        (void)error_phase(limits,input.view.acquisition.has_value(),0,input.upper_fixed);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(const detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);require(input.child,Error::source);const auto& view=input.view;auto usage=primitive(view,limits);const auto* child=*input.child?&**input.child:nullptr;local(view,input.manifest!=nullptr,child!=nullptr);identity(view);child_match(view,child);
        std::optional<b::Owner> manifest;if(input.manifest){usage.manifest_preparation_bytes=manifest_phase(limits,0,input.upper_fixed);const auto source=input.manifest->input();require(source.has_value(),Error::source);manifest=retain(*source,limits);require(manifest->canonical()==input.manifest->canonical()&&*manifest->digest()==*input.manifest->digest(),Error::source);}
        usage.error_preparation_bytes=error_phase(limits,child!=nullptr,0,input.upper_fixed);Sink count;emit(count,view,manifest?&*manifest:nullptr,child);usage=publication(usage,count.count,manifest?&*manifest:nullptr,child,input.upper_fixed,limits);
        return Owner{publish(manifest,*input.child,view,count.count,usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits);local(parsed.view,parsed.has_manifest,parsed.has_child);identity(parsed.view);
        std::optional<b::Owner> manifest;std::optional<e::Owner> child;
        if(parsed.has_manifest){usage.manifest_decode_bytes=plus(detail::fixed_stock(),parsed.manifest.size()/2);bound(usage.manifest_decode_bytes,limits.manifest_decode_bytes);
            const auto canonical=unhex(parsed.manifest);const auto inner=parse_manifest(canonical,limits.manifest);usage.manifest_preparation_bytes=manifest_phase(limits,plus(canonical.size(),inner.bytes),0);
            const Scratch scratch{inner};const auto input=scratch.input(inner);manifest=retain(input,limits);const DigestText digest{manifest->digest()->value()};require(manifest->canonical()==canonical&&digest.view()==parsed.digest,Error::encoding);
        }
        usage.error_preparation_bytes=error_phase(limits,parsed.has_child,parsed.child.size()/2,0);
        if(parsed.has_child){const auto canonical=unhex(parsed.child);auto decoded=e::Owner::decode(canonical,limits.child);require(decoded.has_value(),Error::child);child=std::move(*decoded);}
        child_match(parsed.view,child?&*child:nullptr);usage=publication(usage,parsed.wire,manifest?&*manifest:nullptr,child?&*child:nullptr,0,limits);
        auto state=publish(manifest,child,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::base_build_serialization
