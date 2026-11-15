#include "results/build_projection.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace epycbench::results::build_projection {
namespace {
using Json = nlohmann::json;
void require(bool value, Error error = Error::source) { if (!value) throw error; }
auto plus(std::size_t a, std::size_t b) -> std::size_t {
    require(detail::checked_add(a,b), Error::capacity); return a;
}
auto times(std::size_t a, std::size_t b) -> std::size_t {
    const auto result=detail::checked_multiply(a,b);
    require(result.has_value(),Error::capacity); return *result;
}
void bound(std::size_t value, std::size_t limit) { require(value<=limit,Error::capacity); }
constexpr std::array<std::string_view,3> states{"not_attempted","not_obtained","available"};
constexpr std::array<std::string_view,6> features{"linux","cxx23","testing","asan_ubsan","tsan","native_reference"};
constexpr std::array<std::string_view,20> omissions{
    "project_version","git.revision","compiler.id","compiler.version","compiler.target",
    "compiler.flags","linker.id","linker.version","preset","dependencies.hwloc",
    "dependencies.libnuma","dependencies.libfyaml","dependencies.CLI11","dependencies.nlohmann-json",
    "dependencies.fmt","dependencies.Catch2","build_identity","manifest_digest","source_identity","failure_payload"};
struct SafeFacts {
    Disposition disposition;
    bool dirty{};
    std::array<build_manifest::Feature,6> features{};
    std::size_t count{};
    std::optional<FailureAddress> failure;
};
struct Counts { std::size_t nodes{},depth{},text{},bytes{},objects{},arrays{},strings{},members{},elements{}; };
struct Frame { Json* json{}; std::size_t children{}; bool object{}; };
struct Number {
    std::array<char,20> bytes{};
    std::size_t size{};
    explicit Number(std::uint64_t value) {
        const auto converted=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);
        require(converted.ec==std::errc{}); size=static_cast<std::size_t>(converted.ptr-bytes.data());
    }
    auto view() const noexcept -> std::string_view { return {bytes.data(),size}; }
};
class Sink {
  public:
    Counts count;
    explicit Sink(Json* output=nullptr):output_(output) {}
    void object(std::string_view key={}) { open(key,true,0); }
    void array(std::string_view key,std::size_t size) { open(key,false,size); }
    void end() { require(level_!=0); --level_; count.bytes=plus(count.bytes,1); }
    void text(std::string_view key,std::string_view value) {
        prefix(key); count.strings=plus(count.strings,1); count.text=plus(count.text,value.size());
        count.bytes=plus(count.bytes,plus(value.size(),2));
        if(output_) { Json item=std::string{value}; insert(key,item); }
    }
    void boolean(std::string_view key,bool value) {
        prefix(key); count.bytes=plus(count.bytes,value?4U:5U);
        if(output_) { Json item=value; insert(key,item); }
    }
    void null(std::string_view key) {
        prefix(key); count.bytes=plus(count.bytes,4);
        if(output_) { Json item; insert(key,item); }
    }
    void number(std::string_view key,std::uint64_t value) { const Number number{value}; text(key,number.view()); }
  private:
    Json* output_;
    std::array<Frame,4> frames_{};
    std::size_t level_{};
    void prefix(std::string_view key) {
        count.nodes=plus(count.nodes,1); count.depth=std::max(count.depth,level_+1);
        if(!level_)return;
        auto& frame=frames_[level_-1];
        if(frame.children++)count.bytes=plus(count.bytes,1);
        if(frame.object) {
            count.members=plus(count.members,1); count.text=plus(count.text,key.size());
            count.bytes=plus(count.bytes,plus(key.size(),3));
        } else count.elements=plus(count.elements,1);
    }
    auto insert(std::string_view key,Json& item) -> Json* {
        if(!level_) { output_->swap(item); return output_; }
        auto& frame=frames_[level_-1];
        if(frame.object) {
            core::detail::append_object(*frame.json,key,item);
            return &frame.json->find(key).value();
        }
        frame.json->push_back(std::move(item)); return &frame.json->back();
    }
    void open(std::string_view key,bool object,std::size_t size) {
        prefix(key); require(level_<frames_.size()); count.bytes=plus(count.bytes,1);
        if(object)count.objects=plus(count.objects,1);else count.arrays=plus(count.arrays,1);
        Json* placed=nullptr;
        if(output_) {
            Json item=object?Json::object():Json::array(); core::detail::JsonCleanup cleanup{item};
            if(!object)item.get_ref<Json::array_t&>().reserve(size);
            placed=insert(key,item);
        }
        frames_[level_++]={placed,0,object};
    }
};
void emit(Sink& sink,const SafeFacts& facts) {
    sink.object(); sink.text("schema",profile);
    sink.text("disposition",states[static_cast<std::size_t>(facts.disposition)]);
    if(facts.disposition==Disposition::available) {
        sink.object("manifest"); sink.boolean("git_dirty",facts.dirty); sink.text("cxx_standard","C++23");
        sink.array("enabled_features",facts.count);
        for(std::size_t i=0;i<facts.count;++i)sink.text({},features[static_cast<std::size_t>(facts.features[i])]);
        sink.end(); sink.end();
    }else sink.null("manifest");
    if(facts.failure) {
        sink.object("failure"); sink.text("category","base_build"); sink.text("selector","base_build_acquisition");
        sink.number("sequence",facts.failure->sequence);
        if(facts.failure->creation_ordinal)sink.number("creation_ordinal",*facts.failure->creation_ordinal);
        else sink.null("creation_ordinal");
        sink.end();
    }else sink.null("failure");
    sink.array("omissions",omissions.size());for(auto token:omissions)sink.text({},token);sink.end();sink.end();
}
auto dom_stock(const Counts& c) -> std::size_t {
    auto value=times(c.objects,sizeof(Json::object_t));
    value=plus(value,times(c.members,sizeof(Json::object_t::value_type)));
    value=plus(value,times(c.arrays,sizeof(Json::array_t)));
    value=plus(value,times(c.elements,sizeof(Json)));
    value=plus(value,times(c.strings,sizeof(Json::string_t)));
    return plus(value,c.text);
}
// Named logical frame inventory, conservatively summed across phases. Core
// renderer scratch is separately bounded by exact nodes/depth/output bytes.
struct FixedCells {
    const detail::ResolvedInput* input;
    const Limits* limits;
    std::optional<build_manifest::Input> manifest_input;
    const build_manifest::Owner* getter_this;
    const build_manifest::Owner* digest_this;
    const build_manifest::ManifestDigest *digest_check_return,*digest_value_return;
    const void* getter_fields;
    build_manifest::Input getter_constructed;
    SafeFacts facts;
    Usage usage;
    Sink count_sink,build_sink;
    std::size_t feature_index,prior_feature,feature_ordinal;
    std::unique_ptr<detail::State> candidate,constructor_parameter;
    detail::State* make_unique_return;
    Owner* owner_constructor_this;
    detail::State* state_constructor_this;
    PrivateProvenance* provenance_constructor_this;
    Json dom,open_item,text_item,boolean_item,null_item;
    core::detail::JsonCleanup<Json> dom_cleanup,open_cleanup;
    std::string text_constructed,key_constructed;
    std::expected<std::string,core::Error> rendered;
    core::detail::ExpectedErrorCleanup<std::expected<std::string,core::Error>> rendered_cleanup;
    std::expected<Owner,Error> returned;
    Owner constructed_owner;
    core::Sha256Digest hash_return,initial_hash_return;
    std::string_view hash_empty_input,hash_canonical_input,provenance_run_input,provenance_time_input,checksum_input;
    const SafeFacts* emit_facts;
    Sink* emit_sink;
    std::size_t emit_index;
    std::string_view omission_token;
    const Counts* dom_count;
    std::size_t dom_value;
    Sink *object_this,*array_this,*open_this,*prefix_this,*insert_this,*text_this,*boolean_this,*null_this,*number_this,*end_this;
    Sink* sink_constructor_this;
    Json* sink_constructor_output;
    std::string_view object_key,array_key,open_key,prefix_key,insert_key,text_key,text_value,boolean_key,null_key,number_key;
    std::size_t array_size,open_size;
    bool open_object,boolean_value;
    Frame *prefix_frame,*insert_frame;
    Frame assigned_frame;
    Json* open_placed;
    Json* insert_item;
    Json* insert_return;
    Json::iterator find_return;
    Json *append_object_parameter,*append_object_child;
    std::string_view append_object_key;
    Number number;
    Number* number_constructor_this;
    const Number* view_this;
    std::uint64_t number_value,number_constructor_value;
    std::to_chars_result converted;
    std::string_view number_view;
    std::size_t plus_a,plus_b,times_a,times_b,bound_value,bound_limit;
    std::size_t plus_return,left_sum,right_sum,dom_return,times_return,depth_candidate,prefix_previous_children;
    const std::size_t *max_left,*max_right,*max_return;
    std::size_t* checked_add_value;
    std::size_t checked_add_amount,checked_multiply_a,checked_multiply_b;
    std::optional<std::size_t> multiply_result;
    std::optional<std::size_t> multiply_return;
    std::unexpected<Error> unexpected_result;
    bool add_return,valid_limits_return;
    bool require_value;
    Error require_error,caught_error;
    const Limits* valid_limits_parameter;
    std::size_t valid_limits_maximum;
    std::string valid_limits_empty;
    std::array<std::size_t,7> valid_limits_values;
    std::initializer_list<std::size_t> valid_limits_view;
    std::size_t valid_limits_value;
    core::JcsLimits renderer_limits;
};
}
auto detail::checked_add(std::size_t& value,std::size_t amount) noexcept -> bool {
    if(amount>SIZE_MAX-value)return false;
    value+=amount;return true;
}
auto detail::checked_multiply(std::size_t a,std::size_t b) noexcept -> std::optional<std::size_t> {
    if(a && b>SIZE_MAX/a)return {};
    return a*b;
}
auto detail::valid_limits(const Limits& limits) noexcept -> bool {
    if(limits.features>6 || limits.failure_references>1 || limits.depth>4)return false;
    const auto maximum=std::string{}.max_size();
    for(auto value:{limits.nodes,limits.text_bytes,limits.canonical_bytes,limits.private_text_bytes,
        limits.retained_bytes,limits.consumer_simultaneous_bytes,limits.depth})if(value>maximum)return false;
    return true;
}
auto detail::fixed_stock() noexcept -> std::size_t { return sizeof(FixedCells); }
Owner::Owner(std::unique_ptr<detail::State> state) noexcept:state_(std::move(state)) {}
Owner::~Owner()=default;
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
auto Owner::reduced_canonical() const noexcept -> std::string_view { return state_?std::string_view{state_->canonical}:std::string_view{}; }
auto Owner::reduced_digest() const noexcept -> const core::Sha256Digest* { return state_?&state_->digest:nullptr; }
auto Owner::private_provenance() const noexcept -> const PrivateProvenance* { return state_?&state_->provenance:nullptr; }
auto Owner::usage() const noexcept -> Usage { return state_?state_->usage:Usage{}; }
auto Owner::prepare(const detail::ResolvedInput& input,const Limits& limits) -> std::expected<Owner,Error> {
    try {
        require(detail::valid_limits(limits),Error::invalid_limits);
        const auto source=input.manifest?input.manifest->input():std::optional<build_manifest::Input>{};
        require(static_cast<unsigned>(input.disposition)<states.size());
        require(source.has_value()==(input.disposition==Disposition::available));
        require(input.failure.has_value()==(input.disposition==Disposition::not_obtained));
        SafeFacts facts{.disposition=input.disposition,.failure=input.failure};
        if(source) {
            require(source->compiler.cxx_standard=="C++23" && input.manifest->digest());
            require(source->enabled_features.size()>=2 && source->enabled_features.size()<=6);
            require(source->enabled_features[0]==build_manifest::Feature::linux_platform && source->enabled_features[1]==build_manifest::Feature::cxx23);
            std::size_t prior=0;
            for(std::size_t i=0;i<source->enabled_features.size();++i) {
                const auto index=static_cast<std::size_t>(source->enabled_features[i]);
                require(index<features.size() && (!i || index>prior)); prior=index;
                facts.features[i]=source->enabled_features[i];
            }
            facts.count=source->enabled_features.size();facts.dirty=source->git.dirty;
        }
        Sink counted;emit(counted,facts);
        Usage usage{facts.count,input.failure?1U:0U,counted.count.nodes,counted.count.depth,
            counted.count.text,counted.count.bytes};
        usage.private_text_bytes=plus(input.run.size(),input.time.size());
        if(source)usage.private_text_bytes=plus(usage.private_text_bytes,source->build_identity.size());
        usage.retained_bytes=plus(plus(sizeof(Owner),sizeof(detail::State)),plus(usage.private_text_bytes,usage.canonical_bytes));
        usage.consumer_simultaneous_bytes=plus(plus(input.session_fixed,detail::fixed_stock()),plus(usage.retained_bytes,dom_stock(counted.count)));
        bound(usage.features,limits.features);bound(usage.failure_references,limits.failure_references);
        bound(usage.nodes,limits.nodes);bound(usage.depth,limits.depth);bound(usage.text_bytes,limits.text_bytes);
        bound(usage.canonical_bytes,limits.canonical_bytes);bound(usage.private_text_bytes,limits.private_text_bytes);
        bound(usage.retained_bytes,limits.retained_bytes);bound(usage.consumer_simultaneous_bytes,limits.consumer_simultaneous_bytes);
        auto state=std::make_unique<detail::State>();
        state->provenance.run=input.run;state->provenance.handoff_time=input.time;
        state->provenance.domain=input.domain;state->provenance.origin=input.origin;state->provenance.sequence=input.sequence;
        state->provenance.disposition=input.disposition;state->provenance.failure=input.failure;
        if(source) {
            state->provenance.producer_checksum=source->build_identity;
            state->provenance.manifest_digest=input.manifest->digest()->value();
        }
        Json dom;core::detail::JsonCleanup cleanup{dom};Sink built{&dom};emit(built,facts);
        auto rendered=core::canonicalize_json_jcs(dom,{usage.canonical_bytes,usage.depth,usage.nodes});
        core::detail::ExpectedErrorCleanup rendered_cleanup{rendered};
        require(rendered.has_value() && rendered->size()==usage.canonical_bytes,Error::rendering);
        state->canonical=std::move(*rendered);core::detail::retire_json(dom);
        state->digest=core::sha256(state->canonical);state->usage=usage;
        return Owner{std::move(state)};
    }catch(Error error){return std::unexpected{error};}
    catch(const std::bad_alloc&){throw;}
    catch(const std::length_error&){return std::unexpected{Error::capacity};}
    catch(...){return std::unexpected{Error::rendering};}
}
} // namespace epycbench::results::build_projection
