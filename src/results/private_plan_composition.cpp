#include "results/private_plan_composition.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace epycbench::results::private_plan_composition {
namespace da = document_ancestry_serialization;
namespace rs = resource_service_serialization;
namespace mp = memory_placement_serialization;
namespace kd = kernel_dispatch_serialization;
namespace ce = comparison_experiments_serialization;
namespace me = matrix_experiments_serialization;
namespace sc = scenario;
namespace detail {
struct State {
    da::Owner documents;
    rs::Owner resources;
    mp::Owner memory;
    kd::Owner kernel;
    ce::Owner comparisons;
    me::Owner matrices;
    std::string scenario_canonical, resolved_canonical, canonical;
    sc::ScenarioDigest scenario_digest;
    sc::ResolvedScenarioPlanDigest resolved_digest;
    core::Sha256Digest digest;
    Origin origin;
    Usage usage;
};
auto checked_add(std::size_t& a, std::size_t b) noexcept -> bool {
    if (b > SIZE_MAX - a) return false;
    a += b; return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (a && b > SIZE_MAX / a) return {};
    return a * b;
}
auto state_stock() noexcept -> std::size_t { return sizeof(State); }
}
namespace {
using Json = nlohmann::ordered_json;
using detail::State;
constexpr std::array parent_quantities{
    &Usage::input_bytes, &Usage::canonical_scenario_bytes, &Usage::canonical_resolved_bytes,
    &Usage::envelope_bytes, &Usage::association_entries, &Usage::association_work,
    &Usage::retained_bytes, &Usage::consumer_simultaneous_bytes};
constexpr std::array<std::string_view, 6> roles{
    "document_ancestry", "resource_service", "memory_placement", "kernel_dispatch",
    "comparison_experiments", "matrix_experiments"};
void need(bool condition, Error error = Error::reference) { if (!condition) throw error; }
auto add(std::size_t a, std::size_t b) -> std::size_t { need(detail::checked_add(a, b), Error::capacity); return a; }
auto multiply(std::size_t a, std::size_t b) -> std::size_t {
    const auto result = detail::checked_multiply(a, b); need(result.has_value(), Error::capacity); return *result;
}
void bound(std::size_t value, std::size_t limit) { need(value <= limit, Error::capacity); }
struct Decimal {
    std::array<char, 20> bytes{}; std::size_t length{};
    explicit Decimal(std::size_t value) {
        const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
        need(result.ec == std::errc{}, Error::capacity); length = static_cast<std::size_t>(result.ptr - bytes.data());
    }
    auto text() const -> std::string_view { return {bytes.data(), length}; }
};
struct Inputs {
    std::array<std::string_view, 6> children;
    std::string_view scenario, resolved, scenario_digest, resolved_digest;
    bool encoded{};
};
struct Reader {
    std::string_view input;
    std::size_t offset{};
    void literal(std::string_view value) {
        need(input.substr(offset, value.size()) == value, Error::encoding); offset += value.size();
    }
    auto child() -> std::string_view {
        const auto start = offset;
        need(offset < input.size() && input[offset] == '{', Error::schema);
        std::array<char, 65> stack{}; std::size_t depth{}; bool quoted{}, escaped{};
        do {
            need(offset < input.size(), Error::encoding); const auto byte = input[offset++];
            if (quoted) {
                if (escaped) escaped = false;
                else if (byte == '\\') escaped = true;
                else if (byte == '"') quoted = false;
            } else if (byte == '"') quoted = true;
            else if (byte == '{' || byte == '[') {
                bound(add(depth, 1), stack.size()); stack[depth++] = byte;
            } else if (byte == '}' || byte == ']') {
                need(depth && stack[depth - 1] == (byte == '}' ? '{' : '['), Error::schema); --depth;
            }
        } while (depth || quoted);
        return input.substr(start, offset - start);
    }
    auto quoted() -> std::string_view {
        literal("\""); const auto start = offset;
        while (offset < input.size() && input[offset] != '"') ++offset;
        need(offset < input.size(), Error::encoding);
        const auto value = input.substr(start, offset - start); ++offset; return value;
    }
};
auto nibble(char value) -> unsigned {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    need(value >= 'a' && value <= 'f', Error::encoding); return static_cast<unsigned>(value - 'a') + 10U;
}
void hex(std::string_view value) { need(value.size() % 2 == 0, Error::encoding); for (const char byte : value) (void)nibble(byte); }
void digest_text(std::string_view value) {
    need(value.size() == 71 && value.starts_with("sha256:"), Error::encoding); hex(value.substr(7));
}
auto scan(std::string_view input, const Limits& limits) -> Inputs {
    bound(input.size(), limits.input_bytes); bound(input.size(), limits.envelope_bytes);
    Reader reader{input}; Inputs result; result.encoded = true; reader.literal("{\"children\":[");
    for (std::size_t i = 0; i < roles.size(); ++i) {
        if (i) reader.literal(",");
        reader.literal("[\""); reader.literal(roles[i]); reader.literal("\",");
        result.children[i] = reader.child(); reader.literal("]");
    }
    reader.literal("],\"resolved_plan\":{\"canonical\":"); result.resolved = reader.quoted();
    bound(result.resolved.size() / 2, limits.canonical_resolved_bytes); hex(result.resolved);
    reader.literal(",\"digest\":"); result.resolved_digest = reader.quoted(); digest_text(result.resolved_digest);
    reader.literal("},\"scenario\":{\"canonical\":"); result.scenario = reader.quoted();
    bound(result.scenario.size() / 2, limits.canonical_scenario_bytes); hex(result.scenario);
    reader.literal(",\"digest\":"); result.scenario_digest = reader.quoted(); digest_text(result.scenario_digest);
    reader.literal("},\"schema\":\""); reader.literal(profile); reader.literal("\"}");
    need(reader.offset == input.size(), Error::encoding); return result;
}
struct Sink {
    std::string* output{}; std::size_t size{}; std::size_t limit{};
    void raw(std::string_view text) { size = add(size, text.size()); bound(size, limit); if (output) output->append(text); }
    void hexadecimal(std::string_view text) {
        bound(add(size, add(multiply(text.size(), 2), 2)), limit); raw("\"");
        for (const char octet : text) {
            const auto byte = static_cast<unsigned char>(octet);
            const std::array encoded{"0123456789abcdef"[byte >> 4U], "0123456789abcdef"[byte & 15U]};
            raw({encoded.data(), encoded.size()});
        }
        raw("\"");
    }
};
void render(Sink& sink, const Inputs& input) {
    sink.raw("{\"children\":[");
    for (std::size_t i = 0; i < roles.size(); ++i) {
        if (i) sink.raw(",");
        sink.raw("[\""); sink.raw(roles[i]); sink.raw("\","); sink.raw(input.children[i]); sink.raw("]");
    }
    sink.raw("],\"resolved_plan\":{\"canonical\":"); sink.hexadecimal(input.resolved);
    sink.raw(",\"digest\":\""); sink.raw(input.resolved_digest); sink.raw("\"},\"scenario\":{\"canonical\":");
    sink.hexadecimal(input.scenario); sink.raw(",\"digest\":\""); sink.raw(input.scenario_digest);
    sink.raw("\"},\"schema\":\""); sink.raw(profile); sink.raw("\"}");
}
auto decode_text(std::string_view input, bool encoded) -> std::string {
    if (!encoded) return std::string{input};
    std::string result(input.size() / 2, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>((nibble(input[2 * i]) << 4U) | nibble(input[2 * i + 1]));
    return result;
}
template<class T, class E> auto measured(std::expected<T, E> result) -> T {
    if (!result) {
        if (result.error() == E::capacity) throw Error::capacity;
        if (result.error() == E::state) throw Error::state;
        if (result.error() == E::encoding) throw Error::encoding;
        if (result.error() == E::schema) throw Error::schema;
        throw Error::reference;
    }
    return std::move(*result);
}
struct Facts {
    const da::Owner& documents; const rs::Owner& resources; const mp::Owner& memory;
    const kd::Owner& kernel; const ce::Owner& comparisons; const me::Owner& matrices;
};
auto projection_view(const Facts& state) -> sc::detail::RetainedPlanProjectionView {
    return {*state.documents.default_expanded(), *state.resources.worker_sets(), *state.resources.experiment_workers(),
        *state.resources.service_cpu_pools(), *state.resources.reserved_service_cpus(), *state.resources.reserved_service_cores(),
        *state.resources.has_dedicated_service_placement(), *state.memory.comparison_memory(), *state.kernel.workloads(),
        *state.kernel.experiment_workloads(), *state.comparisons.experiments(), *state.matrices.experiments(),
        *state.resources.topology_fingerprints(), state.resources.resource_profile(), state.resources.service_profile(),
        state.memory.memory_profile(), state.kernel.dispatch_profile(), state.comparisons.expansion_profile()};
}
struct ExperimentSlot {
    const sc::ResolvedComparisonExperiment* comparison{};
    const sc::ExpandedMatrixExperiment* matrix{};
    const sc::ExperimentWorkloadBinding* binding{};
    const sc::ExperimentWorkerReference* workers{};
};
struct VariantSlot {
    const sc::ComparisonVariantMemoryPlacement* memory{};
    const sc::ResolvedComparisonVariant* variant{};
};
struct WorkerIdentity {
    std::string_view pu, core; core::LinuxCpuId cpu; std::size_t experiment; bool service;
};
struct NodeIdentity {
    std::string_view stable; core::LinuxNumaNodeId linux; std::optional<core::ByteCount> page;
};
struct NameReference { std::string_view name; std::size_t experiment, ordinal; };
struct Association {
    const Facts& facts;
    const sc::detail::RetainedPlanProjectionView& view;
    const Limits& limits;
    Usage& usage;
    std::size_t work_used{};
    std::vector<ExperimentSlot> experiments;
    std::vector<VariantSlot> variants;
    std::vector<std::size_t> variant_offsets;
    std::vector<WorkerIdentity> workers;
    std::vector<NodeIdentity> nodes;
    std::vector<NameReference> names;
    std::optional<core::ByteCount> page;
    void work(std::size_t amount = 1) {
        work_used = add(work_used, amount);
        bound(work_used, usage.association_work);
    }
    static auto text(const Json& value) -> std::string_view {
        need(value.is_string()); return value.get_ref<const std::string&>();
    }
    static auto field(const Json& value, std::string_view name) -> const Json& {
        need(value.is_object()); const auto found = value.find(name); need(found != value.end()); return *found;
    }
    static auto number(const Json& value) -> std::uint64_t {
        need(value.is_number_unsigned()); return value.get<std::uint64_t>();
    }
    static auto boolean(const Json& value) -> bool { need(value.is_boolean()); return value.get<bool>(); }
    auto pointer(std::string_view value) -> const Json& {
        const Json* current = &view.effective;
        while (!value.empty()) {
            work(); need(value.front() == '/'); value.remove_prefix(1);
            const auto end = value.find('/'); const auto component = value.substr(0, end);
            work(component.size());
            if (current->is_object()) {
                const auto found = current->find(component); need(found != current->end()); current = &*found;
            } else {
                need(current->is_array() && !component.empty()); std::size_t index{};
                const auto parsed = std::from_chars(component.data(), component.data() + component.size(), index);
                need(parsed.ec == std::errc{} && parsed.ptr == component.data() + component.size());
                need(Decimal{index}.text() == component && index < current->size()); current = &(*current)[index];
            }
            if (end == std::string_view::npos) break;
            value.remove_prefix(end);
        }
        return *current;
    }
    void ratio(const Json& value, const sc::ExactRatio& expected) {
        work(); need(text(field(value, "unit")) == "ratio");
        need(number(field(value, "numerator")) == expected.numerator() &&
             number(field(value, "denominator")) == expected.denominator());
    }
    void comparison(const Json& value, const sc::MatrixComparisonConfiguration& expected, bool matrix) {
        ratio(field(value, "practical_effect"), expected.practical_effect);
        ratio(field(value, "confidence"), expected.confidence);
        const auto& order = field(value, "order"); const auto token = text(field(order, "strategy"));
        switch (expected.order) {
        case sc::MatrixComparisonOrder::randomized_abba: need(token == "randomized-abba"); break;
        case sc::MatrixComparisonOrder::randomized_baab: need(token == "randomized-baab"); break;
        case sc::MatrixComparisonOrder::randomized_balanced: need(token == "randomized-balanced"); break;
        }
        const auto seed = order.find("seed"); need((seed != order.end()) == expected.order_seed.has_value());
        if (expected.order_seed) need(number(*seed) == *expected.order_seed);
        if (matrix) need(text(field(value, "baseline")) == "local-memory");
    }
    template<class Placement> void placement_fields(const Json& value, const Placement& expected) {
        work(); need(text(field(value, "policy")) == "bind" && text(field(value, "page_mode")) == "base");
        ratio(field(value, "placement_threshold"), expected.placement_threshold);
        need(boolean(field(value, "prefault")) == expected.prefault &&
             boolean(field(value, "verify_before")) == expected.verify_before &&
             boolean(field(value, "verify_after")) == expected.verify_after);
    }
    template<class Worker> void worker(const Worker& value, std::size_t experiment, bool service) {
        work(); workers.push_back({value.stable_id.value(), value.core_id.value(), value.linux_cpu, experiment, service});
        if (!service && experiment != SIZE_MAX) {
            need(!view.reserved_service_cpus.contains(value.linux_cpu));
            for (const auto& core : view.reserved_service_cores) { work(); need(core != value.core_id); }
        }
    }
    void node(const sc::ResolvedMemoryNode& value, std::optional<core::ByteCount> page_size = {}) {
        work(); nodes.push_back({value.stable_id.value(), value.linux_node, page_size});
        if (page_size) {
            need(page_size->value() > 0);
            if (page) need(page == page_size); else page = page_size;
        }
    }
    template<class Placement> void placement_nodes(const Placement& value, std::size_t experiment) {
        if constexpr (requires { value.nodes; }) {
            for (const auto& item : value.nodes) node(item, value.page_size);
        } else {
            for (const auto& item : value.worker_nodes) {
                worker(item.worker, experiment, false); node(item.node, value.page_size);
                names.push_back({item.worker.stable_id.value(), experiment, nodes.size() - 1});
            }
        }
    }
    template<class Values> void numeric_array(const Json* source, const Values& values) {
        if (!source) { need(values.empty()); return; }
        need(source->is_array() && source->size() == values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            work(); const auto expected = [&] { if constexpr (requires { values[i].value(); }) return values[i].value(); else return values[i]; }();
            need(number((*source)[i]) == expected);
        }
    }
    static auto optional_field(const Json& value, std::string_view key) -> const Json* {
        need(value.is_object()); const auto found = value.find(key); return found == value.end() ? nullptr : &*found;
    }
    void selector(const sc::MatrixWorkerSelectorTemplate& retained) {
        const auto& source = pointer(retained.source_pointer);
        need(retained.profile == sc::resource_resolution_profile);
        need(text(field(source, "unit")) == (retained.unit == sc::SelectorUnit::core ? "core" : "pu"));
        const auto* where = optional_field(source, "where");
        numeric_array(where ? optional_field(*where, "packages") : nullptr, retained.filters.packages);
        numeric_array(where ? optional_field(*where, "numa_nodes") : nullptr, retained.filters.numa_nodes);
        numeric_array(where ? optional_field(*where, "dies") : nullptr, retained.filters.dies);
        numeric_array(where ? optional_field(*where, "cache_domains") : nullptr, retained.filters.cache_domains);
        for (const auto& [name, value] : {std::pair{"allowed", retained.filters.allowed}, std::pair{"online", retained.filters.online}}) {
            const auto* supplied = where ? optional_field(*where, name) : nullptr;
            need((supplied != nullptr) == value.has_value()); if (supplied) need(boolean(*supplied) == *value);
        }
        const auto* excluded = optional_field(source, "exclude");
        numeric_array(excluded ? optional_field(*excluded, "pus") : nullptr, retained.exclusions.processing_units);
        numeric_array(excluded ? optional_field(*excluded, "cores") : nullptr, retained.exclusions.cores);
        const auto* excluded_names = excluded ? optional_field(*excluded, "worker_sets") : nullptr;
        if (!excluded_names) need(retained.exclusions.worker_sets.empty());
        else {
            need(excluded_names->is_array() && excluded_names->size() == retained.exclusions.worker_sets.size());
            for (std::size_t i = 0; i < excluded_names->size(); ++i) {
                work(); need(text((*excluded_names)[i]) == retained.exclusions.worker_sets[i]);
                need(view.worker_sets.contains(retained.exclusions.worker_sets[i]));
            }
        }
        const auto& smt = field(source, "smt");
        if (retained.smt == sc::SelectorSmtMode::explicit_indices) numeric_array(&field(smt, "indices"), retained.smt_indices);
        else need(text(smt) == (retained.smt == sc::SelectorSmtMode::all ? "all" : "one-per-core") && retained.smt_indices.empty());
        const auto* count = optional_field(source, "count");
        const bool numeric = count && count->is_number_unsigned(); need(numeric == retained.count.has_value());
        if (numeric) need(number(*count) == *retained.count); else if (count) need(text(*count) == "all");
        constexpr std::array orderings{"ascending-pu", "descending-pu", "compact-cache", "spread-cache", "compact-numa", "spread-numa", "random"};
        const auto order = static_cast<std::size_t>(retained.ordering); need(order < orderings.size());
        need(text(field(source, "ordering")) == orderings[order]);
    }
    void ordinary(const sc::ResolvedComparisonExperiment& retained, const ExperimentSlot& slot, const Json& intent, std::size_t ordinal) {
        need(slot.workers && !slot.matrix && retained.workload_id == slot.binding->workload_id);
        need(text(field(intent, "workers")) == slot.workers->worker_set_name);
        need(text(pointer(slot.workers->source_pointer)) == slot.workers->worker_set_name);
        const auto found = view.worker_sets.find(slot.workers->worker_set_name); need(found != view.worker_sets.end());
        need(retained.workers == found->second.processing_units && retained.linux_cpus == found->second.linux_cpus);
        for (const auto& item : retained.workers) worker(item, retained.experiment_index, false);
        const auto& intents = field(intent, "variants"); need(intents.size() == retained.variants.size());
        const auto& configuration = field(intent, "compare"); comparison(configuration, retained.comparison, false);
        need(text(field(configuration, "baseline")) == retained.baseline_variant_id);
        const auto& candidates = field(configuration, "candidates"); need(candidates.size() == retained.candidate_variant_ids.size());
        for (std::size_t i = 0; i < candidates.size(); ++i) { work(); need(text(candidates[i]) == retained.candidate_variant_ids[i]); }
        for (std::size_t i = 0; i < retained.variants.size(); ++i) {
            work(); const auto& variant = retained.variants[i];
            need(variant.variant_index == i && variant.variant_id == text(field(intents[i], "id")));
            const auto offset = variant_offsets[ordinal]; need(offset + i < variants.size());
            auto& joined = variants[offset + i]; need(!joined.variant); joined.variant = &variant;
            need(joined.memory && joined.memory->placement == variant.memory_placement);
            std::visit([&](const auto& placed) {
                need(&pointer(placed.source_pointer) == &field(intents[i], "memory"));
                placement_fields(field(intents[i], "memory"), placed);
                placement_nodes(placed, retained.experiment_index);
                if constexpr (requires { placed.worker_nodes; }) {
                    const auto& intended_nodes = field(field(intents[i], "memory"), "nodes");
                    need(intended_nodes.is_object() && boolean(field(intended_nodes, "local-to-each-worker")));
                    need(placed.worker_nodes.size() == retained.workers.size());
                    for (std::size_t w = 0; w < placed.worker_nodes.size(); ++w) { work(); need(placed.worker_nodes[w].worker == retained.workers[w]); }
                } else {
                    const auto& intended_nodes = field(field(intents[i], "memory"), "nodes");
                    if (intended_nodes.is_array()) {
                        need(intended_nodes.size() == placed.nodes.size());
                        for (std::size_t n = 0; n < placed.nodes.size(); ++n) { work(); need(number(intended_nodes[n]) == placed.nodes[n].linux_node.value()); }
                    } else need(intended_nodes.is_object() && boolean(field(intended_nodes, "local-to-workers")));
                }
            }, variant.memory_placement);
        }
        const auto effect = configuration.find("practical_effect"); need(effect != configuration.end());
        bool origin_found = false;
        for (const auto& [path, origin] : *facts.documents.origins()) {
            work(); const Decimal index{retained.experiment_index};
            constexpr std::string_view prefix = "/experiments/", suffix = "/compare/practical_effect";
            if (path.size() == prefix.size() + index.length + suffix.size() && path.starts_with(prefix) &&
                std::string_view{path}.substr(prefix.size(), index.length) == index.text() && path.ends_with(suffix)) {
                need(!origin_found); origin_found = true; need(origin == retained.practical_effect_origin);
            }
        }
        need(origin_found);
        if (retained.practical_effect_origin.kind == sc::ValueOriginKind::schema_default)
            need(!retained.practical_effect_origin.source_pointer && retained.comparison.practical_effect.numerator() == 1 && retained.comparison.practical_effect.denominator() == 100);
    }
    void matrix(const sc::ExpandedMatrixExperiment& retained, const ExperimentSlot& slot, const Json& intent) {
        need(!slot.workers && !slot.comparison);
        comparison(field(intent, "comparison"), retained.comparison, true);
        const auto& templates = *facts.memory.matrix_memory_templates();
        work(templates.size());
        const auto found = std::ranges::find(templates, retained.experiment_index, &sc::MatrixMemoryPlacementTemplate::experiment_index);
        const bool authored = intent.contains("memory"); need((found != templates.end()) == authored);
        if (authored) {
            need(&pointer(found->source_pointer) == &field(intent, "memory"));
            placement_fields(field(intent, "memory"), *found); need(found->profile == sc::memory_placement_resolution_profile);
        }
        for (const auto& item : retained.worker_axis) node(item);
        for (const auto& item : retained.memory_axis) node(item);
        for (const auto& row : retained.worker_rows) for (const auto& item : row.workers) worker(item, retained.experiment_index, false);
        for (const auto& cell : retained.cells) {
            work(); const auto& placed = cell.memory_placement;
            work(); nodes.push_back({cell.memory_node_stable_id.value(), cell.memory_node, placed.page_size});
            placement_nodes(placed, retained.experiment_index);
            if (authored) {
                need(placed.policy == found->policy && placed.page_mode == found->page_mode && placed.page_size == found->page_size &&
                     placed.placement_threshold == found->placement_threshold && placed.prefault == found->prefault &&
                     placed.verify_before == found->verify_before && placed.verify_after == found->verify_after &&
                     placed.source_pointer == found->source_pointer);
            } else {
                need(placed.policy == sc::MemoryPolicy::bind && placed.page_mode == sc::PageMode::base &&
                     placed.placement_threshold.numerator() == 999 && placed.placement_threshold.denominator() == 1000 &&
                     placed.prefault && placed.verify_before && placed.verify_after && placed.source_pointer.empty());
            }
        }
    }
    void constraints(const ExperimentSlot& slot, const Json& intent) {
        const auto& binding = *slot.binding; work();
        need(text(pointer(binding.source_pointer)) == binding.workload_id &&
             text(field(intent, "workload")) == binding.workload_id);
        const auto found = view.workloads.find(binding.workload_id); need(found != view.workloads.end());
        const bool latency = found->second.kernel_id == kernel::KernelId::dependent_load_latency_v1;
        const auto working_set = std::visit([](const auto& value) {
            if constexpr (requires { value.working_set(); }) return value.working_set();
            else return value.working_set_per_worker();
        }, found->second.configuration);
        std::array<sc::DeferredKernelConstraint, 2> expected{}; std::size_t count{};
        if (slot.matrix) {
            if (!intent.contains("memory")) expected[count++] = sc::DeferredKernelConstraint::working_set_at_least_resolved_page_size;
            if (latency) expected[count++] = sc::DeferredKernelConstraint::exactly_one_worker;
            for (const auto& row : slot.matrix->worker_rows) { work(); need(!latency || row.workers.size() == 1); }
            for (const auto& cell : slot.matrix->cells) { work(); need(working_set >= cell.memory_placement.page_size); }
        } else {
            need(slot.comparison); need(!latency || slot.comparison->workers.size() == 1);
            for (const auto& variant : slot.comparison->variants) {
                work(); std::visit([&](const auto& placed) { need(working_set >= placed.page_size); }, variant.memory_placement);
            }
        }
        need(binding.deferred_constraints.size() == count);
        for (std::size_t i = 0; i < count; ++i) need(binding.deferred_constraints[i] == expected[i]);
    }
    template<class T, class Less> void sort(std::vector<T>& values, Less less) {
        // Explicit heapsort gives a finite comparison envelope independent of a
        // standard-library sort implementation. Every callback consumes work.
        const auto sift = [&](std::size_t root, std::size_t size) {
            while (root < size / 2) {
                auto child = root * 2 + 1;
                if (child + 1 < size && less(values[child], values[child + 1])) ++child;
                if (!less(values[root], values[child])) break;
                std::swap(values[root], values[child]); root = child;
            }
        };
        for (std::size_t i = values.size() / 2; i > 0; --i) sift(i - 1, values.size());
        for (std::size_t size = values.size(); size > 1; --size) {
            std::swap(values[0], values[size - 1]); sift(0, size - 1);
        }
    }
    void repeated_identities() {
        sort(names, [&](const NameReference& a, const NameReference& b) { work(); return a.name < b.name; });
        for (std::size_t i = 1; i < names.size(); ++i) {
            work(); if (names[i - 1].name == names[i].name) {
                const auto& a = nodes[names[i - 1].ordinal]; const auto& b = nodes[names[i].ordinal];
                need(a.stable == b.stable && a.linux == b.linux);
            }
        }
        const auto by_cpu = [&](const WorkerIdentity& a, const WorkerIdentity& b) { work(); return a.cpu < b.cpu; };
        sort(workers, by_cpu);
        for (std::size_t i = 1; i < workers.size(); ++i) { work(); if (workers[i - 1].cpu == workers[i].cpu)
            need(workers[i - 1].pu == workers[i].pu && workers[i - 1].core == workers[i].core); }
        const auto by_pu = [&](const WorkerIdentity& a, const WorkerIdentity& b) { work(); return a.pu < b.pu; };
        sort(workers, by_pu);
        for (std::size_t i = 1; i < workers.size(); ++i) { work(); if (workers[i - 1].pu == workers[i].pu)
            need(workers[i - 1].cpu == workers[i].cpu && workers[i - 1].core == workers[i].core); }
        sort(nodes, [&](const NodeIdentity& a, const NodeIdentity& b) { work(); return a.linux < b.linux; });
        for (std::size_t i = 1; i < nodes.size(); ++i) { work(); if (nodes[i - 1].linux == nodes[i].linux) need(nodes[i - 1].stable == nodes[i].stable); }
        sort(nodes, [&](const NodeIdentity& a, const NodeIdentity& b) { work(); return a.stable < b.stable; });
        for (std::size_t i = 1; i < nodes.size(); ++i) { work(); if (nodes[i - 1].stable == nodes[i].stable) need(nodes[i - 1].linux == nodes[i].linux); }
    }
    void run() {
        const auto& intents = field(view.effective, "experiments"); need(intents.is_array());
        need(experiments.size() == intents.size());
        for (const auto& binding : view.experiment_workloads) {
            work(); need(binding.experiment_index < experiments.size()); auto& slot = experiments[binding.experiment_index]; need(!slot.binding); slot.binding = &binding;
        }
        for (const auto& value : view.comparison_experiments) {
            work(); need(value.experiment_index < experiments.size()); auto& slot = experiments[value.experiment_index]; need(!slot.comparison && !slot.matrix); slot.comparison = &value;
        }
        for (const auto& value : view.matrix_experiments) {
            work(); need(value.experiment_index < experiments.size()); auto& slot = experiments[value.experiment_index]; need(!slot.comparison && !slot.matrix); slot.matrix = &value;
        }
        for (const auto& value : view.experiment_workers) {
            work(); need(value.experiment_index < experiments.size()); auto& slot = experiments[value.experiment_index]; need(!slot.workers && slot.comparison); slot.workers = &value;
        }
        std::size_t ordinal{}, offset{};
        for (std::size_t i = 0; i < experiments.size(); ++i) {
            const auto& slot = experiments[i]; need(slot.binding && (slot.comparison || slot.matrix));
            need(text(field(intents[i], "type")) == (slot.comparison ? "comparison" : "node-pair-matrix"));
            need(text(field(intents[i], "id")) == (slot.comparison ? slot.comparison->experiment_id : slot.matrix->experiment_id));
            if (slot.comparison) {
                need(ordinal < view.comparison_experiments.size() && &view.comparison_experiments[ordinal] == slot.comparison);
                variant_offsets[ordinal++] = offset; offset = add(offset, slot.comparison->variants.size());
            }
        }
        variant_offsets[ordinal] = offset; need(offset == variants.size());
        ordinal = 0;
        for (const auto& value : view.matrix_experiments) {
            (void)value; need(ordinal < facts.resources.matrix_worker_templates()->size());
            const auto& retained = (*facts.resources.matrix_worker_templates())[ordinal++];
            need(retained.experiment_index == value.experiment_index); selector(retained);
        }
        need(ordinal == facts.resources.matrix_worker_templates()->size());
        // Both child vectors retain source order. Their merge is linear.
        std::size_t comparison_index{}, variant_index{};
        for (const auto& memory : view.comparison_memory) {
            work(); need(comparison_index < view.comparison_experiments.size());
            const auto& comparison_value = view.comparison_experiments[comparison_index];
            need(memory.experiment_index == comparison_value.experiment_index && memory.variant_index == variant_index);
            const auto position = variant_offsets[comparison_index] + variant_index;
            need(position < variants.size() && !variants[position].memory); variants[position].memory = &memory;
            if (++variant_index == comparison_value.variants.size()) { ++comparison_index; variant_index = 0; }
        }
        need(comparison_index == view.comparison_experiments.size() && variant_index == 0);
        const auto& declarations = field(view.effective, "workloads"); need(declarations.size() == view.workloads.size());
        for (const auto& [name, workload] : view.workloads) {
            work(); need(&pointer(workload.source_pointer) == &field(declarations, name));
            need(text(field(field(declarations, name), "kernel")) == workload.kernel_token);
        }
        const auto* resources = optional_field(view.effective, "resources");
        const auto* worker_declarations = resources ? optional_field(*resources, "worker_sets") : nullptr;
        need((worker_declarations ? worker_declarations->size() : 0) == view.worker_sets.size());
        for (const auto& [name, set] : view.worker_sets) {
            work(); need(worker_declarations && &pointer(set.source_pointer) == &field(*worker_declarations, name));
            for (const auto& value : set.processing_units) worker(value, SIZE_MAX, false);
        }
        const auto* service_declarations = resources ? optional_field(*resources, "service_cpus") : nullptr;
        need((service_declarations ? service_declarations->size() : 0) == view.service_cpu_pools.size());
        for (const auto& [name, pool] : view.service_cpu_pools) {
            work(); need(service_declarations && &pointer(pool.source_pointer) == &field(*service_declarations, name));
            for (const auto& value : pool.processing_units) worker(value, SIZE_MAX, true);
        }
        ordinal = 0;
        for (std::size_t i = 0; i < experiments.size(); ++i) {
            work(); const auto& slot = experiments[i];
            if (slot.comparison) ordinary(*slot.comparison, slot, intents[i], ordinal++);
            else matrix(*slot.matrix, slot, intents[i]);
            constraints(slot, intents[i]);
        }
        std::size_t authored_memory{};
        for (const auto& retained : view.matrix_experiments) if (intents[retained.experiment_index].contains("memory")) ++authored_memory;
        need(authored_memory == facts.memory.matrix_memory_templates()->size());
        repeated_identities();
    }
};
struct AssociationSize {
    std::size_t experiments{}, variants{}, offsets{}, workers{}, nodes{}, names{}, bytes{};
};
auto association_size(const Facts& facts, const sc::detail::RetainedPlanProjectionView& view) -> AssociationSize {
    AssociationSize size{view.effective.at("experiments").size(), 0, add(view.comparison_experiments.size(), 1)};
    for (const auto& [name, set] : view.worker_sets) { (void)name; size.workers = add(size.workers, set.processing_units.size()); }
    for (const auto& [name, pool] : view.service_cpu_pools) { (void)name; size.workers = add(size.workers, pool.processing_units.size()); }
    for (const auto& comparison : view.comparison_experiments) {
        size.workers = add(size.workers, comparison.workers.size()); size.variants = add(size.variants, comparison.variants.size());
        for (const auto& variant : comparison.variants) std::visit([&](const auto& placement) {
            if constexpr (requires { placement.nodes; }) size.nodes = add(size.nodes, placement.nodes.size());
            else { size.workers = add(size.workers, placement.worker_nodes.size()); size.nodes = add(size.nodes, placement.worker_nodes.size()); size.names = add(size.names, placement.worker_nodes.size()); }
        }, variant.memory_placement);
    }
    for (const auto& matrix : view.matrix_experiments) {
        size.nodes = add(size.nodes, add(matrix.worker_axis.size(), matrix.memory_axis.size()));
        for (const auto& row : matrix.worker_rows) size.workers = add(size.workers, row.workers.size());
        for (const auto& cell : matrix.cells) size.nodes = add(size.nodes, add(1, cell.memory_placement.nodes.size()));
    }
    (void)facts;
    size.bytes = add(multiply(size.experiments, sizeof(ExperimentSlot)), multiply(size.variants, sizeof(VariantSlot)));
    size.bytes = add(size.bytes, multiply(size.offsets, sizeof(std::size_t)));
    size.bytes = add(size.bytes, multiply(size.workers, sizeof(WorkerIdentity)));
    size.bytes = add(size.bytes, multiply(size.nodes, sizeof(NodeIdentity)));
    size.bytes = add(size.bytes, multiply(size.names, sizeof(NameReference)));
    return size;
}
auto sort_work(std::size_t size) -> std::size_t {
    std::size_t height{};
    for (auto width = size; width > 1; width /= 2) ++height;
    return multiply(multiply(2, add(size / 2, size)), height);
}
auto association_work(const AssociationSize& size, const Usage& usage, const Facts& facts,
                      const sc::detail::RetainedPlanProjectionView& view) -> std::size_t {
    // Named conservative operation union: source/fact scans and projector linear
    // searches, ancestry-origin searches, generated pointer traversal, service
    // exclusion, five heapsorts and their adjacent consistency joins.
    auto text = add(usage.document_ancestry.metadata_octets, usage.resource_service.text_bytes);
    text = add(text, add(usage.memory_placement.text_bytes, usage.kernel_dispatch.text_bytes));
    text = add(text, add(usage.comparison_experiments.text_bytes, usage.matrix_experiments.text_bytes));
    auto work = usage.projection_input.pairwise_work;
    work = add(work, multiply(view.comparison_experiments.size(),
        add(facts.documents.origins()->size(), usage.document_ancestry.metadata_octets)));
    work = add(work, multiply(text, add(usage.projection_input.json_nodes, 1)));
    work = add(work, multiply(size.workers, view.reserved_service_cores.size()));
    work = add(work, add(multiply(2, sort_work(size.workers)), add(multiply(2, sort_work(size.nodes)), sort_work(size.names))));
    return add(work, add(multiply(2, size.workers), add(multiply(2, size.nodes), size.names)));
}
struct FramingCells {
    const sc::ScenarioDigestedPlan *capture_plan, *assemble_plan;
    Limits entry_limits;
    std::string_view decode_input, scan_input, scan_quoted_return, child_return;
    Reader *literal_receiver, *child_receiver, *quoted_receiver;
    std::string_view literal_argument;
    std::size_t child_start, child_depth, quoted_start, child_index;
    std::array<char, 65> delimiter_stack;
    bool quoted, escaped;
    char byte, nibble_argument;
    unsigned nibble_return;
    Sink *raw_receiver, *hex_receiver;
    std::string_view raw_argument, hex_argument, decode_text_argument;
    std::array<char, 2> encoded;
    unsigned char hex_byte;
    const char *hex_begin, *hex_end;
    bool decode_encoded;
    std::string decoded_return;
    std::size_t decode_index;
    std::size_t add_left, add_right, multiply_left, multiply_right;
    std::size_t* checked_add_receiver;
    std::optional<std::size_t> product_return;
    const Usage* envelope_usage;
    const Inputs* render_input;
    Sink* render_sink;
    const Limits* phase_limits;
    Usage* phase_usage;
    std::size_t phase_workspace, phase_total;
    std::string max_size_receiver;
};
// Final digest rendering and SHA operate after the projection trees retire.
// They have their own admitted phase rather than borrowing a prior phase's peak.
struct EnvelopeCells {
    std::array<std::uint32_t, 8> state, working;
    std::array<std::uint32_t, 64> schedule;
    std::array<std::byte, 128> final_blocks;
    std::array<std::byte, 32> result;
    std::array<std::uint32_t, 8>* compress_state;
    const std::byte* compress_block;
    std::uint32_t schedule_s0, schedule_s1, round_sum0, round_sum1, choose, majority, temporary1, temporary2;
    std::size_t offset, schedule_index, schedule_offset, round_index, remainder, final_size, length_index, word, byte_index;
    std::uint64_t bit_length;
    std::span<const std::byte> bytes;
    std::string_view text, hex_digits;
    core::Sha256Digest returned;
    const core::Sha256Digest* hex_receiver;
    const std::byte *hex_begin, *hex_end;
    std::byte hex_byte;
    std::uint8_t hex_value;
    std::string hex, typed_result;
    const sc::ScenarioDigest* scenario_receiver;
    const sc::ResolvedScenarioPlanDigest* resolved_receiver;
};
auto envelope_workspace() -> std::size_t { return sizeof(EnvelopeCells) + 2 * (64 + 71); }
struct AssociationCells {
    const Facts* facts;
    const sc::detail::RetainedPlanProjectionView* view;
    const Limits* limits;
    Usage* usage;
    Association *receiver, *work_receiver, *callback_capture, *forwarding_receiver, *const_receiver;
    const Json *value, *source, *current, *intent, *intents, *order, *seed, *where, *excluded, *excluded_names,
        *smt, *count, *variant_intents, *configuration, *candidates, *intended_nodes, *declarations,
        *resources, *worker_declarations, *service_declarations;
    Json::const_iterator found, end, effect;
    std::string_view text, field_name, pointer_text, component, token, prefix, suffix;
    std::size_t amount, end_offset, index, ordinal, offset, variant, worker_index, node_index, authored_memory,
        comparison_index, variant_index, position, heap_root, heap_size, heap_child, heap_index, sort_height, sort_width;
    std::from_chars_result parsed;
    Decimal index_decimal;
    const sc::ExactRatio* ratio;
    const sc::MatrixComparisonConfiguration* comparison_configuration;
    const sc::MatrixWorkerSelectorTemplate* selector;
    const sc::MatrixMemoryPlacementTemplate* memory_template;
    const sc::ResolvedComparisonExperiment* comparison;
    const sc::ResolvedComparisonVariant* comparison_variant;
    const sc::ExpandedMatrixExperiment* matrix;
    const sc::ResolvedMatrixCell* cell;
    const sc::ResolvedMemoryPlacement* placement;
    const sc::ResolvedPerWorkerMemoryPlacement* per_worker;
    const sc::ResolvedPerWorkerMemoryNode* mapping;
    const sc::ResolvedProcessingUnit* worker;
    const sc::ResolvedServiceCpu* service_worker;
    const sc::ResolvedMemoryNode* node;
    const sc::ExperimentWorkloadBinding* binding;
    const ExperimentSlot* slot;
    VariantSlot* variant_slot;
    const WorkerIdentity *worker_left, *worker_right;
    const NodeIdentity *node_left, *node_right;
    const NameReference *name_left, *name_right;
    WorkerIdentity worker_swap;
    NodeIdentity node_swap;
    NameReference name_swap;
    std::array<sc::DeferredKernelConstraint, 2> expected_constraints;
    std::array<std::pair<const char*, std::optional<bool>>, 2> optional_filters;
    std::array<const char*, 7> orderings;
    std::optional<core::ByteCount> page;
    core::ByteCount working_set;
    bool service, matrix_kind, authored, latency, numeric, origin_found;
    std::vector<ExperimentSlot>::iterator experiment_begin, experiment_end;
    std::vector<VariantSlot>::iterator variant_begin, variant_end;
    std::vector<WorkerIdentity>::iterator workers_begin, workers_end;
    std::vector<NodeIdentity>::iterator nodes_begin, nodes_end;
    std::vector<NameReference>::iterator names_begin, names_end;
    std::vector<sc::ExperimentWorkloadBinding>::const_iterator bindings_begin, bindings_end;
    std::vector<sc::ExperimentWorkerReference>::const_iterator references_begin, references_end;
    std::vector<sc::ComparisonVariantMemoryPlacement>::const_iterator memory_begin, memory_end;
    std::vector<sc::ResolvedComparisonExperiment>::const_iterator comparisons_begin, comparisons_end;
    std::vector<sc::ResolvedComparisonVariant>::const_iterator variants_begin, variants_end;
    std::vector<sc::ExpandedMatrixExperiment>::const_iterator matrices_begin, matrices_end;
    std::vector<sc::MatrixWorkerRow>::const_iterator rows_begin, rows_end;
    std::vector<sc::ResolvedMatrixCell>::const_iterator cells_begin, cells_end;
    std::vector<sc::ResolvedMemoryNode>::const_iterator axis_begin, axis_end;
    std::vector<sc::ResolvedPerWorkerMemoryNode>::const_iterator mapping_begin, mapping_end;
    std::vector<sc::MatrixMemoryPlacementTemplate>::const_iterator template_found, template_end;
    std::vector<topology::StableCoreId>::const_iterator core_begin, core_end;
    std::map<std::string, sc::DispatchedWorkload>::const_iterator workload_found, workload_end;
    std::map<std::string, sc::ResolvedWorkerSet>::const_iterator workers_found, sets_end;
    std::map<std::string, sc::ResolvedServiceCpuPool>::const_iterator pools_begin, pools_end;
    da::OriginMap::const_iterator origins_begin, origins_end;
    const da::OriginMap::value_type* origin_entry;
};
struct ParentOperationCells {
    const sc::ScenarioDigestedPlan* plan;
    const Limits* limits_reference;
    Limits limits;
    Usage usage;
    Inputs inputs, rendered_inputs;
    Reader reader;
    Sink measuring, rendering;
    Decimal decimal;
    std::array<std::string_view, 6> child_spans;
    std::array<std::size_t, 6> retained, simultaneous, handles, fixed;
    std::expected<da::Usage, da::Error> document_probe;
    std::expected<rs::Usage, rs::Error> resource_probe;
    std::expected<mp::Usage, mp::Error> memory_probe;
    std::expected<kd::Usage, kd::Error> kernel_probe;
    std::expected<ce::Usage, ce::Error> comparison_probe;
    std::expected<me::Usage, me::Error> matrix_probe;
    da::Owner documents; rs::Owner resources; mp::Owner memory; kd::Owner kernel; ce::Owner comparisons; me::Owner matrices;
    std::expected<da::Owner, da::Error> document_result;
    std::expected<rs::Owner, rs::Error> resource_result;
    std::expected<mp::Owner, mp::Error> memory_result;
    std::expected<kd::Owner, kd::Error> kernel_result;
    std::expected<ce::Owner, ce::Error> comparison_result;
    std::expected<me::Owner, me::Error> matrix_result;
    std::string scenario, resolved, canonical, scenario_digest_text, resolved_digest_text;
    std::expected<sc::ScenarioDigest, core::Error> scenario_digest;
    std::expected<sc::ResolvedScenarioPlanDigest, core::Error> resolved_digest;
    core::detail::ExpectedErrorCleanup<decltype(scenario_digest)> scenario_digest_cleanup;
    core::detail::ExpectedErrorCleanup<decltype(resolved_digest)> resolved_digest_cleanup;
    Facts facts;
    sc::detail::RetainedPlanProjectionView view;
    std::expected<sc::detail::ProjectionInputUsage, sc::detail::ProjectionInputFailure> inspection;
    std::expected<sc::detail::ProjectionResources, sc::detail::ProjectionResourceFailure> projection_resources;
    std::expected<Json, core::Error> projection;
    nlohmann::json unordered;
    core::detail::JsonCleanup<nlohmann::json> unordered_cleanup;
    core::detail::JsonPointerCleanup<Json> projected_cleanup;
    std::expected<std::string, core::Error> reproduced;
    core::detail::ExpectedErrorCleanup<decltype(projection)> projection_error_cleanup;
    core::detail::ExpectedErrorCleanup<decltype(reproduced)> canonical_error_cleanup;
    core::Sha256Digest hash, artifact_hash;
    Association association;
    AssociationSize association_size;
    std::unique_ptr<State> state, constructor_argument;
    Owner converted_owner;
    std::expected<Owner, Error> result;
    std::unexpected<Error> rejection, capacity_rejection;
    Error caught;
    const std::length_error* length_error;
    std::size_t base, prefix, child, phase, envelope_length;
};
void parent_limits(const Limits& limits) {
    for (const auto member : parent_quantities) need(limits.*member <= sc::detail::projection_input_max, Error::invalid_limits);
    need(limits.document_ancestry.tree_depth <= 64, Error::invalid_limits);
    for (const auto value : {limits.projection_input.json_nodes, limits.projection_input.typed_entries,
        limits.projection_input.string_bytes, limits.projection_input.cpu_cardinality, limits.projection_input.pairwise_work})
        need(value <= sc::detail::projection_input_max, Error::invalid_limits);
    need(limits.projection_input.depth <= 64 &&
        sc::detail::projection_resource_detail::valid_limits(limits.scenario_projection) &&
        sc::detail::projection_resource_detail::valid_limits(limits.resolved_projection), Error::invalid_limits);
    bound(detail::fixed_stock(), limits.consumer_simultaneous_bytes);
}
auto envelope_length(const Usage& usage) -> std::size_t {
    constexpr std::string_view digest = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    Inputs empty; empty.scenario_digest = digest; empty.resolved_digest = digest;
    Sink fixed{nullptr, 0, SIZE_MAX}; render(fixed, empty);
    auto total = add(fixed.size, multiply(add(usage.canonical_scenario_bytes, usage.canonical_resolved_bytes), 2));
    for (const auto bytes : {usage.document_ancestry.canonical_bytes, usage.resource_service.canonical_bytes,
        usage.memory_placement.canonical_bytes, usage.kernel_dispatch.canonical_bytes,
        usage.comparison_experiments.canonical_bytes, usage.matrix_experiments.canonical_bytes}) total = add(total, bytes);
    return total;
}
void phase(Usage& usage, const Limits& limits, std::size_t workspace) {
    const auto total = add(detail::fixed_stock(), add(usage.retained_bytes, workspace));
    usage.consumer_simultaneous_bytes = std::max(usage.consumer_simultaneous_bytes, total);
    bound(usage.consumer_simultaneous_bytes, limits.consumer_simultaneous_bytes);
}
void child_phases(Usage& usage, const Limits& limits, bool decoding) {
    usage.envelope_bytes = envelope_length(usage); bound(usage.envelope_bytes, limits.envelope_bytes);
    bound(usage.envelope_bytes, UINT64_MAX / 8);
    bound(usage.envelope_bytes, std::string{}.max_size());
    const auto base = add(sizeof(Owner) + sizeof(State), add(usage.envelope_bytes,
        add(usage.canonical_scenario_bytes, usage.canonical_resolved_bytes)));
    const std::array retained{usage.document_ancestry.retained_bytes, usage.resource_service.retained_bytes,
        usage.memory_placement.retained_bytes, usage.kernel_dispatch.retained_bytes,
        usage.comparison_experiments.retained_bytes, usage.matrix_experiments.retained_bytes};
    const std::array simultaneous{usage.document_ancestry.consumer_simultaneous_bytes, usage.resource_service.consumer_simultaneous_bytes,
        usage.memory_placement.consumer_simultaneous_bytes, usage.kernel_dispatch.consumer_simultaneous_bytes,
        usage.comparison_experiments.consumer_simultaneous_bytes, usage.matrix_experiments.consumer_simultaneous_bytes};
    const std::array handles{sizeof(da::Owner), sizeof(rs::Owner), sizeof(mp::Owner), sizeof(kd::Owner), sizeof(ce::Owner), sizeof(me::Owner)};
    const std::array fixed{
        decoding ? da::detail::decode_fixed_stock() : da::detail::capture_fixed_stock(),
        decoding ? rs::detail::decode_fixed_stock() : rs::detail::capture_fixed_stock(),
        decoding ? mp::detail::decode_fixed_stock() : mp::detail::capture_fixed_stock(),
        decoding ? kd::detail::decode_fixed_stock() : kd::detail::capture_fixed_stock(),
        decoding ? ce::detail::decode_fixed_stock() : ce::detail::capture_fixed_stock(),
        decoding ? me::detail::decode_fixed_stock() : me::detail::capture_fixed_stock()};
    auto prefix = base;
    usage.consumer_simultaneous_bytes = add(detail::fixed_stock(), add(base, *std::max_element(fixed.begin(), fixed.end())));
    for (std::size_t i = 0; i < retained.size(); ++i) {
        need(retained[i] >= handles[i], Error::capacity);
        usage.consumer_simultaneous_bytes = std::max(usage.consumer_simultaneous_bytes,
            add(detail::fixed_stock(), add(prefix, simultaneous[i])));
        prefix = add(prefix, retained[i] - handles[i]);
    }
    usage.retained_bytes = prefix;
    bound(usage.retained_bytes, limits.retained_bytes);
    bound(usage.consumer_simultaneous_bytes, limits.consumer_simultaneous_bytes);
}
void compare_projection(std::expected<Json, core::Error> projected,
                        std::string_view original, const core::Sha256Digest& digest) {
    core::detail::ExpectedErrorCleanup error_cleanup{projected};
    core::detail::JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
    need(projected.has_value(), Error::projection);
    auto unordered = core::detail::clone_json_as<nlohmann::json>(*projected);
    core::detail::JsonCleanup unordered_cleanup{unordered};
    auto canonical = core::canonicalize_json(unordered);
    core::detail::ExpectedErrorCleanup canonical_error_cleanup{canonical};
    need(canonical.has_value(), Error::projection);
    need(*canonical == original && core::sha256(*canonical) == digest, Error::projection);
}
auto assemble(const sc::ScenarioDigestedPlan* plan, const Inputs& input, std::string_view supplied, Limits limits)
    -> std::unique_ptr<State> {
    parent_limits(limits);
    const auto probe_fixed = std::max({
        plan ? da::detail::capture_fixed_stock() : da::detail::decode_fixed_stock(),
        plan ? rs::detail::capture_fixed_stock() : rs::detail::decode_fixed_stock(),
        plan ? mp::detail::capture_fixed_stock() : mp::detail::decode_fixed_stock(),
        plan ? kd::detail::capture_fixed_stock() : kd::detail::decode_fixed_stock(),
        plan ? ce::detail::capture_fixed_stock() : ce::detail::decode_fixed_stock(),
        plan ? me::detail::capture_fixed_stock() : me::detail::decode_fixed_stock()});
    bound(add(detail::fixed_stock(), probe_fixed), limits.consumer_simultaneous_bytes);
    Usage usage; usage.input_bytes = supplied.size(); bound(usage.input_bytes, limits.input_bytes);
    usage.canonical_scenario_bytes = input.scenario.size() / (input.encoded ? 2 : 1);
    usage.canonical_resolved_bytes = input.resolved.size() / (input.encoded ? 2 : 1);
    bound(usage.canonical_scenario_bytes, limits.canonical_scenario_bytes);
    bound(usage.canonical_resolved_bytes, limits.canonical_resolved_bytes);
    bound(usage.canonical_scenario_bytes, std::string{}.max_size()); bound(usage.canonical_resolved_bytes, std::string{}.max_size());
    bound(usage.canonical_scenario_bytes, UINT64_MAX / 8); bound(usage.canonical_resolved_bytes, UINT64_MAX / 8);
    if (plan) {
        usage.document_ancestry = measured(da::measure_capture(*plan, limits.document_ancestry));
        usage.resource_service = measured(rs::measure_capture(*plan, limits.resource_service));
        usage.memory_placement = measured(mp::measure_capture(*plan, limits.memory_placement));
        usage.kernel_dispatch = measured(kd::measure_capture(*plan, limits.kernel_dispatch));
        usage.comparison_experiments = measured(ce::measure_capture(*plan, limits.comparison_experiments));
        usage.matrix_experiments = measured(me::measure_capture(*plan, limits.matrix_experiments));
    } else {
        usage.document_ancestry = measured(da::measure_decode(input.children[0], limits.document_ancestry));
        usage.resource_service = measured(rs::measure_decode(input.children[1], limits.resource_service));
        usage.memory_placement = measured(mp::measure_decode(input.children[2], limits.memory_placement));
        usage.kernel_dispatch = measured(kd::measure_decode(input.children[3], limits.kernel_dispatch));
        usage.comparison_experiments = measured(ce::measure_decode(input.children[4], limits.comparison_experiments));
        usage.matrix_experiments = measured(me::measure_decode(input.children[5], limits.matrix_experiments));
    }
    child_phases(usage, limits, !plan);
    auto scenario = decode_text(input.scenario, input.encoded), resolved = decode_text(input.resolved, input.encoded);
    auto documents = plan ? measured(da::Owner::capture(*plan, limits.document_ancestry)) : measured(da::Owner::decode(input.children[0], limits.document_ancestry));
    auto resources = plan ? measured(rs::Owner::capture(*plan, limits.resource_service)) : measured(rs::Owner::decode(input.children[1], limits.resource_service));
    auto memory = plan ? measured(mp::Owner::capture(*plan, limits.memory_placement)) : measured(mp::Owner::decode(input.children[2], limits.memory_placement));
    auto kernel = plan ? measured(kd::Owner::capture(*plan, limits.kernel_dispatch)) : measured(kd::Owner::decode(input.children[3], limits.kernel_dispatch));
    auto comparisons = plan ? measured(ce::Owner::capture(*plan, limits.comparison_experiments)) : measured(ce::Owner::decode(input.children[4], limits.comparison_experiments));
    auto matrices = plan ? measured(me::Owner::capture(*plan, limits.matrix_experiments)) : measured(me::Owner::decode(input.children[5], limits.matrix_experiments));
    need(documents.usage() == usage.document_ancestry && resources.usage() == usage.resource_service && memory.usage() == usage.memory_placement &&
        kernel.usage() == usage.kernel_dispatch && comparisons.usage() == usage.comparison_experiments && matrices.usage() == usage.matrix_experiments, Error::capacity);
    const Facts facts{documents, resources, memory, kernel, comparisons, matrices};
    const auto view = projection_view(facts);
    phase(usage, limits, sc::detail::projection_resource_detail::walk_fixed_stock());
    const auto inspected = sc::detail::inspect_retained_plan_projection(view, limits.projection_input);
    if (!inspected) throw inspected.error() == sc::detail::ProjectionInputFailure::capacity ? Error::capacity :
        inspected.error() == sc::detail::ProjectionInputFailure::invalid_limits ? Error::invalid_limits : Error::reference;
    usage.projection_input = *inspected;
    {
        const auto size = association_size(facts, view);
        usage.association_entries = add(add(size.experiments, size.variants), add(size.offsets, add(size.workers, add(size.nodes, size.names))));
        bound(usage.association_entries, limits.association_entries);
        usage.association_work = association_work(size, usage, facts, view); bound(usage.association_work, limits.association_work);
        phase(usage, limits, size.bytes);
        Association association{facts, view, limits, usage, 0, {}, {}, {}, {}, {}, {}, {}};
        bound(size.experiments, association.experiments.max_size()); bound(size.variants, association.variants.max_size());
        bound(size.offsets, association.variant_offsets.max_size()); bound(size.workers, association.workers.max_size());
        bound(size.nodes, association.nodes.max_size()); bound(size.names, association.names.max_size());
        association.experiments.resize(size.experiments); association.variants.resize(size.variants);
        association.variant_offsets.resize(size.offsets); association.workers.reserve(size.workers);
        association.nodes.reserve(size.nodes); association.names.reserve(size.names);
        association.run(); need(association.workers.size() == size.workers && association.nodes.size() == size.nodes, Error::capacity);
    }
    const auto projection_resources = sc::detail::measure_retained_projection_resources(view, limits.scenario_projection, limits.resolved_projection);
    if (!projection_resources) throw projection_resources.error() == sc::detail::ProjectionResourceFailure::capacity ? Error::capacity :
        projection_resources.error() == sc::detail::ProjectionResourceFailure::invalid_limits ? Error::invalid_limits : Error::projection;
    usage.scenario_projection = projection_resources->scenario; usage.resolved_projection = projection_resources->resolved;
    auto scenario_digest = plan ? std::expected<sc::ScenarioDigest, core::Error>{plan->scenario_digest()} : sc::ScenarioDigest::parse(input.scenario_digest);
    core::detail::ExpectedErrorCleanup scenario_digest_cleanup{scenario_digest}; need(scenario_digest.has_value(), Error::encoding);
    auto resolved_digest = plan ? std::expected<sc::ResolvedScenarioPlanDigest, core::Error>{plan->resolved_scenario_plan_digest()} : sc::ResolvedScenarioPlanDigest::parse(input.resolved_digest);
    core::detail::ExpectedErrorCleanup resolved_digest_cleanup{resolved_digest}; need(resolved_digest.has_value(), Error::encoding);
    phase(usage, limits, usage.scenario_projection.simultaneous_bytes);
    compare_projection(sc::detail::retained_scenario_semantic_json(view, limits.projection_input), scenario, scenario_digest->digest());
    phase(usage, limits, usage.resolved_projection.simultaneous_bytes);
    compare_projection(sc::detail::retained_resolved_plan_semantic_json(view, *scenario_digest, limits.projection_input), resolved, resolved_digest->digest());
    phase(usage, limits, envelope_workspace());
    const auto scenario_text = scenario_digest->text(), resolved_text = resolved_digest->text();
    Inputs rendered{{documents.canonical(), resources.canonical(), memory.canonical(), kernel.canonical(), comparisons.canonical(), matrices.canonical()},
        scenario, resolved, scenario_text, resolved_text, false};
    std::string canonical; canonical.reserve(usage.envelope_bytes); Sink sink{&canonical, 0, usage.envelope_bytes}; render(sink, rendered);
    need(sink.size == usage.envelope_bytes && (plan || canonical == supplied), Error::encoding);
    const auto digest = core::sha256(canonical);
    return std::unique_ptr<State>{new State{std::move(documents), std::move(resources), std::move(memory), std::move(kernel),
        std::move(comparisons), std::move(matrices), std::move(scenario), std::move(resolved), std::move(canonical),
        *scenario_digest, *resolved_digest, digest, plan ? Origin::plan_captured : Origin::decoded_unassociated, usage}};
}
} // namespace
auto detail::fixed_stock() noexcept -> std::size_t { return sizeof(ParentOperationCells) + sizeof(FramingCells) + sizeof(AssociationCells); }
Owner::Owner(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&& other) noexcept -> Owner& { if (this != &other) state_ = std::move(other.state_); return *this; }
Owner::~Owner() = default;
auto Owner::capture(const sc::ScenarioDigestedPlan& plan, Limits limits) -> std::expected<Owner, Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(), Error::state);
        Inputs input; input.scenario = plan.canonical_scenario_json(); input.resolved = plan.canonical_resolved_scenario_plan_json();
        return Owner{assemble(&plan, input, {}, limits)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view input, Limits limits) -> std::expected<Owner, Error> {
    try { parent_limits(limits); return Owner{assemble(nullptr, scan(input, limits), input, limits)}; }
    catch (Error error) { return std::unexpected{error}; }
    catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::document_ancestry() const noexcept -> const da::Owner* { return state_ ? &state_->documents : nullptr; }
auto Owner::resource_service() const noexcept -> const rs::Owner* { return state_ ? &state_->resources : nullptr; }
auto Owner::memory_placement() const noexcept -> const mp::Owner* { return state_ ? &state_->memory : nullptr; }
auto Owner::kernel_dispatch() const noexcept -> const kd::Owner* { return state_ ? &state_->kernel : nullptr; }
auto Owner::comparison_experiments() const noexcept -> const ce::Owner* { return state_ ? &state_->comparisons : nullptr; }
auto Owner::matrix_experiments() const noexcept -> const me::Owner* { return state_ ? &state_->matrices : nullptr; }
auto Owner::canonical_scenario() const noexcept -> std::string_view { return state_ ? std::string_view{state_->scenario_canonical} : std::string_view{}; }
auto Owner::canonical_resolved_plan() const noexcept -> std::string_view { return state_ ? std::string_view{state_->resolved_canonical} : std::string_view{}; }
auto Owner::scenario_digest() const noexcept -> const sc::ScenarioDigest* { return state_ ? &state_->scenario_digest : nullptr; }
auto Owner::resolved_digest() const noexcept -> const sc::ResolvedScenarioPlanDigest* { return state_ ? &state_->resolved_digest : nullptr; }
auto Owner::canonical() const noexcept -> std::string_view { return state_ ? std::string_view{state_->canonical} : std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_ ? &state_->digest : nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_ ? std::optional{state_->origin} : std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage : Usage{}; }
} // namespace epycbench::results::private_plan_composition
