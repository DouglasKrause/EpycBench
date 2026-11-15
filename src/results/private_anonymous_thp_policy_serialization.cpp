#include "results/private_anonymous_thp_policy_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::anonymous_thp_policy_serialization {
namespace {
using Json=nlohmann::json;
namespace h=anonymous_thp_policy_owner;
namespace t=topology_serialization;
namespace e=error_serialization;
namespace p=process_subject;
constexpr std::array kinds{"ordinary","genesis"};
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array availabilities{"not_attempted","unavailable","failed","retained"};
constexpr std::array reporters{"controller","runner","results"};
constexpr std::array reasons{"not_supported","source_missing","access_denied"};
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
    Json* root_;std::array<Frame,6> frames_{};std::size_t level_{};
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
    void null(){node();expect("null");}
    auto optional()->std::optional<std::uint64_t>{if(next('n')){null();return {};}return number();}
    void finished(){require(level_==0&&at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,6> frames_{};
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
constexpr std::array enumeration_states{"not_attempted","complete","unavailable","failed"};
constexpr std::array states{"not_attempted","available","unavailable","failed"};
constexpr std::array modes{"always","madvise","never","inherit"};
constexpr std::array observation_reasons{"not_supported","source_missing","access_denied","read_failed","malformed_source","unsupported_value"};
constexpr std::array producers{"controller","runner"};
struct SourceView {
    std::string_view canonical,run,digest,effective,physical;std::uint64_t ordinal{};h::Producer producer{};
    std::size_t rows{},errors{},stock{},typed_bytes{36};bool bound{};
};
struct Parsed {detail::View view{};Counts wire;std::string_view source,digest,topology,preparation_error;detail::WireError enumeration;std::size_t rows{},errors{};bool retained{},error_rows{},error_enumeration{};};
struct RepeatedError {const FailureReference* reference{};const e::Owner* error{};};
auto failure_sequence(std::string_view value,std::string_view run)->std::uint64_t{
    require(value.size()>run.size()+1&&value.starts_with(run)&&value[run.size()]==':',Error::reference);
    const auto result=integer(value.substr(run.size()+1));require(result!=0,Error::reference);return result;
}
void fingerprint_syntax(std::string_view text){require(text.size()==71&&text.starts_with("sha256:"),Error::encoding);valid_hex(text.substr(7));}
void size_syntax(std::uint64_t size){require(size>=1024&&(size&(size-1))==0,Error::source);}
void observation_reason(Reader& reader,detail::ObservationView& out){reader.key("reason");if(reader.next('n'))reader.null();else out.reason=enumerated<h::Reason>(reader.text(),observation_reasons);}
void observation_union(const detail::ObservationView& out,unsigned state,bool available){
    require(state<4);if(state==0){require(out.time.empty()&&!out.reason&&!out.failure);return;}
    require(!out.time.empty());if(state==1){require(available&&!out.reason&&!out.failure);return;}
    require(out.reason.has_value());const auto reason=static_cast<unsigned>(*out.reason);
    require((state==2&&reason<3&&!out.failure)||(state==3&&reason>=3&&out.failure));
}
auto parse_source(std::string_view bytes,const Limits& limits,detail::Staging& staging)->SourceView{
    Limits inner{};inner.canonical_bytes=limits.source.canonical_bytes;inner.text_bytes=limits.source.canonical_bytes;inner.nodes=393;inner.depth=4;
    Reader reader{bytes,inner};SourceView out;out.canonical=bytes;out.stock=plus(sizeof(h::Owner),plus(36,bytes.size()));
    // The canonical run follows enumeration. Validate its reference prefix after reading run_id.
    std::string_view enumeration_failure;reader.object();reader.key("enumeration");reader.object();reader.key("failure_event");if(reader.next('n'))reader.null();else enumeration_failure=reader.text();
    reader.key("observed_at");staging.enumeration={};if(reader.next('n'))reader.null();else{staging.enumeration.time=reader.text();require(staging.enumeration.time.size()==30,Error::identity);}
    observation_reason(reader,staging.enumeration);reader.key("state");staging.enumeration.state=enumerated<h::EnumerationState>(reader.text(),enumeration_states);reader.end_object();
    reader.key("ordinal");out.ordinal=reader.number();reader.key("producer");out.producer=enumerated<h::Producer>(reader.text(),producers);reader.key("run_id");out.run=reader.text();require(out.run.size()==36,Error::identity);
    if(!enumeration_failure.empty()){staging.enumeration.failure=failure_sequence(enumeration_failure,out.run);out.stock=plus(out.stock,enumeration_failure.size());++out.errors;}
    observation_union(staging.enumeration,static_cast<unsigned>(staging.enumeration.state),true);
    if(!staging.enumeration.time.empty()){out.stock=plus(out.stock,staging.enumeration.time.size());out.typed_bytes=plus(out.typed_bytes,staging.enumeration.time.size());}
    reader.key("schema");require(reader.text()==h::profile);reader.key("sizes");reader.array();std::uint64_t previous{};
    while(!reader.next(']')){bound(plus(out.rows,1),limits.rows);bound(plus(out.rows,1),limits.source.rows);require(out.rows<staging.rows.size(),Error::capacity);auto& row=staging.rows[out.rows];row={};reader.object();
        reader.key("failure_event");std::string_view failure;if(reader.next('n'))reader.null();else{failure=reader.text();row.failure=failure_sequence(failure,out.run);}
        reader.key("mode");if(reader.next('n'))reader.null();else row.mode=enumerated<h::Mode>(reader.text(),modes);
        reader.key("observed_at");if(reader.next('n'))reader.null();else{row.time=reader.text();require(row.time.size()==30,Error::identity);}
        observation_reason(reader,row);reader.key("size_bytes");row.size=reader.number();size_syntax(row.size);require(row.size>previous);previous=row.size;
        reader.key("state");row.state=enumerated<h::State>(reader.text(),states);reader.end_object();observation_union(row,static_cast<unsigned>(row.state),true);require(row.mode.has_value()==(row.state==h::State::available));
        out.stock=plus(out.stock,plus(sizeof(h::OwnedRow),plus(row.time.size(),failure.size())));out.typed_bytes=plus(out.typed_bytes,row.time.size());if(row.failure)++out.errors;++out.rows;
    }reader.end_array();if(staging.enumeration.state!=h::EnumerationState::complete&&staging.enumeration.state!=h::EnumerationState::failed)require(out.rows==0);
    reader.key("topology");if(reader.next('n'))reader.null();else{out.bound=true;reader.object();reader.key("canonical_sha256");out.digest=reader.octets();require(out.digest.size()==64);reader.key("effective_fingerprint");out.effective=reader.text();fingerprint_syntax(out.effective);reader.key("physical_fingerprint");out.physical=reader.text();fingerprint_syntax(out.physical);reader.end_object();}
    reader.end_object();reader.finished();bound(out.errors,limits.error_children);bound(out.stock,limits.source.retained_bytes);return out;
}
void emit_reference(Sink& sink,std::string_view key,const FailureReference& reference){sink.object(key);if(reference.creation_ordinal)sink.number("creation_ordinal",*reference.creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",reference.sequence);sink.end();}
void emit_error(Sink& sink,const ObservationError& row){if(row.error)sink.octets("error_hex",row.error->canonical());else sink.null("error_hex");if(row.reference)emit_reference(sink,"reference",*row.reference);else sink.null("reference");}
void emit(Sink& sink,const detail::View& view,const h::Owner* source,const detail::Children& children){
    sink.object();if(source){sink.object("anonymous_thp");sink.octets("canonical_hex",source->canonical());const DigestText digest{*source->digest()};sink.text("canonical_sha256",digest.view());sink.text("profile",h::profile);if(children.topology)sink.octets("topology_hex",children.topology->canonical());else sink.null("topology_hex");sink.end();}else sink.null("anonymous_thp");
    sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));if(source){sink.object("enumeration_error");emit_error(sink,children.enumeration);sink.end();}else sink.null("enumeration_error");
    if(children.preparation_error)sink.octets("failure_error_hex",children.preparation_error->canonical());else sink.null("failure_error_hex");if(view.failure)emit_reference(sink,"failure_reference",*view.failure);else sink.null("failure_reference");
    sink.text("host_kind",name(view.kind,kinds));if(view.reason)sink.text("reason",name(*view.reason,reasons));else sink.null("reason");sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);
    if(source){sink.array("size_errors",children.rows.size());for(const auto& row:children.rows){sink.object();emit_error(sink,row);sink.number("size_bytes",row.size.value());sink.end();}sink.end();}else sink.null("size_errors");
    sink.text("state",name(view.slot_state,availabilities));sink.text("timestamp",view.time);sink.end();
}
auto parse_reference(Reader& reader)->FailureReference{reader.object();reader.key("creation_ordinal");const auto ordinal=reader.optional();reader.key("sequence");const auto sequence=reader.number();reader.end_object();return {sequence,ordinal};}
auto parse_error(Reader& reader,const Limits& limits)->detail::WireError{
    detail::WireError out;reader.key("error_hex");if(reader.next('n'))reader.null();else{out.bytes=reader.octets();require(!out.bytes.empty());bound(out.bytes.size()/2,limits.child.canonical_bytes);}reader.key("reference");if(reader.next('n'))reader.null();else out.reference=parse_reference(reader);require(out.reference.has_value()==!out.bytes.empty());return out;
}
auto parse(std::string_view body,const Limits& limits,detail::Staging* staging=nullptr)->Parsed{
    Reader reader{body,limits};Parsed out;reader.object();reader.key("anonymous_thp");if(reader.next('n'))reader.null();else{out.retained=true;reader.object();reader.key("canonical_hex");out.source=reader.octets();bound(out.source.size()/2,limits.source.canonical_bytes);reader.key("canonical_sha256");out.digest=reader.octets();require(out.digest.size()==64);reader.key("profile");require(reader.text()==h::profile);reader.key("topology_hex");if(reader.next('n'))reader.null();else{out.topology=reader.octets();require(!out.topology.empty());bound(out.topology.size()/2,limits.topology.canonical_bytes);}reader.end_object();}
    reader.key("domain");out.view.domain=enumerated<p::Domain>(reader.text(),domains);reader.key("entry_producer");out.view.reporter=enumerated<p::Producer>(reader.text(),reporters);reader.key("enumeration_error");if(reader.next('n'))reader.null();else{out.error_enumeration=true;reader.object();out.enumeration=parse_error(reader,limits);reader.end_object();if(out.enumeration.reference)++out.errors;if(staging)staging->enumeration.wire=out.enumeration;}
    reader.key("failure_error_hex");if(reader.next('n'))reader.null();else{out.preparation_error=reader.octets();require(!out.preparation_error.empty());bound(out.preparation_error.size()/2,limits.child.canonical_bytes);}
    reader.key("failure_reference");if(reader.next('n'))reader.null();else out.view.failure=parse_reference(reader);reader.key("host_kind");out.view.kind=enumerated<Kind>(reader.text(),kinds);reader.key("reason");if(reader.next('n'))reader.null();else out.view.reason=enumerated<Reason>(reader.text(),reasons);reader.key("run_id");out.view.run=reader.text();reader.key("schema");require(reader.text()==profile);reader.key("sequence");out.view.sequence=reader.number();reader.key("size_errors");
    if(reader.next('n'))reader.null();else{out.error_rows=true;reader.array();std::uint64_t previous{};while(!reader.next(']')){bound(plus(out.rows,1),limits.rows);reader.object();const auto wire=parse_error(reader,limits);reader.key("size_bytes");const auto size=reader.number();size_syntax(size);require(previous<size);previous=size;reader.end_object();if(wire.reference)++out.errors;if(staging){require(out.rows<staging->children.rows.size()&&staging->rows[out.rows].size==size);staging->rows[out.rows].wire=wire;}++out.rows;}reader.end_array();}
    reader.key("state");out.view.slot_state=enumerated<SlotState>(reader.text(),availabilities);reader.key("timestamp");out.view.time=reader.text();reader.end_object();reader.finished();out.wire=reader.count;bound(out.errors,limits.error_children);require(out.retained==out.error_rows&&out.retained==out.error_enumeration&&out.view.failure.has_value()==!out.preparation_error.empty());return out;
}
void valid_limits(const Limits& limits){
    require(limits.rows<=54&&limits.error_children<=55&&limits.source.rows<=54&&limits.depth<=5&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
    for(auto bytes:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes,limits.source.canonical_bytes})require(bytes<=std::string{}.max_size(),Error::invalid_limits);
}
void reference_local(const detail::View& view,const FailureReference& ref){require(ref.sequence&&ref.sequence<=view.sequence,Error::reference);if(ref.creation_ordinal)require(ref.sequence==1&&*ref.creation_ordinal<1024,Error::reference);if(view.kind==Kind::genesis)require(ref.sequence==1&&ref.creation_ordinal.has_value(),Error::reference);if(view.kind==Kind::ordinary&&ref.sequence==view.sequence)require(!ref.creation_ordinal,Error::reference);}
void local(const detail::View& view,bool retained){
    require(view.sequence,Error::identity);(void)name(view.domain,domains);(void)name(view.reporter,reporters);(void)name(view.kind,kinds);(void)name(view.slot_state,availabilities);if(view.reason)(void)name(*view.reason,reasons);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    if(view.sequence==1)require(view.reporter==p::Producer::controller,Error::identity);
    require(retained==(view.slot_state==SlotState::retained));require(view.failure.has_value()==(view.slot_state==SlotState::failed));require(view.reason.has_value()==(view.slot_state==SlotState::unavailable));if(view.failure)reference_local(view,*view.failure);
}
auto base(std::size_t upper)->std::size_t{return plus(detail::fixed_stock(),upper);}
auto primitive(const detail::View& view,const Limits& limits,std::size_t upper)->Usage{Usage out;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);bound(plus(base(upper),out.provenance_bytes),limits.publication_bytes);return out;}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);}
void source_limits(const SourceView& source,const detail::View& view){require(source.run==view.run,Error::identity);}
void source_match(const SourceView& source,const h::Owner& owner){
    require(owner.digest()&&owner.enumeration()&&owner.ordinal()&&owner.producer(),Error::source);
    const auto usage=owner.usage();require(owner.run_id()==source.run&&*owner.ordinal()==source.ordinal&&*owner.producer()==source.producer&&source.bound==(owner.topology()!=nullptr),Error::source);
    require(usage.rows==source.rows&&usage.canonical_bytes==source.canonical.size()&&usage.retained_bytes==source.stock,Error::source);
}
auto input_phase(const Limits& limits,bool present,std::size_t scratch,std::size_t upper)->std::size_t{if(!present)return 0;const auto out=plus(base(upper),scratch);bound(out,limits.source_input_bytes);return out;}
auto table_stock(std::size_t rows)->std::size_t{return times(rows,sizeof(ErrorRow));}
auto table_phase(const SourceView& source,const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{const auto out=plus(plus(base(upper),scratch),table_stock(source.rows));bound(out,limits.row_staging_bytes);return out;}
auto backing(const detail::Children& children)->std::size_t{
    std::size_t out{};if(children.topology){const auto usage=children.topology->usage();require(usage.retained_bytes>=sizeof(t::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(t::Owner));}
    if(children.preparation_error){const auto usage=children.preparation_error->usage();require(usage.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(e::Owner));}
    if(children.enumeration.error){const auto usage=children.enumeration.error->usage();require(usage.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(e::Owner));}
    for(const auto& row:children.rows)if(row.error){const auto usage=row.error->usage();require(usage.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(e::Owner));}
    return out;
}
auto child_stock(const Limits& limits,const detail::Staging& staging,bool topology,std::size_t scratch,std::size_t decoded,std::size_t upper)->std::size_t{
    std::size_t allowance{};if(topology){allowance=std::max({limits.topology.topology_decode_bytes,limits.topology.topology_preparation_bytes,limits.topology.error_preparation_bytes,limits.topology.publication_bytes,limits.topology.retained_bytes,plus(t::detail::fixed_stock(),upper?t::detail::session_fixed_stock():0)});bound(decoded,limits.topology.canonical_bytes);}
    else{const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();allowance=std::max({limits.child.consumer_simultaneous_bytes,limits.child.retained_bytes,floor});bound(decoded,limits.child.canonical_bytes);}
    const auto out=plus(plus(plus(plus(plus(base(upper),scratch),table_stock(staging.children.rows.size())),backing(staging.children)),decoded),allowance);bound(out,topology?limits.topology_preparation_bytes:limits.error_preparation_bytes);return out;
}
void topology_match(const detail::View& view,const SourceView& source,const detail::Children& children){
    require(source.bound==children.topology.has_value(),Error::child);if(!source.bound)return;const auto& child=*children.topology;const auto* provenance=child.provenance();const auto* topology=child.topology();require(provenance&&topology&&child.slot_state()==t::SlotState::retained&&!child.failure_error()&&!child.reason(),Error::child);
    require(provenance->run==view.run&&provenance->time==view.time&&provenance->domain==view.domain&&provenance->reporter==view.reporter&&provenance->kind==view.kind&&provenance->sequence==view.sequence,Error::child);
    const DigestText digest{*topology->digest()};const FingerprintText effective{topology->fingerprints()->effective.digest()},physical{topology->fingerprints()->physical.digest()};require(digest.view()==source.digest&&effective.view()==source.effective&&physical.view()==source.physical,Error::child);
}
void error_match(const detail::View& view,const std::optional<FailureReference>& reference,const std::optional<e::Owner>& child,const p::Target& target,std::optional<p::Producer> producer){
    require(reference.has_value()==child.has_value(),Error::child);if(!child)return;reference_local(view,*reference);const auto* value=child->provenance();require(value&&child->error()&&value->association,Error::child);const auto& fact=*value->association;
    require(value->run==view.run&&value->domain==view.domain&&value->sequence==reference->sequence&&value->creation_ordinal==reference->creation_ordinal&&value->kind==(reference->creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure),Error::child);
    if(reference->sequence==view.sequence)require(value->time==view.time&&value->entry_producer==view.reporter,Error::child);
    require(!fact.process_sequence&&(!producer||fact.producer==*producer),Error::child);require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
}
void observation_match(const detail::View& view,const SourceView& source,const detail::ObservationView& observed,const ObservationError& child,p::Selector selector,std::uint64_t key){
    require(child.reference.has_value()==observed.failure.has_value(),Error::reference);if(!observed.failure){require(!child.error,Error::child);return;}
    require(observed.reason&&child.reference->sequence==observed.failure,Error::reference);
    const p::Target target{p::Category::anonymous_thp,selector,source.ordinal,key,static_cast<p::FailureReason>(static_cast<unsigned>(*observed.reason)-2U)};
    error_match(view,child.reference,child.error,target,source.producer==h::Producer::controller?p::Producer::controller:p::Producer::runner);
}
void errors_match(const detail::View& view,const SourceView* source,const detail::Staging& staging){
    error_match(view,view.failure,staging.children.preparation_error,{p::Category::anonymous_thp},{});
    if(!source){require(staging.children.rows.empty()&&!staging.children.enumeration.reference&&!staging.children.enumeration.error&&!staging.children.topology,Error::child);return;}
    require(staging.children.rows.size()==source->rows,Error::child);
    observation_match(view,*source,staging.enumeration,staging.children.enumeration,p::Selector::enumeration,0);
    std::array<RepeatedError,55> identities{};std::size_t count{};
    if(staging.children.enumeration.reference)identities[count++]={&*staging.children.enumeration.reference,&*staging.children.enumeration.error};
    for(std::size_t i=0;i<source->rows;++i){const auto& observed=staging.rows[i];const auto& child=staging.children.rows[i];require(child.size.value()==observed.size,Error::identity);observation_match(view,*source,observed,child,p::Selector::page_size,observed.size);if(child.reference)identities[count++]={&*child.reference,&*child.error};}
    require(count==source->errors,Error::reference);for(std::size_t i=0;i<count;++i)for(std::size_t j=0;j<i;++j)if(*identities[i].reference==*identities[j].reference)require(identities[i].error->canonical()==identities[j].error->canonical(),Error::child);
}
auto source_phase(const SourceView& source,const detail::Staging& staging,const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{
    const auto out=plus(plus(plus(plus(plus(plus(base(upper),scratch),table_stock(source.rows)),backing(staging.children)),times(source.rows,sizeof(h::RowInput))),source.typed_bytes),limits.source.retained_bytes);bound(out,limits.source_preparation_bytes);return out;
}
auto retain_source(const SourceView& source,const detail::Staging& staging,const Limits& limits)->h::Owner{
    auto run=journal::RunId::parse(source.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);
    std::optional<journal::Timestamp> enumeration_time;std::array<std::optional<journal::Timestamp>,54> times;
    if(!staging.enumeration.time.empty()){auto time=journal::Timestamp::parse(staging.enumeration.time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);enumeration_time=std::move(*time);}
    for(std::size_t i=0;i<source.rows;++i)if(!staging.rows[i].time.empty()){auto time=journal::Timestamp::parse(staging.rows[i].time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);times[i]=std::move(*time);}
    std::vector<h::RowInput> rows;rows.reserve(source.rows);for(std::size_t i=0;i<source.rows;++i){const auto& row=staging.rows[i];rows.push_back({core::ByteCount::from_base_units(row.size),row.state,row.mode,times[i]?&*times[i]:nullptr,row.reason,row.failure});}
    const h::EnumerationInput enumeration{staging.enumeration.state,enumeration_time?&*enumeration_time:nullptr,staging.enumeration.reason,staging.enumeration.failure};
    const h::Input input{*run,source.ordinal,source.producer,enumeration,rows,staging.children.topology?staging.children.topology->topology():nullptr};auto kept=h::retain(input,limits.source);core::detail::ExpectedErrorCleanup cleanup{kept};if(!kept)throw kept.error().code.value()=="results.anonymous_thp_policy_owner.capacity"?Error::capacity:Error::source;
    require(kept->canonical()==source.canonical,Error::encoding);source_match(source,*kept);return std::move(*kept);
}
auto dom_stock(const Counts& count)->std::size_t{return plus(plus(plus(times(count.objects,sizeof(Json::object_t)),times(count.arrays,sizeof(Json::array_t))),plus(times(count.members,sizeof(Json::object_t::value_type)),times(count.elements,sizeof(Json)))),plus(times(count.strings,sizeof(Json::string_t)),count.text));}
auto publication(Usage out,const Counts& count,const h::Owner* source,const detail::Children& children,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.retained_bytes=plus(plus(plus(plus(sizeof(Owner),sizeof(detail::State)),out.provenance_bytes),plus(count.bytes,times(children.rows.size(),sizeof(ErrorRow)))),backing(children));std::size_t length{};
    if(source){out.source=source->usage();out.retained_bytes=plus(out.retained_bytes,out.source->retained_bytes-sizeof(h::Owner));length=source->canonical().size();}
    if(children.topology){out.topology=children.topology->usage();length=plus(length,children.topology->canonical().size());}if(children.preparation_error){out.preparation_error=children.preparation_error->usage();length=plus(length,children.preparation_error->canonical().size());}
    if(children.enumeration.error)length=plus(length,children.enumeration.error->canonical().size());
    for(const auto& row:children.rows)if(row.error)length=plus(length,row.error->canonical().size());
    out.publication_bytes=plus(plus(plus(plus(base(upper),out.retained_bytes),dom_stock(count)),times(length,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(std::optional<h::Owner>& source,detail::Children& children,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->slot_state=view.slot_state;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.kind,view.sequence,view.failure,view.reason};state->source=std::move(source);state->children=std::move(children);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->source?&*state->source:nullptr,state->children);auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rg{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
void source_usage(Usage& usage,const SourceView& source){usage.rows=source.rows;usage.error_children=source.errors;}
struct WireCells {
    Sink* emit_sink;const detail::View* emit_view;const h::Owner* emit_identity;const detail::Children* emit_children;
    std::size_t emit_index;DigestText emit_digest;
    Sink* reference_sink;std::string_view reference_key;const FailureReference* reference_value;
    Sink* sink_this;Json* sink_root;std::string_view sink_key,sink_value;std::size_t sink_size;bool sink_object;
    Frame* sink_frame;Frame assigned_frame;Json* sink_placed;Json sink_node;core::detail::JsonCleanup<Json> sink_cleanup;
    std::string sink_encoded,sink_text;Decimal sink_decimal;Json* sink_insert_return;
    Json* append_object_object;std::string_view append_object_name;Json* append_object_child;std::string append_object_key;Json::iterator insert_find_result;
    Sink *object_this,*array_this,*text_this,*octets_this,*null_this,*number_this,*prefix_this,*insert_this;
    std::string_view object_key,array_key,text_key,text_value,octets_key,octets_value,null_key,number_key,prefix_key,insert_key;
    std::size_t array_size;std::uint64_t number_value;Json* insert_node;
    Decimal* decimal_this;std::uint64_t decimal_value;std::to_chars_result decimal_converted;const Decimal* decimal_view_this;std::string_view decimal_view_return;
    Reader* reader_this;std::string_view reader_body;const Limits* reader_limits;char next_char;std::size_t next_at,text_begin;unsigned char reader_character;
    std::string_view reader_expected,reader_key,reader_text_result,reader_text_return,reader_octets_value,reader_octets_return;ParseFrame assigned_parse_frame;
    Reader *reader_number_this,*optional_this,*reader_octets_this,*node_this,*object_reader_this,*array_reader_this,*text_reader_this,*expect_this,*key_this,*null_reader_this,*finished_this;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,unhex_result;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value;std::size_t enum_index,name_index;
    const std::array<const char*,2>* names_two;const std::array<const char*,3>* names_three;const std::array<const char*,4>* names_four;
    const std::array<const char*,6>* names_six;
    Kind name_kind;p::Domain name_domain;p::Producer name_reporter;SlotState name_state;Reason name_reason;h::State name_observation_state;h::Producer name_source_producer;h::Reason name_source_reason;h::EnumerationState name_enumeration_state;h::Mode name_mode;
    std::string_view name_return;
    DigestText* digest_this;const core::Sha256Digest* digest_parameter;std::array<std::byte,32> digest_raw;std::size_t digest_index;unsigned digest_value;
    const core::Sha256Digest* digest_bytes_this;const std::array<std::byte,32>* digest_bytes_return;const DigestText* digest_view_this;std::string_view digest_view_return;
    FingerprintText* fingerprint_this;const core::Sha256Digest* fingerprint_parameter;DigestText fingerprint_suffix;const FingerprintText* fingerprint_view_this;std::string_view fingerprint_view_return,fingerprint_syntax_parameter;
    std::array<char,71>::iterator fingerprint_destination,fingerprint_copy_return,fingerprint_copy_n_return;std::array<char,64>::const_iterator fingerprint_begin,fingerprint_end;
    std::size_t plus_left,plus_right,times_left,times_right;std::optional<std::size_t> times_result;
    std::size_t* checked_value;std::size_t checked_amount,multiply_left,multiply_right,bound_value,bound_limit;bool require_value;Error require_error;
};
// Concrete named logical cells, not measured compiler stack or whole allocator peak.
struct OperationCells {
    WireCells wire;
    const h::Owner* before_source;const detail::View* before_view;const Limits* before_limits;std::size_t before_upper;
    detail::ResolvedInput *preflight_input,*phase_input,*prepare_input;const Limits *preflight_limits,*phase_limits;Limits prepare_limits,decode_limits;
    bool phase_topology;std::size_t phase_amount;std::string_view decode_body,decoded_encoded;
    Usage before_usage,primitive_out,primitive_return,decode_usage,publication_parameter,publication_return,publish_usage;
    std::expected<void,Error> before_return,preflight_return,phase_return;
    std::expected<Owner,Error> owner_return;std::unexpected<Error> unexpected_return;Error caught;const std::length_error* caught_length;
    SourceView before_info,preflight_info,prepare_info,decoded_info,parsed_source_out,parsed_source_return;
    Parsed decoded_parsed,parse_out,filled_parse_return;std::string decoded_source,decoded_topology,decoded_preparation,decoded_error;
    detail::Staging before_staging,decoded_staging;std::optional<h::Owner> prepared_source,decoded_kept;h::Owner retained_return;
    std::expected<t::Owner,t::Error> decoded_topology_result;std::expected<e::Owner,e::Error> decoded_preparation_result,decoded_error_result;
    DigestText decoded_digest;std::size_t decoded_index;const detail::RowView* decoded_row;
    Sink prepare_counter;std::unique_ptr<detail::State> decode_state,publish_state,constructor_state;Owner* constructor_this;Owner constructed_owner;
    const Limits *valid_limits_parameter,*primitive_limits,*input_limits,*table_limits,*child_limits,*source_phase_limits,*publication_limits,*retain_limits;
    std::array<std::size_t,4> limits_bytes;std::initializer_list<std::size_t> limits_byte_list;std::size_t limits_byte;
    std::string limits_string;Json::array_t limits_array;
    const detail::View *local_view,*reference_view,*primitive_view,*identity_view,*source_limits_view,*topology_view,*errors_view,*error_view,*publish_view,*observation_view;
    bool local_retained;const FailureReference* local_ref;std::size_t primitive_upper,base_upper;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<decltype(identity_run)> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<decltype(identity_time)> identity_time_guard;
    std::string_view reference_text,reference_run;std::uint64_t reference_result;
    std::string_view inner_body;const Limits* inner_limits;detail::Staging* inner_staging;Limits inner_reader_limits;Reader inner_reader;
    std::string_view inner_enumeration_failure,inner_failure;std::uint64_t inner_previous;detail::RowView* inner_row;
    detail::EnumerationView empty_enumeration;detail::RowView empty_row;
    Reader* reason_reader;detail::ObservationView* reason_out;const detail::ObservationView* union_out;unsigned union_state,union_reason;bool union_available;std::uint64_t size_parameter;
    std::string_view parse_body;const Limits* parse_limits;detail::Staging* parse_staging;Reader parse_reader;std::uint64_t parse_previous,parse_size;detail::WireError parse_wire;
    Reader* parse_error_reader;const Limits* parse_error_limits;detail::WireError parse_error_out,parse_error_return;
    Reader* parse_reference_reader;std::optional<std::uint64_t> parse_reference_ordinal;std::uint64_t parse_reference_sequence;FailureReference parse_reference_result;
    Sink* emit_error_sink;const ObservationError* emit_error_row;const ErrorRow* emit_row;
    const SourceView *source_limits_source,*match_source,*table_source,*topology_source,*errors_source,*retain_source,*phase_source,*usage_source,*observation_source;
    const h::Owner* match_owner;h::Usage match_usage;
    bool input_present;std::size_t input_scratch,input_upper,input_out,table_rows,table_scratch,table_upper,table_out;
    const detail::Children *backing_children,*topology_children,*publication_children;
    const detail::Staging *child_staging,*errors_staging,*retain_staging,*source_phase_staging;
    std::size_t backing_out;const ErrorRow* backing_row;t::Usage backing_topology_usage;e::Usage backing_preparation_usage,backing_enumeration_usage,backing_error_usage;
    bool child_topology;std::size_t child_scratch,child_decoded,child_upper,child_allowance,child_floor,child_out;
    std::array<std::size_t,6> topology_max_values;std::initializer_list<std::size_t> topology_max_list;std::array<std::size_t,3> error_max_values;std::initializer_list<std::size_t> error_max_list;
    const t::Owner* topology_child;const t::Provenance* topology_provenance;const topology_evidence::Owner* topology_owner;
    DigestText topology_digest;FingerprintText topology_effective,topology_physical;
    const std::optional<FailureReference>* error_reference;const std::optional<e::Owner>* error_child;const p::Target* error_target;std::optional<p::Producer> error_producer;
    const e::Provenance* error_provenance;const e::Association* error_fact;std::vector<p::Target>::const_iterator error_find;
    const detail::ObservationView* observation_observed;const ObservationError* observation_child;p::Selector observation_selector;std::uint64_t observation_key;p::Target observation_target;std::optional<p::Producer> observation_producer;
    std::size_t errors_index,repeated_index,repeated_previous,repeated_count;const detail::RowView* errors_row;const ErrorRow* errors_child;p::Target errors_preparation_target;
    std::array<RepeatedError,55> repeated_identities;RepeatedError repeated_enumeration,repeated_row;
    std::expected<journal::RunId,core::Error> retain_run;core::detail::ExpectedErrorCleanup<decltype(retain_run)> retain_run_guard;
    std::optional<journal::Timestamp> retain_enumeration_time;std::array<std::optional<journal::Timestamp>,54> retain_times;
    std::vector<h::RowInput> retain_rows;std::size_t retain_time_index,retain_row_index;const detail::RowView* retain_row;
    std::expected<journal::Timestamp,core::Error> retain_enumeration_parsed,retain_row_parsed;
    core::detail::ExpectedErrorCleanup<decltype(retain_enumeration_parsed)> retain_enumeration_guard;core::detail::ExpectedErrorCleanup<decltype(retain_row_parsed)> retain_row_guard;
    h::RowInput retain_row_temporary;h::EnumerationInput retain_enumeration;h::Input retain_input;std::span<const h::RowInput> retain_input_span;
    core::ByteCount size_factory_return,size_constructor_return;std::uint64_t size_factory_value,size_constructor_value;core::ByteCount* size_constructor_this;const core::ByteCount* size_value_this;std::uint64_t size_value_return;
    std::expected<h::Owner,core::Error> retain_result;core::detail::ExpectedErrorCleanup<decltype(retain_result)> retain_guard;h::Limits retain_limits_copy;
    std::size_t source_scratch,source_upper,source_phase_out;
    const Counts *dom_counts,*publication_count,*publish_count;const h::Owner* publication_source;std::size_t publication_upper,publication_length,dom_return;
    const ErrorRow* publication_row;std::optional<h::Owner>* publish_source;detail::Children* publish_children;Origin publish_origin;Provenance provenance_temporary;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;Sink publish_sink;core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_guard;
    core::Sha256Digest digest_return,initial_digest;std::string_view initial_digest_input;detail::State* state_constructor_this;Usage* source_usage_parameter;
    const h::Owner *source_usage_this,*source_canonical_this,*source_digest_this,*source_run_this,*source_producer_this,*source_ordinal_this,*source_topology_this,*source_enumeration_this;
    h::Usage source_usage_return;std::string_view source_canonical_return,source_run_return;const core::Sha256Digest* source_digest_return;std::optional<h::Producer> source_producer_return;std::optional<std::uint64_t> source_ordinal_return;const h::TopologyBinding* source_topology_return;const h::Enumeration* source_enumeration_return;
    const t::Owner *topology_usage_this,*topology_canonical_this,*topology_provenance_this,*topology_owner_this,*topology_state_this,*topology_error_this,*topology_reason_this;
    t::Usage topology_usage_return;std::string_view topology_canonical_return;const t::Provenance* topology_provenance_return;const topology_evidence::Owner* topology_owner_return;std::optional<t::SlotState> topology_state_return;const e::Owner* topology_error_return;std::optional<t::Reason> topology_reason_return;
    const topology_evidence::Owner *inner_topology_digest_this,*inner_topology_fingerprints_this;const core::Sha256Digest* inner_topology_digest_return;const epycbench::topology::TopologyFingerprints* inner_topology_fingerprints_return;
    const epycbench::topology::PhysicalTopologyFingerprint* physical_this;const epycbench::topology::EffectiveTopologyFingerprint* effective_this;const core::Sha256Digest *physical_return,*effective_return;
    const e::Owner *error_usage_this,*error_canonical_this,*error_provenance_this,*error_source_this;e::Usage error_usage_return;std::string_view error_canonical_return;const e::Provenance* error_provenance_return;const core::Error* error_source_return;
    std::size_t fixed_return,backing_return,input_phase_return,child_stock_return,source_phase_return,table_phase_return,source_stock_return;
    std::size_t preflight_index;const detail::BindingRow* preflight_binding;ErrorRow* preflight_row;const detail::RowView* preflight_observed;
    const detail::View* bind_view;const detail::ObservationView* bind_observed;ObservationError* bind_child;std::optional<FailureReference> bind_reference;
    const detail::View* decode_error_view;detail::Staging* decode_error_staging;const Limits* decode_error_limits;Usage* decode_error_usage;std::size_t decode_error_scratch;std::string_view decode_error_encoded;std::optional<e::Owner>* decode_error_out;
};
} // namespace
auto detail::checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto detail::checked_multiply(std::size_t left,std::size_t right)noexcept->std::optional<std::size_t>{if(right&&left>SIZE_MAX/right)return {};return left*right;}
auto detail::fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&)noexcept=default;
auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::~Owner()=default;
auto Owner::source()const noexcept->const h::Owner*{return state_&&state_->source?&*state_->source:nullptr;}
auto Owner::topology()const noexcept->const t::Owner*{return state_&&state_->children.topology?&*state_->children.topology:nullptr;}
auto Owner::failure_error()const noexcept->const e::Owner*{return state_&&state_->children.preparation_error?&*state_->children.preparation_error:nullptr;}
auto Owner::enumeration_error()const noexcept->const ObservationError*{return state_&&state_->source?&state_->children.enumeration:nullptr;}
auto Owner::errors()const noexcept->std::span<const ErrorRow>{return state_?std::span<const ErrorRow>{state_->children.rows}:std::span<const ErrorRow>{};}
auto Owner::slot_state()const noexcept->std::optional<SlotState>{return state_?std::optional{state_->slot_state}:std::nullopt;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
namespace {
void bind_observation(const detail::View& view,const detail::ObservationView& observed,ObservationError& child,std::optional<FailureReference> reference){
    require(observed.failure.has_value()==reference.has_value(),Error::reference);child.reference=reference;if(reference){reference_local(view,*reference);require(observed.failure==reference->sequence,Error::reference);}
}
void decode_error(detail::Staging& staging,const Limits& limits,Usage& usage,std::size_t scratch,std::string_view encoded,std::optional<e::Owner>& out){
    if(encoded.empty())return;
    usage.error_preparation_bytes=std::max(usage.error_preparation_bytes,child_stock(limits,staging,false,scratch,encoded.size()/2,0));const auto bytes=unhex(encoded);auto decoded=e::Owner::decode(bytes,limits.child);if(!decoded)throw decoded.error()==e::Error::capacity?Error::capacity:Error::child;out=std::move(*decoded);
}
}
auto Owner::before_bindings(const h::Owner* source,const detail::View& view,const Limits& limits,std::size_t upper)->std::expected<void,Error>{
    try{valid_limits(limits);(void)primitive(view,limits,upper);local(view,source!=nullptr);identity(view);if(source){detail::Staging staging;const auto info=parse_source(source->canonical(),limits,staging);source_limits(info,view);source_match(info,*source);}return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::preflight(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{input.usage=primitive(input.view,limits,input.upper_fixed);require(input.staging&&input.enumeration,Error::source);input.usage.source_input_bytes=input_phase(limits,input.source!=nullptr,0,input.upper_fixed);
        if(input.source){const auto info=parse_source(input.source->canonical(),limits,*input.staging);source_limits(info,input.view);source_match(info,*input.source);require(input.bindings.size()==info.rows,Error::source);source_usage(input.usage,info);input.usage.row_staging_bytes=table_phase(info,limits,0,input.upper_fixed);input.staging->children.rows.resize(info.rows);
            bind_observation(input.view,input.staging->enumeration,input.staging->children.enumeration,input.enumeration->reference);
            for(std::size_t i=0;i<info.rows;++i){const auto& binding=input.bindings[i];auto& row=input.staging->children.rows[i];const auto& observed=input.staging->rows[i];require(binding.size.value()==observed.size,Error::identity);row.size=binding.size;bind_observation(input.view,observed,row,binding.failure.reference);}
        }return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::child_phase(detail::ResolvedInput& input,const Limits& limits,bool topology)->std::expected<void,Error>{
    try{const auto amount=child_stock(limits,*input.staging,topology,0,0,input.upper_fixed);if(topology)input.usage.topology_preparation_bytes=amount;else input.usage.error_preparation_bytes=std::max(input.usage.error_preparation_bytes,amount);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{require(input.staging,Error::source);std::optional<h::Owner> source;if(input.source){const auto info=parse_source(input.source->canonical(),limits,*input.staging);topology_match(input.view,info,input.staging->children);errors_match(input.view,&info,*input.staging);input.usage.source_preparation_bytes=source_phase(info,*input.staging,limits,0,input.upper_fixed);source=retain_source(info,*input.staging,limits);require(*source->digest()==*input.source->digest(),Error::source);}else errors_match(input.view,nullptr,*input.staging);
        Sink count;emit(count,input.view,source?&*source:nullptr,input.staging->children);input.usage=publication(input.usage,count.count,source?&*source:nullptr,input.staging->children,input.upper_fixed,limits);return Owner{publish(source,input.staging->children,input.view,count.count,input.usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits,0);local(parsed.view,parsed.retained);identity(parsed.view);detail::Staging staging;std::optional<h::Owner> source;usage.source_input_bytes=input_phase(limits,parsed.retained,parsed.source.size()/2,0);
        {const auto canonical=unhex(parsed.source);SourceView info;
            if(parsed.retained){info=parse_source(canonical,limits,staging);source_limits(info,parsed.view);require(info.rows==parsed.rows&&info.errors==parsed.errors&&info.bound==!parsed.topology.empty(),Error::reference);source_usage(usage,info);usage.row_staging_bytes=table_phase(info,limits,canonical.size(),0);staging.children.rows.resize(info.rows);(void)parse(body,limits,&staging);
                bind_observation(parsed.view,staging.enumeration,staging.children.enumeration,staging.enumeration.wire.reference);
                for(std::size_t i=0;i<info.rows;++i){const auto& row=staging.rows[i];staging.children.rows[i].size=core::ByteCount::from_base_units(row.size);bind_observation(parsed.view,row,staging.children.rows[i],row.wire.reference);}
                if(info.bound){usage.topology_preparation_bytes=child_stock(limits,staging,true,canonical.size(),parsed.topology.size()/2,0);const auto bytes=unhex(parsed.topology);auto decoded=t::Owner::decode(bytes,limits.topology);if(!decoded)throw decoded.error()==t::Error::capacity?Error::capacity:Error::child;staging.children.topology=std::move(*decoded);}
            }
            decode_error(staging,limits,usage,canonical.size(),parsed.preparation_error,staging.children.preparation_error);
            decode_error(staging,limits,usage,canonical.size(),staging.enumeration.wire.bytes,staging.children.enumeration.error);
            for(std::size_t i=0;i<info.rows;++i){const auto encoded=staging.rows[i].wire.bytes;decode_error(staging,limits,usage,canonical.size(),encoded,staging.children.rows[i].error);}
            errors_match(parsed.view,parsed.retained?&info:nullptr,staging);if(parsed.retained){topology_match(parsed.view,info,staging.children);usage.source_preparation_bytes=source_phase(info,staging,limits,canonical.size(),0);source=retain_source(info,staging,limits);const DigestText digest{*source->digest()};require(digest.view()==parsed.digest,Error::encoding);}
        }
        usage=publication(usage,parsed.wire,source?&*source:nullptr,staging.children,0,limits);auto state=publish(source,staging.children,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::anonymous_thp_policy_serialization
