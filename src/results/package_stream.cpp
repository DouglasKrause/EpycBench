#include "results/package_stream.hpp"

#include "epycbench/core/digest.hpp"
#include "epycbench/kernel/package.h"
#include <map>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <set>
#include <string>

namespace epycbench::results::package_stream {
namespace {
using Json = nlohmann::json;
using U64 = std::uint64_t;

auto number(const Json& value) -> bool {
    return value.is_number_unsigned();
}

auto digest(const Json& value) -> bool {
    return value.is_string() && core::Sha256Digest::parse(value.get_ref<const std::string&>()).has_value();
}

auto accumulate(U64& total,U64 amount) -> bool {
    if (amount>std::numeric_limits<U64>::max()-total) return false;
    total+=amount;
    return true;
}

auto region(const Json& regions,std::uint32_t kind,const Json& name) -> const Json* {
    const auto found=std::ranges::find_if(regions,[&](const auto& row) {
        return row.at("kind")==kind && row.at("name")==name;
    });
    return found==regions.end() ? nullptr : &*found;
}

auto regions_well_formed(const Json& regions) -> bool {
    if (!regions.is_array() || regions.empty() || regions.size()>EPYCBENCH_PACKAGE_MAX_REGIONS) return false;
    std::set<std::string> names;
    for (const auto& row:regions) {
        const auto name=row.at("name").get<std::string>();
        if (name.empty() || !names.insert(name).second || !number(row.at("kind")) ||
            row.at("kind")<EPYCBENCH_PACKAGE_INPUT || row.at("kind")>EPYCBENCH_PACKAGE_SCRATCH ||
            !number(row.at("bytes")) || row.at("bytes")==0 || !number(row.at("alignment")) ||
            !std::has_single_bit(row.at("alignment").get<U64>())) return false;
    }
    return true;
}

auto link(const Json& roles) -> std::pair<Json,Json> {
    const auto& links=roles.at(1U).at("inputs");
    const auto value=links.begin();
    return {value.key(),*value};
}
} // namespace

auto valid_geometry(const Json& plan) -> bool {
    const auto& scenario=plan.at("scenario");
    const auto& roles=scenario.at("roles");
    const auto& execution=scenario.at("execution");
    const auto parts=execution.at("pipeline_chunks").get<std::size_t>();
    const auto repetitions=execution.at("repetitions").get<std::size_t>();
    const auto& shapes=plan.at("geometry");
    if (parts<2U || parts>64U || repetitions==0U || repetitions>64U/parts ||
        roles.size()!=2U || !shapes.is_array() || shapes.size()!=parts*repetitions*2U) return false;
    const auto count=parts*repetitions;
    const auto [input,output]=link(roles);
    std::array<U64,2> partition_end{};
    for (std::size_t c=0;c<count;++c) {
        const auto pass=c/parts;
        if (c%parts==0U) partition_end={};
        for (std::size_t s=0;s<roles.size();++s) {
            const auto& g=shapes.at(c*2U+s);
            if (g.at("chunk")!=c || g.at("pass")!=pass || g.at("rank")!=0 || g.at("role")!=roles.at(s).at("id") ||
                !number(g.at("partition_begin")) || g.at("partition_begin")!=partition_end[s] ||
                !number(g.at("partition_count")) || g.at("partition_count")==0 ||
                !accumulate(partition_end[s],g.at("partition_count").get<U64>()) ||
                !number(g.at("preparation_bytes")) || !g.at("shared_regions").is_array() ||
                !g.at("shared_regions").empty() || g.at("shared_preparation_bytes")!=0 ||
                !regions_well_formed(g.at("regions"))) return false;
            if (pass>0U) {
                const auto& original=shapes.at((c%parts)*2U+s);
                for (const auto* field:{"partition_begin","partition_count","preparation_bytes","regions"})
                    if (g.at(field)!=original.at(field)) return false;
            }
        }
        const auto* source=region(shapes.at(c*2U).at("regions"),EPYCBENCH_PACKAGE_OUTPUT,output);
        const auto* target=region(shapes.at(c*2U+1U).at("regions"),EPYCBENCH_PACKAGE_INPUT,input);
        if (!source || !target || source->at("bytes")!=target->at("bytes")) return false;
    }
    return true;
}

auto retained_shape(const Json& stream,const Json& plan,bool require_handoff) -> bool {
    const auto& scenario=plan.at("scenario");
    const auto& roles=scenario.at("roles");
    const auto& execution=scenario.at("execution");
    const auto parts=execution.at("pipeline_chunks").get<std::size_t>();
    const auto count=parts*execution.at("repetitions").get<std::size_t>();
    const auto& chunks=stream.at("chunks");
    const auto inputs=stream.find("external_inputs");
    const auto handoff=stream.find("logical_handoff");
    if ((handoff!=stream.end()) != require_handoff) return false;
    if (require_handoff && !handoff->is_null() &&
        (!handoff->is_object() || handoff->size()!=4U || !number(handoff->at("bytes")) ||
         !digest(handoff->at("reference_sha256")) || !digest(handoff->at("observed_sha256")) ||
         !digest(handoff->at("binding_sha256")))) return false;
    const auto timestamp=[](const Json& value) { return value.is_null() || number(value); };
    if (stream.at("slot_count")!=2U || stream.at("chunks_per_pass")!=parts || !timestamp(stream.at("release_ns")) ||
        !timestamp(stream.at("completion_ns")) ||
        (!stream.at("error").is_null() && !stream.at("error").is_object()) ||
        inputs==stream.end() || !inputs->is_array() || inputs->size()!=roles.size() ||
        !std::ranges::all_of(*inputs,[](const Json& stage) { return stage.is_null() || stage.is_object(); }) ||
        !chunks.is_array() || chunks.size()!=count) return false;
    for (std::size_t c=0;c<count;++c) {
        const auto& chunk=chunks.at(c);
        const auto& ranks=chunk.at("ranks");
        if (chunk.at("ordinal")!=c || chunk.at("pass")!=c/parts || chunk.at("slot")!=c%2U || !timestamp(chunk.at("handoff_ns")) ||
            !chunk.at("producer_waited").is_boolean() || !chunk.at("consumer_waited").is_boolean() ||
            !ranks.is_array() || ranks.size()!=2U) return false;
        for (std::size_t s=0;s<roles.size();++s) {
            const auto& rank=ranks.at(s);
            if (rank.at("role")!=roles.at(s).at("id") || rank.at("rank")!=0 || rank.at("rank_count")!=1U ||
                !rank.at("placement").is_object() || !rank.at("transfers").is_array() ||
                !rank.at("hardware_counters").is_null() || (!rank.at("error").is_null() && !rank.at("error").is_object()) ||
                (!rank.at("trial").is_null() && !rank.at("trial").is_object())) return false;
        }
    }
    return true;
}

auto handoff_binding(const Json& receipt, const Json& transfers) -> std::string {
    return core::sha256(Json{{"profile", "epycbench.stream-handoff-binding/v1"},
        {"bytes", receipt.at("bytes")}, {"reference_sha256", receipt.at("reference_sha256")},
        {"observed_sha256", receipt.at("observed_sha256")}, {"transfers", transfers}}.dump()).to_hex();
}

auto measured_block(const Json& stream,const Json& plan,const RankValidator& rank_valid)
    -> std::optional<MeasuredBlock> {
    const auto& scenario=plan.at("scenario");
    const auto& roles=scenario.at("roles");
    const auto parts=scenario.at("execution").at("pipeline_chunks").get<std::size_t>();
    const auto& chunks=stream.at("chunks");
    if (!stream.at("error").is_null() || !number(stream.at("release_ns")) ||
        !number(stream.at("completion_ns"))) return std::nullopt;
    MeasuredBlock result{stream.at("release_ns").get<U64>(),stream.at("completion_ns").get<U64>(),{},{},{},{}};
    if (result.completion_ns<=result.release_ns) return std::nullopt;
    std::array<U64,2> last{result.release_ns,result.release_ns},slot_release=last;
    const auto [input,output]=link(roles);
    std::array<std::map<std::string, U64>, 2> input_bytes;
    U64 handoff_bytes{};
    auto handoffs=Json::array();
    for (std::size_t c=0;c<chunks.size();++c) {
        const auto& row=chunks.at(c);
        const auto& ranks=row.at("ranks");
        if (!number(row.at("handoff_ns"))) return std::nullopt;
        const auto handed_off=row.at("handoff_ns").get<U64>();
        for (std::size_t s=0;s<roles.size();++s) {
            const auto& rank=ranks.at(s);
            const auto& g=plan.at("geometry").at(c*2U+s);
            if (!rank.at("error").is_null() || !rank.at("cleanup_error").is_null() || !rank_valid(s,rank,g))
                return std::nullopt;
            const auto& trial=rank.at("trial");
            const auto& work=trial.at("actual");
            const auto begin=trial.at("begin_ns").get<U64>(),end=trial.at("end_ns").get<U64>();
            if (begin<last[s] || end>result.completion_ns ||
                (s==0U && (begin<slot_release[c%2U] || end>handed_off || (c%parts==0U && begin<last[1]))) ||
                (s==1U && begin<handed_off) ||
                !accumulate(result.callback_ns[s],end-begin) ||
                !accumulate(result.work_units[s],work.at("work_units").get<U64>()) ||
                !accumulate(result.output_counts[s],work.at("output_count").get<U64>())) return std::nullopt;
            for (const auto& r : trial.at("regions")) {
                if (r.at("kind") != EPYCBENCH_PACKAGE_INPUT || (s == 1 && r.at("name") == input)) continue;
                if (!accumulate(input_bytes[s][r.at("name").get<std::string>()], r.at("bytes").get<U64>()))
                    return std::nullopt;
            }
            last[s]=end;
        }
        slot_release[c%2U]=last[1];
        const auto& producer=ranks.at(0U);
        const auto& consumer=ranks.at(1U);
        const auto* origin=region(producer.at("trial").at("regions"),EPYCBENCH_PACKAGE_OUTPUT,output);
        const auto* target=region(consumer.at("trial").at("regions"),EPYCBENCH_PACKAGE_INPUT,input);
        const auto& transfers=consumer.at("transfers");
        if (!origin || !target || !producer.at("transfers").empty() ||
            !transfers.is_array() || transfers.size()!=1U) return std::nullopt;
        const auto& transfer=transfers.front();
        if (origin->at("after")!=origin->at("reference") || origin->at("after")!=target->at("before") ||
            origin->at("bytes")!=target->at("bytes") || transfer.at("bytes")!=origin->at("bytes") ||
            transfer.at("sha256")!=origin->at("after") || transfer.at("input")!=input ||
            transfer.at("output")!=output || transfer.at("source_role")!=producer.at("role") ||
            !accumulate(handoff_bytes,origin->at("bytes").get<U64>())) return std::nullopt;
        handoffs.push_back(transfer);
    }
    const auto handoff=stream.find("logical_handoff");
    if (handoff==stream.end()) {
        // Legacy Results can compare the ordered transfers at the original split.
        result.handoff_identity=core::sha256(Json{{"profile", "epycbench.chunked-handoff/v1"},
            {"chunks_per_pass",parts},{"transfers",std::move(handoffs)}}.dump()).to_hex();
    } else {
        if (handoff->is_null() || handoff_bytes>core::Sha256Stream::maximum_input_bytes ||
            handoff->at("bytes")!=handoff_bytes || handoff->at("reference_sha256")!=handoff->at("observed_sha256") ||
            handoff->at("binding_sha256")!=handoff_binding(*handoff,handoffs)) return std::nullopt;
        result.handoff_identity=core::sha256(Json{{"profile", "epycbench.logical-handoff/v1"},
            {"bytes",handoff_bytes},{"sha256",handoff->at("observed_sha256")}}.dump()).to_hex();
    }
    const auto& proofs = stream.at("external_inputs");
    if (!proofs.is_array() || proofs.size() != roles.size()) return std::nullopt;
    for (std::size_t s = 0; s < roles.size(); ++s) {
        if (!proofs[s].is_object() || proofs[s].size() != input_bytes[s].size()) return std::nullopt;
        for (const auto& [name, bytes] : input_bytes[s]) {
            const auto& proof = proofs[s].at(name);
            if (proof.at("bytes") != bytes || !digest(proof.at("sha256"))) return std::nullopt;
        }
    }
    return result;
}

auto input_identity(const Json& stream, const MeasuredBlock& measured) -> std::string {
    auto stages=Json::array();
    const auto& inputs=stream.at("external_inputs");
    for (std::size_t stage=0;stage<measured.work_units.size();++stage)
        stages.push_back({{"inputs",inputs.at(stage)}, {"work_units",measured.work_units[stage]},
                          {"output_count",measured.output_counts[stage]}});
    return core::sha256(Json{{"stages",std::move(stages)},{"handoff",measured.handoff_identity}}.dump()).to_hex();
}
} // namespace epycbench::results::package_stream
