#include "results/private_normalization_serialization.hpp"
#include "epycbench/topology/serialization.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace epycbench::results::normalization_serialization {
namespace ss = source_serialization;
namespace pp = private_plan_composition;
namespace ts = topology_serialization;
namespace sc = scenario;
namespace tp = epycbench::topology;
namespace detail {
auto checked_add(std::size_t& value, std::size_t amount) noexcept -> bool {
    if (amount > SIZE_MAX - value) return false;
    value += amount; return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (a && b > SIZE_MAX / a) return {};
    return a * b;
}
}
namespace {
using Json = nlohmann::ordered_json;
using View = detail::View;
constexpr std::string_view digits = "0123456789abcdef";
constexpr std::array fields{
    &Quantities::input_bytes, &Quantities::nodes, &Quantities::depth, &Quantities::text_bytes,
    &Quantities::canonical_bytes, &Quantities::provenance_bytes, &Quantities::source_preparation_bytes,
    &Quantities::plan_preparation_bytes, &Quantities::topology_preparation_bytes,
    &Quantities::association_entries, &Quantities::association_bytes, &Quantities::association_work,
    &Quantities::publication_bytes, &Quantities::retained_bytes};
void need(bool ok, Error error = Error::association) { if (!ok) throw error; }
auto add(std::size_t a, std::size_t b) -> std::size_t {
    need(detail::checked_add(a, b), Error::capacity); return a;
}
auto mul(std::size_t a, std::size_t b) -> std::size_t {
    const auto result = detail::checked_multiply(a, b); need(result.has_value(), Error::capacity); return *result;
}
void bound(std::size_t n, std::size_t limit) { need(n <= limit, Error::capacity); }
void valid_limits(const Limits& limits) {
    for (auto field : fields) need(limits.*field <= static_cast<std::size_t>(PTRDIFF_MAX), Error::invalid_limits);
    need(limits.depth <= 64 && limits.canonical_bytes <= std::string{}.max_size() &&
         limits.provenance_bytes <= std::string{}.max_size(), Error::invalid_limits);
}
auto domain_name(process_subject::Domain domain) -> std::string_view {
    if (domain == process_subject::Domain::supplied_observation) return "supplied_observation";
    if (domain == process_subject::Domain::synthetic_execution) return "synthetic_execution";
    throw Error::schema;
}
void metadata(const View& view, const Limits& limits) {
    (void)domain_name(view.domain);
    need(view.sequence > 1 && view.topology_sequence && view.topology_sequence <= view.sequence, Error::reference);
    bound(add(view.run.size(), view.time.size()), limits.provenance_bytes);
    bound(18, limits.nodes); bound(2, limits.depth);
}
auto nibble(char c) -> unsigned char {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    throw Error::encoding;
}
void valid_hex(std::string_view text) {
    need(text.size() % 2 == 0, Error::encoding);
    for (std::size_t i = 0; i < text.size(); ++i) (void)nibble(text[i]);
}
auto number(std::string_view text) -> std::uint64_t {
    need(!text.empty() && text.front() != '0', Error::encoding);
    std::uint64_t value{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        need(c >= '0' && c <= '9', Error::encoding);
        const auto digit = static_cast<std::uint64_t>(c - '0');
        need(value <= (UINT64_MAX - digit) / 10, Error::encoding); value = value * 10 + digit;
    }
    return value;
}
auto unhex(std::string_view text) -> std::string {
    std::string result(text.size() / 2, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>((nibble(text[2 * i]) << 4) | nibble(text[2 * i + 1]));
    return result;
}
struct Wire {
    View view{};
    std::array<std::string_view, 3> bodies{}, hashes{}; // Source, Plan, Topology.
    std::array<std::size_t, 3> lengths{};
};
struct Reader {
    std::string_view input; std::size_t offset{};
    void literal(std::string_view text) {
        need(input.substr(offset, text.size()) == text, Error::schema); offset += text.size();
    }
    auto quoted() -> std::string_view {
        literal("\""); const auto begin = offset;
        while (offset < input.size() && input[offset] != '"') {
            const auto c = static_cast<unsigned char>(input[offset++]);
            need(c >= 0x20 && c <= 0x7e && c != '\\', Error::encoding);
        }
        need(offset < input.size(), Error::schema);
        const auto result = input.substr(begin, offset - begin); ++offset; return result;
    }
    void child(Wire& wire, std::size_t index) {
        literal("{\"canonical_hex\":"); wire.bodies[index] = quoted();
        literal(",\"canonical_sha256\":"); wire.hashes[index] = quoted(); literal("}");
        valid_hex(wire.bodies[index]); valid_hex(wire.hashes[index]);
        need(wire.hashes[index].size() == 64, Error::encoding); wire.lengths[index] = wire.bodies[index].size() / 2;
    }
};
auto parse(std::string_view input, const Limits& limits) -> Wire {
    bound(input.size(), limits.input_bytes); bound(input.size(), limits.canonical_bytes);
    Reader r{input}; Wire result;
    r.literal("{\"domain\":"); const auto domain = r.quoted();
    if (domain == "supplied_observation") result.view.domain = process_subject::Domain::supplied_observation;
    else if (domain == "synthetic_execution") result.view.domain = process_subject::Domain::synthetic_execution;
    else throw Error::schema;
    r.literal(",\"kind\":\"resolved\",\"plan\":"); r.child(result, 1);
    r.literal(",\"producer\":\"controller\",\"run_id\":"); result.view.run = r.quoted();
    r.literal(",\"schema\":\""); r.literal(profile); r.literal("\",\"sequence\":"); result.view.sequence = number(r.quoted());
    r.literal(",\"source\":"); r.child(result, 0);
    r.literal(",\"timestamp\":"); result.view.time = r.quoted();
    r.literal(",\"topology\":"); r.child(result, 2);
    r.literal(",\"topology_event_sequence\":"); result.view.topology_sequence = number(r.quoted());
    r.literal("}"); need(r.offset == input.size(), Error::schema); metadata(result.view, limits);
    bound(result.lengths[0], limits.source.canonical_bytes);
    bound(result.lengths[1], limits.plan.envelope_bytes);
    bound(result.lengths[2], limits.topology.canonical_bytes);
    return result;
}
struct Sink {
    std::string* output{}; std::size_t size{}, limit{}, text{};
    void raw(std::string_view value) { size = add(size, value.size()); bound(size, limit); if (output) output->append(value); }
    void quoted(std::string_view value) { text = add(text, value.size()); raw("\""); raw(value); raw("\""); }
    void key(std::string_view value) { quoted(value); raw(":"); }
    void decimal(std::uint64_t value) {
        std::array<char, 21> bytes{}; auto begin = bytes.size();
        do { bytes[--begin] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        quoted({bytes.data() + begin, bytes.size() - begin});
    }
    void hex(std::string_view value) {
        text = add(text, mul(value.size(), 2)); raw("\"");
        if (!output) { size = add(size, mul(value.size(), 2)); bound(size, limit); }
        else for (std::size_t i = 0; i < value.size(); ++i) { const auto byte = static_cast<unsigned char>(value[i]); const std::array<char, 2> pair{digits[byte >> 4], digits[byte & 15]}; raw({pair.data(), pair.size()}); }
        raw("\"");
    }
    void digest(const core::Sha256Digest& value) {
        std::array<char, 64> text_bytes{};
        for (std::size_t i = 0; i < value.bytes().size(); ++i) {
            const auto byte = std::to_integer<unsigned char>(value.bytes()[i]);
            text_bytes[2 * i] = digits[byte >> 4]; text_bytes[2 * i + 1] = digits[byte & 15];
        }
        quoted({text_bytes.data(), text_bytes.size()});
    }
    void child(std::string_view canonical, const core::Sha256Digest& hash) {
        raw("{"); key("canonical_hex"); hex(canonical); raw(","); key("canonical_sha256"); digest(hash); raw("}");
    }
};
void emit(Sink& sink, const View& view, const ss::Owner& source, const pp::Owner& plan, const ts::Owner& topology) {
    sink.raw("{"); sink.key("domain"); sink.quoted(domain_name(view.domain));
    sink.raw(","); sink.key("kind"); sink.quoted("resolved");
    sink.raw(","); sink.key("plan"); sink.child(plan.canonical(), *plan.digest());
    sink.raw(","); sink.key("producer"); sink.quoted("controller");
    sink.raw(","); sink.key("run_id"); sink.quoted(view.run);
    sink.raw(","); sink.key("schema"); sink.quoted(profile);
    sink.raw(","); sink.key("sequence"); sink.decimal(view.sequence);
    sink.raw(","); sink.key("source"); sink.child(source.canonical(), *source.digest());
    sink.raw(","); sink.key("timestamp"); sink.quoted(view.time);
    sink.raw(","); sink.key("topology"); sink.child(topology.canonical(), *topology.digest());
    sink.raw(","); sink.key("topology_event_sequence"); sink.decimal(view.topology_sequence); sink.raw("}");
}
void digest_matches(const core::Sha256Digest& digest, std::string_view text) {
    need(text.size() == 64, Error::identity);
    for (std::size_t i = 0; i < 32; ++i) {
        const auto byte = std::to_integer<unsigned char>(digest.bytes()[i]);
        need(text[2 * i] == digits[byte >> 4] && text[2 * i + 1] == digits[byte & 15], Error::identity);
    }
}
auto phase_usage(const Limits& limits, std::size_t upper, const ss::Owner* source, const pp::Owner* plan,
                 std::size_t hs, std::size_t hp, std::size_t ht) -> Usage {
    Usage out; const auto fixed = add(detail::fixed_stock(), upper);
    const auto sf = add(ss::detail::fixed_stock(), upper ? ss::detail::session_fixed_stock() : 0);
    const auto tf = add(ts::detail::fixed_stock(), upper ? ts::detail::session_fixed_stock() : 0);
    const auto s = std::max({limits.source.error_preparation_bytes, limits.source.source_preparation_bytes,
                            limits.source.publication_bytes, limits.source.retained_bytes, sf});
    const auto p = std::max({limits.plan.consumer_simultaneous_bytes, limits.plan.retained_bytes, pp::detail::fixed_stock()});
    const auto t = std::max({limits.topology.topology_decode_bytes, limits.topology.topology_preparation_bytes,
                            limits.topology.error_preparation_bytes, limits.topology.publication_bytes,
                            limits.topology.retained_bytes, tf});
    out.source_preparation_bytes = add(add(fixed, hs), s);
    if (source) out.source = source->usage();
    out.plan_preparation_bytes = add(add(add(fixed, out.source.retained_bytes), hp), p);
    if (plan) out.plan = plan->usage();
    out.topology_preparation_bytes = add(add(add(add(fixed, out.source.retained_bytes), out.plan.retained_bytes), ht), t);
    return out;
}
struct Work {
    std::size_t used{}, limit{};
    void debit(std::size_t n = 1) { need(n <= limit - used, Error::capacity); used += n; }
    auto equal(std::string_view a, std::string_view b) -> bool { debit(add(1, add(a.size(), b.size()))); return a == b; }
    auto less(std::string_view a, std::string_view b) -> bool { debit(add(1, add(a.size(), b.size()))); return a < b; }
};
void associate_metadata(const View& view, const ss::Owner& source, const pp::Owner& plan, const ts::Owner& topology, Work& work) {
    work.debit();
    need(source.provenance() && source.source() && plan.document_ancestry() && topology.provenance() && topology.topology(), Error::child);
    const auto& sp = *source.provenance(); const auto& tpv = *topology.provenance(); const auto input = source.source()->input();
    work.debit(4);
    need(sp.kind == ss::Kind::normalization_source && work.equal(sp.run, view.run) && work.equal(sp.time, view.time) && sp.domain == view.domain &&
         sp.sequence == view.sequence && !sp.acquisition, Error::identity);
    work.debit();
    need(input.availability == authored_source::Availability::available && !input.acquisition_failure &&
         input.rejected_overrides.empty() && !source.acquisition_error(), Error::source);
    need(work.equal(input.name, plan.document_ancestry()->source_name()), Error::identity);
    work.debit();
    need(topology.slot_state() == ts::SlotState::retained && !topology.failure_error() && !topology.reason() && !tpv.failure, Error::source);
    work.debit(2);
    need(work.equal(tpv.run, view.run) && tpv.domain == view.domain && tpv.sequence == view.topology_sequence, Error::identity);
    work.debit();
    if (view.topology_sequence == view.sequence) {
        work.debit(2);
        need(tpv.kind == ts::Kind::ordinary && tpv.reporter == process_subject::Producer::controller && work.equal(tpv.time, view.time), Error::identity);
    }
    work.debit(2);
    need(plan.resource_service()->topology_fingerprints() && topology.topology()->fingerprints() &&
         *plan.resource_service()->topology_fingerprints() == *topology.topology()->fingerprints(), Error::identity);
}
struct PuRow {
    const tp::StableProcessingUnitId* stable{}; const tp::StableCoreId* core{};
    const tp::ProcessingUnit* pu{}; const tp::NumaNode* node{}; std::size_t memberships{};
};
struct NodeRow {
    const tp::StableNumaNodeId* stable{}; const tp::NumaNode* node{};
    std::optional<core::LinuxNumaNodeId> linux; bool worker_eligible{}, memory_eligible{};
};
auto index_bytes(std::size_t pus, std::size_t nodes) -> std::size_t {
    return add(mul(pus, sizeof(PuRow)), mul(nodes, add(sizeof(NodeRow), sizeof(const NodeRow*))));
}
// Iterative heapsort uses named comparators and explicit nonrecursive frames.
auto index_less(const PuRow& a, const PuRow& b, Work& work) -> bool {
    return work.less(a.stable->value(), b.stable->value());
}
auto index_less(const NodeRow& a, const NodeRow& b, Work& work) -> bool {
    return work.less(a.stable->value(), b.stable->value());
}
auto index_less(const NodeRow* a, const NodeRow* b, Work& work) -> bool {
    work.debit(); return a->linux != b->linux ? a->linux < b->linux : work.less(a->stable->value(), b->stable->value());
}
template<class T> void heap_sift(std::vector<T>& values, Work& work, std::size_t root, std::size_t end) {
    work.debit();
    while (root < end / 2) {
        work.debit(); auto child = root * 2 + 1;
        if (child + 1 < end && index_less(values[child], values[child + 1], work)) ++child;
        if (!index_less(values[root], values[child], work)) break;
        work.debit(); std::swap(values[root], values[child]); root = child;
    }
}
template<class T> void heap_sort(std::vector<T>& values, Work& work) {
    work.debit();
    for (std::size_t root = values.size() / 2; root; --root) { work.debit(); heap_sift(values, work, root - 1, values.size()); }
    for (std::size_t end = values.size(); end > 1; --end) {
        work.debit(2); std::swap(values[0], values[end - 1]); heap_sift(values, work, 0, end - 1);
    }
}
class Checker {
  public:
    const pp::Owner& plan; const tp::LoadedTopology& loaded; Work& work; std::size_t membership_bound;
    std::vector<PuRow> pus; std::vector<NodeRow> nodes; std::vector<const NodeRow*> axis;
    core::ByteCount page;
    Checker(const pp::Owner& p, const tp::LoadedTopology& l, Work& w, std::size_t membership)
        : plan(p), loaded(l), work(w), membership_bound(membership), page(core::ByteCount::from_base_units(0)) {}
    template<class Id> auto member(const core::SparseIdSet<Id>& set, Id id) -> bool {
        work.debit(add(1, membership_bound)); return set.contains(id);
    }
    auto pages(const tp::NumaNode& node) -> bool {
        work.debit();
        for (std::size_t i = 0; i < node.page_sizes.size(); ++i) { work.debit(); if (node.page_sizes[i] == page) return true; }
        return false;
    }
    void build() {
        work.debit(4); const auto& machine = loaded.topology.machine();
        need(!machine.page_sizes.empty() && machine.page_sizes.front().value() > 0);
        need(machine.effective_cpus.availability() == tp::Availability::available && machine.effective_cpus.value() &&
             machine.effective_nodes.availability() == tp::Availability::available && machine.effective_nodes.value());
        page = machine.page_sizes.front();
        pus.reserve(loaded.topology.processing_units().size()); nodes.reserve(loaded.topology.numa_nodes().size());
        axis.reserve(loaded.topology.numa_nodes().size());
        for (const auto& pu : loaded.topology.processing_units()) {
            work.debit(); PuRow row; row.pu = &pu;
            for (const auto& stable : loaded.stable_ids.processing_units()) { work.debit(); if (stable.handle == pu.id) { row.stable = &stable.semantic_id; break; } }
            for (const auto& core : loaded.stable_ids.cores()) { work.debit(); if (core.handle == pu.core_id) { row.core = &core.semantic_id; break; } }
            need(row.stable && row.core);
            for (const auto& node : loaded.topology.numa_nodes()) {
                work.debit(); if (member(node.member_pus, pu.id)) { row.node = &node; ++row.memberships; }
            }
            pus.push_back(row);
        }
        for (const auto& node : loaded.topology.numa_nodes()) {
            work.debit(); NodeRow row; row.node = &node;
            for (const auto& stable : loaded.stable_ids.numa_nodes()) { work.debit(); if (stable.handle == node.id) { row.stable = &stable.semantic_id; break; } }
            need(row.stable);
            if (node.linux_node_id.availability() == tp::Availability::available && node.linux_node_id.value())
                row.linux = core::LinuxNumaNodeId{*node.linux_node_id.value()};
            if (row.linux && member(*machine.effective_nodes.value(), core::NumaNodeId{row.linux->value()})) {
                for (const auto& pu : loaded.topology.processing_units()) {
                    work.debit(); if (member(node.member_pus, pu.id) && member(*machine.effective_cpus.value(), pu.linux_cpu_id)) { row.worker_eligible = true; break; }
                }
                row.memory_eligible = node.installed_memory.availability() == tp::Availability::available &&
                    node.installed_memory.value() && node.installed_memory.value()->value() > 0 && pages(node);
            }
            nodes.push_back(row);
        }
        heap_sort(pus, work);
        heap_sort(nodes, work);
        for (const auto& node : nodes) { work.debit(); if (node.linux) axis.push_back(&node); }
        heap_sort(axis, work);
    }
    auto find_pu(std::string_view key) -> const PuRow& {
        work.debit(); std::size_t first{}, last = pus.size();
        while (first < last) { work.debit(); const auto middle = first + (last - first) / 2; if (work.less(pus[middle].stable->value(), key)) first = middle + 1; else last = middle; }
        work.debit(); need(first < pus.size() && work.equal(pus[first].stable->value(), key)); return pus[first];
    }
    auto find_node(std::string_view key) -> const NodeRow& {
        work.debit(); std::size_t first{}, last = nodes.size();
        while (first < last) { work.debit(); const auto middle = first + (last - first) / 2; if (work.less(nodes[middle].stable->value(), key)) first = middle + 1; else last = middle; }
        work.debit(); need(first < nodes.size() && work.equal(nodes[first].stable->value(), key)); return nodes[first];
    }
    auto pu(const tp::StableProcessingUnitId& stable, core::LinuxCpuId cpu, const tp::StableCoreId& core) -> const PuRow& {
        work.debit(); const auto& found = find_pu(stable.value());
        work.debit(); need(found.pu->linux_cpu_id == cpu && work.equal(found.core->value(), core.value()) &&
             member(*loaded.topology.machine().effective_cpus.value(), cpu)); return found;
    }
    auto pu(const sc::ResolvedProcessingUnit& value) -> const PuRow& { work.debit(); return pu(value.stable_id, value.linux_cpu, value.core_id); }
    auto node(const sc::ResolvedMemoryNode& value, bool memory) -> const NodeRow& {
        work.debit(); const auto& found = find_node(value.stable_id.value());
        work.debit(); need(found.linux && *found.linux == value.linux_node);
        if (memory) need(found.memory_eligible);
        return found;
    }
    void placement(const sc::ResolvedMemoryPlacement& value) {
        work.debit(2); need(value.page_size == page);
        for (const auto& n : value.nodes) { work.debit(); (void)node(n, true); }
    }
    void placement(const sc::ComparisonMemoryPlacement& value) {
        work.debit();
        if (const auto* single = std::get_if<sc::ResolvedMemoryPlacement>(&value)) placement(*single);
        else {
            const auto& each = std::get<sc::ResolvedPerWorkerMemoryPlacement>(value); work.debit(); need(each.page_size == page);
            for (const auto& mapping : each.worker_nodes) {
                work.debit(); const auto& worker = pu(mapping.worker); const auto& target = node(mapping.node, true);
                need(worker.memberships == 1 && worker.node == target.node);
            }
        }
    }
    auto child(const Json& object, std::string_view key) -> const Json& {
        work.debit(); need(object.is_object());
        for (auto it = object.begin(); it != object.end(); ++it) { work.debit(); if (work.equal(it.key(), key)) return it.value(); }
        throw Error::association;
    }
    void local(const sc::ResolvedComparisonExperiment& experiment, const sc::ResolvedComparisonVariant& variant) {
        work.debit(); const auto& effective = *plan.document_ancestry()->default_expanded();
        const auto& experiments = child(effective, "experiments"); work.debit(); need(experiments.is_array() && experiment.experiment_index < experiments.size());
        const auto& variants = child(experiments[experiment.experiment_index], "variants"); work.debit(); need(variants.is_array() && variant.variant_index < variants.size());
        const auto& mode = child(child(variants[variant.variant_index], "memory"), "nodes");
        if (mode.is_array()) { need(std::holds_alternative<sc::ResolvedMemoryPlacement>(variant.memory_placement)); return; }
        work.debit(); need(mode.is_object() && mode.size() == 1); const auto& key = mode.begin().key();
        if (work.equal(key, "local-to-each-worker")) { need(std::holds_alternative<sc::ResolvedPerWorkerMemoryPlacement>(variant.memory_placement)); return; }
        need(work.equal(key, "local-to-workers") && std::holds_alternative<sc::ResolvedMemoryPlacement>(variant.memory_placement));
        const auto& memory = std::get<sc::ResolvedMemoryPlacement>(variant.memory_placement); std::size_t retained{};
        for (std::size_t i = 0; i < experiment.workers.size(); ++i) {
            work.debit(); const auto& current = pu(experiment.workers[i]); need(current.memberships == 1); bool seen{};
            for (std::size_t prior = 0; prior < i; ++prior) { work.debit(); if (pu(experiment.workers[prior]).node == current.node) { seen = true; break; } }
            if (!seen) { need(retained < memory.nodes.size() && node(memory.nodes[retained++], true).node == current.node); }
        }
        work.debit(); need(retained == memory.nodes.size());
    }
    void matrix(const sc::ExpandedMatrixExperiment& value) {
        work.debit(); std::size_t worker{}, memory{};
        for (const auto* candidate : axis) {
            work.debit();
            if (candidate->worker_eligible) {
                need(worker < value.worker_axis.size() && node(value.worker_axis[worker++], false).node == candidate->node);
            }
            if (member(*loaded.topology.machine().effective_nodes.value(), core::NumaNodeId{candidate->linux->value()})) {
                need(candidate->node->installed_memory.availability() == tp::Availability::available && candidate->node->installed_memory.value());
                if (candidate->node->installed_memory.value()->value() > 0) need(candidate->memory_eligible);
            }
            if (candidate->memory_eligible) {
                need(memory < value.memory_axis.size() && node(value.memory_axis[memory++], true).node == candidate->node);
            }
        }
        work.debit(2); need(worker == value.worker_axis.size() && memory == value.memory_axis.size());
        for (const auto& row : value.worker_rows) {
            work.debit(); const auto& row_node = find_node(row.stable_node_id.value()); need(row_node.linux && *row_node.linux == row.linux_node);
            for (const auto& w : row.workers) { work.debit(); const auto& selected = pu(w); need(selected.memberships == 1 && selected.node == row_node.node); }
        }
        for (const auto& cell : value.cells) {
            work.debit(); need(cell.worker_row_index < value.worker_rows.size());
            const auto& from = find_node(value.worker_rows[cell.worker_row_index].stable_node_id.value());
            const auto& to = find_node(cell.memory_node_stable_id.value()); need(to.linux && *to.linux == cell.memory_node);
            placement(cell.memory_placement); bool found{};
            for (const auto& distance : loaded.topology.numa_distances().directed_distances) {
                work.debit(); if (distance.from == from.node->id && distance.to == to.node->id) { need(distance.value == cell.distance.value); found = true; break; }
            }
            need(found);
        }
    }
    void run() {
        work.debit(); build();
        for (const auto& entry : *plan.resource_service()->worker_sets()) {
            work.debit(); for (const auto& w : entry.second.processing_units) { work.debit(); (void)pu(w); }
        }
        for (const auto& entry : *plan.resource_service()->service_cpu_pools()) {
            work.debit(); for (const auto& s : entry.second.processing_units) { work.debit(); (void)pu(s.stable_id, s.linux_cpu, s.core_id); }
        }
        for (const auto& value : *plan.memory_placement()->comparison_memory()) { work.debit(); placement(value.placement); }
        for (const auto& value : *plan.memory_placement()->matrix_memory_templates()) { work.debit(); need(value.page_size == page); }
        for (const auto& value : *plan.comparison_experiments()->experiments()) {
            work.debit(); for (const auto& w : value.workers) { work.debit(); (void)pu(w); }
            for (const auto& v : value.variants) { work.debit(); placement(v.memory_placement); local(value, v); }
        }
        for (const auto& value : *plan.matrix_experiments()->experiments()) { work.debit(); matrix(value); }
    }
};
void topology_association(const pp::Owner& plan, const ts::Owner& topology, const Limits& limits,
                          std::size_t upper, Usage& usage, Work& work) {
    const auto* evidence = topology.topology(); need(evidence && evidence->fingerprints(), Error::child);
    const auto counts = evidence->usage();
    const auto entries = add(counts.processing_units, mul(2, counts.numa_nodes));
    bound(entries, limits.association_entries);
    bound(counts.processing_units, std::vector<PuRow>{}.max_size());
    bound(counts.numa_nodes, std::vector<NodeRow>{}.max_size());
    bound(counts.numa_nodes, std::vector<const NodeRow*>{}.max_size());
    usage.association_entries = entries;
    usage.association_bytes = add(add(add(add(add(add(detail::fixed_stock(), upper), usage.source.retained_bytes),
        usage.plan.retained_bytes), usage.topology.retained_bytes), evidence->canonical().size()), index_bytes(counts.processing_units, counts.numa_nodes));
    bound(usage.association_bytes, limits.association_bytes);
    work.debit();
    auto loaded = tp::deserialize_topology(evidence->canonical(), tp::TopologyJsonInputPolicy::require_canonical);
    core::detail::ExpectedErrorCleanup cleanup{loaded}; need(loaded.has_value(), Error::topology);
    need(work.equal(loaded->canonical_json, evidence->canonical()) && loaded->fingerprints == *evidence->fingerprints(), Error::identity);
    work.debit(2);
    need(loaded->topology.processing_units().size() == counts.processing_units && loaded->topology.numa_nodes().size() == counts.numa_nodes, Error::topology);
    Checker checker{plan, *loaded, work, add(counts.array_entries, counts.expanded_ranges)}; checker.run();
    usage.association_work = work.used;
}
auto publish(std::optional<ss::Owner>& source, std::optional<pp::Owner>& plan,
             std::optional<ts::Owner>& topology, const View& view, const Limits& limits,
             Usage usage, std::size_t upper, Origin origin, std::string_view original = {}) -> std::unique_ptr<detail::State> {
    need(source && plan && topology, Error::child);
    Work work{0, limits.association_work}; work.debit();
    associate_metadata(view, *source, *plan, *topology, work);
    usage.topology = topology->usage(); topology_association(*plan, *topology, limits, upper, usage, work);
    Sink count{nullptr, 0, limits.canonical_bytes, 0}; emit(count, view, *source, *plan, *topology);
    usage.nodes = 18; usage.depth = 2; usage.text_bytes = count.text; usage.canonical_bytes = count.size;
    usage.provenance_bytes = add(view.run.size(), view.time.size()); bound(usage.text_bytes, limits.text_bytes);
    need(usage.source.retained_bytes >= sizeof(ss::Owner) && usage.plan.retained_bytes >= sizeof(pp::Owner) &&
         usage.topology.retained_bytes >= sizeof(ts::Owner), Error::child);
    usage.retained_bytes = add(add(add(add(add(sizeof(Owner), sizeof(detail::State)), usage.provenance_bytes), usage.canonical_bytes),
        usage.source.retained_bytes - sizeof(ss::Owner)), add(usage.plan.retained_bytes - sizeof(pp::Owner), usage.topology.retained_bytes - sizeof(ts::Owner)));
    usage.publication_bytes = add(add(detail::fixed_stock(), upper), usage.retained_bytes);
    bound(usage.retained_bytes, limits.retained_bytes); bound(usage.publication_bytes, limits.publication_bytes);
    std::string canonical; canonical.reserve(count.size); Sink output{&canonical, 0, count.size, 0}; emit(output, view, *source, *plan, *topology);
    need(output.size == count.size && output.text == count.text && (original.empty() || canonical == original), Error::encoding);
    Provenance provenance{std::string{view.run}, std::string{view.time}, view.domain, view.sequence, view.topology_sequence};
    const auto hash = core::sha256(canonical);
    return std::unique_ptr<detail::State>{new detail::State{std::move(provenance), std::move(source), std::move(plan),
        std::move(topology), std::move(canonical), hash, usage, origin}};
}
// Concrete source-local call/layout union; dynamic children/indexes and accepted
// nested library scratch are charged separately, never inferred from these cells.
using V = std::string_view;
using Z = std::size_t;
using U = std::uint64_t;
using Void = std::expected<void, Error>;
using Dig = core::Sha256Digest;
using LoadedResult = std::expected<tp::LoadedTopology,core::Error>;
using LoadedGuard = core::detail::ExpectedErrorCleanup<LoadedResult>;
struct DecodeCall { V input; Limits limits; std::expected<Owner,Error> returned; std::unexpected<Error> failure; Error code; };
struct PrepareCall { const detail::ResolvedInput* input; Limits limits; std::expected<Owner,Error> returned; std::unexpected<Error> failure; Error code; };
struct PreflightCall { const detail::ResolvedInput* input; const Limits* limits; Void result; bool valid; View view; authored_source::Input source_input; authored_source::Usage record_usage; };
struct InputConstruction { detail::ResolvedInput* self; const authored_source::ResolutionRecord* record; View view; std::optional<ss::Owner>* source; std::optional<pp::Owner>* plan; std::optional<ts::Owner>* topology; Z upper; };
struct LowerLocals { Wire wire; Usage usage; View view; Z upper; std::array<Z,8> stocks; std::array<Z,3> scratch_lengths; Origin origin; };
struct PreparedLocals { std::optional<ss::Owner> source; std::optional<pp::Owner> plan; std::optional<ts::Owner> topology; ss::Usage source_usage; pp::Usage plan_usage; ts::Usage topology_usage; };
struct ChildDecodeSource { V wire; ss::Limits limits; std::string scratch; std::expected<ss::Owner,ss::Error> result; ss::Usage usage; Error mapped; };
struct ChildDecodePlan { V wire; pp::Limits limits; std::string scratch; std::expected<pp::Owner,pp::Error> result; pp::Usage usage; Error mapped; };
struct ChildDecodeTopology { V wire; ts::Limits limits; std::string scratch; std::expected<ts::Owner,ts::Error> result; ts::Usage usage; Error mapped; };
struct PrimitiveRead { Reader reader; const Limits* limits; Wire wire; Void result; V literal; Z root_member,child_index; };
struct ReadLiteral { Reader* receiver; V expected; Z index; bool matched; };
struct ReadQuoted { Reader* receiver; V result; Z begin,length; char character; };
struct ReadDecimal { V text; U value,digit; Z index; bool valid; };
struct ReadHex { V text; Z index; unsigned char character,nibble; bool valid; };
struct Unhex { V text; std::string result; Z index; unsigned char high,low; char byte; };
struct LimitCheck { const Limits* limits; const Wire* wire; Z maximum,length; bool valid; Void result; };
struct CheckedAdd { Z* destination; Z amount,ceiling; bool success; };
struct CheckedMultiply { Z left,right,result; bool success; };
struct ChildBudget { const Limits* limits; const Usage* usage; Z fixed,prefix,scratch,allowance,floor; bool capture; Void result; };
struct MetadataJoin { const View* parent; const ss::Owner* source; const pp::Owner* plan; const ts::Owner* topology; const ss::Provenance* source_provenance; const ts::Provenance* topology_provenance; authored_source::Input source_input; V source_name; Void result; Work* work; };
struct TextEqual { V left,right; Work* work; bool result; };
struct ChildDigest { const Dig* digest; V expected; std::array<char,64> text; Z index; unsigned char byte; bool equal; };
struct Reload { const ts::Owner* child; const Limits* limits; const topology_evidence::Owner* evidence; topology_evidence::Usage usage; topology_evidence::Limits bounds; V canonical; tp::TopologyJsonInputPolicy policy; LoadedResult loaded; LoadedGuard guard; Void result; };
struct ReloadGuardCall { LoadedGuard* self; LoadedResult* owner; nlohmann::json* context; };
struct IndexStorage { std::vector<PuRow> pus; std::vector<NodeRow> nodes; std::vector<const NodeRow*> axis; Work work; };
struct IndexCounts { const tp::LoadedTopology* loaded; const Limits* limits; Z pu_count,node_count,available_count,entries,bytes,membership_bound; Void result; };
struct BuildPuRows { const tp::LoadedTopology* loaded; std::vector<PuRow>* destination; Work* work; PuRow row; Z pu_index,stable_index,core_index,node_index; const tp::StableProcessingUnitEntry* stable; const tp::StableCoreEntry* core; };
struct BuildNodeRows { const tp::LoadedTopology* loaded; std::vector<NodeRow>* destination; Work* work; NodeRow row; Z node_index,stable_index,pu_index,page_index; const tp::StableNumaNodeEntry* stable; };
template<class T> struct HeapFrame { T* data; Z count,root,end,child,selected; T swap; Work* work; };
struct FindPu { std::span<const PuRow> rows; V key; Z first,last,middle; const PuRow* found; Work* work; };
struct FindNode { std::span<const NodeRow> rows; V key; Z first,last,middle; const NodeRow* found; Work* work; };
struct Associate { const pp::Owner* plan; const tp::LoadedTopology* loaded; std::span<const PuRow> pus; std::span<const NodeRow> nodes; std::span<const NodeRow* const> axis; Work* work; const Limits* limits; Usage* usage; Void result; };
struct PlanLoops { const pp::Owner* plan; std::array<Z,8> indices; std::array<V,3> identities; const Json *effective,*experiments,*experiment,*variants,*variant,*memory,*nodes; };
struct BorrowedGetters { const authored_source::ResolutionRecord* record; const authored_source::SourceBundle* source; authored_source::Input input_return; authored_source::Usage usage_return; const sc::ScenarioDigestedPlan* plan_return; std::span<const authored_source::SafeDiagnostic> diagnostics_return; const ss::Owner* source_owner; const pp::Owner* plan_owner; const ts::Owner* topology_owner; const topology_evidence::Owner* topology_return; const tp::TopologyFingerprints* fingerprints_return; V canonical_return; const Dig* digest_return; const Json* effective_return; };
struct IndexCompare { const PuRow *pu_left,*pu_right; const NodeRow *node_left,*node_right; Work* work; V left,right; bool result; };
struct LookupMember { const Json* object; V key; Json::const_iterator current,end; const Json* result; Work* work; };
struct ValidatePu { const PuRow* found; const tp::StableProcessingUnitId* stable; const tp::StableCoreId* core; core::LinuxCpuId cpu; Work* work; Void result; };
struct ValidateNode { const NodeRow* found; const tp::StableNumaNodeId* stable; core::LinuxNumaNodeId linux; core::ByteCount page; bool memory_required; Work* work; Void result; };
struct LocalMembership { std::span<const PuRow> pus; std::span<const NodeRow> nodes; Work* work; Z worker,previous_worker,retained_node; const PuRow* current; const NodeRow* selected; bool seen,per_worker; };
struct MatrixAxes { std::span<const NodeRow* const> axis; Work* work; Z node_index,retained_worker,retained_memory; const NodeRow* node; bool has_matrix; Void result; };
struct DirectedDistance { const tp::NumaDistanceMatrix* matrix; core::NumaNodeId from,to; std::optional<std::uint32_t> result; Z index; Work* work; };
struct PageCheck { std::span<const core::ByteCount> pages; core::ByteCount required; Z index; bool found; Work* work; };
template<class Id> struct Membership { const core::SparseIdSet<Id>* set; Id value; Z bound; bool result; Work* work; };
struct WorkDebit { Work* receiver; Z units; bool success; };
struct Emit { const View* metadata; const ss::Owner* source; const pp::Owner* plan; const ts::Owner* topology; Sink sink; Z child_index,measured; Void result; };
struct SinkRaw { Sink* receiver; V text; Z length; bool success; };
struct SinkHex { Sink* receiver; V bytes; Z index; unsigned char value; std::array<char,2> pair; };
struct SinkDecimal { Sink* receiver; U number; std::array<char,21> digits; Z begin,count; char digit; };
struct Publish { std::optional<ss::Owner>* source; std::optional<pp::Owner>* plan; std::optional<ts::Owner>* topology; const View* view; Usage usage; Origin origin; std::string canonical; Provenance provenance; Dig digest; std::unique_ptr<detail::State> state; std::expected<Owner,Error> returned; };
struct StateConstruction { detail::State* self; Provenance* provenance; std::optional<ss::Owner>* source; std::optional<pp::Owner>* plan; std::optional<ts::Owner>* topology; std::string* canonical; Dig digest; Usage usage; Origin origin; };
struct OwnerConstruction { Owner* self; std::unique_ptr<detail::State> state; Owner *move_source,*move_destination; };
struct RangeMirror { std::uint32_t first,last; };
struct StatelessComparator {};
template<class Id> struct ContainsCells { const core::SparseIdSet<Id>* receiver; Id argument; std::uint32_t value; std::vector<RangeMirror>::const_iterator candidate; std::uint32_t needle; const RangeMirror* range; StatelessComparator comparator; std::vector<RangeMirror>::const_iterator predecessor; };
// Additional source-real receivers, by-value phases and explicit range iterators
// supplement the accepted worksheet; no dynamic payload is hidden in these cells.
struct PhaseCallCells {
    const Limits* limits; Z upper; const ss::Owner* source; const pp::Owner* plan;
    Z hs,hp,ht; Usage returned; Z fixed,sf,tf,s,p,t;
    std::array<Z,5> source_maximum; std::array<Z,3> plan_maximum; std::array<Z,6> topology_maximum;
    std::initializer_list<Z> source_list,plan_list,topology_list;
};
struct PrimitiveExtraCells {
    std::string capacity_string; std::vector<PuRow> capacity_pus; std::vector<NodeRow> capacity_nodes; std::vector<const NodeRow*> capacity_axis;
    process_subject::Domain domain; V domain_result; std::size_t Quantities::* field;
    std::size_t Quantities::* const* field_begin; std::size_t Quantities::* const* field_end;
    Z add_left,add_right,mul_left,mul_right; std::optional<Z> multiply_result;
    bool condition; Error condition_error; Z bound_value,bound_limit;
    Reader* child_receiver; Wire* child_wire; Z child_index;
    Sink *key_receiver,*quoted_receiver,*digest_receiver,*child_receiver_sink;
    V key,quoted,child_canonical; const Dig *digest,*child_hash; std::array<char,64> digest_text;
    Z digest_index; unsigned char digest_byte; char hex_character;
    Sink counter; const Limits* publish_limits; Z upper; V original;
    Checker checker; Sink* emit_receiver;
};
struct CheckerCallCells {
    Checker *constructor_self,*find_pu_self,*find_node_self,*build_self,*pages_self,*pu_value_self,*pu_self,*node_self,*placement_self,*variant_self,*child_self,*local_self,*matrix_self,*run_self;
    const pp::Owner* constructor_plan; const tp::LoadedTopology* constructor_loaded; Work* constructor_work; Z constructor_membership;
    const tp::Machine* machine; const tp::NumaNode* page_node;
    const sc::ResolvedProcessingUnit* pu_value; const sc::ResolvedMemoryNode* node_value;
    const sc::ResolvedMemoryPlacement* placement; const sc::ComparisonMemoryPlacement* variant;
    const sc::ResolvedPerWorkerMemoryPlacement* each;
    const sc::ResolvedComparisonExperiment* experiment; const sc::ResolvedComparisonVariant* comparison_variant;
    const sc::ExpandedMatrixExperiment* matrix; const Json* mode; const std::string* mode_key;
    const PuRow *mapping_worker,*local_previous,*row_worker; const NodeRow *mapping_target,*row_node,*from,*to;
    bool distance_found;
};
template<class T> struct VectorVisit { const std::vector<T>* range; typename std::vector<T>::const_iterator begin,end; const T* value; };
template<class T> struct SpanVisit { std::span<const T> range; typename std::span<const T>::iterator begin,end; const T* value; };
using WorkerMap = std::remove_cvref_t<decltype(*std::declval<resource_service_serialization::Owner>().worker_sets())>;
using ServiceMap = std::remove_cvref_t<decltype(*std::declval<resource_service_serialization::Owner>().service_cpu_pools())>;
template<class T> struct MapVisit { const T* range; typename T::const_iterator begin,end; const typename T::value_type* value; };
template<class T> struct HeapCallCells { std::vector<T>* sort_values; Work* sort_work; Z root,end; std::vector<T>* sift_values; Work* sift_work; Z sift_root,sift_end,child; T *swap_left,*swap_right; T swap_value; };
struct HashCells {
    std::array<std::uint32_t,8> state, working;
    std::array<std::uint32_t,64> schedule;
    std::array<std::byte,128> final_blocks;
    std::array<std::byte,32> result, constructor_argument;
    std::array<std::uint32_t,8>* compress_state; const std::byte* block;
    std::uint32_t s0,s1,sum0,sum1,choose,majority,t1,t2;
    std::size_t offset,schedule_index,schedule_offset,round_index,remainder,length_index,word,byte_index,copy_index;
    unsigned final_size; std::uint64_t bit_length;
    std::span<const std::byte> bytes, tail; std::string_view text;
    core::Sha256Digest returned; core::Sha256Digest* constructor_this;
};
} // namespace
namespace detail {
auto fixed_stock() noexcept -> std::size_t {
    return sizeof(DecodeCall) +
        sizeof(PrepareCall) +
        sizeof(PreflightCall) +
        sizeof(InputConstruction) +
        sizeof(LowerLocals) +
        sizeof(PreparedLocals) +
        sizeof(ChildDecodeSource) +
        sizeof(ChildDecodePlan) +
        sizeof(ChildDecodeTopology) +
        sizeof(PrimitiveRead) +
        sizeof(ReadLiteral) +
        sizeof(ReadQuoted) +
        sizeof(ReadDecimal) +
        sizeof(ReadHex) +
        sizeof(Unhex) +
        sizeof(LimitCheck) +
        sizeof(CheckedAdd) +
        sizeof(CheckedMultiply) +
        sizeof(ChildBudget) +
        sizeof(MetadataJoin) +
        sizeof(TextEqual) +
        sizeof(ChildDigest) +
        sizeof(Reload) +
        sizeof(ReloadGuardCall) +
        sizeof(IndexStorage) +
        sizeof(IndexCounts) +
        sizeof(BuildPuRows) +
        sizeof(BuildNodeRows) +
        sizeof(HeapFrame<PuRow>) +
        sizeof(HeapFrame<NodeRow>) +
        sizeof(HeapFrame<const NodeRow*>) +
        sizeof(FindPu) +
        sizeof(FindNode) +
        sizeof(Associate) +
        sizeof(PlanLoops) +
        sizeof(BorrowedGetters) +
        sizeof(IndexCompare) +
        sizeof(LookupMember) +
        sizeof(ValidatePu) +
        sizeof(ValidateNode) +
        sizeof(LocalMembership) +
        sizeof(MatrixAxes) +
        sizeof(DirectedDistance) +
        sizeof(PageCheck) +
        sizeof(Membership<core::LinuxCpuId>) +
        sizeof(Membership<core::NumaNodeId>) +
        sizeof(Membership<core::ProcessingUnitId>) +
        sizeof(WorkDebit) +
        sizeof(Emit) +
        sizeof(SinkRaw) +
        sizeof(SinkHex) +
        sizeof(SinkDecimal) +
        sizeof(Publish) +
        sizeof(StateConstruction) +
        sizeof(OwnerConstruction) +
        sizeof(ContainsCells<core::LinuxCpuId>) +
        sizeof(ContainsCells<core::NumaNodeId>) +
        sizeof(ContainsCells<core::ProcessingUnitId>) +
        sizeof(PhaseCallCells) +
        sizeof(PrimitiveExtraCells) +
        sizeof(CheckerCallCells) +
        sizeof(HashCells) +
        sizeof(VectorVisit<tp::ProcessingUnit>) +
        sizeof(VectorVisit<tp::NumaNode>) +
        sizeof(SpanVisit<tp::StableProcessingUnitEntry>) +
        sizeof(SpanVisit<tp::StableCoreEntry>) +
        sizeof(SpanVisit<tp::StableNumaNodeEntry>) +
        sizeof(VectorVisit<NodeRow>) +
        sizeof(VectorVisit<const NodeRow*>) +
        sizeof(MapVisit<WorkerMap>) +
        sizeof(MapVisit<ServiceMap>) +
        sizeof(HeapCallCells<PuRow>) +
        sizeof(HeapCallCells<NodeRow>) +
        sizeof(HeapCallCells<const NodeRow*>) +
        sizeof(VectorVisit<tp::ProcessingUnit>) +
        sizeof(VectorVisit<tp::NumaNode>) +
        sizeof(VectorVisit<sc::ResolvedProcessingUnit>) +
        sizeof(VectorVisit<sc::ResolvedProcessingUnit>) +
        sizeof(VectorVisit<sc::ResolvedProcessingUnit>) +
        sizeof(VectorVisit<sc::ResolvedMemoryNode>) + sizeof(VectorVisit<sc::ResolvedPerWorkerMemoryNode>) +
        sizeof(VectorVisit<sc::ComparisonVariantMemoryPlacement>) + sizeof(VectorVisit<sc::MatrixMemoryPlacementTemplate>) +
        sizeof(VectorVisit<sc::ResolvedComparisonExperiment>) + sizeof(VectorVisit<sc::ResolvedComparisonVariant>) +
        sizeof(VectorVisit<sc::ExpandedMatrixExperiment>) + sizeof(VectorVisit<sc::MatrixWorkerRow>) +
        sizeof(VectorVisit<sc::ResolvedMatrixCell>) + sizeof(VectorVisit<tp::NumaDistance>) +
        sizeof(VectorVisit<typename decltype(std::declval<ServiceMap::mapped_type>().processing_units)::value_type>);
}
}
auto Owner::preflight(const detail::ResolvedInput& input, const Limits& limits) -> std::expected<void,Error> {
    try {
        valid_limits(limits); metadata(input.view, limits);
        need(input.record && input.record->source() && input.record->source()->logical_bytes() &&
             input.record->state() == authored_source::State::resolved && input.record->plan() &&
             !input.record->upstream_error() && input.record->diagnostics().empty(), Error::source);
        const auto source_input = input.record->source()->input();
        need(source_input.availability == authored_source::Availability::available &&
             !source_input.acquisition_failure && source_input.rejected_overrides.empty(), Error::source);
        const auto usage = phase_usage(limits, input.upper_fixed,
            *input.source ? &**input.source : nullptr, *input.plan ? &**input.plan : nullptr, 0,0,0);
        if (!*input.source) bound(usage.source_preparation_bytes, limits.source_preparation_bytes);
        else if (!*input.plan) bound(usage.plan_preparation_bytes, limits.plan_preparation_bytes);
        else bound(usage.topology_preparation_bytes, limits.topology_preparation_bytes);
        return {};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::prepare(const detail::ResolvedInput& input, Limits limits) -> std::expected<Owner,Error> {
    try {
        need(*input.source && *input.plan && *input.topology, Error::child);
        auto usage = phase_usage(limits, input.upper_fixed, &**input.source, &**input.plan, 0,0,0);
        return Owner{publish(*input.source, *input.plan, *input.topology, input.view, limits, usage, input.upper_fixed, Origin::session_selected)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view body, Limits limits) -> std::expected<Owner,Error> {
    try {
        valid_limits(limits); bound(18, limits.nodes); bound(2, limits.depth);
        const auto wire = parse(body, limits); need(body.size() >= 100, Error::schema); bound(body.size() - 100, limits.text_bytes);
        std::optional<ss::Owner> source; std::optional<pp::Owner> plan; std::optional<ts::Owner> topology;
        auto usage = phase_usage(limits, 0, nullptr, nullptr, wire.lengths[0], wire.lengths[1], wire.lengths[2]);
        bound(usage.source_preparation_bytes, limits.source_preparation_bytes);
        {
            const auto scratch = unhex(wire.bodies[0]); auto result = ss::Owner::decode(scratch, limits.source);
            if (!result) throw result.error() == ss::Error::capacity ? Error::capacity : result.error() == ss::Error::invalid_limits ? Error::invalid_limits : Error::child;
            digest_matches(*result->digest(), wire.hashes[0]); source = std::move(*result);
        }
        usage = phase_usage(limits, 0, &*source, nullptr, wire.lengths[0], wire.lengths[1], wire.lengths[2]);
        bound(usage.plan_preparation_bytes, limits.plan_preparation_bytes);
        {
            const auto scratch = unhex(wire.bodies[1]); auto result = pp::Owner::decode(scratch, limits.plan);
            if (!result) throw result.error() == pp::Error::capacity ? Error::capacity : result.error() == pp::Error::invalid_limits ? Error::invalid_limits : Error::child;
            digest_matches(*result->digest(), wire.hashes[1]); plan = std::move(*result);
        }
        usage = phase_usage(limits, 0, &*source, &*plan, wire.lengths[0], wire.lengths[1], wire.lengths[2]);
        bound(usage.topology_preparation_bytes, limits.topology_preparation_bytes);
        {
            const auto scratch = unhex(wire.bodies[2]); auto result = ts::Owner::decode(scratch, limits.topology);
            if (!result) throw result.error() == ts::Error::capacity ? Error::capacity : result.error() == ts::Error::invalid_limits ? Error::invalid_limits : Error::child;
            digest_matches(*result->digest(), wire.hashes[2]); topology = std::move(*result);
        }
        usage.input_bytes = body.size();
        return Owner{publish(source, plan, topology, wire.view, limits, usage, 0, Origin::decoded_unassociated, body)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
Owner::Owner(std::unique_ptr<detail::State> state) noexcept : state_(std::move(state)) {}
Owner::~Owner() = default;
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
auto Owner::provenance() const noexcept -> const Provenance* { return state_ ? &state_->provenance : nullptr; }
auto Owner::source() const noexcept -> const ss::Owner* { return state_ && state_->source ? &*state_->source : nullptr; }
auto Owner::plan() const noexcept -> const pp::Owner* { return state_ && state_->plan ? &*state_->plan : nullptr; }
auto Owner::topology() const noexcept -> const ts::Owner* { return state_ && state_->topology ? &*state_->topology : nullptr; }
auto Owner::canonical() const noexcept -> std::string_view { return state_ ? state_->canonical : std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_ ? &state_->digest : nullptr; }
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage : Usage{}; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_ ? std::optional{state_->origin} : std::nullopt; }
} // namespace epycbench::results::normalization_serialization
