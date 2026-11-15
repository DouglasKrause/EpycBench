#include "results/private_system_controls_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::system_controls_serialization {
namespace {
using Json=nlohmann::json;
namespace h=host_controls;
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
    auto boolean()->bool{node();if(body_.substr(at_,4)=="true"){expect("true");return true;}expect("false");return false;}
    auto uint32()->std::uint32_t{node();const auto start=at_;while(at_<body_.size()&&body_[at_]>='0'&&body_[at_]<='9')++at_;const auto value=integer(body_.substr(start,at_-start));require(value<=UINT32_MAX);return static_cast<std::uint32_t>(value);}
    void finished(){require(level_==0&&at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,5> frames_{};
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
constexpr std::array slots{"smt_control","smt_active","numa_balancing","thp_top_level_enabled","thp_defrag"};
constexpr std::array states{"not_attempted","available","unavailable","failed"};
constexpr std::array observation_reasons{"not_supported","source_missing","access_denied","read_failed","malformed_source","unsupported_value"};
constexpr std::array producers{"controller","runner"};
constexpr std::array smt{"on","off","forceoff","notsupported","notimplemented","threads"};
constexpr std::array enabled{"always","madvise","never"};
constexpr std::array defrag{"always","defer","defer+madvise","madvise","never"};
constexpr std::array selectors{p::Selector::smt_control,p::Selector::smt_active,p::Selector::numa_mask,p::Selector::thp_enabled,p::Selector::thp_defrag};
struct ObservationView {std::string_view id,time;h::State state{};h::Value value;std::optional<h::Reason> reason;std::optional<std::uint64_t> failure;};
struct ControlsView {
    std::string_view canonical,run;std::uint64_t ordinal{};h::Producer producer{};std::array<ObservationView,5> observations;
    bool bound{};std::string_view digest,effective,physical;const h::TopologyBinding* source_binding{};
};
struct Parsed {detail::View view{};Counts wire;std::string_view controls,digest,topology;std::array<std::string_view,6> errors;bool retained{},rows{};};
auto failure_sequence(std::string_view value,std::string_view run)->std::uint64_t {
    require(value.size()>run.size()+1&&value.starts_with(run)&&value[run.size()]==':',Error::reference);
    const auto result=integer(value.substr(run.size()+1));require(result!=0,Error::reference);return result;
}
void observation_id(std::string_view id,std::string_view run,std::uint64_t ordinal,std::size_t index){
    const Decimal number{ordinal};require(id.starts_with(run),Error::identity);id.remove_prefix(run.size());
    constexpr std::string_view marker=":system-controls:";require(id.starts_with(marker),Error::identity);id.remove_prefix(marker.size());
    require(id.starts_with(number.view()),Error::identity);id.remove_prefix(number.size);require(id.size()>1&&id.front()==':',Error::identity);id.remove_prefix(1);require(id==slots[index],Error::identity);
}
auto source_view(const h::Owner& owner)->ControlsView {
    require(owner.digest()&&owner.producer()&&owner.ordinal(),Error::source);ControlsView out;
    out.canonical=owner.canonical();out.run=owner.run_id();out.ordinal=*owner.ordinal();out.producer=*owner.producer();out.bound=owner.topology()!=nullptr;out.source_binding=owner.topology();
    const auto observations=owner.observations();require(observations.size()==5,Error::source);
    for(std::size_t i=0;i<5;++i){const auto& source=observations[i];auto& row=out.observations[i];row.id=source.id;row.time=source.observed_at?std::string_view{*source.observed_at}:std::string_view{};
        row.state=source.state;row.value=source.value;row.reason=source.reason;if(source.failure_event)row.failure=failure_sequence(*source.failure_event,out.run);}
    return out;
}
void validate_controls(const ControlsView& c,std::string_view run){
    require(c.run==run&&c.run.size()==36,Error::identity);(void)name(c.producer,producers);
    for(std::size_t i=0;i<5;++i){const auto& row=c.observations[i];observation_id(row.id,run,c.ordinal,i);(void)name(row.state,states);if(row.reason)(void)name(*row.reason,observation_reasons);
        if(row.state==h::State::not_attempted){require(row.time.empty()&&row.value.index()==0&&!row.reason&&!row.failure);continue;}
        require(row.time.size()==30,Error::identity);
        if(row.state==h::State::available){require(row.value.index()==i+1&&!row.reason&&!row.failure);
            if(i==0){const auto& control=std::get<h::SmtControl>(row.value);(void)name(control.mode,smt);require(control.mode==h::SmtMode::threads?(control.threads_per_core&&*control.threads_per_core>0):!control.threads_per_core);}
            else if(i==2)require(std::get<std::uint32_t>(row.value)<=3);
            else if(i==3)(void)name(std::get<h::ThpEnabled>(row.value),enabled);
            else if(i==4)(void)name(std::get<h::ThpDefrag>(row.value),defrag);
        }else{require(row.value.index()==0&&row.reason.has_value());const auto reason=static_cast<unsigned>(*row.reason);
            require(row.state==h::State::unavailable?(reason<3&&!row.failure):(reason>=3&&row.failure&&*row.failure!=0));}
    }
}
void fingerprint_syntax(std::string_view text){require(text.size()==71&&text.starts_with("sha256:"),Error::encoding);valid_hex(text.substr(7));}
auto parse_controls(std::string_view body,const Limits& limits)->ControlsView {
    Limits inner{};inner.canonical_bytes=limits.controls.canonical_bytes;inner.text_bytes=limits.controls.canonical_bytes;inner.nodes=128;inner.depth=5;
    Reader r{body,inner};ControlsView out;out.canonical=body;r.object();r.key("observations");r.array();
    for(std::size_t i=0;i<5;++i){auto& row=out.observations[i];r.object();r.key("failure_event");std::string_view failure;if(r.next('n'))r.null();else failure=r.text();
        r.key("id");row.id=r.text();r.key("observed_at");if(r.next('n'))r.null();else row.time=r.text();r.key("reason");if(r.next('n'))r.null();else row.reason=enumerated<h::Reason>(r.text(),observation_reasons);
        r.key("slot");require(r.text()==slots[i]);r.key("state");row.state=enumerated<h::State>(r.text(),states);r.key("value");
        if(r.next('n'))r.null();else if(i==0){r.object();r.key("mode");const auto mode=enumerated<h::SmtMode>(r.text(),smt);r.key("threads_per_core");std::optional<std::uint32_t> threads;if(r.next('n'))r.null();else threads=r.uint32();row.value=h::SmtControl{mode,threads};r.end_object();}
        else if(i==1)row.value=r.boolean();else if(i==2)row.value=r.uint32();else if(i==3)row.value=enumerated<h::ThpEnabled>(r.text(),enabled);else row.value=enumerated<h::ThpDefrag>(r.text(),defrag);
        // RunId has a fixed canonical width; validate against the root after parsing it.
        if(!failure.empty()){require(failure.size()>37);row.failure=failure_sequence(failure,failure.substr(0,36));require(row.id.starts_with(failure.substr(0,36)),Error::reference);}r.end_object();
    }
    r.end_array();r.key("ordinal");out.ordinal=r.number();r.key("producer");out.producer=enumerated<h::Producer>(r.text(),producers);r.key("run_id");out.run=r.text();r.key("schema");require(r.text()==h::profile);
    r.key("topology");if(r.next('n'))r.null();else{out.bound=true;r.object();r.key("canonical_sha256");out.digest=r.octets();require(out.digest.size()==64);r.key("effective_fingerprint");out.effective=r.text();fingerprint_syntax(out.effective);r.key("physical_fingerprint");out.physical=r.text();fingerprint_syntax(out.physical);r.end_object();}
    r.end_object();r.finished();return out;
}
void emit_reference(Sink& sink,std::string_view key,const FailureReference& ref){sink.object(key);if(ref.creation_ordinal)sink.number("creation_ordinal",*ref.creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",ref.sequence);sink.end();}
void emit(Sink& sink,const detail::View& view,const h::Owner* controls,const detail::Children& children){
    sink.object();if(controls){sink.object("controls");sink.octets("canonical_hex",controls->canonical());const DigestText digest{*controls->digest()};sink.text("canonical_sha256",digest.view());sink.text("profile",h::profile);if(children.topology)sink.octets("topology_hex",children.topology->canonical());else sink.null("topology_hex");sink.end();}else sink.null("controls");
    sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));
    if(children.errors[0])sink.octets("failure_error_hex",children.errors[0]->canonical());else sink.null("failure_error_hex");
    if(view.failures[0])emit_reference(sink,"failure_reference",*view.failures[0]);else sink.null("failure_reference");sink.text("host_kind",name(view.kind,kinds));
    if(controls){sink.array("observation_errors",5);for(std::size_t i=1;i<6;++i){if(children.errors[i]){sink.object();sink.octets("error_hex",children.errors[i]->canonical());emit_reference(sink,"reference",*view.failures[i]);sink.end();}else sink.null({});}sink.end();}else sink.null("observation_errors");
    if(view.reason)sink.text("reason",name(*view.reason,reasons));else sink.null("reason");sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.text("state",name(view.slot_state,availabilities));sink.text("timestamp",view.time);sink.end();
}
auto parse_reference(Reader& r)->FailureReference{r.object();r.key("creation_ordinal");const auto ordinal=r.optional();r.key("sequence");const auto sequence=r.number();r.end_object();return {sequence,ordinal};}
auto parse(std::string_view body,const Limits& limits)->Parsed {
    Reader r{body,limits};Parsed out;r.object();r.key("controls");if(r.next('n'))r.null();else{out.retained=true;r.object();r.key("canonical_hex");out.controls=r.octets();bound(out.controls.size()/2,limits.controls.canonical_bytes);r.key("canonical_sha256");out.digest=r.octets();require(out.digest.size()==64);r.key("profile");require(r.text()==h::profile);r.key("topology_hex");if(r.next('n'))r.null();else{out.topology=r.octets();require(!out.topology.empty());bound(out.topology.size()/2,limits.topology.canonical_bytes);}r.end_object();}
    r.key("domain");out.view.domain=enumerated<p::Domain>(r.text(),domains);r.key("entry_producer");out.view.reporter=enumerated<p::Producer>(r.text(),reporters);r.key("failure_error_hex");if(r.next('n'))r.null();else{out.errors[0]=r.octets();require(!out.errors[0].empty());bound(out.errors[0].size()/2,limits.child.canonical_bytes);}
    r.key("failure_reference");if(r.next('n'))r.null();else out.view.failures[0]=parse_reference(r);r.key("host_kind");out.view.kind=enumerated<Kind>(r.text(),kinds);
    r.key("observation_errors");if(r.next('n'))r.null();else{out.rows=true;r.array();for(std::size_t i=1;i<6;++i){if(r.next('n'))r.null();else{r.object();r.key("error_hex");out.errors[i]=r.octets();require(!out.errors[i].empty());bound(out.errors[i].size()/2,limits.child.canonical_bytes);r.key("reference");out.view.failures[i]=parse_reference(r);r.end_object();}}r.end_array();}
    r.key("reason");if(r.next('n'))r.null();else out.view.reason=enumerated<Reason>(r.text(),reasons);r.key("run_id");out.view.run=r.text();r.key("schema");require(r.text()==profile);r.key("sequence");out.view.sequence=r.number();r.key("state");out.view.slot_state=enumerated<SlotState>(r.text(),availabilities);r.key("timestamp");out.view.time=r.text();r.end_object();r.finished();out.wire=r.count;return out;
}
void valid_limits(const Limits& limits){for(auto n:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes,limits.controls.canonical_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);require(limits.depth<=5&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);}
void reference_local(const detail::View& view,const FailureReference& ref){require(ref.sequence&&ref.sequence<=view.sequence,Error::reference);if(ref.creation_ordinal)require(ref.sequence==1&&*ref.creation_ordinal<1024,Error::reference);if(view.kind==Kind::genesis)require(ref.sequence==1&&ref.creation_ordinal.has_value(),Error::reference);if(view.kind==Kind::ordinary&&ref.sequence==view.sequence)require(!ref.creation_ordinal,Error::reference);}
void local(const detail::View& view,bool retained){
    require(view.sequence,Error::identity);(void)name(view.domain,domains);(void)name(view.reporter,reporters);(void)name(view.kind,kinds);(void)name(view.slot_state,availabilities);if(view.reason)(void)name(*view.reason,reasons);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    if(view.sequence==1)require(view.reporter==p::Producer::controller,Error::identity);
    require(retained==(view.slot_state==SlotState::retained));require(view.failures[0].has_value()==(view.slot_state==SlotState::failed));require(view.reason.has_value()==(view.slot_state==SlotState::unavailable));
    for(std::size_t i=0;i<6;++i){if(i&&!retained)require(!view.failures[i]);if(view.failures[i])reference_local(view,*view.failures[i]);}
}
auto primitive(const detail::View& view,const Limits& limits,std::size_t upper)->Usage{Usage out;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);bound(plus(plus(detail::fixed_stock(),upper),out.provenance_bytes),limits.publication_bytes);return out;}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);}
auto control_stock(const ControlsView& c)->std::size_t {
    auto out=plus(sizeof(h::Owner),plus(c.run.size(),c.canonical.size()));
    for(const auto& o:c.observations){out=plus(out,plus(o.id.size(),o.time.size()));if(o.failure){const Decimal number{*o.failure};out=plus(out,plus(c.run.size()+1,number.size));}}return out;
}
auto time_stock(const ControlsView& c)->std::size_t{std::size_t out=36;for(const auto& o:c.observations)out=plus(out,o.time.size());return out;}
void controls_limits(const ControlsView& c,const Limits& limits){bound(5,limits.controls.observations);bound(c.canonical.size(),limits.controls.canonical_bytes);bound(control_stock(c),limits.controls.retained_bytes);}
auto input_phase(const Limits& limits,bool present,std::size_t scratch,std::size_t upper)->std::size_t{if(!present)return 0;const auto out=plus(plus(plus(detail::fixed_stock(),upper),scratch),186);bound(out,limits.controls_input_bytes);return out;}
auto backing(const detail::Children& children)->std::size_t{std::size_t out{};if(children.topology){const auto u=children.topology->usage();require(u.retained_bytes>=sizeof(t::Owner),Error::child);out=plus(out,u.retained_bytes-sizeof(t::Owner));}for(const auto& child:children.errors)if(child){const auto u=child->usage();require(u.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,u.retained_bytes-sizeof(e::Owner));}return out;}
auto child_phase_stock(const Limits& limits,const detail::Children& children,bool topology,std::size_t scratch,std::size_t decoded,std::size_t upper)->std::size_t{
    std::size_t allowance{};
    if(topology){allowance=std::max({limits.topology.topology_decode_bytes,limits.topology.topology_preparation_bytes,limits.topology.error_preparation_bytes,limits.topology.publication_bytes,limits.topology.retained_bytes,plus(t::detail::fixed_stock(),upper?t::detail::session_fixed_stock():0)});bound(decoded,limits.topology.canonical_bytes);}
    else{const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();allowance=std::max({limits.child.consumer_simultaneous_bytes,limits.child.retained_bytes,floor});bound(decoded,limits.child.canonical_bytes);}
    const auto out=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),scratch),backing(children)),decoded),allowance);bound(out,topology?limits.topology_preparation_bytes:limits.error_preparation_bytes);return out;
}
void controls_references(const detail::View& view,const ControlsView& c){validate_controls(c,view.run);for(std::size_t i=0;i<5;++i){const auto& row=c.observations[i];const auto& ref=view.failures[i+1];require(row.failure.has_value()==ref.has_value(),Error::reference);if(ref)require(row.failure==ref->sequence,Error::reference);}}
void topology_match(const detail::View& view,const ControlsView& c,const detail::Children& children){
    require(c.bound==children.topology.has_value(),Error::child);if(!c.bound)return;const auto& child=*children.topology;const auto* provenance=child.provenance();const auto* topology=child.topology();
    require(provenance&&topology&&child.slot_state()==t::SlotState::retained&&!child.failure_error()&&!child.reason(),Error::child);
    require(provenance->run==view.run&&provenance->time==view.time&&provenance->domain==view.domain&&provenance->reporter==view.reporter&&provenance->kind==view.kind&&provenance->sequence==view.sequence,Error::child);
    if(c.source_binding)require(*topology->digest()==c.source_binding->canonical_sha256&&*topology->fingerprints()==c.source_binding->fingerprints,Error::child);
    else{const DigestText digest{*topology->digest()};const FingerprintText effective{topology->fingerprints()->effective.digest()},physical{topology->fingerprints()->physical.digest()};require(digest.view()==c.digest&&effective.view()==c.effective&&physical.view()==c.physical,Error::child);}
}
void errors_match(const detail::View& view,const ControlsView* controls,const detail::Children& children){
    for(std::size_t i=0;i<6;++i){const auto& ref=view.failures[i];const auto& child=children.errors[i];require(ref.has_value()==child.has_value(),Error::child);if(!child)continue;
        const auto* value=child->provenance();require(value&&child->error()&&value->association,Error::child);const auto& fact=*value->association;
        require(value->run==view.run&&value->domain==view.domain&&value->sequence==ref->sequence&&value->creation_ordinal==ref->creation_ordinal&&value->kind==(ref->creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure),Error::child);
        if(ref->sequence==view.sequence)require(value->time==view.time&&value->entry_producer==view.reporter,Error::child);
        require(!fact.process_sequence,Error::child);
        p::Target target{p::Category::system_controls};if(i){require(controls,Error::child);const auto& row=controls->observations[i-1];require(row.reason.has_value(),Error::child);target.selector=selectors[i-1];target.ordinal=controls->ordinal;target.reason=static_cast<p::FailureReason>(static_cast<unsigned>(*row.reason)-2U);require(fact.producer==(controls->producer==h::Producer::controller?p::Producer::controller:p::Producer::runner),Error::child);}
        require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
        for(std::size_t j=0;j<i;++j)if(view.failures[j]==ref)require(children.errors[j]&&children.errors[j]->canonical()==child->canonical(),Error::child);
    }
}
auto retain_controls(const ControlsView& c,const detail::Children& children,const Limits& limits)->h::Owner{
    auto run=journal::RunId::parse(c.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);
    std::array<std::optional<journal::Timestamp>,5> times;h::Input input{*run,c.ordinal,c.producer,{},children.topology?children.topology->topology():nullptr};
    for(std::size_t i=0;i<5;++i){const auto& row=c.observations[i];if(!row.time.empty()){auto parsed=journal::Timestamp::parse(row.time);core::detail::ExpectedErrorCleanup tg{parsed};require(parsed.has_value(),Error::identity);times[i]=std::move(*parsed);}input.observations[i]={row.state,row.value,times[i]?&*times[i]:nullptr,row.reason,row.failure};}
    auto kept=h::retain(input,limits.controls);core::detail::ExpectedErrorCleanup cleanup{kept};if(!kept)throw kept.error().code.value()=="results.host_controls.capacity"?Error::capacity:Error::source;
    require(kept->canonical()==c.canonical&&kept->usage().retained_bytes==control_stock(c),Error::encoding);return std::move(*kept);
}
auto controls_phase(const ControlsView& c,const detail::Children& children,const Limits& limits,std::size_t scratch,std::size_t upper)->std::size_t{const auto out=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),scratch),time_stock(c)),backing(children)),limits.controls.retained_bytes);bound(out,limits.controls_preparation_bytes);return out;}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
auto publication(Usage out,const Counts& count,const h::Owner* controls,const detail::Children& children,std::size_t upper,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.retained_bytes=plus(plus(plus(sizeof(Owner),sizeof(detail::State)),out.provenance_bytes),plus(out.canonical_bytes,backing(children)));std::size_t length{};
    if(controls){out.controls=controls->usage();out.retained_bytes=plus(out.retained_bytes,out.controls->retained_bytes-sizeof(h::Owner));length=controls->canonical().size();}
    if(children.topology){out.topology=children.topology->usage();length=plus(length,children.topology->canonical().size());}for(std::size_t i=0;i<6;++i)if(children.errors[i]){out.errors[i]=children.errors[i]->usage();length=plus(length,children.errors[i]->canonical().size());}
    out.publication_bytes=plus(plus(plus(plus(plus(detail::fixed_stock(),upper),out.retained_bytes),dom_stock(count)),times(length,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(std::optional<h::Owner>& controls,detail::Children& children,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->slot_state=view.slot_state;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.kind,view.sequence,view.failures,view.reason};state->controls=std::move(controls);state->children=std::move(children);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->controls?&*state->controls:nullptr,state->children);
    auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rg{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
// Logical named-cell allowance, not a literal stack/allocator measurement.
struct WireCells {
    Sink* emit_sink;const detail::View* emit_view;const h::Owner* emit_controls;const detail::Children* emit_children;
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
    Reader *reader_number_this,*optional_this,*reader_octets_this,*node_this,*object_reader_this,*array_reader_this,*text_reader_this,*expect_this,*key_this,*null_reader_this,*finished_this,*boolean_this,*uint32_this;
    std::size_t uint32_start;std::uint64_t uint32_value;
    std::string_view integer_value;char integer_character;std::uint64_t integer_result;std::from_chars_result integer_converted;
    std::string_view hex_value,unhex_value;std::string hex_result,unhex_result;std::size_t hex_index,unhex_index;unsigned char hex_byte;
    std::string_view valid_hex_value;char valid_hex_character,nibble_character;
    std::string_view enum_value;std::size_t enum_index,name_index;
    const std::array<const char*,2>* names_two;const std::array<const char*,3>* names_three;const std::array<const char*,4>* names_four;
    const std::array<const char*,5>* names_five;const std::array<const char*,6>* names_six;
    Kind name_kind;p::Domain name_domain;p::Producer name_reporter;SlotState name_state;Reason name_reason;h::State name_observation_state;h::Producer name_source_producer;h::Reason name_source_reason;h::SmtMode name_smt;h::ThpEnabled name_enabled;h::ThpDefrag name_defrag;
    std::string_view name_return;
    DigestText* digest_this;const core::Sha256Digest* digest_parameter;std::array<std::byte,32> digest_raw;std::size_t digest_index;unsigned digest_value;
    const core::Sha256Digest* digest_bytes_this;const std::array<std::byte,32>* digest_bytes_return;const DigestText* digest_view_this;std::string_view digest_view_return;
    FingerprintText* fingerprint_this;const core::Sha256Digest* fingerprint_parameter;DigestText fingerprint_suffix;const FingerprintText* fingerprint_view_this;std::string_view fingerprint_view_return,fingerprint_syntax_parameter;
    std::array<char,71>::iterator fingerprint_destination,fingerprint_copy_return,fingerprint_copy_n_return;std::array<char,64>::const_iterator fingerprint_begin,fingerprint_end;
    std::size_t plus_left,plus_right,times_left,times_right;std::optional<std::size_t> times_result;
    std::size_t* checked_value;std::size_t checked_amount,multiply_left,multiply_right,bound_value,bound_limit;bool require_value;Error require_error;
};
struct OperationCells {
    WireCells wire;
    detail::ResolvedInput *preflight_input,*phase_input,*prepare_input;const Limits *preflight_limits,*phase_limits;bool phase_topology;std::size_t phase_amount;
    Limits prepare_limits,decode_limits;std::string_view decode_body;
    Usage primitive_out,primitive_return,decode_usage,publication_parameter,publication_return,publish_usage;
    std::expected<void,Error> preflight_return,phase_return;std::expected<Owner,Error> owner_return;
    std::unexpected<Error> unexpected_return;Error caught;const std::length_error* caught_length;
    ControlsView preflight_controls,prepare_controls,parsed_controls_return,parse_controls_out,source_out;
    std::optional<ControlsView> decode_controls;
    Parsed decoded_parsed,parse_out;std::string decoded_controls,decoded_child;detail::Children decoded_children;
    std::optional<h::Owner> prepared_controls,decoded_kept;h::Owner retained_return;
    std::expected<t::Owner,t::Error> decoded_topology_result;std::expected<e::Owner,e::Error> decoded_error_result;
    DigestText decoded_digest;std::size_t decoded_index;
    Sink prepare_counter;std::unique_ptr<detail::State> decode_state,publish_state,constructor_state;Owner* constructor_this;Owner constructed_owner;
    const Limits *valid_limits_parameter,*primitive_limits,*input_limits,*controls_limits_parameter,*child_limits,*controls_phase_limits,*publication_limits,*retain_limits;
    std::array<std::size_t,4> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;std::string limits_string;Json::array_t limits_array;
    const detail::View *local_view,*reference_view,*primitive_view,*identity_view,*controls_references_view,*topology_view,*errors_view,*publish_view;
    bool local_retained;std::size_t local_index;const FailureReference* local_ref;
    std::size_t primitive_upper;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<decltype(identity_run)> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<decltype(identity_time)> identity_time_guard;
    const h::Owner* source_parameter;std::span<const h::Observation> source_observations;std::size_t source_index;const h::Observation* source_row;ObservationView* source_destination;
    std::string_view reference_text,reference_run;std::uint64_t reference_result;
    std::string_view id_text,id_run,id_marker;std::uint64_t id_ordinal;std::size_t id_index;Decimal id_number;
    const ControlsView *validate_controls_parameter,*control_stock_parameter,*time_stock_parameter,*controls_limits_view,*controls_references_controls,*topology_controls,*errors_controls,*retain_controls_view,*controls_phase_view;
    std::string_view validate_run;std::size_t validate_index;const ObservationView* validate_row;const h::SmtControl* validate_control;unsigned validate_reason;
    std::string_view inner_body;const Limits* inner_limits;Limits inner_reader_limits;Reader inner_reader;std::size_t inner_index;ObservationView* inner_row;std::string_view inner_failure;
    h::SmtMode inner_mode;std::optional<std::uint32_t> inner_threads;h::SmtControl inner_control;
    std::string_view parse_body;const Limits* parse_limits;Reader parse_reader;std::size_t parse_index;
    Reader* parse_reference_reader;std::optional<std::uint64_t> parse_reference_ordinal;std::uint64_t parse_reference_sequence;FailureReference parse_reference_result;
    std::size_t control_stock_out,time_stock_out;const ObservationView *stock_observation,*time_observation;Decimal stock_number;
    bool input_present;std::size_t input_scratch,input_upper,input_out;
    const detail::Children *backing_children,*phase_children,*topology_children,*errors_children,*retain_children,*controls_phase_children,*publication_children;
    std::size_t backing_out;const std::optional<e::Owner>* backing_child;t::Usage backing_topology_usage;e::Usage backing_error_usage;
    bool child_topology;std::size_t child_scratch,child_decoded,child_upper,child_allowance,child_floor,child_out;
    std::array<std::size_t,6> topology_max_values;std::initializer_list<std::size_t> topology_max_list;std::array<std::size_t,3> error_max_values;std::initializer_list<std::size_t> error_max_list;
    std::size_t controls_reference_index;const ObservationView* controls_reference_row;const std::optional<FailureReference>* controls_reference_ref;
    const t::Owner* topology_child;const t::Provenance* topology_provenance;const topology_evidence::Owner* topology_owner;
    DigestText topology_digest;FingerprintText topology_effective,topology_physical;
    std::size_t errors_index,errors_prior;const std::optional<FailureReference>* errors_ref;const std::optional<e::Owner>* errors_child;const e::Provenance* errors_provenance;const e::Association* errors_fact;
    p::Target errors_target;const ObservationView* errors_row;std::vector<p::Target>::const_iterator errors_find;
    std::expected<journal::RunId,core::Error> retain_run;core::detail::ExpectedErrorCleanup<decltype(retain_run)> retain_run_guard;
    std::array<std::optional<journal::Timestamp>,5> retain_times;h::Input retain_input;std::size_t retain_index;const ObservationView* retain_row;
    std::expected<journal::Timestamp,core::Error> retain_time;core::detail::ExpectedErrorCleanup<decltype(retain_time)> retain_time_guard;
    h::Slot retained_slot;std::expected<h::Owner,core::Error> retain_result;core::detail::ExpectedErrorCleanup<decltype(retain_result)> retain_guard;h::Limits retain_limits_copy;
    std::size_t controls_scratch,controls_upper,controls_out;
    const Counts *dom_counts,*publication_count,*publish_count;const h::Owner* publication_controls;std::size_t publication_upper,publication_length,publication_index,dom_return;
    std::optional<h::Owner>* publish_controls;detail::Children* publish_children;Origin publish_origin;Provenance provenance_temporary;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;Sink publish_sink;core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_guard;
    core::Sha256Digest digest_return,initial_digest;std::string_view initial_digest_input;detail::State* state_constructor_this;
    const h::Owner *controls_usage_this,*controls_canonical_this,*controls_digest_this,*controls_run_this,*controls_producer_this,*controls_ordinal_this,*controls_observations_this,*controls_topology_this;
    h::Usage controls_usage_return;std::string_view controls_canonical_return,controls_run_return;const core::Sha256Digest* controls_digest_return;std::optional<h::Producer> controls_producer_return;std::optional<std::uint64_t> controls_ordinal_return;std::span<const h::Observation> controls_observations_return;const h::TopologyBinding* controls_topology_return;
    const t::Owner *topology_usage_this,*topology_canonical_this,*topology_provenance_this,*topology_owner_this,*topology_state_this,*topology_error_this,*topology_reason_this;
    t::Usage topology_usage_return;std::string_view topology_canonical_return;const t::Provenance* topology_provenance_return;const topology_evidence::Owner* topology_owner_return;std::optional<t::SlotState> topology_state_return;const e::Owner* topology_error_return;std::optional<t::Reason> topology_reason_return;
    const topology_evidence::Owner *inner_topology_digest_this,*inner_topology_fingerprints_this;const core::Sha256Digest* inner_topology_digest_return;const epycbench::topology::TopologyFingerprints* inner_topology_fingerprints_return;
    const epycbench::topology::PhysicalTopologyFingerprint* physical_this;const epycbench::topology::EffectiveTopologyFingerprint* effective_this;const core::Sha256Digest *physical_return,*effective_return;
    const e::Owner *error_usage_this,*error_canonical_this,*error_provenance_this,*error_source_this;e::Usage error_usage_return;std::string_view error_canonical_return;const e::Provenance* error_provenance_return;const core::Error* error_source_return;
    std::size_t fixed_return,backing_return,input_phase_return,child_phase_return,controls_phase_return,control_stock_return,time_stock_return;
};
} // namespace

auto detail::checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto detail::checked_multiply(std::size_t left,std::size_t right)noexcept->std::optional<std::size_t>{if(right&&left>SIZE_MAX/right)return {};return left*right;}
auto detail::fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&)noexcept=default;
auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::~Owner()=default;
auto Owner::controls()const noexcept->const h::Owner*{return state_&&state_->controls?&*state_->controls:nullptr;}
auto Owner::topology()const noexcept->const t::Owner*{return state_&&state_->children.topology?&*state_->children.topology:nullptr;}
auto Owner::failure_error()const noexcept->const e::Owner*{return state_&&state_->children.errors[0]?&*state_->children.errors[0]:nullptr;}
auto Owner::observation_error(std::size_t i)const noexcept->const e::Owner*{return state_&&i<5&&state_->children.errors[i+1]?&*state_->children.errors[i+1]:nullptr;}
auto Owner::slot_state()const noexcept->std::optional<SlotState>{return state_?std::optional{state_->slot_state}:std::nullopt;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::preflight(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{valid_limits(limits);input.usage=primitive(input.view,limits,input.upper_fixed);local(input.view,input.controls!=nullptr);identity(input.view);
        input.usage.controls_input_bytes=input_phase(limits,input.controls!=nullptr,0,input.upper_fixed);
        if(input.controls){const auto controls=source_view(*input.controls);controls_references(input.view,controls);controls_limits(controls,limits);}return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::child_phase(detail::ResolvedInput& input,const Limits& limits,bool topology)->std::expected<void,Error>{
    try{require(input.children,Error::source);const auto amount=child_phase_stock(limits,*input.children,topology,0,0,input.upper_fixed);if(topology)input.usage.topology_preparation_bytes=amount;else input.usage.error_preparation_bytes=std::max(input.usage.error_preparation_bytes,amount);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{require(input.children,Error::source);std::optional<h::Owner> controls;
        if(input.controls){const auto source=source_view(*input.controls);topology_match(input.view,source,*input.children);errors_match(input.view,&source,*input.children);input.usage.controls_preparation_bytes=controls_phase(source,*input.children,limits,0,input.upper_fixed);controls=retain_controls(source,*input.children,limits);require(*controls->digest()==*input.controls->digest(),Error::source);}
        else{require(!input.children->topology,Error::child);errors_match(input.view,nullptr,*input.children);}
        Sink count;emit(count,input.view,controls?&*controls:nullptr,*input.children);input.usage=publication(input.usage,count.count,controls?&*controls:nullptr,*input.children,input.upper_fixed,limits);
        return Owner{publish(controls,*input.children,input.view,count.count,input.usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits,0);local(parsed.view,parsed.retained);require(parsed.rows==parsed.retained);identity(parsed.view);
        for(std::size_t i=0;i<6;++i)require(parsed.view.failures[i].has_value()==!parsed.errors[i].empty());
        detail::Children children;std::optional<h::Owner> controls;
        usage.controls_input_bytes=input_phase(limits,parsed.retained,parsed.controls.size()/2,0);
        {
            const auto canonical=unhex(parsed.controls);std::optional<ControlsView> source;
            if(parsed.retained){source=parse_controls(canonical,limits);controls_references(parsed.view,*source);controls_limits(*source,limits);require(source->bound==!parsed.topology.empty(),Error::child);}
            else require(parsed.topology.empty());
            if(!parsed.topology.empty()){usage.topology_preparation_bytes=child_phase_stock(limits,children,true,canonical.size(),parsed.topology.size()/2,0);const auto bytes=unhex(parsed.topology);auto decoded=t::Owner::decode(bytes,limits.topology);require(decoded.has_value(),Error::child);children.topology=std::move(*decoded);}
            if(source)topology_match(parsed.view,*source,children);
            for(std::size_t i=0;i<6;++i)if(!parsed.errors[i].empty()){usage.error_preparation_bytes=std::max(usage.error_preparation_bytes,child_phase_stock(limits,children,false,canonical.size(),parsed.errors[i].size()/2,0));const auto bytes=unhex(parsed.errors[i]);auto decoded=e::Owner::decode(bytes,limits.child);require(decoded.has_value(),Error::child);children.errors[i]=std::move(*decoded);}
            errors_match(parsed.view,source?&*source:nullptr,children);
            if(source){usage.controls_preparation_bytes=controls_phase(*source,children,limits,canonical.size(),0);controls=retain_controls(*source,children,limits);const DigestText digest{*controls->digest()};require(digest.view()==parsed.digest,Error::encoding);}
        }
        usage=publication(usage,parsed.wire,controls?&*controls:nullptr,children,0,limits);auto state=publish(controls,children,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::system_controls_serialization
