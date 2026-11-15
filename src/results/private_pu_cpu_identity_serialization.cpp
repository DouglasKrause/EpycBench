#include "results/private_pu_cpu_identity_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace epycbench::results::pu_identity_serialization {
namespace {
using Json=nlohmann::json;
namespace h=pu_cpu_identity;
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
    auto unsigned_number()->std::uint32_t{node();const auto begin=at_;while(at_<body_.size()&&body_[at_]>='0'&&body_[at_]<='9')++at_;const auto value=integer(body_.substr(begin,at_-begin));require(value<=UINT32_MAX,Error::encoding);return static_cast<std::uint32_t>(value);}
    auto boolean()->bool{node();if(body_.substr(at_,4)=="true"){expect("true");return true;}expect("false");return false;}
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
constexpr std::array states{"not_attempted","available","unavailable","failed"};
constexpr std::array observation_reasons{"not_supported","source_missing","access_denied","read_failed","malformed_source","unsupported_value"};
constexpr std::array producers{"controller","runner"};
constexpr std::array slots{"signature","microcode"};
constexpr std::array selectors{p::Selector::signature,p::Selector::microcode};
struct SourceView {
    std::string_view canonical,run,unrepresented,digest,effective,physical;
    std::uint64_t ordinal{};h::Producer producer{};std::size_t rows{},errors{},stock{},typed_bytes{};
    const h::TopologyBinding* binding{};
};
struct Parsed {
    detail::View view{};Counts wire;std::string_view source,digest,topology,preparation_error;
    std::size_t rows{},errors{};bool retained{},error_rows{};
};
struct TypedRow {
    std::optional<epycbench::topology::StableProcessingUnitId> stable;
    std::array<std::optional<journal::Timestamp>,2> times;
};
auto failure_sequence(std::string_view value,std::string_view run)->std::uint64_t {
    require(value.size()>run.size()+1&&value.starts_with(run)&&value[run.size()]==':',Error::reference);
    const auto result=integer(value.substr(run.size()+1));require(result!=0,Error::reference);return result;
}
void observation_id(std::string_view id,std::string_view run,std::uint64_t ordinal,std::uint32_t cpu,std::size_t slot){
    require(id.starts_with(run),Error::identity);id.remove_prefix(run.size());
    constexpr std::string_view marker=":cpu-identity:";require(id.starts_with(marker),Error::identity);id.remove_prefix(marker.size());
    const Decimal ordinal_text{ordinal},cpu_text{cpu};require(id.starts_with(ordinal_text.view()),Error::identity);id.remove_prefix(ordinal_text.size);
    require(id.starts_with(":"),Error::identity);id.remove_prefix(1);require(id.starts_with(cpu_text.view()),Error::identity);id.remove_prefix(cpu_text.size);
    require(id.starts_with(":"),Error::identity);id.remove_prefix(1);require(id==slots[slot],Error::identity);
}
auto word(std::string_view text)->std::uint32_t{
    require(text.size()==10&&text.starts_with("0x"),Error::encoding);std::uint32_t value{};
    for(char ch:text.substr(2))value=(value<<4U)|nibble(ch);
    return value;
}
auto signature(std::uint32_t eax)->h::DecodedSignature{
    const auto base=(eax>>8U)&15U;const auto family=base==15?base+((eax>>20U)&255U):base;
    require(family<=255);const auto model=((eax>>4U)&15U)+(base==15?((eax>>16U)&15U)*16U:0U);
    return {eax,family,model,eax&15U};
}
void fingerprint_syntax(std::string_view text){require(text.size()==71&&text.starts_with("sha256:"),Error::encoding);valid_hex(text.substr(7));}
void validate_observation(const detail::ObservationView& row,const SourceView& source,std::uint32_t cpu,std::size_t slot){
    observation_id(row.id,source.run,source.ordinal,cpu,slot);(void)name(row.state,states);if(row.reason)(void)name(*row.reason,observation_reasons);
    if(row.state==h::State::not_attempted){require(row.time.empty()&&row.value.index()==0&&!row.reason&&!row.failure);return;}
    require(row.time.size()==30,Error::identity);
    if(row.state==h::State::available){require(row.value.index()==slot+1&&!row.reason&&!row.failure);if(slot==0){const auto value=std::get<h::DecodedSignature>(row.value);require(signature(value.eax)==value);}}
    else{require(row.value.index()==0&&row.reason.has_value());const auto reason=static_cast<unsigned>(*row.reason);
        require(row.state==h::State::unavailable?(reason<3&&!row.failure):(reason>=3&&row.failure&&*row.failure!=0));}
}
void account_row(SourceView& source,const detail::RowView& row){
    require(row.stable.size()<=13&&row.stable.starts_with("pu/"),Error::identity);
    require(integer(row.stable.substr(3))==row.cpu.value(),Error::identity);
    source.stock=plus(source.stock,plus(sizeof(h::OwnedRow),row.stable.size()));source.typed_bytes=plus(source.typed_bytes,row.stable.size());
    for(std::size_t i=0;i<2;++i){const auto& slot=row.slots[i];validate_observation(slot,source,row.cpu.value(),i);
        source.stock=plus(source.stock,plus(slot.id.size(),slot.time.size()));source.typed_bytes=plus(source.typed_bytes,slot.time.size());
        if(slot.failure){source.errors=plus(source.errors,1);const Decimal number{*slot.failure};source.stock=plus(source.stock,plus(source.run.size()+1,number.size));}}
}
auto source_view(const h::Owner& owner,const Limits& limits,std::span<detail::RowView> rows={})->SourceView{
    require(owner.digest()&&owner.topology()&&owner.ordinal()&&owner.producer(),Error::source);
    SourceView out;out.canonical=owner.canonical();out.run=owner.run_id();out.unrepresented=owner.unrepresented_present_cpus();out.ordinal=*owner.ordinal();out.producer=*owner.producer();out.binding=owner.topology();out.rows=owner.rows().size();
    bound(out.rows,limits.rows);bound(out.rows,limits.identity.processing_units);bound(times(out.rows,2),limits.observations);bound(times(out.rows,2),limits.identity.observations);
    require(rows.empty()||rows.size()==out.rows,Error::source);out.stock=plus(sizeof(h::Owner),plus(plus(out.run.size(),out.canonical.size()),out.unrepresented.size()));out.typed_bytes=36;
    std::uint32_t previous{};std::size_t i{};
    for(const auto& owned:owner.rows()){
        require(i==0||previous<owned.linux_cpu.value(),Error::source);previous=owned.linux_cpu.value();
        detail::RowView row{owned.stable_pu_id,owned.linux_cpu,owned.topology_online,{},{}};
        const std::array<const h::Observation*,2> observations{&owned.signature,&owned.microcode};
        for(std::size_t slot=0;slot<2;++slot){const auto& input=*observations[slot];auto& output=row.slots[slot];output.id=input.id;output.state=input.state;output.value=input.value;output.time=input.observed_at?std::string_view{*input.observed_at}:std::string_view{};output.reason=input.reason;if(input.failure_event)output.failure=failure_sequence(*input.failure_event,out.run);}
        account_row(out,row);if(!rows.empty())rows[i]=row;++i;
    }
    require(owner.usage().retained_bytes==out.stock,Error::source);return out;
}
auto parse_source(std::string_view bytes,const Limits& limits,std::span<detail::RowView> rows={})->SourceView{
    Limits inner{};inner.canonical_bytes=limits.identity.canonical_bytes;inner.text_bytes=limits.identity.canonical_bytes;inner.nodes=23563;inner.depth=6;
    Reader reader{bytes,inner};SourceView out;out.canonical=bytes;out.typed_bytes=36;
    // The run and ordinal precede validation of derived IDs in the second pass below.
    reader.object();reader.key("ordinal");out.ordinal=reader.number();reader.key("processing_units");reader.array();
    const auto row_start=reader.position();std::uint32_t previous{};
    while(!reader.next(']')){
        bound(plus(out.rows,1),limits.rows);bound(plus(out.rows,1),limits.identity.processing_units);bound(times(plus(out.rows,1),2),limits.observations);bound(times(plus(out.rows,1),2),limits.identity.observations);
        detail::RowView row{};reader.object();reader.key("linux_cpu");row.cpu=core::LinuxCpuId{reader.unsigned_number()};require(out.rows==0||previous<row.cpu.value());previous=row.cpu.value();
        for(const auto slot:{std::size_t{1},std::size_t{0}}){reader.key(slots[slot]);reader.object();auto& observed=row.slots[slot];reader.key("failure_event");std::string_view failure;if(reader.next('n'))reader.null();else failure=reader.text();
            reader.key("id");observed.id=reader.text();reader.key("observed_at");if(reader.next('n'))reader.null();else observed.time=reader.text();reader.key("reason");if(reader.next('n'))reader.null();else observed.reason=enumerated<h::Reason>(reader.text(),observation_reasons);
            reader.key("state");observed.state=enumerated<h::State>(reader.text(),states);reader.key("value");if(reader.next('n'))reader.null();else{reader.object();if(slot==0){reader.key("eax");const auto eax=word(reader.text());reader.key("family");const auto family=reader.unsigned_number();reader.key("model");const auto model=reader.unsigned_number();reader.key("stepping");const auto stepping=reader.unsigned_number();observed.value=h::DecodedSignature{eax,family,model,stepping};require(signature(eax)==std::get<h::DecodedSignature>(observed.value));}else{reader.key("revision");observed.value=h::Microcode{word(reader.text())};}reader.end_object();}
            if(!failure.empty()){require(failure.size()>37);observed.failure=failure_sequence(failure,failure.substr(0,36));require(observed.id.starts_with(failure.substr(0,36)),Error::reference);}reader.end_object();}
        reader.key("stable_pu_id");row.stable=reader.text();reader.key("topology_online");row.online=reader.boolean();reader.end_object();
        // Derive a fixed-width run view for this row, then compare it to the root.
        require(row.slots[0].id.size()>36&&row.slots[1].id.size()>36,Error::identity);
        if(out.rows==0)out.run=row.slots[0].id.substr(0,36);
        account_row(out,row);if(!rows.empty()){require(out.rows<rows.size());rows[out.rows]=row;}++out.rows;
    }
    (void)row_start;reader.end_array();reader.key("producer");out.producer=enumerated<h::Producer>(reader.text(),producers);reader.key("run_id");const auto run=reader.text();require(run.size()==36&&(out.rows==0||out.run==run),Error::identity);out.run=run;
    reader.key("schema");require(reader.text()==h::profile);reader.key("topology");reader.object();reader.key("canonical_sha256");out.digest=reader.octets();require(out.digest.size()==64);reader.key("effective_fingerprint");out.effective=reader.text();fingerprint_syntax(out.effective);reader.key("physical_fingerprint");out.physical=reader.text();fingerprint_syntax(out.physical);reader.end_object();
    reader.key("unrepresented_present_cpus");out.unrepresented=reader.text();reader.end_object();reader.finished();require(rows.empty()||rows.size()==out.rows);
    out.stock=plus(out.stock,plus(sizeof(h::Owner),plus(plus(out.run.size(),bytes.size()),out.unrepresented.size())));return out;
}
void emit_reference(Sink& sink,std::string_view key,const FailureReference& ref){sink.object(key);if(ref.creation_ordinal)sink.number("creation_ordinal",*ref.creation_ordinal);else sink.null("creation_ordinal");sink.number("sequence",ref.sequence);sink.end();}
void emit_error(Sink& sink,std::string_view key,const std::optional<e::Owner>& error,const std::optional<FailureReference>& reference){
    if(!error){sink.null(key);return;}require(reference.has_value(),Error::source);sink.object(key);sink.octets("error_hex",error->canonical());emit_reference(sink,"reference",*reference);sink.end();
}
void emit(Sink& sink,const detail::View& view,const h::Owner* identity,const detail::Children& children){
    sink.object();sink.text("domain",name(view.domain,domains));sink.text("entry_producer",name(view.reporter,reporters));
    if(children.preparation_error)sink.octets("failure_error_hex",children.preparation_error->canonical());else sink.null("failure_error_hex");
    if(view.failure)emit_reference(sink,"failure_reference",*view.failure);else sink.null("failure_reference");sink.text("host_kind",name(view.kind,kinds));
    if(identity){sink.array("observation_errors",children.rows.size());for(const auto& row:children.rows){sink.object();sink.number("linux_cpu",row.cpu.value());emit_error(sink,"microcode",row.errors[1],row.references[1]);emit_error(sink,"signature",row.errors[0],row.references[0]);sink.end();}sink.end();}else sink.null("observation_errors");
    if(identity){require(children.topology.has_value(),Error::child);sink.object("pu_identity");sink.octets("canonical_hex",identity->canonical());const DigestText digest{*identity->digest()};sink.text("canonical_sha256",digest.view());sink.text("profile",h::profile);sink.octets("topology_hex",children.topology->canonical());sink.end();}else sink.null("pu_identity");
    if(view.reason)sink.text("reason",name(*view.reason,reasons));else sink.null("reason");sink.text("run_id",view.run);sink.text("schema",profile);sink.number("sequence",view.sequence);sink.text("state",name(view.slot_state,availabilities));sink.text("timestamp",view.time);sink.end();
}
auto parse_reference(Reader& reader)->FailureReference{reader.object();reader.key("creation_ordinal");const auto ordinal=reader.optional();reader.key("sequence");const auto sequence=reader.number();reader.end_object();return {sequence,ordinal};}
auto parse(std::string_view body,const Limits& limits,std::span<detail::RowView> rows={})->Parsed{
    Reader reader{body,limits};Parsed out;reader.object();reader.key("domain");out.view.domain=enumerated<p::Domain>(reader.text(),domains);reader.key("entry_producer");out.view.reporter=enumerated<p::Producer>(reader.text(),reporters);
    reader.key("failure_error_hex");if(reader.next('n'))reader.null();else{out.preparation_error=reader.octets();require(!out.preparation_error.empty());bound(out.preparation_error.size()/2,limits.child.canonical_bytes);}
    reader.key("failure_reference");if(reader.next('n'))reader.null();else out.view.failure=parse_reference(reader);reader.key("host_kind");out.view.kind=enumerated<Kind>(reader.text(),kinds);
    reader.key("observation_errors");if(reader.next('n'))reader.null();else{out.error_rows=true;reader.array();std::uint32_t previous{};while(!reader.next(']')){
        bound(plus(out.rows,1),limits.rows);bound(times(plus(out.rows,1),2),limits.observations);reader.object();reader.key("linux_cpu");const auto cpu=reader.number();require(cpu<=UINT32_MAX&&(out.rows==0||previous<cpu));previous=static_cast<std::uint32_t>(cpu);
        if(!rows.empty())require(out.rows<rows.size()&&rows[out.rows].cpu.value()==cpu,Error::identity);
        for(const auto slot:{std::size_t{1},std::size_t{0}}){reader.key(slots[slot]);detail::WireError wire;if(reader.next('n'))reader.null();else{reader.object();reader.key("error_hex");wire.bytes=reader.octets();require(!wire.bytes.empty());bound(wire.bytes.size()/2,limits.child.canonical_bytes);reader.key("reference");wire.reference=parse_reference(reader);reader.end_object();out.errors=plus(out.errors,1);bound(out.errors,limits.error_children);}if(!rows.empty())rows[out.rows].wire[slot]=wire;}
        reader.end_object();++out.rows;
    }reader.end_array();}
    reader.key("pu_identity");if(reader.next('n'))reader.null();else{out.retained=true;reader.object();reader.key("canonical_hex");out.source=reader.octets();bound(out.source.size()/2,limits.identity.canonical_bytes);reader.key("canonical_sha256");out.digest=reader.octets();require(out.digest.size()==64);reader.key("profile");require(reader.text()==h::profile);reader.key("topology_hex");out.topology=reader.octets();require(!out.topology.empty());bound(out.topology.size()/2,limits.topology.canonical_bytes);reader.end_object();}
    reader.key("reason");if(reader.next('n'))reader.null();else out.view.reason=enumerated<Reason>(reader.text(),reasons);reader.key("run_id");out.view.run=reader.text();reader.key("schema");require(reader.text()==profile);reader.key("sequence");out.view.sequence=reader.number();reader.key("state");out.view.slot_state=enumerated<SlotState>(reader.text(),availabilities);reader.key("timestamp");out.view.time=reader.text();reader.end_object();reader.finished();out.wire=reader.count;
    require(out.retained==out.error_rows&&out.view.failure.has_value()==!out.preparation_error.empty());require(rows.empty()||out.rows==rows.size());return out;
}
void valid_limits(const Limits& limits){
    require(limits.rows<=1024&&limits.observations<=2048&&limits.error_children<=2048&&limits.identity.processing_units<=1024&&limits.identity.observations<=2048,Error::invalid_limits);
    require(limits.depth<=6&&limits.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
    for(auto n:{limits.text_bytes,limits.canonical_bytes,limits.provenance_bytes,limits.identity.canonical_bytes})require(n<=std::string{}.max_size(),Error::invalid_limits);
}
void reference_local(const detail::View& view,const FailureReference& ref){require(ref.sequence&&ref.sequence<=view.sequence,Error::reference);if(ref.creation_ordinal)require(ref.sequence==1&&*ref.creation_ordinal<1024,Error::reference);if(view.kind==Kind::genesis)require(ref.sequence==1&&ref.creation_ordinal.has_value(),Error::reference);if(view.kind==Kind::ordinary&&ref.sequence==view.sequence)require(!ref.creation_ordinal,Error::reference);}
void local(const detail::View& view,bool retained){
    require(view.sequence,Error::identity);(void)name(view.domain,domains);(void)name(view.reporter,reporters);(void)name(view.kind,kinds);(void)name(view.slot_state,availabilities);if(view.reason)(void)name(*view.reason,reasons);
    if(view.kind==Kind::genesis)require(view.sequence==1,Error::reference);
    if(view.sequence==1)require(view.reporter==p::Producer::controller,Error::identity);
    require(retained==(view.slot_state==SlotState::retained));require(view.failure.has_value()==(view.slot_state==SlotState::failed));require(view.reason.has_value()==(view.slot_state==SlotState::unavailable));if(view.failure)reference_local(view,*view.failure);
}
auto base(std::size_t upper,std::size_t dynamic)->std::size_t{return plus(plus(detail::fixed_stock(),upper),dynamic);}
auto primitive(const detail::View& view,const Limits& limits,std::size_t upper)->Usage{Usage out;out.provenance_bytes=plus(view.run.size(),view.time.size());bound(out.provenance_bytes,limits.provenance_bytes);bound(plus(base(upper,0),out.provenance_bytes),limits.publication_bytes);return out;}
void identity(const detail::View& view){auto run=journal::RunId::parse(view.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);auto time=journal::Timestamp::parse(view.time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);}
void source_limits(const SourceView& source,const detail::View& view,const Limits& limits){
    require(source.run==view.run,Error::identity);bound(source.rows,limits.rows);bound(source.rows,limits.identity.processing_units);bound(times(source.rows,2),limits.observations);bound(times(source.rows,2),limits.identity.observations);bound(source.errors,limits.error_children);bound(source.stock,limits.identity.retained_bytes);bound(source.canonical.size(),limits.identity.canonical_bytes);
}
auto input_phase(const Limits& limits,bool present,std::size_t scratch,std::size_t upper,std::size_t dynamic)->std::size_t{if(!present)return 0;const auto out=plus(base(upper,dynamic),scratch);bound(out,limits.identity_input_bytes);return out;}
auto row_stock(std::size_t count)->std::size_t{return times(count,sizeof(detail::RowView)+sizeof(ErrorRow));}
auto staging_phase(const Limits& limits,std::size_t count,std::size_t scratch,std::size_t upper,std::size_t dynamic)->std::size_t{const auto out=plus(plus(base(upper,dynamic),scratch),row_stock(count));bound(out,limits.row_staging_bytes);return out;}
auto backing(const detail::Children& children)->std::size_t{
    std::size_t out{};if(children.topology){const auto usage=children.topology->usage();require(usage.retained_bytes>=sizeof(t::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(t::Owner));}
    if(children.preparation_error){const auto usage=children.preparation_error->usage();require(usage.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(e::Owner));}
    for(const auto& row:children.rows)for(const auto& child:row.errors)if(child){const auto usage=child->usage();require(usage.retained_bytes>=sizeof(e::Owner),Error::child);out=plus(out,usage.retained_bytes-sizeof(e::Owner));}
    return out;
}
auto child_phase_stock(const Limits& limits,const detail::Children& children,bool topology,std::size_t scratch,std::size_t decoded,std::size_t upper,std::size_t dynamic)->std::size_t{
    std::size_t allowance{};
    if(topology){allowance=std::max({limits.topology.topology_decode_bytes,limits.topology.topology_preparation_bytes,limits.topology.error_preparation_bytes,limits.topology.publication_bytes,limits.topology.retained_bytes,plus(t::detail::fixed_stock(),upper?t::detail::session_fixed_stock():0)});bound(decoded,limits.topology.canonical_bytes);}
    else{const auto floor=upper?plus(e::detail::fixed_stock(),e::detail::session_fixed_stock()):e::detail::decode_fixed_stock();allowance=std::max({limits.child.consumer_simultaneous_bytes,limits.child.retained_bytes,floor});bound(decoded,limits.child.canonical_bytes);}
    const auto out=plus(plus(plus(plus(plus(base(upper,dynamic),scratch),row_stock(children.rows.size())),backing(children)),decoded),allowance);bound(out,topology?limits.topology_preparation_bytes:limits.error_preparation_bytes);return out;
}
void topology_match(const detail::View& view,const SourceView& source,const detail::Children& children){
    require(children.topology.has_value(),Error::child);const auto& child=*children.topology;const auto* provenance=child.provenance();const auto* topology=child.topology();
    require(provenance&&topology&&child.slot_state()==t::SlotState::retained&&!child.failure_error()&&!child.reason(),Error::child);
    require(provenance->run==view.run&&provenance->time==view.time&&provenance->domain==view.domain&&provenance->reporter==view.reporter&&provenance->kind==view.kind&&provenance->sequence==view.sequence,Error::child);
    if(source.binding)require(*topology->digest()==source.binding->canonical_sha256&&*topology->fingerprints()==source.binding->fingerprints,Error::child);
    else{const DigestText digest{*topology->digest()};const FingerprintText effective{topology->fingerprints()->effective.digest()},physical{topology->fingerprints()->physical.digest()};require(digest.view()==source.digest&&effective.view()==source.effective&&physical.view()==source.physical,Error::child);}
}
void error_match(const detail::View& view,const std::optional<FailureReference>& reference,const std::optional<e::Owner>& child,const p::Target& target,std::optional<p::Producer> producer){
    require(reference.has_value()==child.has_value(),Error::child);if(!child)return;reference_local(view,*reference);
    const auto* value=child->provenance();require(value&&child->error()&&value->association,Error::child);const auto& fact=*value->association;
    require(value->run==view.run&&value->domain==view.domain&&value->sequence==reference->sequence&&value->creation_ordinal==reference->creation_ordinal&&value->kind==(reference->creation_ordinal?e::Kind::creation_failure:e::Kind::ordinary_failure),Error::child);
    if(reference->sequence==view.sequence)require(value->time==view.time&&value->entry_producer==view.reporter,Error::child);
    require(!fact.process_sequence&&(!producer||fact.producer==*producer),Error::child);require(std::find(fact.targets.begin(),fact.targets.end(),target)!=fact.targets.end(),Error::child);
}
void errors_match(const detail::View& view,const SourceView* source,std::span<const detail::RowView> rows,const detail::Children& children){
    error_match(view,view.failure,children.preparation_error,{p::Category::pu_identity},{});
    if(!source){require(rows.empty()&&children.rows.empty()&&!children.topology,Error::child);return;}
    require(rows.size()==source->rows&&children.rows.size()==rows.size(),Error::child);
    for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];const auto& owned=children.rows[i];require(row.cpu==owned.cpu,Error::identity);
        for(std::size_t slot=0;slot<2;++slot){const auto& observed=row.slots[slot];const auto& reference=owned.references[slot];const auto& child=owned.errors[slot];require(observed.failure.has_value()==reference.has_value(),Error::reference);if(!reference){require(!child,Error::child);continue;}require(observed.failure==reference->sequence&&observed.reason,Error::reference);
            const p::Target target{p::Category::pu_identity,selectors[slot],source->ordinal,row.cpu.value(),static_cast<p::FailureReason>(static_cast<unsigned>(*observed.reason)-2U)};
            error_match(view,reference,child,target,source->producer==h::Producer::controller?p::Producer::controller:p::Producer::runner);
            for(std::size_t previous=0;previous<=i;++previous)for(std::size_t earlier=0;earlier<2;++earlier){if(previous==i&&earlier>=slot)break;const auto& prior=children.rows[previous];if(prior.references[earlier]==reference)require(prior.errors[earlier]&&prior.errors[earlier]->canonical()==child->canonical(),Error::child);}
        }
    }
}
auto source_phase(const SourceView& source,const detail::Children& children,const Limits& limits,std::size_t scratch,std::size_t upper,std::size_t dynamic)->std::size_t{
    const auto typed=times(source.rows,sizeof(TypedRow)+sizeof(h::Row));const auto out=plus(plus(plus(plus(plus(plus(plus(base(upper,dynamic),scratch),row_stock(source.rows)),backing(children)),typed),source.typed_bytes),limits.identity.retained_bytes),0);
    bound(out,limits.identity_preparation_bytes);return out;
}
auto retain_source(const SourceView& source,std::span<const detail::RowView> rows,const detail::Children& children,const Limits& limits)->h::Owner{
    auto run=journal::RunId::parse(source.run);core::detail::ExpectedErrorCleanup rg{run};require(run.has_value(),Error::identity);require(children.topology&&children.topology->topology(),Error::child);
    std::vector<TypedRow> typed;typed.resize(rows.size());std::vector<h::Row> inputs;inputs.reserve(rows.size());
    for(std::size_t i=0;i<rows.size();++i){auto id=epycbench::topology::StableProcessingUnitId::parse(rows[i].stable);core::detail::ExpectedErrorCleanup ig{id};require(id.has_value(),Error::identity);typed[i].stable=std::move(*id);
        for(std::size_t slot=0;slot<2;++slot)if(!rows[i].slots[slot].time.empty()){auto time=journal::Timestamp::parse(rows[i].slots[slot].time);core::detail::ExpectedErrorCleanup tg{time};require(time.has_value(),Error::identity);typed[i].times[slot]=std::move(*time);}}
    for(std::size_t i=0;i<rows.size();++i){std::array<h::Slot,2> slots_input;for(std::size_t slot=0;slot<2;++slot){const auto& observed=rows[i].slots[slot];auto& input=slots_input[slot];input.state=observed.state;input.observed_at=typed[i].times[slot]?&*typed[i].times[slot]:nullptr;input.reason=observed.reason;input.failure_event=observed.failure;if(observed.value.index()==1)input.value=h::Signature{std::get<h::DecodedSignature>(observed.value).eax};else if(observed.value.index()==2)input.value=std::get<h::Microcode>(observed.value);}inputs.push_back({*typed[i].stable,rows[i].cpu,slots_input[0],slots_input[1]});}
    const h::Input input{*run,source.ordinal,source.producer,*children.topology->topology(),inputs};auto kept=h::retain(input,limits.identity);core::detail::ExpectedErrorCleanup cleanup{kept};if(!kept)throw kept.error().code.value()=="results.pu_cpu_identity.capacity"?Error::capacity:Error::source;
    require(kept->canonical()==source.canonical&&kept->usage().retained_bytes==source.stock,Error::encoding);return std::move(*kept);
}
auto dom_stock(const Counts& count)->std::size_t{return plus(plus(plus(times(count.objects,sizeof(Json::object_t)),times(count.arrays,sizeof(Json::array_t))),plus(times(count.members,sizeof(Json::object_t::value_type)),times(count.elements,sizeof(Json)))),plus(times(count.strings,sizeof(Json::string_t)),count.text));}
auto publication(Usage out,const Counts& count,const h::Owner* source,const detail::Children& children,std::size_t upper,std::size_t dynamic,const Limits& limits)->Usage{
    out.nodes=count.nodes;out.depth=count.depth;out.text_bytes=count.text;out.canonical_bytes=count.bytes;bound(out.nodes,limits.nodes);bound(out.depth,limits.depth);bound(out.text_bytes,limits.text_bytes);bound(out.canonical_bytes,limits.canonical_bytes);
    out.retained_bytes=plus(plus(plus(plus(sizeof(Owner),sizeof(detail::State)),out.provenance_bytes),plus(count.bytes,times(children.rows.size(),sizeof(ErrorRow)))),backing(children));std::size_t length{};
    if(source){out.identity=source->usage();out.retained_bytes=plus(out.retained_bytes,out.identity->retained_bytes-sizeof(h::Owner));length=source->canonical().size();}
    if(children.topology){out.topology=children.topology->usage();length=plus(length,children.topology->canonical().size());}
    if(children.preparation_error){out.preparation_error=children.preparation_error->usage();length=plus(length,children.preparation_error->canonical().size());}
    for(const auto& row:children.rows)for(const auto& child:row.errors)if(child)length=plus(length,child->canonical().size());
    out.publication_bytes=plus(plus(plus(plus(base(upper,dynamic),out.retained_bytes),dom_stock(count)),times(length,2)),plus(out.provenance_bytes,21));bound(out.retained_bytes,limits.retained_bytes);bound(out.publication_bytes,limits.publication_bytes);return out;
}
auto publish(std::optional<h::Owner>& source,detail::Children& children,const detail::View& view,const Counts& count,Usage usage,Origin origin)->std::unique_ptr<detail::State>{
    auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=origin;state->slot_state=view.slot_state;state->provenance={std::string{view.run},std::string{view.time},view.domain,view.reporter,view.kind,view.sequence,view.failure,view.reason};state->identity=std::move(source);state->children=std::move(children);
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit(sink,view,state->identity?&*state->identity:nullptr,state->children);auto rendered=core::canonicalize_json_jcs(dom,{count.bytes,count.depth,count.nodes});core::detail::ExpectedErrorCleanup rg{rendered};require(rendered.has_value()&&rendered->size()==count.bytes,Error::encoding);state->canonical=std::move(*rendered);state->digest=core::sha256(state->canonical);return state;
}
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
    Kind name_kind;p::Domain name_domain;p::Producer name_reporter;SlotState name_state;Reason name_reason;h::State name_observation_state;h::Producer name_source_producer;h::Reason name_source_reason;
    std::string_view name_return;
    DigestText* digest_this;const core::Sha256Digest* digest_parameter;std::array<std::byte,32> digest_raw;std::size_t digest_index;unsigned digest_value;
    const core::Sha256Digest* digest_bytes_this;const std::array<std::byte,32>* digest_bytes_return;const DigestText* digest_view_this;std::string_view digest_view_return;
    FingerprintText* fingerprint_this;const core::Sha256Digest* fingerprint_parameter;DigestText fingerprint_suffix;const FingerprintText* fingerprint_view_this;std::string_view fingerprint_view_return,fingerprint_syntax_parameter;
    std::array<char,71>::iterator fingerprint_destination,fingerprint_copy_return,fingerprint_copy_n_return;std::array<char,64>::const_iterator fingerprint_begin,fingerprint_end;
    std::size_t unsigned_begin;std::uint64_t unsigned_value;Reader *unsigned_this,*boolean_this;std::uint32_t unsigned_return;bool boolean_return;
    std::size_t plus_left,plus_right,times_left,times_right;std::optional<std::size_t> times_result;
    std::size_t* checked_value;std::size_t checked_amount,multiply_left,multiply_right,bound_value,bound_limit;bool require_value;Error require_error;
};
// Sum of concrete named logical cells; not compiler stack or allocator bookkeeping.
struct OperationCells {
    WireCells wire;
    const h::Owner* before_source;const detail::View* before_view;const Limits* before_limits;std::size_t before_upper,before_dynamic;
    detail::ResolvedInput *preflight_input,*phase_input,*prepare_input;const Limits *preflight_limits,*phase_limits;
    bool phase_topology;std::size_t phase_amount;Limits prepare_limits,decode_limits;std::string_view decode_body;
    Usage before_usage,primitive_out,primitive_return,decode_usage,publication_parameter,publication_return,publish_usage;
    std::expected<std::size_t,Error> before_return;std::expected<void,Error> preflight_return,phase_return;
    std::expected<Owner,Error> owner_return;std::unexpected<Error> unexpected_return;Error caught;const std::length_error* caught_length;
    SourceView before_info,preflight_info,prepare_info,decoded_info,source_out,parsed_source_out;
    Parsed decoded_parsed,parse_out,filled_parse_return;
    std::string decoded_source,decoded_topology,decoded_error;
    detail::Children decoded_children;std::vector<detail::RowView> decoded_rows;
    std::optional<h::Owner> prepared_source,decoded_kept;h::Owner retained_return;
    std::expected<t::Owner,t::Error> decoded_topology_result;std::expected<e::Owner,e::Error> decoded_error_result;
    DigestText decoded_digest;std::size_t decoded_index,decoded_slot;std::string_view decoded_encoded;
    Sink prepare_counter;std::unique_ptr<detail::State> decode_state,publish_state,constructor_state;Owner* constructor_this;Owner constructed_owner;
    const Limits *valid_limits_parameter,*primitive_limits,*source_limits_parameter,*input_limits,*staging_limits,*child_limits,*source_phase_limits,*publication_limits,*retain_limits;
    std::array<std::size_t,4> limits_values;std::initializer_list<std::size_t> limits_list;std::size_t limits_value;std::string limits_string;Json::array_t limits_array;
    const detail::View *local_view,*reference_view,*primitive_view,*identity_view,*source_limits_view,*topology_view,*errors_view,*error_view,*publish_view;
    bool local_retained;const FailureReference* local_ref;std::size_t primitive_upper,base_upper,base_dynamic;
    std::expected<journal::RunId,core::Error> identity_run;core::detail::ExpectedErrorCleanup<decltype(identity_run)> identity_run_guard;
    std::expected<journal::Timestamp,core::Error> identity_time;core::detail::ExpectedErrorCleanup<decltype(identity_time)> identity_time_guard;
    const h::Owner* source_parameter;const Limits* source_limits;std::span<detail::RowView> source_rows;
    std::uint32_t source_previous;std::size_t source_index;const h::OwnedRow* source_owned;detail::RowView source_row;
    std::array<const h::Observation*,2> source_observations;std::size_t source_slot;const h::Observation* source_input;detail::ObservationView* source_output;
    std::string_view reference_text,reference_run;std::uint64_t reference_result;
    std::string_view id_text,id_run,id_marker;std::uint64_t id_ordinal;std::uint32_t id_cpu;std::size_t id_slot;Decimal id_ordinal_text,id_cpu_text;
    std::string_view word_text;std::uint32_t word_value;char word_character;
    std::uint32_t signature_eax,signature_base,signature_family,signature_model;h::DecodedSignature signature_return,signature_compare;
    const detail::ObservationView* validate_observation;const SourceView* validate_source;std::uint32_t validate_cpu;std::size_t validate_slot;unsigned validate_reason;h::DecodedSignature validate_value;
    SourceView* account_source;const detail::RowView* account_row;std::size_t account_slot;const detail::ObservationView* account_observed;Decimal account_number;
    std::string_view inner_body;const Limits* inner_limits;std::span<detail::RowView> inner_rows;Limits inner_reader_limits;Reader inner_reader;
    std::size_t inner_row_start,inner_slot;std::uint32_t inner_previous;detail::RowView inner_row;detail::ObservationView* inner_observed;std::string_view inner_failure,inner_run;
    std::array<std::size_t,2> inner_slot_values;std::initializer_list<std::size_t> inner_slot_list;std::uint32_t inner_eax,inner_family,inner_model,inner_stepping;
    h::DecodedSignature inner_signature;h::Microcode inner_microcode;
    std::string_view parse_body;const Limits* parse_limits;std::span<detail::RowView> parse_rows;Reader parse_reader;std::uint32_t parse_previous;std::uint64_t parse_cpu;std::size_t parse_slot;detail::WireError parse_wire;
    std::array<std::size_t,2> parse_slot_values;std::initializer_list<std::size_t> parse_slot_list;
    Reader* parse_reference_reader;std::optional<std::uint64_t> parse_reference_ordinal;std::uint64_t parse_reference_sequence;FailureReference parse_reference_result;
    Sink* emit_error_sink;std::string_view emit_error_key;const std::optional<e::Owner>* emit_error_value;const std::optional<FailureReference>* emit_error_reference;const ErrorRow* emit_row;
    bool input_present;std::size_t input_scratch,input_upper,input_dynamic,input_out;
    std::size_t staging_count,staging_scratch,staging_upper,staging_dynamic,staging_out,row_stock_count;
    const SourceView *source_limits_source,*topology_source,*errors_source,*retain_source,*phase_source;
    const detail::Children *backing_children,*phase_children,*topology_children,*errors_children,*retain_children,*source_phase_children,*publication_children;
    std::size_t backing_out;const ErrorRow* backing_row;const std::optional<e::Owner>* backing_child;t::Usage backing_topology_usage;e::Usage backing_preparation_usage,backing_error_usage;
    bool child_topology;std::size_t child_scratch,child_decoded,child_upper,child_dynamic,child_allowance,child_floor,child_out;
    std::array<std::size_t,6> topology_max_values;std::initializer_list<std::size_t> topology_max_list;std::array<std::size_t,3> error_max_values;std::initializer_list<std::size_t> error_max_list;
    const t::Owner* topology_child;const t::Provenance* topology_provenance;const topology_evidence::Owner* topology_owner;
    DigestText topology_digest;FingerprintText topology_effective,topology_physical;
    const std::optional<FailureReference>* error_reference;const std::optional<e::Owner>* error_child;const p::Target* error_target;std::optional<p::Producer> error_producer;
    const e::Provenance* error_provenance;const e::Association* error_fact;std::vector<p::Target>::const_iterator error_find;
    std::span<const detail::RowView> errors_rows;std::size_t errors_index,errors_slot,errors_previous,errors_earlier;
    const detail::RowView* errors_row;const ErrorRow *errors_owned,*errors_prior;const detail::ObservationView* errors_observed;
    const std::optional<FailureReference>* errors_reference;const std::optional<e::Owner>* errors_child;p::Target errors_target,errors_preparation_target;std::optional<p::Producer> errors_producer;
    std::expected<journal::RunId,core::Error> retain_run;core::detail::ExpectedErrorCleanup<decltype(retain_run)> retain_run_guard;
    std::vector<TypedRow> retain_typed;std::vector<h::Row> retain_inputs;std::span<const detail::RowView> retain_rows;std::size_t retain_index,retain_slot;
    std::expected<epycbench::topology::StableProcessingUnitId,core::Error> retain_id;core::detail::ExpectedErrorCleanup<decltype(retain_id)> retain_id_guard;
    std::expected<journal::Timestamp,core::Error> retain_time;core::detail::ExpectedErrorCleanup<decltype(retain_time)> retain_time_guard;
    std::array<h::Slot,2> retain_slots;const detail::ObservationView* retain_observed;h::Slot* retain_destination;h::Signature retain_signature;h::Microcode retain_microcode;h::Row retain_row;
    h::Input retain_input;std::expected<h::Owner,core::Error> retain_result;core::detail::ExpectedErrorCleanup<decltype(retain_result)> retain_guard;h::Limits retain_limits_copy;
    std::size_t source_scratch,source_upper,source_dynamic,source_typed,source_phase_out;
    const Counts *dom_counts,*publication_count,*publish_count;const h::Owner* publication_source;std::size_t publication_upper,publication_dynamic,publication_length,dom_return;
    const ErrorRow* publication_row;const std::optional<e::Owner>* publication_error;
    std::optional<h::Owner>* publish_source;detail::Children* publish_children;Origin publish_origin;Provenance provenance_temporary;
    Json publish_dom;core::detail::JsonCleanup<Json> publish_guard;Sink publish_sink;core::JcsLimits jcs_limits;std::expected<std::string,core::Error> rendered;core::detail::ExpectedErrorCleanup<decltype(rendered)> rendered_guard;
    core::Sha256Digest digest_return,initial_digest;std::string_view initial_digest_input;detail::State* state_constructor_this;
    const h::Owner *source_usage_this,*source_canonical_this,*source_digest_this,*source_run_this,*source_producer_this,*source_ordinal_this,*source_rows_this,*source_topology_this,*source_unrepresented_this;
    h::Usage source_usage_return;std::string_view source_canonical_return,source_run_return,source_unrepresented_return;const core::Sha256Digest* source_digest_return;std::optional<h::Producer> source_producer_return;std::optional<std::uint64_t> source_ordinal_return;std::span<const h::OwnedRow> source_rows_return;const h::TopologyBinding* source_topology_return;
    const t::Owner *topology_usage_this,*topology_canonical_this,*topology_provenance_this,*topology_owner_this,*topology_state_this,*topology_error_this,*topology_reason_this;
    t::Usage topology_usage_return;std::string_view topology_canonical_return;const t::Provenance* topology_provenance_return;const topology_evidence::Owner* topology_owner_return;std::optional<t::SlotState> topology_state_return;const e::Owner* topology_error_return;std::optional<t::Reason> topology_reason_return;
    const topology_evidence::Owner *inner_topology_digest_this,*inner_topology_fingerprints_this;const core::Sha256Digest* inner_topology_digest_return;const epycbench::topology::TopologyFingerprints* inner_topology_fingerprints_return;
    const epycbench::topology::PhysicalTopologyFingerprint* physical_this;const epycbench::topology::EffectiveTopologyFingerprint* effective_this;const core::Sha256Digest *physical_return,*effective_return;
    const e::Owner *error_usage_this,*error_canonical_this,*error_provenance_this,*error_source_this;e::Usage error_usage_return;std::string_view error_canonical_return;const e::Provenance* error_provenance_return;const core::Error* error_source_return;
    std::size_t fixed_return,backing_return,input_phase_return,child_phase_return,source_phase_return,staging_return;
    std::size_t preflight_index,preflight_slot;const detail::BindingRow* preflight_binding;ErrorRow* preflight_row;const detail::ObservationView* preflight_observed;
    ErrorRow assigned_error_row;detail::RowView assigned_row_view;std::vector<detail::RowView> retired_rows;
};
} // namespace

auto detail::checked_add(std::size_t& value,std::size_t amount)noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto detail::checked_multiply(std::size_t left,std::size_t right)noexcept->std::optional<std::size_t>{if(right&&left>SIZE_MAX/right)return {};return left*right;}
auto detail::fixed_stock()noexcept->std::size_t{return sizeof(OperationCells);}
Owner::Owner(std::unique_ptr<detail::State> state)noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&)noexcept=default;
auto Owner::operator=(Owner&&)noexcept->Owner& =default;
Owner::~Owner()=default;
auto Owner::identity()const noexcept->const h::Owner*{return state_&&state_->identity?&*state_->identity:nullptr;}
auto Owner::topology()const noexcept->const t::Owner*{return state_&&state_->children.topology?&*state_->children.topology:nullptr;}
auto Owner::failure_error()const noexcept->const e::Owner*{return state_&&state_->children.preparation_error?&*state_->children.preparation_error:nullptr;}
auto Owner::errors()const noexcept->std::span<const ErrorRow>{return state_?std::span<const ErrorRow>{state_->children.rows}:std::span<const ErrorRow>{};}
auto Owner::slot_state()const noexcept->std::optional<SlotState>{return state_?std::optional{state_->slot_state}:std::nullopt;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::before_bindings(const h::Owner* source,const detail::View& view,const Limits& limits,std::size_t upper)->std::expected<std::size_t,Error>{
    try{valid_limits(limits);(void)primitive(view,limits,upper);local(view,source!=nullptr);pu_identity_serialization::identity(view);
        if(!source)return 0;
        const auto info=source_view(*source,limits);source_limits(info,view,limits);const auto dynamic=times(info.rows,sizeof(detail::BindingRow));bound(plus(base(upper,0),dynamic),limits.binding_bytes);return dynamic;
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::preflight(detail::ResolvedInput& input,const Limits& limits)->std::expected<void,Error>{
    try{input.usage=primitive(input.view,limits,input.upper_fixed);require(input.staging,Error::source);input.usage.identity_input_bytes=input_phase(limits,input.identity!=nullptr,0,input.upper_fixed,input.upper_dynamic);
        if(input.identity){const auto info=source_view(*input.identity,limits);source_limits(info,input.view,limits);require(input.bindings.size()==info.rows,Error::source);
            input.usage.rows=info.rows;input.usage.observations=times(info.rows,2);input.usage.error_children=info.errors;input.usage.binding_bytes=plus(base(input.upper_fixed,0),input.upper_dynamic);input.usage.row_staging_bytes=staging_phase(limits,info.rows,0,input.upper_fixed,input.upper_dynamic);
            input.staging->rows.resize(info.rows);input.staging->children.rows.resize(info.rows);(void)source_view(*input.identity,limits,input.staging->rows);
            for(std::size_t i=0;i<info.rows;++i){const auto& binding=input.bindings[i];auto& row=input.staging->children.rows[i];require(binding.cpu==input.staging->rows[i].cpu,Error::identity);row.cpu=binding.cpu;
                for(std::size_t slot=0;slot<2;++slot){row.references[slot]=binding.slots[slot].reference;const auto& observed=input.staging->rows[i].slots[slot];require(observed.failure.has_value()==row.references[slot].has_value(),Error::reference);if(row.references[slot]){reference_local(input.view,*row.references[slot]);require(observed.failure==row.references[slot]->sequence,Error::reference);}}}
        }return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::child_phase(detail::ResolvedInput& input,const Limits& limits,bool topology)->std::expected<void,Error>{
    try{const auto amount=child_phase_stock(limits,input.staging->children,topology,0,0,input.upper_fixed,input.upper_dynamic);if(topology)input.usage.topology_preparation_bytes=amount;else input.usage.error_preparation_bytes=std::max(input.usage.error_preparation_bytes,amount);return {};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::prepare(detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{require(input.staging,Error::source);std::optional<h::Owner> source;
        if(input.identity){const auto info=source_view(*input.identity,limits);topology_match(input.view,info,input.staging->children);errors_match(input.view,&info,input.staging->rows,input.staging->children);input.usage.identity_preparation_bytes=source_phase(info,input.staging->children,limits,0,input.upper_fixed,input.upper_dynamic);source=retain_source(info,input.staging->rows,input.staging->children,limits);require(*source->digest()==*input.identity->digest(),Error::source);}
        else errors_match(input.view,nullptr,{},input.staging->children);
        std::vector<detail::RowView>{}.swap(input.staging->rows);Sink count;emit(count,input.view,source?&*source:nullptr,input.staging->children);input.usage=publication(input.usage,count.count,source?&*source:nullptr,input.staging->children,input.upper_fixed,input.upper_dynamic,limits);
        return Owner{publish(source,input.staging->children,input.view,count.count,input.usage,Origin::session_selected)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{valid_limits(limits);const auto parsed=parse(body,limits);auto usage=primitive(parsed.view,limits,0);local(parsed.view,parsed.retained);pu_identity_serialization::identity(parsed.view);
        detail::Children children;std::optional<h::Owner> source;usage.identity_input_bytes=input_phase(limits,parsed.retained,parsed.source.size()/2,0,0);
        {
            const auto canonical=unhex(parsed.source);std::vector<detail::RowView> rows;SourceView info;
            if(parsed.retained){info=parse_source(canonical,limits);source_limits(info,parsed.view,limits);require(info.rows==parsed.rows&&info.errors==parsed.errors,Error::reference);usage.rows=info.rows;usage.observations=times(info.rows,2);usage.error_children=info.errors;usage.row_staging_bytes=staging_phase(limits,info.rows,canonical.size(),0,0);rows.resize(info.rows);children.rows.resize(info.rows);(void)parse_source(canonical,limits,rows);(void)parse(body,limits,rows);
                for(std::size_t i=0;i<rows.size();++i){children.rows[i].cpu=rows[i].cpu;for(std::size_t slot=0;slot<2;++slot){children.rows[i].references[slot]=rows[i].wire[slot].reference;require(rows[i].slots[slot].failure.has_value()==rows[i].wire[slot].reference.has_value(),Error::reference);if(rows[i].wire[slot].reference){reference_local(parsed.view,*rows[i].wire[slot].reference);require(rows[i].slots[slot].failure==rows[i].wire[slot].reference->sequence,Error::reference);}}}
                usage.topology_preparation_bytes=child_phase_stock(limits,children,true,canonical.size(),parsed.topology.size()/2,0,0);const auto bytes=unhex(parsed.topology);auto decoded=t::Owner::decode(bytes,limits.topology);if(!decoded)throw decoded.error()==t::Error::capacity?Error::capacity:Error::child;children.topology=std::move(*decoded);
            }
            if(!parsed.preparation_error.empty()){usage.error_preparation_bytes=child_phase_stock(limits,children,false,canonical.size(),parsed.preparation_error.size()/2,0,0);const auto bytes=unhex(parsed.preparation_error);auto decoded=e::Owner::decode(bytes,limits.child);if(!decoded)throw decoded.error()==e::Error::capacity?Error::capacity:Error::child;children.preparation_error=std::move(*decoded);}
            for(std::size_t i=0;i<rows.size();++i)for(std::size_t slot=0;slot<2;++slot)if(!rows[i].wire[slot].bytes.empty()){const auto encoded=rows[i].wire[slot].bytes;usage.error_preparation_bytes=std::max(usage.error_preparation_bytes,child_phase_stock(limits,children,false,canonical.size(),encoded.size()/2,0,0));const auto bytes=unhex(encoded);auto decoded=e::Owner::decode(bytes,limits.child);if(!decoded)throw decoded.error()==e::Error::capacity?Error::capacity:Error::child;children.rows[i].errors[slot]=std::move(*decoded);}
            errors_match(parsed.view,parsed.retained?&info:nullptr,rows,children);
            if(parsed.retained){topology_match(parsed.view,info,children);usage.identity_preparation_bytes=source_phase(info,children,limits,canonical.size(),0,0);source=retain_source(info,rows,children,limits);const DigestText digest{*source->digest()};require(digest.view()==parsed.digest,Error::encoding);}
        }
        usage=publication(usage,parsed.wire,source?&*source:nullptr,children,0,0,limits);auto state=publish(source,children,parsed.view,parsed.wire,usage,Origin::decoded_unassociated);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::pu_identity_serialization
