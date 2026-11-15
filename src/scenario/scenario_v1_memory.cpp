#include "scenario/scenario_v1_memory.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

auto location_for(const ServiceCpuResolvedScenario& scenario, const std::string& pointer)
    -> std::optional<SourceLocation> {
    const auto& locations = scenario.resource_resolved()
                                .default_expanded()
                                .unit_normalized()
                                .structurally_valid()
                                .authored()
                                .locations();
    if (const auto found = locations.find(pointer); found != locations.end())
        return found->second.value;
    return std::nullopt;
}

class Diagnostics {
  public:
    explicit Diagnostics(const ServiceCpuResolvedScenario& scenario) : scenario_{scenario} {}

    void add(std::string code, std::string pointer, std::string classification,
             std::map<std::string, std::string> context = {}) {
        if (capped_) return;
        if (values_.size() == 63U) {
            values_.push_back({core::StableErrorCode{"scenario.memory.diagnostic_limit"}, "",
                               std::nullopt, {{"classification", "diagnostic_limit"}}});
            capped_ = true;
            return;
        }
        context.insert_or_assign("classification", std::move(classification));
        values_.push_back({core::StableErrorCode{std::move(code)}, pointer,
                           location_for(scenario_, pointer), std::move(context)});
    }

    [[nodiscard]] auto capped() const noexcept -> bool { return capped_; }
    [[nodiscard]] auto take() noexcept -> std::vector<ScenarioDiagnostic> {
        return std::move(values_);
    }

  private:
    const ServiceCpuResolvedScenario& scenario_;
    std::vector<ScenarioDiagnostic> values_;
    bool capped_{};
};

struct NodeView {
    const topology::NumaNode* node{};
    topology::StableNumaNodeId stable_id;
    core::LinuxNumaNodeId linux_node;
};

struct CommonMemory {
    ExactRatio threshold;
    bool prefault{};
    bool verify_before{};
    bool verify_after{};
};

class Resolver {
  public:
    Resolver(const ServiceCpuResolvedScenario& scenario, const ResourceResolutionContext& context,
             core::ByteCount base_page_size)
        : scenario_{scenario}, context_{context}, base_page_size_{base_page_size},
          diagnostics_{scenario} {}

    auto run() -> std::expected<MemoryPlacementPayload, core::Error> {
        auto indexed = build_indexes();
        if (!indexed) return std::unexpected{std::move(indexed.error())};
        const auto& experiments =
            scenario_.resource_resolved().default_expanded().value().at("experiments");
        for (std::size_t experiment_index = 0;
             experiment_index < experiments.size() && !diagnostics_.capped(); ++experiment_index) {
            const auto& experiment = experiments.at(experiment_index);
            if (core::detail::json_string_equal(experiment.at("type"), "comparison")) {
                auto resolved = resolve_comparison(experiment, experiment_index);
                if (!resolved) return std::unexpected{std::move(resolved.error())};
            } else {
                auto compiled = compile_matrix(experiment, experiment_index);
                if (!compiled) return std::unexpected{std::move(compiled.error())};
            }
        }
        return MemoryPlacementPayload{.comparison = std::move(comparison_),
                                      .matrix_templates = std::move(matrix_templates_),
                                      .diagnostics = diagnostics_.take()};
    }

  private:
    auto build_indexes() -> std::expected<void, core::Error> {
        for (const auto& node : context_.topology.numa_nodes()) {
            if (node.linux_node_id.availability() != topology::Availability::available)
                continue;
            const auto linux_id = *node.linux_node_id.value();
            const auto stable = context_.stable_ids.numa_node_id(node.id);
            if (!stable)
                return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
            NodeView view{&node, stable->get(), core::LinuxNumaNodeId{linux_id}};
            if (!nodes_by_linux_.emplace(linux_id, std::move(view)).second)
                return std::unexpected{memory_internal_error("invalid_topology")};
        }
        return {};
    }

    auto common_memory(const Json& memory) -> std::expected<CommonMemory, core::Error> {
        try {
            if (!core::detail::json_string_equal(memory.at("policy"), "bind") ||
                !core::detail::json_string_equal(memory.at("page_mode"), "base"))
                return std::unexpected{memory_internal_error("normalized_memory_invariant")};
            const auto& ratio = memory.at("placement_threshold");
            if (!core::detail::json_string_equal(ratio.at("unit"), "ratio"))
                return std::unexpected{memory_internal_error("normalized_ratio_invariant")};
            auto threshold = ExactRatio::create(ratio.at("numerator").get<std::uint64_t>(),
                                                ratio.at("denominator").get<std::uint64_t>());
            if (!threshold)
                return std::unexpected{memory_internal_error("normalized_ratio_invariant")};
            return CommonMemory{std::move(*threshold), memory.at("prefault").get<bool>(),
                                memory.at("verify_before").get<bool>(),
                                memory.at("verify_after").get<bool>()};
        } catch (const std::exception&) {
            return std::unexpected{memory_internal_error("normalized_memory_shape")};
        }
    }

    auto experiment_worker_set(std::size_t experiment_index)
        -> std::expected<const ResolvedWorkerSet*, core::Error> {
        const auto& resources = scenario_.resource_resolved();
        const auto reference = std::ranges::find_if(
            resources.experiment_workers(), [experiment_index](const auto& value) {
                return value.experiment_index == experiment_index;
            });
        if (reference == resources.experiment_workers().end())
            return std::unexpected{memory_internal_error("worker_reference_missing")};
        const auto found = resources.worker_sets().find(reference->worker_set_name);
        if (found == resources.worker_sets().end())
            return std::unexpected{memory_internal_error("worker_set_missing")};
        return &found->second;
    }

    auto resolve_comparison(const Json& experiment, std::size_t experiment_index)
        -> std::expected<void, core::Error> {
        auto workers = experiment_worker_set(experiment_index);
        if (!workers) return std::unexpected{std::move(workers.error())};
        const auto& variants = experiment.at("variants");
        for (std::size_t variant_index = 0;
             variant_index < variants.size() && !diagnostics_.capped(); ++variant_index) {
            const auto pointer = "/experiments/" + std::to_string(experiment_index) +
                                 "/variants/" + std::to_string(variant_index) + "/memory";
            auto resolved = resolve_memory(variants.at(variant_index).at("memory"), pointer,
                                           **workers, experiment_index, variant_index);
            if (!resolved) return std::unexpected{std::move(resolved.error())};
        }
        return {};
    }

    auto eligible(const NodeView& view, const std::string& pointer,
                  std::optional<std::size_t> node_index) -> bool {
        const auto add_context = [node_index]() {
            std::map<std::string, std::string> context;
            if (node_index) context.emplace("node_array_index", std::to_string(*node_index));
            return context;
        };
        if (!context_.topology.machine().effective_nodes.value()->contains(
                core::NumaNodeId{view.linux_node.value()})) {
            diagnostics_.add("scenario.memory.node_not_effective", pointer,
                             "node_not_effective", add_context());
            return false;
        }
        if (view.node->installed_memory.availability() != topology::Availability::available) {
            diagnostics_.add("scenario.memory.unavailable_attribute", pointer,
                             "installed_memory_unavailable", add_context());
            return false;
        }
        if (view.node->installed_memory.value()->value() == 0U) {
            diagnostics_.add("scenario.memory.node_not_effective", pointer,
                             "installed_memory_zero", add_context());
            return false;
        }
        return true;
    }

    auto explicit_nodes(const Json& nodes, const std::string& pointer)
        -> std::vector<const NodeView*> {
        std::vector<const NodeView*> result;
        std::set<std::uint32_t> seen;
        bool failed = false;
        for (std::size_t index = 0; index < nodes.size() && !diagnostics_.capped(); ++index) {
            const auto item_pointer = pointer + "/nodes/" + std::to_string(index);
            const auto linux_id = nodes.at(index).get<std::uint32_t>();
            if (!seen.insert(linux_id).second) {
                diagnostics_.add("scenario.memory.invalid_node", item_pointer, "duplicate_node",
                                 {{"node_array_index", std::to_string(index)}});
                failed = true;
                continue;
            }
            const auto found = nodes_by_linux_.find(linux_id);
            if (found == nodes_by_linux_.end()) {
                diagnostics_.add("scenario.memory.invalid_node", item_pointer, "unknown_node",
                                 {{"node_array_index", std::to_string(index)}});
                failed = true;
                continue;
            }
            if (!eligible(found->second, item_pointer, index)) {
                failed = true;
                continue;
            }
            result.push_back(&found->second);
        }
        if (failed) result.clear();
        return result;
    }

    auto local_nodes(const ResolvedWorkerSet& workers, const std::string& pointer)
        -> std::expected<std::vector<const NodeView*>, core::Error> {
        std::vector<const NodeView*> result;
        std::set<std::uint32_t> seen;
        bool failed = false;
        for (const auto& worker : workers.processing_units) {
            const auto handle = context_.stable_ids.processing_unit(worker.stable_id);
            if (!handle)
                return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
            const auto memberships = context_.topology.numa_nodes_for_pu(*handle);
            if (memberships.size() > 1U)
                return std::unexpected{memory_internal_error("invalid_topology")};
            if (memberships.empty()) {
                diagnostics_.add("scenario.memory.unavailable_attribute",
                                 pointer + "/nodes/local-to-workers",
                                 "worker_numa_membership_unavailable");
                failed = true;
                continue;
            }
            const auto node = context_.topology.numa_node(memberships.front());
            if (!node || node->linux_node_id.availability() != topology::Availability::available)
                return std::unexpected{memory_internal_error("invalid_topology")};
            const auto found = nodes_by_linux_.find(*node->linux_node_id.value());
            if (found == nodes_by_linux_.end())
                return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
            if (!eligible(found->second, pointer + "/nodes/local-to-workers", std::nullopt)) {
                failed = true;
                continue;
            }
            if (seen.insert(found->first).second) result.push_back(&found->second);
        }
        if (failed) result.clear();
        if (result.empty() && !failed)
            diagnostics_.add("scenario.memory.empty_selection", pointer + "/nodes",
                             "local_to_workers_empty");
        return result;
    }

    auto per_worker_nodes(const ResolvedWorkerSet& workers, const std::string& pointer)
        -> std::expected<std::vector<ResolvedPerWorkerMemoryNode>, core::Error> {
        std::vector<ResolvedPerWorkerMemoryNode> result;
        bool failed = false;
        const auto member_pointer = pointer + "/nodes/local-to-each-worker";
        for (const auto& worker : workers.processing_units) {
            const auto handle = context_.stable_ids.processing_unit(worker.stable_id);
            if (!handle)
                return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
            const auto memberships = context_.topology.numa_nodes_for_pu(*handle);
            if (memberships.size() > 1U)
                return std::unexpected{memory_internal_error("invalid_topology")};
            if (memberships.empty()) {
                diagnostics_.add("scenario.memory.unavailable_attribute", member_pointer,
                                 "worker_numa_membership_unavailable");
                failed = true;
                continue;
            }
            const auto node = context_.topology.numa_node(memberships.front());
            if (!node || node->linux_node_id.availability() != topology::Availability::available)
                return std::unexpected{memory_internal_error("invalid_topology")};
            const auto found = nodes_by_linux_.find(*node->linux_node_id.value());
            if (found == nodes_by_linux_.end())
                return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
            if (!eligible(found->second, member_pointer, std::nullopt)) {
                failed = true;
                continue;
            }
            result.push_back({worker, {found->second.stable_id, found->second.linux_node}});
        }
        if (failed) result.clear();
        if (result.empty() && !failed)
            diagnostics_.add("scenario.memory.empty_selection", pointer + "/nodes",
                             "local_to_each_worker_empty");
        return result;
    }

    auto validate_pages(const std::vector<const NodeView*>& nodes, const std::string& pointer)
        -> bool {
        bool valid = true;
        for (const auto* view : nodes) {
            if (view->node->page_sizes.empty()) {
                diagnostics_.add("scenario.memory.unavailable_attribute", pointer + "/page_mode",
                                 "page_size_evidence_unavailable");
                valid = false;
            } else if (!std::ranges::contains(view->node->page_sizes, base_page_size_)) {
                diagnostics_.add("scenario.memory.page_size_unavailable",
                                 pointer + "/page_mode", "base_page_size_unavailable");
                valid = false;
            }
        }
        return valid;
    }

    auto resolve_memory(const Json& memory, const std::string& pointer,
                        const ResolvedWorkerSet& workers, std::size_t experiment_index,
                        std::size_t variant_index) -> std::expected<void, core::Error> {
        auto common = common_memory(memory);
        if (!common) return std::unexpected{std::move(common.error())};
        if (memory.at("nodes").is_object() &&
            memory.at("nodes").contains("local-to-each-worker")) {
            auto mapping = per_worker_nodes(workers, pointer);
            if (!mapping) return std::unexpected{std::move(mapping.error())};
            if (mapping->empty()) return {};
            std::vector<const NodeView*> page_nodes;
            page_nodes.reserve(mapping->size());
            for (const auto& entry : *mapping) {
                const auto found = nodes_by_linux_.find(entry.node.linux_node.value());
                if (found == nodes_by_linux_.end())
                    return std::unexpected{memory_internal_error("stable_id_index_incomplete")};
                page_nodes.push_back(&found->second);
            }
            if (!validate_pages(page_nodes, pointer)) return {};
            comparison_.push_back(
                {experiment_index, variant_index,
                 ResolvedPerWorkerMemoryPlacement{
                     MemoryPolicy::bind, std::move(*mapping), PageMode::base, base_page_size_,
                     std::move(common->threshold), common->prefault, common->verify_before,
                     common->verify_after, pointer}});
            return {};
        }
        std::vector<const NodeView*> nodes;
        if (memory.at("nodes").is_array()) {
            nodes = explicit_nodes(memory.at("nodes"), pointer);
        } else {
            auto local = local_nodes(workers, pointer);
            if (!local) return std::unexpected{std::move(local.error())};
            nodes = std::move(*local);
        }
        if (nodes.empty()) return {};
        if (!validate_pages(nodes, pointer)) return {};

        std::vector<ResolvedMemoryNode> records;
        std::set<core::LinuxNumaNodeId> membership;
        for (const auto* node : nodes) {
            records.push_back({node->stable_id, node->linux_node});
            membership.insert(node->linux_node);
        }
        comparison_.push_back(
            {experiment_index, variant_index,
             ResolvedMemoryPlacement{MemoryPolicy::bind, std::move(records),
                                     std::move(membership), PageMode::base, base_page_size_,
                                     std::move(common->threshold), common->prefault,
                                     common->verify_before, common->verify_after, pointer}});
        return {};
    }

    auto compile_matrix(const Json& experiment, std::size_t experiment_index)
        -> std::expected<void, core::Error> {
        if (!experiment.contains("memory")) return {};
        const auto pointer = "/experiments/" + std::to_string(experiment_index) + "/memory";
        auto common = common_memory(experiment.at("memory"));
        if (!common) return std::unexpected{std::move(common.error())};
        matrix_templates_.push_back(
            {experiment_index, MemoryPolicy::bind, PageMode::base, base_page_size_,
             std::move(common->threshold), common->prefault, common->verify_before,
             common->verify_after, pointer, std::string{memory_placement_resolution_profile}});
        return {};
    }

    const ServiceCpuResolvedScenario& scenario_;
    const ResourceResolutionContext& context_;
    core::ByteCount base_page_size_;
    Diagnostics diagnostics_;
    std::map<std::uint32_t, NodeView> nodes_by_linux_;
    std::vector<ComparisonVariantMemoryPlacement> comparison_;
    std::vector<MatrixMemoryPlacementTemplate> matrix_templates_;
};

} // namespace

auto memory_internal_error(std::string classification, std::string underlying) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!underlying.empty()) context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.memory.internal_failure"},
            .message = "Memory placement resolution failed internally",
            .operation = "resolve_memory_placement",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto resolve_v1_memory_placement(const ServiceCpuResolvedScenario& scenario,
                                 const ResourceResolutionContext& context,
                                 core::ByteCount base_page_size)
    -> std::expected<MemoryPlacementPayload, core::Error> {
    try {
        return Resolver{scenario, context, base_page_size}.run();
    } catch (const std::exception&) {
        return std::unexpected{memory_internal_error("normalized_memory_shape")};
    } catch (...) {
        return std::unexpected{memory_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario::detail
