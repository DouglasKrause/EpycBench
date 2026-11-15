#include "results/private_error_serialization.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/journal_records.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <numeric>

namespace epycbench::results::error_serialization {
namespace {
using Json=nlohmann::json;
namespace p=process_subject;
static_assert(sizeof(Json::number_float_t)==sizeof(std::uint64_t));
static_assert(std::is_same_v<Json::number_integer_t,std::int64_t>);
static_assert(std::is_same_v<Json::number_unsigned_t,std::uint64_t>);
constexpr std::array domains{"supplied_observation","synthetic_execution"};
constexpr std::array producers{"controller","runner","results"};
constexpr std::array kinds{"ordinary_failure","creation_failure","source_resolution"};
constexpr std::array categories{"topology","system_controls","kernel_identity","pu_identity","frequency","anonymous_thp","process_thp","authored_source","base_build","preflight_temperature"};
constexpr std::array selectors{"owner_preparation","smt_control","smt_active","numa_mask","thp_enabled","thp_defrag","sysname","release","version","signature","microcode","uncovered_cpu","enumeration","page_size","process_observation","source_acquisition","base_build_acquisition","cpu_control_observation"};
constexpr std::array reasons{"none","read_failed","malformed_source","unsupported_value"};
constexpr std::string_view digits="0123456789abcdef";
void require(bool value,Error error=Error::schema){if(!value)throw error;}
auto plus(std::size_t a,std::size_t b)->std::size_t {require(detail::checked_add(a,b),Error::capacity);return a;}
auto times(std::size_t a,std::size_t b)->std::size_t {auto value=detail::checked_multiply(a,b);require(value.has_value(),Error::capacity);return *value;}
void bound(std::size_t value,std::size_t limit){require(value<=limit,Error::capacity);}
template<class Enum,class Names> auto name(Enum e,const Names& names)->std::string_view {
    const auto i=static_cast<std::size_t>(e);require(i<names.size(),Error::source);return names[i];
}
template<class Enum,class Names> auto enum_value(std::string_view text,const Names& names)->Enum {
    for(std::size_t i=0;i<names.size();++i)if(text==names[i])return static_cast<Enum>(i);
    throw Error::schema;
}
template<class Enum,class Names> auto core_name(Enum value,const Names& names)->std::string_view {
    for(const auto& [e,text]:names)if(e==value)return text;
    throw Error::source;
}
template<class Enum,class Names> auto core_enum(std::string_view text,const Names& names)->Enum {
    for(const auto& [e,label]:names)if(text==label)return e;
    throw Error::schema;
}
struct Decimal {
    std::array<char,21> data{};std::size_t size{};
    template<class T> explicit Decimal(T value) {
        const auto result=std::to_chars(data.data(),data.data()+data.size(),value);
        require(result.ec==std::errc{},Error::source);size=static_cast<std::size_t>(result.ptr-data.data());
    }
    auto view()const noexcept->std::string_view{return {data.data(),size};}
};
auto nibble(char ch)->unsigned {
    if(ch>='0' && ch<='9')return static_cast<unsigned>(ch-'0');
    if(ch>='a' && ch<='f')return static_cast<unsigned>(ch-'a')+10U;
    throw Error::encoding;
}
void hex_valid(std::string_view text){require(text.size()%2U==0U,Error::encoding);for(char ch:text)(void)nibble(ch);}
auto unhex(std::string_view text)->std::string {
    std::string out(text.size()/2U,'\0');
    for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<char>((nibble(text[2*i])<<4U)|nibble(text[2*i+1]));
    return out;
}
auto hex(std::string_view bytes)->std::string {
    std::string out(times(bytes.size(),2U),'0');
    for(std::size_t i=0;i<bytes.size();++i){const auto byte=static_cast<unsigned char>(bytes[i]);out[2*i]=digits[byte>>4U];out[2*i+1]=digits[byte&15U];}
    return out;
}
template<class T> auto integer(std::string_view text)->T {
    require(!text.empty(),Error::encoding);
    const bool negative=text.front()=='-';const auto magnitude=negative?text.substr(1):text;
    require(!magnitude.empty() && (magnitude.size()==1 || magnitude.front()!='0') && !(negative && magnitude=="0"),Error::encoding);
    for(char ch:magnitude)require(ch>='0' && ch<='9',Error::encoding);
    T value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    require(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),Error::encoding);return value;
}
struct Counts {std::size_t nodes{},depth{},text{},bytes{},objects{},arrays{},strings{},members{},elements{};};
struct WireFrame {Json* value{};std::size_t children{};bool object{};};
class Sink {
  public:
    Counts count;
    explicit Sink(Json* root=nullptr):root_(root){}
    void object(std::string_view key={}){open_container(key,true,0);}
    void array(std::string_view key,std::size_t size){open_container(key,false,size);}
    void end(){require(level_!=0);--level_;count.bytes=plus(count.bytes,1);}
    void text(std::string_view key,std::string_view value){prefix(key);count.strings=plus(count.strings,1);count.text=plus(count.text,value.size());count.bytes=plus(count.bytes,plus(value.size(),2));if(root_){Json node=std::string{value};insert(key,node);}}
    void octets(std::string_view key,std::string_view value){if(root_){auto encoded=hex(value);text(key,encoded);}else{prefix(key);count.strings=plus(count.strings,1);count.text=plus(count.text,times(value.size(),2));count.bytes=plus(count.bytes,plus(times(value.size(),2),2));}}
    void boolean(std::string_view key,bool value){prefix(key);count.bytes=plus(count.bytes,value?4U:5U);if(root_){Json node=value;insert(key,node);}}
    void null(std::string_view key){prefix(key);count.bytes=plus(count.bytes,4);if(root_){Json node;insert(key,node);}}
    template<class T> void number(std::string_view key,T value){const Decimal decimal{value};text(key,decimal.view());}
    void optional(std::string_view key,std::optional<std::uint64_t> value){if(value)number(key,*value);else null(key);}
  private:
    Json* root_;std::array<WireFrame,7> frames_{};std::size_t level_{};
    void prefix(std::string_view key){count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);if(!level_)return;auto& frame=frames_[level_-1];if(frame.children++)count.bytes=plus(count.bytes,1);if(frame.object){count.members=plus(count.members,1);count.text=plus(count.text,key.size());count.bytes=plus(count.bytes,plus(key.size(),3));}else count.elements=plus(count.elements,1);}
    auto insert(std::string_view key,Json& node)->Json* {
        if(!level_){root_->swap(node);return root_;}auto& frame=frames_[level_-1];
        if(frame.object){core::detail::append_object(*frame.value,key,node);return &frame.value->find(key).value();}
        frame.value->push_back(std::move(node));return &frame.value->back();
    }
    void open_container(std::string_view key,bool object,std::size_t size){prefix(key);require(level_<frames_.size());count.bytes=plus(count.bytes,1);if(object)++count.objects;else ++count.arrays;Json* placed=nullptr;if(root_){Json node=object?Json::object():Json::array();core::detail::JsonCleanup cleanup{node};if(!object)node.get_ref<Json::array_t&>().reserve(size);placed=insert(key,node);}frames_[level_++]={placed,0,object};}
};
struct FactView {
    p::Producer producer{};
    std::optional<std::uint64_t> process,secondary,secondary_ordinal;
    std::span<const p::Target> targets;
};
struct Metadata {
    std::string_view run,time;
    p::Domain domain{};p::Producer reporter{};Kind kind{};std::uint64_t sequence{};
    std::optional<std::uint64_t> ordinal;
    std::optional<FactView> fact;
};
auto metadata(const Provenance& p)->Metadata {
    Metadata out{p.run,p.time,p.domain,p.entry_producer,p.kind,p.sequence,p.creation_ordinal,{}};
    if(p.association){const auto& a=*p.association;out.fact=FactView{a.producer,a.process_sequence,a.secondary_sequence,a.secondary_creation_ordinal,a.targets};}return out;
}
void context_rows(Sink& out,const Json& value){
    out.array({},value.is_binary()?3U:(value.is_null()||value.is_discarded()?1U:2U));
    switch(value.type()){
    case Json::value_t::null:out.text({},"null");break;
    case Json::value_t::boolean:out.text({},"boolean");out.boolean({},value.get<bool>());break;
    case Json::value_t::number_integer:out.text({},"signed");out.number({},value.get<std::int64_t>());break;
    case Json::value_t::number_unsigned:out.text({},"unsigned");out.number({},value.get<std::uint64_t>());break;
    case Json::value_t::number_float:{out.text({},"binary64");auto bits=std::bit_cast<std::uint64_t>(value.get_ref<const double&>());std::array<char,16> text{};for(std::size_t i=0;i<16;++i)text[15-i]=digits[(bits>>(i*4U))&15U];out.text({},{text.data(),text.size()});break;}
    case Json::value_t::string:out.text({},"string");out.octets({},value.get_ref<const std::string&>());break;
    case Json::value_t::binary:{out.text({},"binary");const auto& bytes=value.get_binary();out.octets({},{reinterpret_cast<const char*>(bytes.data()),bytes.size()});out.optional({},bytes.has_subtype()?std::optional<std::uint64_t>{bytes.subtype()}:std::nullopt);break;}
    case Json::value_t::discarded:out.text({},"discarded");break;
    case Json::value_t::array:out.text({},"array");out.number({},value.size());break;
    case Json::value_t::object:out.text({},"object");out.array({},value.size());for(auto it=value.cbegin();it!=value.cend();++it)out.octets({},it.key());out.end();break;
    }
    out.end();if(value.is_structured())for(const auto& child:value)context_rows(out,child);
}
void emit_error(Sink& out,std::string_view key,const core::Error& e,std::size_t context_nodes){
    out.object(key);out.text("category",core_name(e.category,core::detail::error_category_names));out.octets("code_hex",e.code.value());out.array("context",context_nodes);context_rows(out,e.context);out.end();out.text("continuation",core_name(e.continuation,core::detail::continuation_policy_names));out.octets("message_hex",e.message);out.octets("operation_hex",e.operation);
    if(e.os_error){out.object("os_error");out.octets("domain_hex",e.os_error->domain);out.octets("message_hex",e.os_error->message);out.number("value",e.os_error->value);out.end();}else out.null("os_error");out.text("severity",core_name(e.severity,core::detail::error_severity_names));out.end();
}
void emit(Sink& out,const Metadata& m,const core::Error& e,std::size_t context_nodes){
    out.object();
    if(m.fact){const auto& f=*m.fact;out.object("association");out.optional("process_sequence",f.process);out.text("producer",name(f.producer,producers));if(f.secondary){out.object("secondary");out.optional("creation_ordinal",f.secondary_ordinal);out.number("sequence",*f.secondary);out.end();}else out.null("secondary");out.array("targets",f.targets.size());for(const auto& t:f.targets){out.array({},5);out.text({},name(t.category,categories));out.text({},name(t.selector,selectors));out.number({},t.ordinal);out.number({},t.key);out.text({},name(t.reason,reasons));out.end();}out.end();out.end();}else out.null("association");
    out.text("domain",name(m.domain,domains));out.text("entry_producer",name(m.reporter,producers));
    emit_error(out,"error",e,context_nodes);
    out.text("run_id",m.run);out.text("schema",profile);out.number("sequence",m.sequence);out.object("source");out.optional("creation_ordinal",m.ordinal);out.text("kind",name(m.kind,kinds));out.end();out.text("timestamp",m.time);out.end();
}
auto dom_stock(const Counts& c)->std::size_t{return plus(plus(plus(times(c.objects,sizeof(Json::object_t)),times(c.arrays,sizeof(Json::array_t))),plus(times(c.members,sizeof(Json::object_t::value_type)),times(c.elements,sizeof(Json)))),plus(times(c.strings,sizeof(Json::string_t)),c.text));}
struct ParseFrame {std::size_t children{};bool object{};};
class Reader {
  public:
    Counts count;
    Reader(std::string_view body,const Limits& limits):body_(body),limits_(limits){bound(body.size(),limits.canonical_bytes);count.bytes=body.size();}
    auto position()const noexcept->std::size_t{return at_;}
    auto slice(std::size_t begin)const noexcept->std::string_view{return body_.substr(begin,at_-begin);}
    auto next(char c)const noexcept->bool{const auto at=at_+(at_<body_.size() && body_[at_]==','?1U:0U);return at<body_.size() && body_[at]==c;}
    void expect(std::string_view value){require(body_.substr(at_,value.size())==value,Error::encoding);at_+=value.size();}
    void key(std::string_view expected){require(level_ && frames_[level_-1].object);auto& frame=frames_[level_-1];if(frame.children++)expect(",");expect("\"");expect(expected);expect("\":");++count.members;count.text=plus(count.text,expected.size());}
    void object(){node();expect("{");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,true};++count.objects;}
    void array(){node();expect("[");require(level_<frames_.size(),Error::capacity);frames_[level_++]={0,false};++count.arrays;}
    void end_object(){expect("}");require(level_!=0);--level_;}
    void end_array(){expect("]");require(level_!=0);--level_;}
    auto text()->std::string_view{node();expect("\"");const auto begin=at_;while(at_<body_.size() && body_[at_]!='"'){const auto byte=static_cast<unsigned char>(body_[at_]);require(byte>=0x20 && byte<=0x7e && byte!='\\',Error::encoding);++at_;}require(at_<body_.size(),Error::encoding);const auto result=body_.substr(begin,at_-begin);++at_;++count.strings;count.text=plus(count.text,result.size());bound(count.text,limits_.text_bytes);return result;}
    auto octets()->std::string_view{const auto value=text();hex_valid(value);return value;}
    template<class T> auto number()->T{return integer<T>(text());}
    auto null()->bool{node();if(next('n')){expect("null");return true;}throw Error::schema;}
    auto optional()->std::optional<std::uint64_t>{if(next('n')){(void)null();return {};}return number<std::uint64_t>();}
    auto boolean()->bool{node();if(next('t')){expect("true");return true;}expect("false");return false;}
    void finished(){require(level_==0 && at_==body_.size(),Error::encoding);}
  private:
    std::string_view body_;const Limits& limits_;std::size_t at_{},level_{};std::array<ParseFrame,7> frames_{};
    void node(){if(level_ && !frames_[level_-1].object){if(frames_[level_-1].children++)expect(",");++count.elements;}count.nodes=plus(count.nodes,1);count.depth=std::max(count.depth,level_+1);bound(count.nodes,limits_.nodes);bound(count.depth,limits_.depth);}
};
struct ContextFrame {Json* value{};std::size_t remaining{};std::string_view keys;};
struct Scratch {
    core::Error error;
    Provenance provenance;
    ~Scratch(){core::detail::retire_json(error.context);}
};
struct Parsed {
    Metadata metadata;
    FactView fact;
    Counts wire;
    private_error::Usage error;
    std::size_t target_count{},maximum_octets{},maximum_key{},maximum_binary{};
};
auto target_valid(const p::Target& t,const FactView& f)->bool {
    using C=p::Category;using S=p::Selector;using R=p::FailureReason;
    if(t.category==C::preflight_temperature)return t.selector==S::cpu_control_observation && t.ordinal==0 && t.reason>=R::read_failed && t.reason<=R::unsupported_value && f.producer==p::Producer::controller && !f.process;
    if(t.category==C::authored_source || t.category==C::base_build)return t.selector==(t.category==C::authored_source?S::source_acquisition:S::base_build_acquisition) && t.ordinal==0 && t.key==0 && t.reason==R::none && !f.process && f.producer==p::Producer::controller;
    if(t.category<C::topology || t.category>C::process_thp)return false;
    if(t.selector==S::owner_preparation)return t.ordinal==0 && t.key==0 && t.reason==R::none && !f.process;
    if(t.reason<R::read_failed || t.reason>R::unsupported_value)return false;
    if(t.category==C::process_thp)return t.selector==S::process_observation && f.process && t.key==*f.process;
    if(f.process)return false;
    switch(t.category){
    case C::system_controls:return t.selector>=S::smt_control && t.selector<=S::thp_defrag && t.key==0;
    case C::kernel_identity:return t.selector>=S::sysname && t.selector<=S::version && t.key==0;
    case C::pu_identity:return (t.selector==S::signature || t.selector==S::microcode) && t.key<=UINT32_MAX;
    case C::frequency:return t.selector==S::uncovered_cpu && t.key<=UINT32_MAX;
    case C::anonymous_thp:return (t.selector==S::enumeration && t.key==0) || (t.selector==S::page_size && t.key>=1024 && (t.key&(t.key-1))==0);
    default:return false;
    }
}
void local_metadata(const Metadata& m){
    require(m.sequence!=0,Error::identity);
    require((m.kind==Kind::creation_failure)==m.ordinal.has_value(),Error::schema);
    if(m.ordinal)require(m.sequence==1 && *m.ordinal<1024,Error::schema);
    require((m.kind==Kind::source_resolution)==!m.fact,Error::schema);
    if(m.sequence==1 || m.kind==Kind::source_resolution)require(m.reporter==p::Producer::controller,Error::schema);
    if(!m.fact)return;
    const auto& f=*m.fact;
    if(f.process){require(*f.process && *f.process<=m.sequence,Error::schema);if(*f.process==m.sequence)require(m.reporter==p::Producer::controller,Error::schema);}
    if(f.secondary){require(*f.secondary!=0,Error::schema);if(m.kind==Kind::creation_failure)require(*f.secondary==1 && f.secondary_ordinal && *f.secondary_ordinal<*m.ordinal,Error::schema);else require(*f.secondary<m.sequence,Error::schema);if(f.secondary_ordinal)require(*f.secondary==1 && *f.secondary_ordinal<1024,Error::schema);}else require(!f.secondary_ordinal,Error::schema);
}
void identity(const Metadata& m){
    auto run=journal::RunId::parse(m.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);
    auto time=journal::Timestamp::parse(m.time);core::detail::ExpectedErrorCleanup time_cleanup{time};require(time.has_value(),Error::identity);
}
class Parser {
  public:
    Parser(std::string_view body,const Limits& limits,Scratch* output=nullptr,std::size_t target_capacity=0):reader_(body,limits),limits_(limits),output_(output),target_capacity_(target_capacity){result_.error.retained_bytes=private_error::Owner::fixed_bytes();}
    auto parse()->Parsed {
        auto& r=reader_;auto& m=result_.metadata;
        r.object();r.key("association");association();
        r.key("domain");m.domain=enum_value<p::Domain>(r.text(),domains);
        r.key("entry_producer");m.reporter=enum_value<p::Producer>(r.text(),producers);
        r.key("error");error();r.key("run_id");m.run=r.text();r.key("schema");require(r.text()==profile,Error::schema);r.key("sequence");m.sequence=r.number<std::uint64_t>();
        r.key("source");r.object();r.key("creation_ordinal");m.ordinal=r.optional();r.key("kind");m.kind=enum_value<Kind>(r.text(),kinds);r.end_object();r.key("timestamp");m.time=r.text();r.end_object();r.finished();
        if(has_fact_)m.fact=result_.fact;
        local_metadata(m);result_.wire=r.count;
        if(output_){auto& p=output_->provenance;p.run=m.run;p.time=m.time;p.domain=m.domain;p.entry_producer=m.reporter;p.kind=m.kind;p.sequence=m.sequence;p.creation_ordinal=m.ordinal;}
        return result_;
    }
    auto parse_value()->Parsed {
        error();reader_.finished();result_.wire=reader_.count;return result_;
    }
  private:
    Reader reader_;const Limits& limits_;Scratch* output_;std::size_t target_capacity_;Parsed result_;bool has_fact_{};
    void string_stock(std::size_t size){result_.error.string_bytes=plus(result_.error.string_bytes,size);result_.error.retained_bytes=plus(result_.error.retained_bytes,size);result_.maximum_octets=std::max(result_.maximum_octets,size);bound(result_.error.string_bytes,limits_.string_bytes);}
    auto octets()->std::string_view{const auto value=reader_.octets();string_stock(value.size()/2);return value;}
    void association(){
        auto& r=reader_;if(r.next('n')){(void)r.null();return;}has_fact_=true;auto& f=result_.fact;
        r.object();r.key("process_sequence");f.process=r.optional();r.key("producer");f.producer=enum_value<p::Producer>(r.text(),producers);r.key("secondary");
        if(r.next('n'))(void)r.null();else{r.object();r.key("creation_ordinal");f.secondary_ordinal=r.optional();r.key("sequence");f.secondary=r.number<std::uint64_t>();r.end_object();}
        if(output_){output_->provenance.association.emplace();auto& a=*output_->provenance.association;a.producer=f.producer;a.process_sequence=f.process;a.secondary_sequence=f.secondary;a.secondary_creation_ordinal=f.secondary_ordinal;a.targets.reserve(target_capacity_);}
        r.key("targets");r.array();while(!r.next(']')){r.array();p::Target target;target.category=enum_value<p::Category>(r.text(),categories);target.selector=enum_value<p::Selector>(r.text(),selectors);target.ordinal=r.number<std::uint64_t>();target.key=r.number<std::uint64_t>();target.reason=enum_value<p::FailureReason>(r.text(),reasons);r.end_array();require(target_valid(target,f));result_.target_count=plus(result_.target_count,1);bound(result_.target_count,limits_.targets);if(output_)output_->provenance.association->targets.push_back(target);}r.end_array();r.end_object();require(result_.target_count!=0);
        if(output_){const auto& targets=output_->provenance.association->targets;std::vector<std::size_t> index(targets.size());std::iota(index.begin(),index.end(),0U);std::sort(index.begin(),index.end(),[&](auto a,auto b){return targets[a]<targets[b];});for(std::size_t i=1;i<index.size();++i)require(targets[index[i-1]]!=targets[index[i]]);}
    }
    void error(){
        auto& r=reader_;r.object();r.key("category");const auto category=core_enum<core::ErrorCategory>(r.text(),core::detail::error_category_names);if(output_)output_->error.category=category;
        r.key("code_hex");const auto code=octets();if(output_)output_->error.code=core::StableErrorCode{unhex(code)};
        r.key("context");context();r.key("continuation");const auto continuation=core_enum<core::ContinuationPolicy>(r.text(),core::detail::continuation_policy_names);if(output_)output_->error.continuation=continuation;
        r.key("message_hex");const auto message=octets();if(output_)output_->error.message=unhex(message);
        r.key("operation_hex");const auto operation=octets();if(output_)output_->error.operation=unhex(operation);
        r.key("os_error");if(r.next('n'))(void)r.null();else{r.object();r.key("domain_hex");const auto domain=octets();r.key("message_hex");const auto os_message=octets();r.key("value");const auto value=r.number<int>();r.end_object();if(output_)output_->error.os_error=core::OsError{value,unhex(domain),unhex(os_message)};}
        r.key("severity");const auto severity=core_enum<core::ErrorSeverity>(r.text(),core::detail::error_severity_names);if(output_)output_->error.severity=severity;r.end_object();
    }
    void context(){
        auto& r=reader_;std::array<ContextFrame,65> stack{};std::size_t active{};r.array();
        while(!r.next(']')){
            require(result_.error.nodes==0 || active!=0);result_.error.nodes=plus(result_.error.nodes,1);bound(result_.error.nodes,limits_.context_nodes);result_.error.depth=std::max(result_.error.depth,active);bound(active,limits_.context_depth);
            r.array();const auto tag=r.text();Json node;core::detail::JsonCleanup cleanup{node};std::size_t children{};std::string_view keys;
            if(tag=="null"){}
            else if(tag=="boolean"){const bool value=r.boolean();if(output_)node=value;}
            else if(tag=="signed"){const auto value=r.number<std::int64_t>();if(output_)node=value;}
            else if(tag=="unsigned"){const auto value=r.number<std::uint64_t>();if(output_)node=value;}
            else if(tag=="binary64"){const auto value=r.octets();require(value.size()==16,Error::encoding);std::uint64_t bits{};for(char ch:value)bits=(bits<<4U)|nibble(ch);if(output_)node=std::bit_cast<double>(bits);}
            else if(tag=="string"){const auto value=octets();result_.error.retained_bytes=plus(result_.error.retained_bytes,sizeof(Json::string_t));if(output_)node=unhex(value);}
            else if(tag=="binary"){const auto value=r.octets();const auto subtype=r.optional();const auto size=value.size()/2;result_.error.binary_bytes=plus(result_.error.binary_bytes,size);bound(result_.error.binary_bytes,limits_.binary_bytes);result_.error.retained_bytes=plus(result_.error.retained_bytes,plus(sizeof(Json::binary_t),size));result_.maximum_binary=std::max(result_.maximum_binary,size);result_.maximum_octets=std::max(result_.maximum_octets,size);if(output_){auto decoded=unhex(value);Json::binary_t bytes;bytes.assign(decoded.begin(),decoded.end());if(subtype)bytes.set_subtype(*subtype);node=std::move(bytes);}}
            else if(tag=="discarded"){if(output_)node=Json(Json::value_t::discarded);}
            else if(tag=="array"){const auto declared=r.number<std::uint64_t>();require(declared<=limits_.context_nodes-result_.error.nodes,Error::capacity);require(declared<=Json::array_t{}.max_size(),Error::capacity);children=static_cast<std::size_t>(declared);result_.error.retained_bytes=plus(result_.error.retained_bytes,plus(sizeof(Json::array_t),times(children,sizeof(Json))));if(output_){node=Json::array();node.get_ref<Json::array_t&>().reserve(children);}}
            else if(tag=="object"){r.array();const auto begin=r.position();std::string_view previous;while(!r.next(']')){const auto value=octets();if(children)require(previous<value,Error::schema);previous=value;children=plus(children,1);bound(children,limits_.context_nodes-result_.error.nodes);result_.maximum_key=std::max(result_.maximum_key,value.size()/2);}r.end_array();keys=r.slice(begin);result_.error.retained_bytes=plus(result_.error.retained_bytes,plus(sizeof(Json::object_t),times(children,sizeof(Json::object_t::value_type))));if(output_)node=Json::object();}
            else throw Error::schema;
            r.end_array();Json* placed=nullptr;
            if(output_){if(!active){output_->error.context.swap(node);placed=&output_->error.context;}else{auto& parent=stack[active-1];if(parent.value->is_object()){auto& cursor=parent.keys;if(cursor.front()=='['||cursor.front()==',')cursor.remove_prefix(1);require(cursor.front()=='"');cursor.remove_prefix(1);const auto end=cursor.find('"');auto key=unhex(cursor.substr(0,end));cursor.remove_prefix(end+1);core::detail::append_object(*parent.value,key,node);placed=&parent.value->find(key).value();}else{parent.value->push_back(std::move(node));placed=&parent.value->back();}}}
            if(active)--stack[active-1].remaining;
            if(children){require(active<stack.size(),Error::capacity);stack[active++]={placed,children,keys};}
            while(active && stack[active-1].remaining==0)--active;
        }
        r.end_array();require(result_.error.nodes!=0 && active==0,Error::schema);
    }
};
constexpr std::array quantity_members{&Quantities::context_nodes,&Quantities::context_depth,&Quantities::string_bytes,&Quantities::binary_bytes,&Quantities::targets,&Quantities::nodes,&Quantities::depth,&Quantities::text_bytes,&Quantities::canonical_bytes,&Quantities::provenance_bytes,&Quantities::retained_bytes,&Quantities::consumer_simultaneous_bytes};
void valid_limits(const Limits& l){
    require(l.context_depth<=64 && l.depth<=7 && l.targets<=65536,Error::invalid_limits);
    require(l.context_nodes<=Json::array_t{}.max_size() && l.nodes<=Json::array_t{}.max_size(),Error::invalid_limits);
    require(l.string_bytes<=std::string{}.max_size() && l.binary_bytes<=Json::binary_t{}.max_size() && l.text_bytes<=std::string{}.max_size() && l.canonical_bytes<=std::string{}.max_size() && l.provenance_bytes<=std::string{}.max_size(),Error::invalid_limits);
}
void maxima(const Json& node,Parsed& measured){
    if(node.is_string())measured.maximum_octets=std::max(measured.maximum_octets,node.get_ref<const std::string&>().size());
    if(node.is_binary()){measured.maximum_binary=std::max(measured.maximum_binary,node.get_binary().size());measured.maximum_octets=std::max(measured.maximum_octets,node.get_binary().size());}
    if(node.is_object())for(auto it=node.cbegin();it!=node.cend();++it){measured.maximum_key=std::max(measured.maximum_key,it.key().size());measured.maximum_octets=std::max(measured.maximum_octets,it.key().size());}
    if(node.is_structured())for(const auto& child:node)maxima(child,measured);
}
struct EncoderCells {
    Sink* value_emit_sink;std::string_view value_emit_key;const core::Error* value_emit_error;std::size_t value_emit_nodes;
    const detail::ResolvedInput* input;Limits limits;Metadata metadata,metadata_return;
    Parsed measured;private_error::Usage usage_return;Usage usage,usage_returned;
    const private_error::Owner* source_owner;const core::Error* source_error;
    Sink counting,building;Json dom;core::detail::JsonCleanup<Json> dom_cleanup;
    std::unique_ptr<detail::State> candidate,constructor_argument;
    std::expected<private_error::Owner,core::Error> retained;
    core::detail::ExpectedErrorCleanup<std::expected<private_error::Owner,core::Error>> retained_cleanup;
    std::expected<std::string,core::Error> rendered;
    core::detail::ExpectedErrorCleanup<std::expected<std::string,core::Error>> rendered_cleanup;
    std::expected<journal::RunId,core::Error> run;
    std::expected<journal::Timestamp,core::Error> timestamp;
    core::detail::ExpectedErrorCleanup<std::expected<journal::RunId,core::Error>> run_cleanup;
    core::detail::ExpectedErrorCleanup<std::expected<journal::Timestamp,core::Error>> timestamp_cleanup;
    core::JcsLimits renderer_limits;private_error::Limits retain_limits;
    std::expected<Owner,Error> returned;Owner constructed_owner;std::unexpected<Error> unexpected;
    core::Sha256Digest hash_return,initial_hash;std::string_view hash_input,empty_hash_input;
    Owner* owner_constructor_this;detail::State* state_constructor_this;
    std::size_t fixed,retained_bytes,simultaneous,dom_bytes,scratch_bytes,temporary_bytes;
    const Parsed* usage_parameter;std::size_t upper_parameter;bool decode_parameter;
    const Usage* fits_usage;const Limits* fits_limits;std::size_t Quantities::* fits_member;
    const Limits* limits_parameter;Json::array_t limit_array;Json::binary_t limit_binary;std::string limit_string;
    const core::Error* retained_source;const Metadata* copy_metadata;detail::State* copy_state;
    const Provenance* metadata_source;Metadata metadata_constructed;FactView fact_constructed;
    const Metadata* identity_metadata;
    const Metadata* local_metadata_parameter;const FactView* local_metadata_fact;
    const Metadata* emit_metadata;const core::Error* emit_error;Sink* emit_sink;std::size_t emit_nodes;
    const FactView* emit_fact;const p::Target* emit_target;
    struct RecursiveEmit {Sink* sink;const Json* value;const Json* child;const Json* range;Json::const_iterator iterator,child_begin,child_end;std::uint64_t bits;std::array<char,16> text;std::size_t index;const Json::binary_t* bytes;std::string_view binary_view;std::optional<std::uint64_t> subtype;} ;
    std::array<RecursiveEmit,65> emit_frames;
    struct RecursiveMaxima {const Json* node;Parsed* measured;const Json* child;const Json* range;Json::const_iterator iterator,child_begin,child_end;};
    std::array<RecursiveMaxima,65> maxima_frames;
    Sink *sink_constructor_this,*object_this,*array_this,*end_this,*text_this,*octets_this,*boolean_this,*null_this,*number_this,*optional_this,*prefix_this,*insert_this,*open_this;
    Json* sink_root;std::string_view object_key,array_key,text_key,text_value,octets_key,octets_value,boolean_key,null_key,number_key,optional_key,prefix_key,insert_key,open_key;
    std::size_t array_size,open_size;bool boolean_value,open_object;std::optional<std::uint64_t> optional_value;
    WireFrame *prefix_frame,*insert_frame;WireFrame assigned_frame;
    Json text_node,boolean_node,null_node,open_node;core::detail::JsonCleanup<Json> open_cleanup;
    Json *insert_node,*insert_return,*open_placed;Json::iterator find_return;
    std::string text_constructed,hex_encoded,hex_return,hex_output,key_constructed;
    std::string_view hex_input,append_key;Json *append_object,*append_child;
    std::size_t hex_index;unsigned char hex_byte;
    Decimal decimal;Decimal* decimal_this;std::uint64_t number_unsigned;std::int64_t number_signed;
    std::uint64_t decimal_unsigned;std::int64_t decimal_signed;std::to_chars_result converted;
    const Decimal* decimal_view_this;std::string_view decimal_view_return;
    const Counts* dom_parameter;std::size_t dom_return;
    std::size_t plus_a,plus_b,plus_return,times_a,times_b,times_return,bound_value,bound_limit;
    std::size_t* add_value;std::size_t add_amount,multiply_a,multiply_b;
    std::optional<std::size_t> multiplied,multiply_return;bool add_return,required;
    Error required_error,caught;
    const std::size_t *max_left,*max_right,*max_result;std::size_t max_candidate;
    std::string_view enum_return;std::size_t enum_index;
    p::Domain domain_value; p::Producer producer_value;Kind kind_value;p::Category category_value;p::Selector selector_value;p::FailureReason reason_value;
    const void* enum_names;core::ErrorCategory core_category;core::ErrorSeverity core_severity;core::ContinuationPolicy core_continuation;
    const void* core_names;const void* enum_pair;std::string_view enum_label;
    std::array<std::size_t,3> maxima_values;std::initializer_list<std::size_t> maxima_list;
    const FactView* copy_fact;Association constructed_association;std::vector<p::Target> constructed_targets;
    std::span<const p::Target>::iterator targets_begin,targets_end;
    detail::State *clone_state,*render_state;const core::Error* clone_source;const private_error::Usage* clone_usage;
    const Counts* render_counts;const core::Error* render_source;private_error::Usage cloned_usage_return;
    Json::array_t* reserved_array;std::size_t previous_children,inner_sum,outer_sum,inner_product,outer_product;
    std::optional<FactView> constructed_fact;const detail::View* input_view;
    Provenance* copied_provenance;const Association* metadata_association;
    std::span<const p::Target> target_range;std::span<const p::Target>::iterator emit_begin,emit_end;
};
struct DecoderCells {
    std::string_view body;Limits limits;Parser counter,parser;Parsed counted,parsed;
    Scratch scratch;Usage usage;Metadata view;std::unique_ptr<detail::State> candidate;
    const Provenance* source_provenance;
    std::array<ContextFrame,65> context_frames;std::size_t active,children;
    ContextFrame assigned_context;ContextFrame* parent;Json node;core::detail::JsonCleanup<Json> node_cleanup;
    Json* placed;std::string_view keys,tag,previous,key_value,code,message,operation,os_domain,os_message;
    std::string decoded,key,code_decoded,message_decoded,operation_decoded,domain_decoded,os_message_decoded;
    Json::binary_t binary;core::StableErrorCode constructed_code;core::OsError constructed_os;
    std::uint64_t declared,bits;std::int64_t signed_value;std::uint64_t unsigned_value;int os_value;bool boolean_value;
    std::optional<std::uint64_t> subtype;std::size_t key_begin,key_end,binary_size,index;
    std::vector<std::size_t> target_index;const std::vector<p::Target>* comparator_targets;
    std::size_t comparator_left,comparator_right; p::Target target;std::size_t duplicate_index;
    Parser* parser_constructor_this;Reader* reader_constructor_this;
    std::string_view parser_body,reader_body;const Limits *parser_limits,*reader_limits;
    Scratch* parser_output;std::size_t target_capacity;
    Reader* reader;Parser *parse_this,*association_this,*error_this,*context_this,*stock_this,*octets_this;
    std::size_t stock_size;std::string_view octets_return;Metadata* parsed_metadata;FactView* parsed_fact;
    Association* association;Provenance* provenance;
    Reader *position_this,*slice_this,*next_this,*expect_this,*key_this,*object_this,*array_this,*end_object_this,*end_array_this,*text_this,*octets_reader_this,*number_this,*null_this,*optional_this,*boolean_this,*finished_this,*node_this;
    std::size_t slice_begin,position_return,begin,peek_at;char peek_char;std::string_view expected,key_expected,text_result,text_return,hex_return,number_text;
    ParseFrame assigned_parse;ParseFrame* parse_frame;unsigned char parsed_byte;
    std::string_view integer_text,integer_magnitude,unhex_input,valid_hex_input;
    bool negative;std::uint64_t integer_unsigned;std::int64_t integer_signed;int integer_int;
    std::from_chars_result converted;std::string unhex_output;std::size_t unhex_index;char nibble_char;unsigned nibble_return;
    const Metadata* local_metadata;const FactView* local_fact;
    const p::Target* valid_target;const FactView* target_fact;
    std::string_view enum_text;const void* enum_names;std::size_t enum_index;const void* enum_pair;std::string_view enum_label;
    std::expected<Owner,Error> returned;Owner constructed_owner;std::unexpected<Error> unexpected;
    const std::vector<p::Target>* duplicate_targets;std::string_view* key_cursor;
    std::string_view cursor_key,context_value,context_binary,context_float;
    std::size_t context_key_begin,context_key_end;char binary64_char,integer_char,hex_char;
    core::ErrorCategory parsed_category;core::ErrorSeverity parsed_severity;core::ContinuationPolicy parsed_continuation;
    Json::array_t array_domain;Json array_constructed,object_constructed,discarded_constructed;
    Json::array_t* context_array;Json::iterator context_find_return;
    const char *hex_begin,*hex_end,*integer_begin,*integer_end;
    std::string::iterator binary_begin,binary_end;

};
auto measured_usage(const Parsed& m,std::size_t upper,bool decode)->Usage {
    Usage u{m.error.nodes,m.error.depth,m.error.string_bytes,m.error.binary_bytes,m.target_count,m.wire.nodes,m.wire.depth,m.wire.text,m.wire.bytes,plus(m.metadata.run.size(),m.metadata.time.size()),0,0};
    require(m.error.retained_bytes>=sizeof(private_error::Owner),Error::source);
    u.retained_bytes=plus(plus(sizeof(Owner)+sizeof(detail::State),u.provenance_bytes),plus(times(u.targets,sizeof(p::Target)),plus(m.error.retained_bytes-sizeof(private_error::Owner),u.canonical_bytes)));
    // Separate child-factory clone allowance and decoder's original Error scratch.
    u.consumer_simultaneous_bytes=plus(plus(upper,sizeof(EncoderCells)),u.retained_bytes);
    u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,m.error.retained_bytes);
    if(decode)u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,plus(sizeof(DecoderCells),m.error.retained_bytes));
    u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,dom_stock(m.wire));
    if(decode)u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,times(u.targets,sizeof(std::size_t)));
    // Two identity validator string results; one live hex buffer, decoded
    // key/copy, and decoded binary string/vector constructor coexistence.
    u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,u.provenance_bytes);
    u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,times(m.maximum_octets,2));
    if(decode)u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,plus(times(m.maximum_key,2),times(m.maximum_binary,2)));
    u.consumer_simultaneous_bytes=plus(u.consumer_simultaneous_bytes,plus(std::size_t{21},times(decode?2U:1U,sizeof(Json::object_t))));
    return u;
}
void fits(const Usage& usage,const Limits& limits){for(auto member:quantity_members)bound(usage.*member,limits.*member);}
void copy_metadata(detail::State& state,const Metadata& m){
    auto& v=state.provenance;v.run=m.run;v.time=m.time;v.domain=m.domain;v.entry_producer=m.reporter;v.kind=m.kind;v.sequence=m.sequence;v.creation_ordinal=m.ordinal;
    if(m.fact){const auto& f=*m.fact;v.association.emplace(Association{f.producer,f.process,f.secondary,f.secondary_ordinal,{f.targets.begin(),f.targets.end()}});}
}
void clone_error(detail::State& state,const core::Error& source,const private_error::Usage& usage){
    auto retained=private_error::retain(source,{usage.nodes,usage.depth,usage.string_bytes,usage.binary_bytes,usage.retained_bytes});core::detail::ExpectedErrorCleanup cleanup{retained};require(retained.has_value(),Error::source);require(retained->usage()==usage,Error::source);state.error=std::move(*retained);
}
void render(detail::State& state,const Counts& counts){
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};const auto m=metadata(state.provenance);emit(sink,m,*state.error->source(),state.usage.context_nodes);
    auto rendered=core::canonicalize_json_jcs(dom,{counts.bytes,counts.depth,counts.nodes});core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value(),Error::encoding);state.canonical=std::move(*rendered);state.digest=core::sha256(state.canonical);
}

// Ordinary value-composition entry stock supplements the unchanged inherited
// parser/render allowance. It is a logical bound, not a compiler stack claim.
struct ValueCells {
    const private_error::Owner* source;const core::Error* error;
    std::string_view body,key;Limits limits;Usage usage;Parsed parsed;
    Sink counter;Parser* parser;Scratch* scratch;
    private_error::Usage retained_usage;
    std::expected<private_error::Owner,core::Error> retained;
    core::detail::ExpectedErrorCleanup<std::expected<private_error::Owner,core::Error>> retained_cleanup;
    std::expected<private_error::Owner,Error> returned_owner;
    std::expected<std::string,Error> returned_text;
    std::string rendered;Usage* usage_output;
};
void value_limits(const Limits& limits){valid_limits(limits);require(limits.targets==0U&&limits.provenance_bytes==0U,Error::invalid_limits);}
auto value_usage(const Parsed& measured,bool decoding)->Usage {
    auto usage=measured_usage(measured,0U,decoding);
    usage.consumer_simultaneous_bytes=plus(usage.consumer_simultaneous_bytes,sizeof(ValueCells));
    usage.retained_bytes=decoding?measured.error.retained_bytes:measured.wire.bytes;
    usage.targets=0U;usage.provenance_bytes=0U;return usage;
}
auto render_value(const core::Error& error,const Parsed& measured)->std::string {
    Json dom;core::detail::JsonCleanup cleanup{dom};Sink sink{&dom};emit_error(sink,{},error,measured.error.nodes);
    auto rendered=core::canonicalize_json_jcs(dom,{measured.wire.bytes,measured.wire.depth,measured.wire.nodes});
    core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};require(rendered.has_value(),Error::encoding);return std::move(*rendered);
}
} // namespace

auto detail::checked_add(std::size_t& value,std::size_t amount) noexcept->bool{if(amount>SIZE_MAX-value)return false;value+=amount;return true;}
auto detail::checked_multiply(std::size_t a,std::size_t b) noexcept->std::optional<std::size_t>{if(b && a>SIZE_MAX/b)return {};return a*b;}
auto detail::fixed_stock() noexcept->std::size_t{return sizeof(EncoderCells);}
auto detail::decode_fixed_stock() noexcept->std::size_t{return sizeof(DecoderCells);}
Owner::Owner(std::unique_ptr<detail::State> state) noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept->Owner& =default;
Owner::~Owner()=default;
auto Owner::canonical()const noexcept->std::string_view{return state_?std::string_view{state_->canonical}:std::string_view{};}
auto Owner::digest()const noexcept->const core::Sha256Digest*{return state_?&state_->digest:nullptr;}
auto Owner::error()const noexcept->const core::Error*{return state_ && state_->error?state_->error->source():nullptr;}
auto Owner::provenance()const noexcept->const Provenance*{return state_?&state_->provenance:nullptr;}
auto Owner::origin()const noexcept->std::optional<Origin>{return state_?std::optional{state_->origin}:std::nullopt;}
auto Owner::usage()const noexcept->Usage{return state_?state_->usage:Usage{};}
auto Owner::prepare(const detail::ResolvedInput& input,Limits limits)->std::expected<Owner,Error>{
    try{
        valid_limits(limits);require(input.error && input.error->source(),Error::source);
        const auto& v=input.view;Metadata m{v.run,v.time,v.domain,v.entry_producer,v.kind,v.sequence,v.creation_ordinal,{}};
        if(v.fact)m.fact=FactView{v.fact->producer,v.fact->process_sequence,v.fact->secondary_of,v.fact->secondary_creation_ordinal,v.fact->targets};
        local_metadata(m);Parsed measured;measured.metadata=m;measured.error=input.error->usage();measured.target_count=m.fact?m.fact->targets.size():0;
        const auto& source=*input.error->source();Sink count;emit(count,m,source,measured.error.nodes);measured.wire=count.count;
        measured.maximum_octets=std::max({source.code.value().size(),source.message.size(),source.operation.size()});if(source.os_error)measured.maximum_octets=std::max({measured.maximum_octets,source.os_error->domain.size(),source.os_error->message.size()});maxima(source.context,measured);
        const auto usage=measured_usage(measured,input.upper_fixed,false);fits(usage,limits);identity(m);
        auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=Origin::session_selected;copy_metadata(*state,m);clone_error(*state,source,measured.error);render(*state,measured.wire);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto Owner::decode(std::string_view body,Limits limits)->std::expected<Owner,Error>{
    try{
        valid_limits(limits);Parser counter{body,limits};const auto counted=counter.parse();const auto usage=measured_usage(counted,0,true);fits(usage,limits);identity(counted.metadata);
        auto state=std::make_unique<detail::State>();state->usage=usage;state->origin=Origin::decoded_unassociated;
        {Scratch scratch;Parser parser{body,limits,&scratch,counted.target_count};const auto parsed=parser.parse();require(parsed.error==counted.error,Error::source);clone_error(*state,scratch.error,counted.error);state->provenance=std::move(scratch.provenance);}
        render(*state,counted.wire);require(state->canonical==body,Error::encoding);return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}

auto detail::encode_value(const private_error::Owner& source,Limits limits,Usage& output)->std::expected<std::string,Error>{
    try {
        value_limits(limits);require(source.source(),Error::source);Parsed measured;measured.error=source.usage();
        const auto& error=*source.source();Sink count;emit_error(count,{},error,measured.error.nodes);measured.wire=count.count;
        measured.maximum_octets=std::max({error.code.value().size(),error.message.size(),error.operation.size()});
        if(error.os_error)measured.maximum_octets=std::max({measured.maximum_octets,error.os_error->domain.size(),error.os_error->message.size()});
        maxima(error.context,measured);const auto usage=value_usage(measured,false);fits(usage,limits);
        auto rendered=render_value(error,measured);output=usage;return rendered;
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
auto detail::decode_value(std::string_view body,Limits limits,Usage& output)->std::expected<private_error::Owner,Error>{
    try {
        value_limits(limits);Parser counter{body,limits};const auto counted=counter.parse_value();const auto usage=value_usage(counted,true);fits(usage,limits);
        Scratch scratch;Parser parser{body,limits,&scratch};const auto parsed=parser.parse_value();require(parsed.error==counted.error,Error::source);
        const auto& bounds=counted.error;auto retained=private_error::retain(scratch.error,{bounds.nodes,bounds.depth,bounds.string_bytes,bounds.binary_bytes,bounds.retained_bytes});
        core::detail::ExpectedErrorCleanup retained_cleanup{retained};require(retained.has_value()&&retained->usage()==bounds,Error::source);
        auto rendered=render_value(*retained->source(),counted);require(rendered==body,Error::encoding);output=usage;return std::move(*retained);
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::error_serialization
