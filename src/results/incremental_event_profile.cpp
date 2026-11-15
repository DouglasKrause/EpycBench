#include "results/incremental_event_profile.hpp"
#include "results/private_execution_plan.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace epycbench::results::event_profile {
namespace detail {
void require(bool condition, Error error) { if (!condition) throw error; }
auto add(std::size_t a, std::size_t b) -> std::size_t {
    require(b <= SIZE_MAX - a, Error::capacity); return a + b;
}
auto multiply(std::size_t a, std::size_t b) -> std::size_t {
    require(!a || b <= SIZE_MAX / a, Error::capacity); return a * b;
}
void Work::reserve_peak(std::size_t amount) {
    require(amount<=peak_limit,Error::capacity); peak=std::max(peak,amount);
}
void Work::debit(std::size_t amount) {
    require(used <= limit && amount <= limit - used, Error::capacity); used += amount;
}
auto Work::equal(std::string_view a, std::string_view b) -> bool {
    debit(add(a.size(), b.size())); return a == b;
}
namespace {
using Json = nlohmann::json;
// Each admitted tree has depth <= 64. Empty descendants before the library
// container destructor runs, including when SAX construction throws bad_alloc.
void retire(Json& value) noexcept {
    if (value.is_structured()) {
        for (auto& child : value) retire(child);
        value.clear();
    }
}
struct Tree {
    Json value;
    ~Tree() { retire(value); }
};
struct Counter : nlohmann::json_sax<Json> {
    const Limits& limits;
    Shape result{};
    std::size_t nesting{};
    explicit Counter(const Limits& value) : limits(value) {}
    void node() {
        result.nodes = add(result.nodes, 1);
        result.depth = std::max(result.depth, add(nesting, 1));
        require(result.nodes <= limits.nodes && result.depth <= limits.depth, Error::capacity);
    }
    void text(std::size_t size) {
        result.text = add(result.text, size);
        require(result.text <= limits.text_bytes, Error::capacity);
    }
    auto null() -> bool override { node(); return true; }
    auto boolean(bool) -> bool override { node(); return true; }
    auto number_integer(number_integer_t value) -> bool override {
        require(value >= -9007199254740991LL && value <= 9007199254740991LL, Error::canonical);
        node(); return true;
    }
    auto number_unsigned(number_unsigned_t value) -> bool override {
        require(value <= 9007199254740991ULL, Error::canonical); node(); return true;
    }
    auto number_float(number_float_t value, const string_t& token) -> bool override {
        require(std::isfinite(value), Error::canonical);
        result.maximum_token_bytes = std::max(result.maximum_token_bytes, token.size());
        node(); return true;
    }
    auto string(string_t& value) -> bool override {
        result.maximum_token_bytes = std::max(result.maximum_token_bytes, value.size());
        node(); text(value.size()); return true;
    }
    auto binary(binary_t&) -> bool override { throw Error::schema; }
    auto start_object(std::size_t) -> bool override { node(); ++nesting; return true; }
    auto start_array(std::size_t) -> bool override { node(); ++nesting; return true; }
    auto key(string_t& value) -> bool override {
        result.members = add(result.members, 1);
        result.maximum_token_bytes = std::max(result.maximum_token_bytes, value.size());
        result.key_text_bytes = add(result.key_text_bytes, value.size());
        text(value.size()); return true;
    }
    auto end_object() -> bool override { --nesting; return true; }
    auto end_array() -> bool override { --nesting; return true; }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) -> bool override {
        throw Error::schema;
    }
};
struct Builder final : nlohmann::json_sax<Json> {
    struct Frame { Json* value{}; std::string key; };
    Tree root;
    std::array<Frame, 64> frames{};
    std::size_t depth{};
    auto destination() -> Json& {
        if (!depth) return root.value;
        auto& f = frames[depth - 1];
        if (f.value->is_array()) { f.value->push_back(nullptr); return f.value->back(); }
        return (*f.value)[f.key];
    }
    template<class T> auto scalar(T&& value) -> bool { destination() = std::forward<T>(value); return true; }
    auto null() -> bool override { return scalar(nullptr); }
    auto boolean(bool v) -> bool override { return scalar(v); }
    auto number_integer(number_integer_t v) -> bool override { return scalar(v); }
    auto number_unsigned(number_unsigned_t v) -> bool override { return scalar(v); }
    auto number_float(number_float_t v, const string_t&) -> bool override { return scalar(v); }
    auto string(string_t& v) -> bool override { return scalar(std::move(v)); }
    auto binary(binary_t&) -> bool override { throw Error::schema; }
    auto begin(bool object) -> bool {
        require(depth < frames.size(), Error::capacity);
        auto& v = destination(); v = object ? Json::object() : Json::array();
        frames[depth++].value = &v; return true;
    }
    auto start_object(std::size_t) -> bool override { return begin(true); }
    auto start_array(std::size_t) -> bool override { return begin(false); }
    auto key(string_t& v) -> bool override {
        require(!frames[depth - 1].value->contains(v), Error::schema);
        frames[depth - 1].key = std::move(v); return true;
    }
    auto end_object() -> bool override { --depth; return true; }
    auto end_array() -> bool override { --depth; return true; }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) -> bool override {
        throw Error::schema;
    }
};
void fields(const Json& j, std::initializer_list<std::string_view> names) {
    require(j.is_object() && j.size() == names.size(), Error::schema);
    for (auto name : names) require(j.contains(name), Error::schema);
}
auto text(const Json& j) -> const std::string& {
    require(j.is_string(), Error::schema); return j.get_ref<const std::string&>();
}
auto integer(std::string_view s) -> std::uint64_t {
    require(!s.empty() && (s.size() == 1 || s.front() != '0'), Error::schema);
    std::uint64_t n{}; const auto r = std::from_chars(s.data(), s.data()+s.size(), n);
    require(r.ec == std::errc{} && r.ptr == s.data()+s.size(), Error::schema); return n;
}
auto name(Kind k) -> std::string_view {
    switch(k) {
    case Kind::creation: return "run_created";
    case Kind::normalization: return "scenario_resolved";
    case Kind::source_outcome: return "source_not_resolved";
    case Kind::topology: return "topology_retained";
    case Kind::failure: return "failure_recorded";
    case Kind::planning: return "execution_planned";
    case Kind::preflight: return "preflight_completed";
    case Kind::pair_started: return "pair_started";
    case Kind::request_accepted: return "request_accepted";
    case Kind::pair_terminal: return "pair_terminal";
    case Kind::event_eof: return "event_eof";
    case Kind::child_reaped: return "child_reaped";
    case Kind::lifecycle_progress: return "lifecycle_progress";
    case Kind::placement: return "placement_retained";
    case Kind::preparation: return "preparation_associated";
    case Kind::calibration: return "calibration_associated";
    case Kind::warmup: return "warmup_associated";
    case Kind::block_opened: return "block_opened";
    case Kind::block_closed: return "block_closed";
    case Kind::observation: return "observation_completed";
    case Kind::failed_observation: return "observation_failed";
    case Kind::monitoring: return "capture_retained";
    case Kind::stopping: return "stopping_retained";
    case Kind::analysis: return "pair_analysis";
    case Kind::finalization: return "pair_evidence_finalized";
    case Kind::run_closed: return "run_closed";
    }
    throw Error::unsupported;
}
auto category(Kind k) -> std::string_view {
    switch(k) {
    case Kind::creation: return "creation";
    case Kind::normalization: case Kind::source_outcome: return "normalization";
    case Kind::topology: return "topology";
    case Kind::failure: return "failures";
    case Kind::planning:return "planning";case Kind::preflight:return "preflight";
    case Kind::pair_started:case Kind::request_accepted:case Kind::pair_terminal:case Kind::event_eof:case Kind::child_reaped:case Kind::lifecycle_progress:return "runner_lifecycle";
    case Kind::placement:return "placement";case Kind::preparation:case Kind::calibration:return "calibration";
    case Kind::warmup:return "warmup";case Kind::block_opened:case Kind::block_closed:return "blocks";
    case Kind::observation:case Kind::failed_observation:return "observations";
    case Kind::monitoring:return "monitoring";case Kind::stopping:return "stopping";case Kind::analysis:return "analysis";
    case Kind::finalization:case Kind::run_closed:return "finalization";
    }
    throw Error::unsupported;
}
auto kind(std::string_view n) -> Kind {
    for(unsigned i{};i<=static_cast<unsigned>(Kind::run_closed);++i){const auto k=static_cast<Kind>(i);
        if(name(k)==n)return k;}
    throw Error::unsupported;
}
auto ref_name(RefKind k) -> std::string_view {
    switch(k) {
    case RefKind::creation: return "run_created";
    case RefKind::topology: return "topology_retained";
    case RefKind::creation_failure: return "creation_failure";
    case RefKind::ordinary_failure: return "ordinary_failure";
    case RefKind::source_resolution_failure: return "source_resolution_failure";
    }
    throw Error::reference;
}
auto producer(process_subject::Producer p) -> std::string_view {
    switch(p) {
    case process_subject::Producer::controller: return "controller";
    case process_subject::Producer::runner: return "runner";
    case process_subject::Producer::results: return "results";
    }
    throw Error::association;
}
auto event_id(std::string_view run, std::uint64_t sequence) -> std::string {
    return std::string{run} + ":" + std::to_string(sequence);
}
auto reference(const Json& j, std::string_view run) -> Reference {
    fields(j,{"event_id","kind","ordinal"}); const auto& id = text(j.at("event_id"));
    require(id.size() > run.size()+1 && id.starts_with(run) && id[run.size()]==':', Error::reference);
    const auto sequence = integer(std::string_view{id}.substr(run.size()+1));
    require(sequence != 0, Error::reference);
    for (auto k : {RefKind::creation,RefKind::topology,RefKind::creation_failure,RefKind::ordinary_failure,RefKind::source_resolution_failure})
        if (text(j.at("kind")) == ref_name(k)) return {sequence,integer(text(j.at("ordinal"))),k};
    throw Error::reference;
}
void write_reference(Json& j, Reference r, std::string_view run) {
    j = Json::object(); j["event_id"] = event_id(run,r.sequence);
    j["kind"] = ref_name(r.kind); j["ordinal"] = std::to_string(r.ordinal);
}
auto canonical(const Json& j, const Limits& limits) -> std::string {
    auto value = core::canonicalize_json_jcs(j,{limits.body_bytes,limits.depth,limits.nodes});
    core::detail::ExpectedErrorCleanup cleanup{value}; require(value.has_value(),Error::canonical);
    return std::move(*value);
}
template<class Owner, class L> auto child(std::string_view body, const L& limits) -> Owner {
    auto result = Owner::decode(body,limits); require(result.has_value(),Error::child);
    return std::move(*result);
}
}
auto json_stock(const Shape& s) -> std::size_t {
    // Logical containers, value/key cells, tree links and decoded string bytes.
    return add(add(multiply(s.nodes, sizeof(Json)+sizeof(Json::array_t)+sizeof(Json::object_t)),
                   multiply(s.members,sizeof(std::string)+sizeof(Json)+4*sizeof(void*))),
               add(s.text,add(s.nodes,s.members)));
}
auto workspace(const Shape& s) -> std::size_t {
    // Conservative logical preparation stock, not allocator/process heap size:
    // UTF-32 decoding (4 byte/input byte), UTF-16 ordering (up to 4), lexer,
    // input/output/preimage/record strings, plus canonical member/path cells.
    // Closed C2a keys fix path depth; no arbitrary payload tree reaches JCS.
    const auto member_cells=sizeof(std::string)+2*sizeof(void*)+
        sizeof(std::vector<std::uint32_t>)+sizeof(std::vector<std::uint16_t>);
    return add(add(sizeof(Builder)+sizeof(Counter),json_stock(s)),
        add(multiply(add(s.bytes,1),16),multiply(s.nodes,member_cells)));
}
auto dom_workspace(const Shape& s, std::size_t depth) -> std::size_t {
    // The caller owns and admits the borrowed input separately. This phase
    // owns the DOM, at most one input-length raw lexer history, the largest
    // decoded string/key/numeric token, and overlapping Builder frame keys.
    // Count every observed key byte to cover keys retained at any frame depth.
    // Full C2 JCS/preimage stock remains in workspace(), above.
    static_assert(sizeof(Builder)+sizeof(Counter)<=4096);
    require(s.depth<=depth && depth<=64,Error::capacity);
    const auto parser = add(add(s.bytes, 1),
                            add(add(s.maximum_token_bytes, 1), s.key_text_bytes));
    return add(json_stock(s),add(parser,
        add(4096,multiply(depth,sizeof(Json*)+sizeof(std::size_t)))));
}
auto shape(std::string_view body, const Limits& limits, Work& work) -> Shape {
    require(limits.depth <= 64 && body.size() <= limits.body_bytes,Error::capacity);
    const auto lexical=add(sizeof(Counter),multiply(add(body.size(),1),2));
    require(lexical<=limits.candidate_bytes,Error::capacity);work.reserve_peak(lexical);
    work.debit(body.size()); Counter counter{limits};
    require(Json::sax_parse(body,&counter),Error::schema); counter.result.bytes=body.size();
    return counter.result;
}
auto parse_tree(std::string_view body,const Limits& limits,Work& work)->Json {
    const auto size=shape(body,limits,work);
    const auto allowance=workspace(size);require(allowance<=limits.candidate_bytes,Error::capacity);
    work.reserve_peak(allowance);work.debit(body.size());
    Builder builder;require(Json::sax_parse(body,&builder),Error::schema);
    return std::move(builder.root.value);
}
auto parse_tree_dom(std::string_view body,const Limits& limits,Work& work)->Json {
    const auto size=shape(body,limits,work);
    const auto allowance=dom_workspace(size,limits.depth);
    require(allowance<=limits.candidate_bytes,Error::capacity);
    work.reserve_peak(allowance);work.debit(body.size());
    Builder builder;require(Json::sax_parse(body,&builder),Error::schema);
    return std::move(builder.root.value);
}
namespace {
// Inspect a borrowed tree before cloning or rendering it. The byte quantity is
// an escape upper bound; no dump or ordinary unguarded JSON parse is needed.
auto borrowed_shape(const Json& value,const Limits& limits,Work& work)->Shape {
    Shape result;
    const auto visit=[&](const auto& self,const Json& node,std::size_t depth)->void {
        result.nodes=add(result.nodes,1U);result.depth=std::max(result.depth,depth);
        require(result.nodes<=limits.nodes&&depth<=limits.depth&&depth<=64U,Error::capacity);
        work.debit(1U);require(!node.is_binary()&&!node.is_discarded(),Error::schema);
        if(node.is_number_float())require(std::isfinite(node.get<double>()),Error::canonical);
        if(node.is_string()) {
            const auto size=node.get_ref<const std::string&>().size();
            result.text=add(result.text,size);work.debit(size);
        }
        if(node.is_object())for(auto it=node.begin();it!=node.end();++it) {
            result.members=add(result.members,1U);result.text=add(result.text,it.key().size());
            work.debit(it.key().size());
        }
        require(result.text<=limits.text_bytes,Error::capacity);
        if(node.is_structured())for(const auto& child_value:node)self(self,child_value,add(depth,1U));
    };
    visit(visit,value,1U);
    result.bytes=add(multiply(result.text,6U),multiply(result.nodes,32U));
    return result;
}
auto artifact_representation(Kind k)->std::string_view {
    require(k==Kind::creation||k==Kind::planning,Error::schema);
    return k==Kind::creation?"utf8_creation_children":"utf8_planning_children";
}
void artifact_fields(const Json& value,Kind k,bool compact) {
    if(k==Kind::creation) {
        if(compact)fields(value,{"representation","artifacts","bindings","domain","entry_producer","failures","run_id","schema","sequence","timestamp"});
        else fields(value,{"artifacts","bindings","domain","entry_producer","failures","run_id","schema","sequence","timestamp"});
        require(text(value.at("schema"))==run_creation_serialization::profile,Error::schema);
        fields(value.at("artifacts"),{"base_build","host","source"});
        for(auto key:{"base_build","host","source"})
            require(!text(value.at("artifacts").at(key)).empty(),Error::schema);
    } else {
        require(k==Kind::planning,Error::schema);
        if(compact)fields(value,{"representation","evidence","evidence_digest","execution_plan_digest","schema","semantic"});
        else fields(value,{"evidence_digest","evidence_hex","execution_plan_digest","schema","semantic_hex"});
        require(text(value.at("schema"))==execution_plan::artifact_profile,Error::schema);
        require(!text(value.at(compact?"evidence":"evidence_hex")).empty(),Error::schema);
        require(!text(value.at(compact?"semantic":"semantic_hex")).empty(),Error::schema);
    }
    if(compact)require(text(value.at("representation"))==artifact_representation(k),Error::schema);
}
auto decoded_octets(std::string_view encoded,Work& work)->std::string {
    require(encoded.size()%2U==0U,Error::schema);work.debit(encoded.size());
    const auto nibble=[](char c)->unsigned {
        if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');
        if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a')+10U;
        throw Error::schema;
    };
    std::string result(encoded.size()/2U,'\0');
    for(std::size_t i{};i<result.size();++i)
        result[i]=static_cast<char>((nibble(encoded[2U*i])<<4U)|nibble(encoded[2U*i+1U]));
    return result;
}
auto encoded_octets(std::string_view bytes,Work& work)->std::string {
    const auto size=multiply(bytes.size(),2U);work.debit(size);
    std::string result(size,'0');constexpr std::string_view digits="0123456789abcdef";
    for(std::size_t i{};i<bytes.size();++i) {
        const auto byte=static_cast<unsigned char>(bytes[i]);
        result[2U*i]=digits[byte>>4U];result[2U*i+1U]=digits[byte&15U];
    }
    return result;
}
void replace_octets(Json& object,std::string_view before,std::string_view after,bool decode,Work& work) {
    auto bytes=decode?decoded_octets(text(object.at(before)),work):encoded_octets(text(object.at(before)),work);
    Json leaf=std::move(bytes);core::detail::JsonCleanup cleanup{leaf};
    core::detail::set_json(object,after,std::move(leaf));
    if(before!=after)object.erase(object.find(before));
}
}
auto compact_artifact(Kind k,std::string_view bytes,const Limits& limits,Work& work,std::size_t overlap)->Json {
    (void)artifact_representation(k);
    const auto dimensions=shape(bytes,limits,work);
    // The old parent tree, decoded leaf under construction and compact owned
    // tree may overlap. Admit them before SAX/clone/string allocation; the
    // enclosing event's preparation stock is supplied explicitly by its caller.
    const auto stock=add(workspace(dimensions),add(json_stock(dimensions),add(bytes.size(),1024U)));
    const auto peak=add(overlap,stock);require(peak<=limits.candidate_bytes,Error::capacity);work.reserve_peak(peak);
    Tree tree;tree.value=parse_tree(bytes,limits,work);artifact_fields(tree.value,k,false);
    work.debit(bytes.size());require(work.equal(canonical(tree.value,limits),bytes),Error::canonical);
    if(k==Kind::creation)for(auto key:{"base_build","host","source"})
        replace_octets(tree.value.at("artifacts"),key,key,true,work);
    else {
        replace_octets(tree.value,"evidence_hex","evidence",true,work);
        replace_octets(tree.value,"semantic_hex","semantic",true,work);
    }
    tree.value["representation"]=artifact_representation(k);
    (void)borrowed_shape(tree.value,limits,work);
    return Json(std::move(tree.value));
}
auto artifact_bytes(Kind k,const Json& value,Version version,const Limits& limits,Work& work,
    std::string& reconstructed,std::size_t overlap)->std::string_view {
    if(value.is_string())return text(value);
    require(version==Version::compact_v3&&reconstructed.empty(),Error::schema);
    artifact_fields(value,k,true);
    const auto dimensions=borrowed_shape(value,limits,work);
    std::size_t octets{};
    if(k==Kind::creation)for(auto key:{"base_build","host","source"})
        octets=add(octets,text(value.at("artifacts").at(key)).size());
    else octets=add(text(value.at("evidence")).size(),text(value.at("semantic")).size());
    require(octets<=dimensions.text,Error::capacity);
    const auto other=dimensions.text-octets;
    Shape expanded=dimensions;
    expanded.text=add(multiply(octets,2U),other);
    expanded.bytes=add(add(multiply(octets,2U),multiply(other,6U)),multiply(dimensions.nodes,32U));
    // Exact known leaves become lowercase hex. Their output requires two bytes
    // per input byte, never JSON escaping. All other text retains the ordinary
    // escape bound. Include the original compact clone and reconstructed parent
    // string while guarded JCS/partial trees are still alive.
    const auto stock=add(workspace(expanded),add(json_stock(dimensions),expanded.bytes));
    const auto peak=add(overlap,stock);require(peak<=limits.candidate_bytes,Error::capacity);work.reserve_peak(peak);
    Tree tree;tree.value=core::detail::clone_json(value);tree.value.erase("representation");
    if(k==Kind::creation)for(auto key:{"base_build","host","source"})
        replace_octets(tree.value.at("artifacts"),key,key,false,work);
    else {
        replace_octets(tree.value,"evidence","evidence_hex",false,work);
        replace_octets(tree.value,"semantic","semantic_hex",false,work);
    }
    work.debit(expanded.bytes);reconstructed=canonical(tree.value,limits);
    return reconstructed;
}
auto child_allowance(Kind k, const ChildLimits& l) -> std::size_t {
    auto sum=[](std::initializer_list<std::size_t> values) {
        std::size_t n{}; for(auto v:values) n=add(n,v); return n;
    };
    switch(k) {
    case Kind::creation: return sum({l.creation.retained_bytes,l.creation.table_preparation_bytes,
        l.creation.error_preparation_bytes,l.creation.child_preparation_bytes[0],
        l.creation.child_preparation_bytes[1],l.creation.child_preparation_bytes[2],
        l.creation.identity_index_bytes,l.creation.publication_bytes});
    case Kind::normalization: return sum({l.normalization.retained_bytes,l.normalization.source_preparation_bytes,
        l.normalization.plan_preparation_bytes,l.normalization.topology_preparation_bytes,
        l.normalization.association_bytes,l.normalization.publication_bytes});
    case Kind::source_outcome: return sum({l.source_outcome.retained_bytes,l.source_outcome.source_preparation_bytes,
        l.source_outcome.upstream_preparation_bytes,l.source_outcome.publication_bytes});
    case Kind::topology: return sum({l.topology.retained_bytes,l.topology.topology_decode_bytes,
        l.topology.topology_preparation_bytes,l.topology.error_preparation_bytes,l.topology.publication_bytes});
    case Kind::failure: return sum({l.error.retained_bytes,l.error.consumer_simultaneous_bytes});
    default:throw Error::unsupported;
    }
    throw Error::unsupported;
}
namespace {
auto availability_origin(const Availability& value) -> std::string_view {
    return value.source == AvailabilitySource::supplied_process_fact ? "supplied_synthetic" : "actual_availability";
}
void validate_availability(const Availability& value, process_subject::Producer reporter, Kind kind) {
    require(value.transition != 0 && value.delivery != 0, Error::sequence);
    if (value.source == AvailabilitySource::producer_callback) {
        require(reporter == process_subject::Producer::runner && value.quiescence_epoch && *value.quiescence_epoch != 0, Error::association);
    } else {
        const bool semantic_handoff = value.source == AvailabilitySource::controller_observation &&
            (((kind == Kind::finalization || kind == Kind::request_accepted) && reporter == process_subject::Producer::runner) ||
             ((kind == Kind::analysis || kind == Kind::run_closed) && reporter == process_subject::Producer::results));
        require((value.source == AvailabilitySource::controller_observation || value.source == AvailabilitySource::supplied_process_fact) &&
            (reporter == process_subject::Producer::controller || semantic_handoff) &&
            !value.quiescence_epoch && value.operation == 0, Error::association);
        if (value.source == AvailabilitySource::supplied_process_fact)
            require(!value.native_platform && !value.acquired_utc, Error::association);
    }
}
auto read_availability(const Json& value, process_subject::Producer reporter, Kind kind) -> Availability {
    fields(value,{"source","operation","transition","delivery","quiescence_epoch","input_origin","platform","utc"});
    Availability result;
    const auto& source = text(value.at("source"));
    if (source == "producer_callback") result.source = AvailabilitySource::producer_callback;
    else if (source == "controller_observation") result.source = AvailabilitySource::controller_observation;
    else { require(source == "supplied_process_fact",Error::schema); result.source = AvailabilitySource::supplied_process_fact; }
    result.operation = integer(text(value.at("operation")));
    result.transition = integer(text(value.at("transition")));
    result.delivery = integer(text(value.at("delivery")));
    if (!value.at("quiescence_epoch").is_null()) result.quiescence_epoch = integer(text(value.at("quiescence_epoch")));
    require(text(value.at("input_origin")) == "retained_supplied_evidence",Error::schema);
    const auto& platform = text(value.at("platform"));
    require(platform == "injected_operations" || platform == "native_operations",Error::schema);
    result.native_platform = platform == "native_operations";
    const auto& utc = text(value.at("utc")); require(utc == "supplied" || utc == "acquired",Error::schema);
    result.acquired_utc = utc == "acquired";
    validate_availability(result,reporter,kind);
    return result;
}
void write_availability(Json& value, const Availability& source) {
    value = Json::object();
    value["source"] = source.source == AvailabilitySource::producer_callback ? "producer_callback" :
        source.source == AvailabilitySource::controller_observation ? "controller_observation" : "supplied_process_fact";
    value["operation"] = std::to_string(source.operation); value["transition"] = std::to_string(source.transition);
    value["delivery"] = std::to_string(source.delivery);
    value["quiescence_epoch"] = source.quiescence_epoch ? Json(std::to_string(*source.quiescence_epoch)) : Json(nullptr);
    value["input_origin"] = "retained_supplied_evidence";
    value["platform"] = source.native_platform ? "native_operations" : "injected_operations";
    value["utc"] = source.acquired_utc ? "acquired" : "supplied";
}
}
auto decode(std::string_view body, std::string_view run, std::uint64_t sequence,
            const std::optional<core::Sha256Digest>& previous, const Limits& limits, Work& work) -> Candidate {
    const auto size=shape(body,limits,work);
    require(workspace(size) <= limits.candidate_bytes,Error::capacity);
    Builder builder; work.debit(body.size()); require(Json::sax_parse(body,&builder),Error::schema);
    auto& root=builder.root.value;
    fields(root,{"schema","run_id","sequence","event_id","type","timestamp","producer","previous_digest","payload","digest"});
    const auto& profile = text(root.at("schema"));
    require(profile==schema_name || profile==progress_schema_name || profile==compact_schema_name,Error::schema);
    const auto version = profile == schema_name ? Version::supplied_v1 :
        profile == progress_schema_name ? Version::progress_v2 : Version::compact_v3;
    require(text(root.at("run_id"))==run && text(root.at("event_id"))==event_id(run,sequence),Error::identity);
    require(integer(text(root.at("sequence")))==sequence && sequence!=0,Error::sequence);
    auto timestamp=journal::Timestamp::parse(text(root.at("timestamp")));
    core::detail::ExpectedErrorCleanup timestamp_cleanup{timestamp}; require(timestamp.has_value(),Error::identity);
    if(previous) require(text(root.at("previous_digest"))==previous->to_hex(),Error::digest);
    else require(root.at("previous_digest").is_null(),Error::digest);
    const auto& payload=root.at("payload");
    if (version == Version::supplied_v1) fields(payload,{"kind","scope","facts"});
    else fields(payload,{"kind","scope","facts","availability"});
    const auto k=kind(text(payload.at("kind")));
    require(text(root.at("type"))==category(k),Error::schema);
    const auto& scope=payload.at("scope"); fields(scope,{"experiment_id","pair_id","block_id","observation_id"});

    const auto& facts=payload.at("facts");
    const bool supplied=static_cast<unsigned>(k)>static_cast<unsigned>(Kind::failure)||(k==Kind::failure&&facts.contains("origin"));
    if(supplied){
        require(limits.composition&&facts.is_object()&&facts.contains("origin"),Error::association);
        Scope selected;std::array slots{&selected.experiment,&selected.pair,&selected.block,&selected.observation};
        constexpr std::array keys{"experiment_id","pair_id","block_id","observation_id"};
        for(std::size_t i{};i<keys.size();++i)if(!scope.at(keys[i]).is_null())*slots[i]=integer(text(scope.at(keys[i])));
        process_subject::Producer reporter{};const auto& token=text(root.at("producer"));
        if(token=="controller")reporter=process_subject::Producer::controller;else if(token=="runner")reporter=process_subject::Producer::runner;
        else{require(token=="results",Error::schema);reporter=process_subject::Producer::results;}
        std::optional<Availability> availability;
        if (is_progress(version) && !payload.at("availability").is_null())
            availability = read_availability(payload.at("availability"),reporter,k);
        require(text(facts.at("origin")) == (availability ? availability_origin(*availability) : "supplied_synthetic"),Error::association);
        // One Tree backs the aliasing shared view; the two-handle allowance is
        // conservative logical control stock, not an allocator heap claim.
        const auto retained=add(sizeof(std::monostate),add(sizeof(Tree),add(2U*sizeof(std::shared_ptr<const Json>),json_stock(size))));
        const auto allowance=add(workspace(size),retained);require(allowance<=limits.candidate_bytes,Error::capacity);work.reserve_peak(allowance);
        work.debit(body.size());require(canonical(root,limits)==body,Error::canonical);const auto expected=text(root.at("digest"));
        root.erase("digest");work.debit(body.size());const auto digest=core::sha256(canonical(root,limits));require(expected==digest.to_hex(),Error::digest);
        work.debit(add(size.nodes,size.text));
        auto backing=std::make_shared<Tree>();backing->value=core::detail::clone_json(facts);
        std::shared_ptr<const Json> owned{backing,&backing->value};
        return {{sequence,k,text(root.at("timestamp")),std::string{body},digest,std::monostate{},{},{},{},Role::primary,std::move(owned),selected,reporter,version,availability},size,retained,allowance};
    }
    if (is_progress(version)) require(payload.at("availability").is_null(),Error::association);
    for(const auto& v:scope)require(v.is_null(),Error::schema);
    std::optional<Reference> creation,topology,primary; Role role=Role::primary;
    if(k==Kind::creation) fields(facts,{"artifact"});
    else if(k==Kind::normalization) {
        fields(facts,{"artifact","creation_ref","topology_ref"});
        creation=reference(facts.at("creation_ref"),run); topology=reference(facts.at("topology_ref"),run);
    } else if(k==Kind::failure) {
        fields(facts,{"artifact","source","role","primary_ref"});
        require(text(facts.at("source"))=="private_error",Error::unsupported);
        const auto& r=text(facts.at("role")); require(r=="primary"||r=="secondary",Error::schema);
        role=r=="primary"?Role::primary:Role::secondary;
        if(!facts.at("primary_ref").is_null()) primary=reference(facts.at("primary_ref"),run);
    } else {
        fields(facts,{"artifact","creation_ref"}); creation=reference(facts.at("creation_ref"),run);
    }
    const auto allowance=child_allowance(k,limits.child);
    const auto preparation=add(workspace(size),allowance);
    require(preparation<=limits.candidate_bytes,Error::capacity);work.reserve_peak(preparation);
    work.debit(body.size()); require(canonical(root,limits)==body,Error::canonical);
    const auto expected=text(root.at("digest")); root.erase("digest");
    work.debit(body.size()); const auto digest=core::sha256(canonical(root,limits));
    require(expected==digest.to_hex(),Error::digest);
    std::string reconstructed;
    const auto bytes=artifact_bytes(k,facts.at("artifact"),version,limits,work,reconstructed,workspace(size));
    const auto reconstruction_stock=reconstructed.empty()?0U:add(reconstructed.capacity(),1U);
    const auto child_preparation=add(preparation,reconstruction_stock);
    require(child_preparation<=limits.candidate_bytes,Error::capacity);work.reserve_peak(child_preparation);
    work.debit(bytes.size());
    Artifact artifact=[&]() -> Artifact {
        switch(k) {
        case Kind::creation: return child<run_creation_serialization::Owner>(bytes,limits.child.creation);
        case Kind::normalization: return child<normalization_serialization::Owner>(bytes,limits.child.normalization);
        case Kind::source_outcome: return child<source_outcome_serialization::Owner>(bytes,limits.child.source_outcome);
        case Kind::topology: return child<topology_serialization::Owner>(bytes,limits.child.topology);
        case Kind::failure: return child<error_serialization::Owner>(bytes,limits.child.error);
        default:throw Error::unsupported;
        }
        throw Error::unsupported;
    }();
    auto expected_producer=std::string_view{"controller"};
    if(k==Kind::failure) expected_producer=producer(std::get<error_serialization::Owner>(artifact).provenance()->entry_producer);
    require(text(root.at("producer"))==expected_producer,Error::association);
    const auto retained=std::visit([](const auto& a)->std::size_t{ if constexpr(std::is_same_v<std::decay_t<decltype(a)>,std::monostate>)throw Error::state;else return a.usage().retained_bytes; },artifact);
    return {{sequence,k,text(root.at("timestamp")),std::string{body},digest,std::move(artifact),creation,topology,primary,role,{},{},process_subject::Producer::controller,version,{}},
            size,retained,allowance};
}
auto encode(const Input& input, std::string_view run, std::uint64_t sequence,
            const std::optional<core::Sha256Digest>& previous,const Limits& limits,Work& work) -> std::string {
    const auto k=static_cast<Kind>(input.artifact.index());
    const auto bytes=std::visit([](auto* a) {
        require(a && a->origin().has_value(),Error::state);
        using Origin=typename decltype(a->origin())::value_type;
        require(*a->origin()==Origin::session_selected,Error::association);
        return a->canonical();
    },input.artifact);
    // Escaped artifact bytes dominate the known, fixed-size C2a envelope. Check
    // input length before copying; the exact canonical body is checked below.
    require(bytes.size()<=limits.body_bytes,Error::capacity);
    // At most 40 value nodes / 35 members / depth five for these five payloads.
    // 1024 covers the fixed envelope, three decimal references and UTC/UUID.
    const auto metadata_node = is_progress(limits.version) ? 1U : 0U;
    const auto overhead = is_progress(limits.version) ? 2048U : 1024U;
    const Shape upper{40U+metadata_node,35U+metadata_node,add(bytes.size(),overhead),5,add(multiply(bytes.size(),6),overhead)};
    require(workspace(upper)<=limits.candidate_bytes,Error::capacity);work.reserve_peak(workspace(upper));
    work.debit(bytes.size()); Tree tree; auto& root=tree.value; root=Json::object();
    root["schema"]=schema_for(limits.version); root["run_id"]=run; root["sequence"]=std::to_string(sequence);
    root["event_id"]=event_id(run,sequence); root["timestamp"]=input.timestamp.text(); root["type"]=category(k);
    root["producer"]= k==Kind::failure ? producer(std::get<const error_serialization::Owner*>(input.artifact)->provenance()->entry_producer) : "controller";
    if(previous) root["previous_digest"]=previous->to_hex(); else root["previous_digest"]=nullptr;
    auto& payload=root["payload"]; payload=Json::object(); payload["kind"]=name(k);
    if (is_progress(limits.version)) payload["availability"] = nullptr;
    auto& scope=payload["scope"]; scope=Json::object();
    for(auto key:{"experiment_id","pair_id","block_id","observation_id"}) scope[key]=nullptr;
    auto& facts=payload["facts"]; facts=Json::object(); facts["artifact"]=bytes;
    if(k==Kind::creation) require(!input.creation&&!input.topology&&!input.primary&&input.role==Role::primary,Error::reference);
    else if(k==Kind::failure) {
        require(!input.creation&&!input.topology,Error::reference);
        require(input.role==Role::primary || input.role==Role::secondary,Error::schema);
        facts["source"]="private_error"; facts["role"]=input.role==Role::primary?"primary":"secondary";
        if(input.primary) write_reference(facts["primary_ref"],*input.primary,run); else facts["primary_ref"]=nullptr;
    } else {
        require(input.creation.has_value()&&!input.primary&&input.role==Role::primary,Error::reference);
        write_reference(facts["creation_ref"],*input.creation,run);
        if(k==Kind::normalization) { require(input.topology.has_value(),Error::reference); write_reference(facts["topology_ref"],*input.topology,run); }
        else require(!input.topology,Error::reference);
    }
    if(limits.version==Version::compact_v3&&k==Kind::creation) {
        auto stored=compact_artifact(k,bytes,limits,work,workspace(upper));
        core::detail::JsonCleanup cleanup{stored};
        core::detail::set_json(facts,"artifact",std::move(stored));
        const auto stored_size=borrowed_shape(root,limits,work);
        require(workspace(stored_size)<=limits.candidate_bytes,Error::capacity);work.reserve_peak(workspace(stored_size));
    }
    auto preimage=canonical(root,limits); work.debit(preimage.size());
    root["digest"]=core::sha256(preimage).to_hex(); auto body=canonical(root,limits);
    (void)shape(body,limits,work); return body;
}

auto event_name(Kind kind)->std::string_view{return name(kind);}
auto encode(const SuppliedEvent& input,std::string_view run,std::uint64_t sequence,
    const std::optional<core::Sha256Digest>& previous,const Limits& limits,Work& work)->std::string {
    require(limits.composition&&(static_cast<unsigned>(input.kind)>static_cast<unsigned>(Kind::failure)||input.kind==Kind::failure),Error::state);
    require(!input.admission || is_progress(limits.version),Error::association);
    if (input.admission) validate_availability(input.admission->availability(),input.producer,input.kind);
    require(input.facts.is_object()&&input.facts.contains("origin")&&text(input.facts.at("origin"))==
        (input.admission ? availability_origin(input.admission->availability()) : "supplied_synthetic"),Error::association);
    // Inspect the borrowed tree with bounded recursion before dump or any copy.
    Shape measured;
    const auto count=[&](const auto& self,const Json& node,std::size_t depth)->void {
        measured.nodes=add(measured.nodes,1U);measured.depth=std::max(measured.depth,depth);
        require(measured.nodes<=limits.nodes&&depth<=limits.depth&&depth<=64U,Error::capacity);work.debit(1U);
        require(!node.is_binary()&&!node.is_discarded(),Error::schema);
        if(node.is_number_float())require(std::isfinite(node.get<double>()),Error::canonical);
        if(node.is_string()){const auto bytes=node.get_ref<const std::string&>().size();measured.text=add(measured.text,bytes);work.debit(bytes);}
        if(node.is_object())for(auto it=node.begin();it!=node.end();++it){measured.members=add(measured.members,1U);measured.text=add(measured.text,it.key().size());work.debit(it.key().size());}
        require(measured.text<=limits.text_bytes,Error::capacity);
        if(node.is_structured())for(const auto& child:node)self(self,child,add(depth,1U));
    };
    count(count,input.facts,1U);
    measured.bytes=add(multiply(measured.text,6U),multiply(measured.nodes,32U));
    const auto metadata_nodes = is_progress(limits.version) ? 9U : 0U;
    const auto overhead = is_progress(limits.version) ? 2048U : 1024U;
    const Shape upper{add(measured.nodes,40U+metadata_nodes),add(measured.members,35U+metadata_nodes),add(measured.text,overhead),add(measured.depth,3U),add(measured.bytes,overhead)};
    require(workspace(upper)<=limits.candidate_bytes,Error::capacity);work.reserve_peak(workspace(upper));
    work.debit(measured.bytes);const auto facts=input.facts.dump();(void)shape(facts,limits,work);
    Tree tree;auto& root=tree.value;
    root=Json::object();root["schema"]=schema_for(limits.version);root["run_id"]=run;root["sequence"]=std::to_string(sequence);
    root["event_id"]=event_id(run,sequence);root["timestamp"]=input.timestamp.text();root["type"]=category(input.kind);
    root["producer"]=producer(input.producer);root["previous_digest"]=previous?Json(previous->to_hex()):Json(nullptr);
    auto& payload=root["payload"];payload=Json::object();payload["kind"]=name(input.kind);
    if (is_progress(limits.version)) {
        if (input.admission) write_availability(payload["availability"],input.admission->availability());
        else payload["availability"] = nullptr;
    }
    core::detail::set_json(payload,"facts",input.facts);
    if(limits.version==Version::compact_v3&&input.kind==Kind::planning) {
        const auto& selected=payload.at("facts").at("artifact");
        if(selected.is_string()) {
            auto stored=compact_artifact(input.kind,text(selected),limits,work,workspace(upper));
            core::detail::JsonCleanup cleanup{stored};
            core::detail::set_json(payload.at("facts"),"artifact",std::move(stored));
        } else artifact_fields(selected,input.kind,true);
    }
    auto& scope=payload["scope"];
    const std::array values{input.scope.experiment,input.scope.pair,input.scope.block,input.scope.observation};
    constexpr std::array keys{"experiment_id","pair_id","block_id","observation_id"};
    for(std::size_t i{};i<keys.size();++i)scope[keys[i]]=values[i]?Json(std::to_string(*values[i])):Json(nullptr);
    if(limits.version==Version::compact_v3&&input.kind==Kind::planning) {
        const auto stored_size=borrowed_shape(root,limits,work);
        require(workspace(stored_size)<=limits.candidate_bytes,Error::capacity);work.reserve_peak(workspace(stored_size));
    }
    auto preimage=canonical(root,limits);work.debit(preimage.size());root["digest"]=core::sha256(preimage).to_hex();auto body=canonical(root,limits);
    (void)shape(body,limits,work);return body;
}
} // namespace detail
} // namespace epycbench::results::event_profile
