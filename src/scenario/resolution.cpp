#include "epycbench/scenario/resolution.hpp"

#include "epycbench/scenario/default_expansion.hpp"
#include "epycbench/scenario/kernel_dispatch.hpp"
#include "epycbench/scenario/matrix_expansion.hpp"
#include "epycbench/scenario/memory_placement_resolution.hpp"
#include "epycbench/scenario/service_cpu_resolution.hpp"
#include "epycbench/scenario/structural_validation.hpp"
#include "epycbench/scenario/unit_normalization.hpp"
#include "scenario/retained_plan_projection_resources.hpp"

#include <utility>
#include <array>
#include <algorithm>
#include <limits>

namespace epycbench::scenario {
namespace {

template <typename Result>
auto authored_failure(Result&& result) -> ScenarioResolutionResult {
    return {.scenario = std::nullopt, .diagnostics = std::move(result.diagnostics)};
}

struct CallerCapacity {};
auto capacity_error() -> core::Error {
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.resolution.caller_capacity"},
            .message = "Scenario exceeds caller resolution limits",
            .operation = "resolve_scenario", .os_error = std::nullopt,
            .context = nlohmann::json::object()};
}
void require_capacity(bool value) { if (!value) throw CallerCapacity{}; }
auto checked_product(std::size_t a, std::size_t b) -> std::size_t {
    require_capacity(b == 0 || a <= std::numeric_limits<std::size_t>::max() / b);
    return a * b;
}
auto checked_sum(std::size_t a, std::size_t b) -> std::size_t {
    require_capacity(b <= std::numeric_limits<std::size_t>::max() - a);
    return a + b;
}
struct TreeStock { std::size_t nodes{}, bytes{}, text{}, members{}; };
auto inspect_tree(const nlohmann::ordered_json& root, const ScenarioResolutionLimits& limits) -> TreeStock {
    using Json = nlohmann::ordered_json;
    struct Frame { const Json* parent{}; Json::const_iterator next; };
    std::array<Frame, 64> frames;
    std::size_t depth{}, nodes{}, bytes{}, text{}, members{};
    const Json* current = &root;
    auto debit = [&](std::size_t n) {
        require_capacity(n <= limits.bytes - bytes); bytes += n;
    };
    auto key = [&](const auto& iterator) {
        ++members;
        const auto length = iterator.key().size();
        text = checked_sum(text, length);
        debit(checked_sum(sizeof(Json::string_t), length));
    };
    for (;;) {
        require_capacity(nodes < limits.nodes); ++nodes; debit(sizeof(Json));
        if (current->is_string()) {
            const auto length = current->get_ref<const Json::string_t&>().size();
            text = checked_sum(text, length); debit(length);
        }
        if (current->is_structured() && !current->empty()) {
            require_capacity(depth < frames.size());
            auto next = current->begin();
            if (current->is_object()) key(next);
            const auto* child = &next.value();
            frames[depth++] = {current, ++next}; current = child; continue;
        }
        while (depth != 0 && frames[depth - 1].next == frames[depth - 1].parent->end()) --depth;
        if (depth == 0) break;
        auto& frame = frames[depth - 1];
        if (frame.parent->is_object()) key(frame.next);
        current = &frame.next.value(); ++frame.next;
    }
    const auto bound = checked_sum(checked_product(6, text),
        checked_sum(checked_product(64, nodes), checked_product(2, members)));
    require_capacity(bound <= limits.canonical_bytes);
    return {nodes, bytes, text, members};
}
void inspect_input(const AuthoredScenario& source, const ResourceResolutionContext& context,
                   const ScenarioResolutionLimits& limits) {
    require_capacity(context.topology.processing_units().size() <= limits.processing_units);
    require_capacity(context.topology.numa_nodes().size() <= limits.numa_nodes);
    (void)inspect_tree(source.value(), limits);
    const auto& root = source.value();
    const auto count = [&](std::string_view name) -> std::size_t {
        const auto found = root.find(name); return found == root.end() ? 0 : found->size();
    };
    require_capacity(count("workloads") <= limits.workloads);
    require_capacity(count("experiments") <= limits.experiments);
    if (const auto resources = root.find("resources"); resources != root.end() && resources->is_object()) {
        std::size_t selectors{};
        for (const auto name : {"worker_sets", "service_cpus"})
            if (const auto found = resources->find(name); found != resources->end())
                selectors = checked_sum(selectors, found->size());
        require_capacity(selectors <= limits.selector_sets);
    }
    std::size_t variants{};
    if (const auto experiments = root.find("experiments"); experiments != root.end() && experiments->is_array())
        for (const auto& experiment : *experiments)
            if (const auto found = experiment.find("variants"); found != experiment.end())
                variants = checked_sum(variants, found->size());
    require_capacity(variants <= limits.variants);
}

auto source_stock(const AuthoredScenario& source, const ScenarioResolutionLimits& limits)
    -> std::size_t {
    auto stock = checked_sum(sizeof(source), inspect_tree(source.value(), limits).bytes);
    stock = checked_sum(stock, source.source_name().size());
    for (const auto& entry : source.locations())
        stock = checked_sum(stock, checked_sum(sizeof(entry), entry.first.size()));
    require_capacity(stock <= limits.retained_bytes);
    return stock;
}
void admit_tree_bound(TreeStock stock, const ScenarioResolutionLimits& limits) {
    require_capacity(stock.nodes <= limits.nodes && stock.bytes <= limits.bytes);
    const auto canonical = checked_sum(checked_product(6, stock.text),
        checked_sum(checked_product(64, stock.nodes), checked_product(2, stock.members)));
    require_capacity(canonical <= limits.canonical_bytes);
}
void before_units(const AuthoredScenario& source, std::size_t live,
                  const ScenarioResolutionLimits& limits) {
    const auto input = inspect_tree(source.value(), limits);
    // normalize_field replaces one scalar by at most a four-node ratio object:
    // three keys, the unit string, and two bounded uint64 values.
    TreeStock upper{checked_product(input.nodes, 4),
        checked_sum(input.bytes, checked_product(input.nodes,
            3 * (sizeof(nlohmann::ordered_json) + sizeof(std::string)) + 64)),
        checked_sum(input.text, checked_product(input.nodes, 64)),
        checked_sum(input.members, checked_product(input.nodes, 3))};
    admit_tree_bound(upper, limits);
    require_capacity(checked_sum(live, checked_sum(input.bytes, upper.bytes)) <= limits.retained_bytes);
}
void before_defaults(const UnitNormalizedScenario& source, std::size_t live,
                     const ScenarioResolutionLimits& limits) {
    const auto input = inspect_tree(source.value(), limits);
    TreeStock declarations{};
    if (const auto found = source.value().find("defaults"); found != source.value().end())
        declarations = inspect_tree(*found, limits);
    // The closed v1 expansion has six Memory bindings per variant/matrix,
    // two selector bindings, three comparison bindings and twenty run bindings.
    // Charge a complete declarations clone at each site conservatively.
    const auto& root = source.value();
    const auto experiment_count = root.at("experiments").size();
    std::size_t variant_count{}, selectors{};
    for (const auto& experiment : root.at("experiments"))
        if (const auto found = experiment.find("variants"); found != experiment.end())
            variant_count = checked_sum(variant_count, found->size());
    if (const auto resources = root.find("resources"); resources != root.end())
        if (const auto found = resources->find("worker_sets"); found != resources->end())
            selectors = found->size();
    const auto sites = checked_sum(44, checked_sum(
        checked_product(6, checked_sum(variant_count, experiment_count)),
        checked_sum(checked_product(2, checked_sum(selectors, experiment_count)),
                    checked_product(3, experiment_count))));
    const auto added_nodes = checked_sum(checked_product(sites,
        checked_sum(declarations.nodes, 4)), 64);
    TreeStock upper{checked_sum(input.nodes, added_nodes),
        checked_sum(input.bytes, checked_sum(checked_product(sites,
            checked_sum(declarations.bytes, 4 * (sizeof(nlohmann::ordered_json) + sizeof(std::string)) + 128)),
            64 * sizeof(nlohmann::ordered_json))),
        checked_sum(input.text, checked_product(sites, checked_sum(declarations.text, 128))),
        checked_sum(input.members, added_nodes)};
    admit_tree_bound(upper, limits);
    std::size_t longest_pointer{};
    for (const auto& entry : source.structurally_valid().authored().locations())
        longest_pointer = std::max(longest_pointer, entry.first.size());
    const auto origins = checked_product(sites,
        checked_sum(sizeof(std::pair<const std::string, ValueOrigin>),
                    checked_product(2, checked_sum(longest_pointer, 256))));
    // Input tree, output/reorder cells, defaults clone and origin map coexist.
    const auto overlap = checked_sum(live, checked_sum(checked_product(2, upper.bytes),
        checked_sum(declarations.bytes, origins)));
    require_capacity(overlap <= limits.retained_bytes);
}

void before_digest(const MatrixExpandedScenario& source, std::size_t live,
                   const ScenarioResolutionLimits& limits) {
    detail::ProjectionResourceLimits projection{};
    projection.nodes = limits.nodes;
    projection.objects = limits.nodes;
    projection.arrays = limits.nodes;
    projection.object_members = limits.nodes;
    projection.string_values = limits.nodes;
    projection.key_bytes = limits.bytes;
    projection.string_bytes = limits.bytes;
    projection.depth = 64;
    projection.canonical_bytes = limits.canonical_bytes;
    projection.utf32_reserved_elements = limits.retained_bytes / sizeof(std::uint32_t);
    projection.utf16_elements = limits.retained_bytes / sizeof(std::uint16_t);
    projection.canonical_member_elements = limits.nodes;
    projection.quoted_key_bytes = limits.retained_bytes;
    projection.path_bytes = limits.retained_bytes;
    projection.index_decimal_bytes = limits.retained_bytes;
    projection.ordered_staging_bytes = limits.retained_bytes;
    projection.unordered_clone_bytes = limits.retained_bytes;
    projection.projector_auxiliary_bytes = limits.retained_bytes;
    projection.fixed_bytes = limits.retained_bytes;
    projection.error_bytes = limits.retained_bytes;
    projection.simultaneous_bytes = limits.retained_bytes;
    projection.work_units = checked_product(limits.nodes, limits.nodes);
    const auto measured = detail::measure_retained_projection_resources(
        detail::retained_plan_projection_view(source), projection, projection);
    require_capacity(measured.has_value());
    // Both canonical trees and both byte strings remain alive in the existing
    // digest owner. Charge both complete measured peaks, not their maximum.
    const auto overlap = checked_sum(live, checked_sum(
        measured->scenario.simultaneous_bytes, measured->resolved.simultaneous_bytes));
    require_capacity(overlap <= limits.retained_bytes);
}

auto before_typed_resolution(const DefaultExpandedScenario& source,
    const ResourceResolutionContext& context, std::size_t live,
    const ScenarioResolutionLimits& limits) -> std::size_t {
    const auto tree = inspect_tree(source.value(), limits);
    for (const auto& [pointer, origin] : source.origins()) {
        live = checked_sum(live, checked_sum(
            sizeof(std::pair<const std::string, ValueOrigin>), pointer.size()));
        if (origin.source_pointer) live = checked_sum(live, origin.source_pointer->size());
    }
    const auto& root = source.value();
    std::size_t selectors{}, variants{}, maximum_indices{1};
    const auto inspect_selector = [&](const auto& declaration) {
        if (const auto select = declaration.find("select"); select != declaration.end())
            if (const auto smt = select->find("smt"); smt != select->end() && smt->is_object())
                maximum_indices = std::max(maximum_indices, smt->at("indices").size());
    };
    if (const auto resources = root.find("resources"); resources != root.end()) {
        for (const auto name : {"worker_sets", "service_cpus"}) {
            const auto found = resources->find(name);
            if (found == resources->end()) continue;
            selectors = checked_sum(selectors, found->size());
            for (const auto& declaration : *found) inspect_selector(declaration);
        }
    }
    for (const auto& experiment : root.at("experiments")) {
        if (const auto found = experiment.find("variants"); found != experiment.end())
            variants = checked_sum(variants, found->size());
        if (const auto found = experiment.find("workers_per_node"); found != experiment.end())
            inspect_selector(*found);
    }
    require_capacity(selectors <= limits.selector_sets && variants <= limits.variants);
    const auto pus = context.topology.processing_units().size();
    // Explicit SMT indices can repeat before the lower semantic validator rejects
    // them. Bound their materialization too; do not assume successful uniqueness.
    const auto selected = checked_product(pus, maximum_indices);
    require_capacity(selected <= limits.workers_per_set);
    const auto associations = checked_product(selectors, selected);
    require_capacity(associations <= limits.worker_associations);
    std::size_t identity_text{};
    for (const auto& pu : context.topology.processing_units()) {
        const auto stable = context.stable_ids.processing_unit_id(pu.id);
        const auto core = context.stable_ids.core_id(pu.core_id);
        if (stable && core)
            identity_text = checked_sum(identity_text,
                checked_sum(stable->get().value().size(), core->get().value().size()));
    }
    identity_text = checked_product(identity_text, maximum_indices);
    std::size_t node_text{};
    for (const auto& node : context.topology.numa_nodes())
        if (const auto id = context.stable_ids.numa_node_id(node.id))
            node_text = checked_sum(node_text, id->get().value().size());
    const auto nodes = context.topology.numa_nodes().size();
    const auto indexes = checked_sum(pus, checked_sum(nodes,
        checked_sum(context.topology.cores().size(), checked_sum(context.topology.packages().size(),
            checked_sum(context.topology.dies().size(), context.topology.caches().size())))));
    // Named live roles: retained worker/service maps, resolve_one's worker-map
    // copy, selection materialization, and service-pool conversion. Count the
    // maximal admitted selection for each declared set before any of these calls.
    const auto worker_payload = checked_sum(checked_product(associations,
        sizeof(ResolvedProcessingUnit) + sizeof(ResolvedServiceCpu) +
        4 * sizeof(core::LinuxCpuId) + 2 * sizeof(topology::StableCoreId)),
        checked_product(selectors, checked_product(identity_text, 3)));
    const auto worker_maps = checked_sum(worker_payload, checked_product(selectors,
        sizeof(ResolvedWorkerSet) + sizeof(ResolvedServiceCpuPool) + 2 * sizeof(std::string)));
    // Selector indexes, grouping/projected/unit/bucket/output arrays and their
    // identity copies coexist. Reserve each named payload family conservatively.
    const auto selection = checked_sum(checked_product(indexes,
        sizeof(ResolvedProcessingUnit) + 4 * sizeof(void*) + sizeof(std::string)),
        checked_sum(checked_product(selected,
            sizeof(ResolvedProcessingUnit) + 8 * sizeof(void*) +
            4 * sizeof(std::string) + 4 * sizeof(core::LinuxCpuId)),
            checked_product(identity_text, 5)));
    const auto memory = checked_sum(checked_product(variants,
        checked_sum(sizeof(ComparisonVariantMemoryPlacement),
            checked_sum(checked_product(selected, sizeof(ResolvedPerWorkerMemoryNode)),
                checked_sum(checked_product(nodes, sizeof(ResolvedMemoryNode) +
                    sizeof(core::LinuxNumaNodeId) + 2 * sizeof(void*)),
                    checked_sum(identity_text, checked_product(selected, node_text)))))),
        checked_product(root.at("experiments").size(),
            sizeof(MatrixMemoryPlacementTemplate) + sizeof(MatrixWorkerSelectorTemplate) +
            sizeof(ExperimentWorkerReference)));
    // Names/dependency graph/DFS ordering, normalized service declaration and
    // selector-template copies are bounded by the actual complete input tree.
    const auto declarations = checked_product(checked_sum(selectors, 8), tree.bytes);
    const auto retained = checked_sum(tree.bytes,
        checked_sum(checked_product(worker_maps, 2), checked_sum(memory, declarations)));
    require_capacity(checked_sum(live, checked_sum(retained, selection)) <= limits.retained_bytes);
    return checked_sum(live, checked_sum(retained, selection));
}

auto resolve_impl(const ScenarioSource& source, const ResourceResolutionContext& topology,
                  const std::optional<ScenarioResolutionLimits>& limits)
    -> std::expected<ScenarioResolutionResult, core::Error> {
    if (limits) {
        require_capacity(source.contents.size() <= limits->bytes);
        require_capacity(topology.topology.processing_units().size() <= limits->processing_units);
        require_capacity(topology.topology.numa_nodes().size() <= limits->numa_nodes);
    }
    auto authored = parse_authored_scenario(source);
    if (!authored) {
        return std::unexpected{std::move(authored.error())};
    }
    std::size_t live_stock{};
    if (limits) {
        inspect_input(*authored, topology, *limits);
        live_stock = checked_sum(source.contents.size(), source_stock(*authored, *limits));
        before_units(*authored, live_stock, *limits);
    }
    auto structured = validate_scenario_structure(std::move(*authored));
    if (!structured) {
        return std::unexpected{std::move(structured.error())};
    }
    if (!structured->scenario) {
        return authored_failure(std::move(*structured));
    }
    auto normalized = normalize_scenario_units(std::move(*structured->scenario));
    if (!normalized) {
        return std::unexpected{std::move(normalized.error())};
    }
    if (!normalized->scenario) {
        return authored_failure(std::move(*normalized));
    }
    if (limits) {
        live_stock = checked_sum(live_stock, inspect_tree(normalized->scenario->value(), *limits).bytes);
        before_defaults(*normalized->scenario, live_stock, *limits);
    }
    auto defaults = expand_scenario_defaults(std::move(*normalized->scenario));
    if (!defaults) {
        return std::unexpected{std::move(defaults.error())};
    }
    if (limits) live_stock = before_typed_resolution(*defaults, topology, live_stock, *limits);
    auto resources = resolve_scenario_resources(std::move(*defaults), topology);
    if (!resources) {
        return std::unexpected{std::move(resources.error())};
    }
    if (!resources->scenario) {
        return authored_failure(std::move(*resources));
    }
    auto services = resolve_service_cpus(std::move(*resources->scenario), topology);
    if (!services) {
        return std::unexpected{std::move(services.error())};
    }
    if (!services->scenario) {
        return authored_failure(std::move(*services));
    }
    auto memory = resolve_memory_placement(std::move(*services->scenario), topology);
    if (!memory) {
        return std::unexpected{std::move(memory.error())};
    }
    if (!memory->scenario) {
        return authored_failure(std::move(*memory));
    }
    auto kernels = dispatch_kernel_parameters(std::move(*memory->scenario));
    if (!kernels) {
        return std::unexpected{std::move(kernels.error())};
    }
    if (!kernels->scenario) {
        return authored_failure(std::move(*kernels));
    }
    std::size_t expansion_bytes{};
    auto expansion_limits = limits ? limits->expansion : MatrixExpansionLimits{};
    if (limits) {
        require_capacity(live_stock <= limits->retained_bytes);
        expansion_limits.bytes = std::min(expansion_limits.bytes, limits->retained_bytes - live_stock);
    }
    auto matrices = limits
        ? expand_node_pair_matrices(std::move(*kernels->scenario), topology, expansion_limits, &expansion_bytes)
        : expand_node_pair_matrices(std::move(*kernels->scenario), topology);
    if (!matrices) {
        return std::unexpected{std::move(matrices.error())};
    }
    if (!matrices->scenario) {
        return authored_failure(std::move(*matrices));
    }
    if (limits) before_digest(*matrices->scenario, checked_sum(live_stock, expansion_bytes), *limits);
    auto digested = compute_scenario_plan_digests(std::move(*matrices->scenario));
    if (!digested) {
        return std::unexpected{std::move(digested.error())};
    }
    return ScenarioResolutionResult{.scenario = std::move(*digested), .diagnostics = {}};
}

} // namespace

auto resolve_scenario(const ScenarioSource& source, const ResourceResolutionContext& topology)
    -> std::expected<ScenarioResolutionResult, core::Error> {
    return resolve_impl(source, topology, std::nullopt);
}
auto resolve_scenario(const ScenarioSource& source, const ResourceResolutionContext& topology,
                      ScenarioResolutionLimits limits)
    -> std::expected<ScenarioResolutionResult, core::Error> {
    try { return resolve_impl(source, topology, limits); }
    catch (const CallerCapacity&) { return std::unexpected{capacity_error()}; }
}

} // namespace epycbench::scenario
