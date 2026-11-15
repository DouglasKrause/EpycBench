#include "scenario/scenario_v1_matrix.hpp"
#include "core/json_ownership.hpp"

#include "scenario/resource_selector.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace epycbench::scenario::detail {
namespace {

using Json = nlohmann::ordered_json;

struct NodeView {
    const topology::NumaNode* node{};
    topology::StableNumaNodeId stable_id;
    core::LinuxNumaNodeId linux_node;
};

auto default_expanded(const KernelDispatchedScenario& scenario) -> const DefaultExpandedScenario& {
    return scenario.memory_placement_resolved()
        .service_cpu_resolved()
        .resource_resolved()
        .default_expanded();
}

auto location_for(const KernelDispatchedScenario& scenario, const std::string& pointer)
    -> std::optional<SourceLocation> {
    const auto& locations =
        default_expanded(scenario).unit_normalized().structurally_valid().authored().locations();
    if (const auto found = locations.find(pointer); found != locations.end())
        return found->second.value;
    return std::nullopt;
}

class Diagnostics {
  public:
    explicit Diagnostics(const KernelDispatchedScenario& scenario) : scenario_{scenario} {}

    void add(std::string code, std::string pointer, std::string classification,
             std::map<std::string, std::string> context = {},
             std::optional<SourceLocation> location = std::nullopt) {
        if (capped_)
            return;
        if (values_.size() == 63U) {
            values_.push_back({core::StableErrorCode{"scenario.matrix.diagnostic_limit"},
                               "",
                               std::nullopt,
                               {{"classification", "diagnostic_limit"}}});
            capped_ = true;
            return;
        }
        context.insert_or_assign("classification", std::move(classification));
        values_.push_back({core::StableErrorCode{std::move(code)}, pointer,
                           location ? std::move(location) : location_for(scenario_, pointer),
                           std::move(context)});
    }

    [[nodiscard]] auto capped() const noexcept -> bool { return capped_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return values_.size(); }
    [[nodiscard]] auto take() noexcept -> std::vector<ScenarioDiagnostic> {
        return std::move(values_);
    }

  private:
    const KernelDispatchedScenario& scenario_;
    std::vector<ScenarioDiagnostic> values_;
    bool capped_{};
};

auto ratio(const Json& value) -> std::expected<ExactRatio, core::Error> {
    try {
        if (!core::detail::json_string_equal(value.at("unit"), "ratio"))
            return std::unexpected{matrix_internal_error("normalized_ratio_invariant")};
        auto result = ExactRatio::create(value.at("numerator").get<std::uint64_t>(),
                                         value.at("denominator").get<std::uint64_t>());
        if (!result)
            return std::unexpected{matrix_internal_error("normalized_ratio_invariant")};
        return result;
    } catch (const std::exception&) {
        return std::unexpected{matrix_internal_error("normalized_ratio_shape")};
    }
}

auto order(const Json& value) -> std::optional<MatrixComparisonOrder> {
    if (core::detail::json_string_equal(value, "randomized-abba"))
        return MatrixComparisonOrder::randomized_abba;
    if (core::detail::json_string_equal(value, "randomized-baab"))
        return MatrixComparisonOrder::randomized_baab;
    if (core::detail::json_string_equal(value, "randomized-balanced"))
        return MatrixComparisonOrder::randomized_balanced;
    return std::nullopt;
}

struct ExpansionCapacity {};
class ExpansionBudget {
  public:
    explicit ExpansionBudget(std::optional<MatrixExpansionLimits> limits) : limits_{limits} {}
    void take(std::size_t MatrixExpansionLimits::* field, std::size_t value) {
        if (!limits_) return;
        if (value > (*limits_).*field - used_.*field) throw ExpansionCapacity{};
        used_.*field += value;
    }
    void bytes(std::size_t count, std::size_t width) {
        if (!limits_) return;
        if (width != 0 && count > (limits_->bytes - used_.bytes) / width)
            throw ExpansionCapacity{};
        take(&MatrixExpansionLimits::bytes, count * width);
    }
    void text(std::string_view value) { bytes(value.size(), 1); }
    auto charged_bytes() const noexcept -> std::size_t { return used_.bytes; }
    void worker(const ResolvedProcessingUnit& value) {
        bytes(1, sizeof(value)); text(value.stable_id.value()); text(value.core_id.value());
    }
    void memory(const ComparisonMemoryPlacement& value) {
        std::visit([&](const auto& placement) {
            text(placement.source_pointer);
            if constexpr (requires { placement.nodes; }) {
                bytes(placement.nodes.size(), sizeof(ResolvedMemoryNode));
                for (const auto& node : placement.nodes) text(node.stable_id.value());
                bytes(placement.linux_nodes.size(), sizeof(core::LinuxNumaNodeId));
            } else {
                bytes(placement.worker_nodes.size(), sizeof(placement.worker_nodes.front()));
                for (const auto& node : placement.worker_nodes) {
                    worker(node.worker); text(node.node.stable_id.value());
                }
            }
        }, value);
    }
  private:
    std::optional<MatrixExpansionLimits> limits_;
    MatrixExpansionLimits used_{};
};

class Expander {
  public:
    Expander(const KernelDispatchedScenario& scenario, const ResourceResolutionContext& context,
             core::ByteCount base_page_size,
             std::optional<MatrixExpansionLimits> limits = std::nullopt)
        : scenario_{scenario}, context_{context}, base_page_size_{base_page_size},
          diagnostics_{scenario}, budget_{limits} {}

    auto charged_bytes() const noexcept -> std::size_t { return budget_.charged_bytes(); }

    auto run() -> std::expected<MatrixExpansionPayload, core::Error> {
        auto indexed = build_indexes();
        if (!indexed)
            return std::unexpected{std::move(indexed.error())};
        const auto& experiments = default_expanded(scenario_).value().at("experiments");
        for (std::size_t index = 0; index < experiments.size() && !diagnostics_.capped(); ++index) {
            const auto& experiment = experiments.at(index);
            auto expanded =
                core::detail::json_string_equal(experiment.at("type"), "node-pair-matrix")
                    ? expand(experiment, index)
                    : retain_comparison(experiment, index);
            if (!expanded)
                return std::unexpected{std::move(expanded.error())};
        }
        return MatrixExpansionPayload{std::move(experiments_), std::move(comparisons_),
                                      diagnostics_.take()};
    }

  private:
    auto build_indexes() -> std::expected<void, core::Error> {
        for (const auto& node : context_.topology.numa_nodes()) {
            if (node.linux_node_id.availability() != topology::Availability::available ||
                !node.linux_node_id.value())
                continue;
            const auto stable = context_.stable_ids.numa_node_id(node.id);
            if (!stable)
                return std::unexpected{matrix_internal_error("stable_id_index_incomplete")};
            budget_.bytes(1, sizeof(std::pair<const std::uint32_t, NodeView>));
            budget_.text(stable->get().value());
            NodeView view{&node, stable->get(), core::LinuxNumaNodeId{*node.linux_node_id.value()}};
            if (!nodes_.emplace(view.linux_node.value(), std::move(view)).second)
                return std::unexpected{matrix_internal_error("invalid_topology")};
        }
        return {};
    }

    auto worker_axis(std::size_t experiment_index) -> std::vector<const NodeView*> {
        budget_.bytes(nodes_.size(), sizeof(const NodeView*));
        std::vector<const NodeView*> result;
        const auto& machine = context_.topology.machine();
        for (const auto& [unused, view] : nodes_) {
            static_cast<void>(unused);
            if (!machine.effective_nodes.value()->contains(
                    core::NumaNodeId{view.linux_node.value()}))
                continue;
            const auto capable = std::ranges::any_of(view.node->member_pus, [&](const auto pu_id) {
                const auto pu = context_.topology.processing_unit(pu_id);
                return pu && machine.effective_cpus.value()->contains(pu->linux_cpu_id);
            });
            if (capable)
                result.push_back(&view);
        }
        sort_axis(result);
        if (result.empty())
            diagnostics_.add(
                "scenario.matrix.empty_axis", path(experiment_index, "worker_nodes"),
                "worker_axis_empty",
                {{"experiment_index", to_string(experiment_index)}, {"axis_category", "worker"}});
        return result;
    }

    auto memory_axis(std::size_t experiment_index) -> std::vector<const NodeView*> {
        budget_.bytes(nodes_.size(), sizeof(const NodeView*));
        std::vector<const NodeView*> result;
        const auto pointer = path(experiment_index, "memory_nodes");
        for (const auto& [unused, view] : nodes_) {
            static_cast<void>(unused);
            if (!context_.topology.machine().effective_nodes.value()->contains(
                    core::NumaNodeId{view.linux_node.value()}))
                continue;
            if (view.node->installed_memory.availability() != topology::Availability::available ||
                !view.node->installed_memory.value()) {
                diagnostics_.add("scenario.matrix.unavailable_attribute", pointer,
                                 "installed_memory_unavailable",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"axis_category", "memory"}});
                continue;
            }
            if (view.node->installed_memory.value()->value() == 0U)
                continue;
            if (view.node->page_sizes.empty()) {
                diagnostics_.add("scenario.matrix.unavailable_attribute", pointer,
                                 "page_size_evidence_unavailable",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"axis_category", "memory"},
                                  {"capability_category", "base_page"}});
                continue;
            }
            if (!std::ranges::contains(view.node->page_sizes, base_page_size_)) {
                diagnostics_.add("scenario.matrix.page_size_unavailable", pointer,
                                 "base_page_size_unavailable",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"axis_category", "memory"},
                                  {"capability_category", "base_page"}});
                continue;
            }
            result.push_back(&view);
        }
        sort_axis(result);
        if (result.empty())
            diagnostics_.add(
                "scenario.matrix.empty_axis", pointer, "memory_axis_empty",
                {{"experiment_index", to_string(experiment_index)}, {"axis_category", "memory"}});
        return result;
    }

    static void sort_axis(std::vector<const NodeView*>& axis) {
        std::ranges::sort(axis, [](const NodeView* left, const NodeView* right) {
            if (left->linux_node != right->linux_node)
                return left->linux_node < right->linux_node;
            return left->stable_id < right->stable_id;
        });
    }

    auto selector_template(std::size_t experiment_index)
        -> std::expected<const MatrixWorkerSelectorTemplate*, core::Error> {
        const auto& templates = scenario_.memory_placement_resolved()
                                    .service_cpu_resolved()
                                    .resource_resolved()
                                    .matrix_worker_templates();
        const auto found = std::ranges::find_if(templates, [experiment_index](const auto& item) {
            return item.experiment_index == experiment_index;
        });
        if (found == templates.end())
            return std::unexpected{matrix_internal_error("worker_template_missing")};
        return &*found;
    }

    auto binding(std::size_t experiment_index)
        -> std::expected<const ExperimentWorkloadBinding*, core::Error> {
        const auto& bindings = scenario_.experiment_workloads();
        const auto found = std::ranges::find_if(bindings, [experiment_index](const auto& item) {
            return item.experiment_index == experiment_index;
        });
        if (found == bindings.end())
            return std::unexpected{matrix_internal_error("workload_binding_missing")};
        return &*found;
    }

    auto workload(const ExperimentWorkloadBinding& item)
        -> std::expected<const DispatchedWorkload*, core::Error> {
        const auto found = scenario_.workloads().find(item.workload_id);
        if (found == scenario_.workloads().end())
            return std::unexpected{matrix_internal_error("dispatched_workload_missing")};
        return &found->second;
    }

    auto resolve_row(const Json& experiment, std::size_t experiment_index, std::size_t row_index,
                     const NodeView& node, const MatrixWorkerSelectorTemplate& retained)
        -> std::expected<std::optional<MatrixWorkerRow>, core::Error> {
        if (retained.experiment_index != experiment_index ||
            retained.profile != resource_resolution_profile)
            return std::unexpected{matrix_internal_error("worker_template_invariant")};
        const auto pointer = path(experiment_index, "workers_per_node");
        const auto& resources =
            scenario_.memory_placement_resolved().service_cpu_resolved().resource_resolved();
        auto selected = resolve_v1_selection(
            default_expanded(scenario_), context_, experiment.at("workers_per_node"), pointer,
            "matrix-row-" + to_string(experiment_index) + '-' + to_string(row_index),
            resources.worker_sets(),
            SelectionOverrides{node.linux_node.value(), std::string{node.stable_id.value()}});
        if (!selected)
            return std::unexpected{
                matrix_internal_error("worker_selector_internal", selected.error().code.value())};
        if (!selected->diagnostics.empty() || !selected->processing_units) {
            const auto& first = selected->diagnostics.empty()
                                    ? std::optional<ScenarioDiagnostic>{}
                                    : std::optional{selected->diagnostics.front()};
            diagnostics_.add("scenario.matrix.worker_resolution_failed",
                             first ? first->json_pointer : pointer + "/select",
                             first && first->safe_context.contains("classification")
                                 ? first->safe_context.at("classification")
                                 : "selector_failed",
                             {{"experiment_index", to_string(experiment_index)},
                              {"worker_row_index", to_string(row_index)}},
                             first ? first->location : std::nullopt);
            return std::optional<MatrixWorkerRow>{};
        }
        core::CpuSet cpus;
        for (const auto& pu : *selected->processing_units)
            cpus = cpus.set_union(core::CpuSet{pu.linux_cpu});
        return std::optional{MatrixWorkerRow{node.stable_id, node.linux_node,
                                             std::move(*selected->processing_units),
                                             std::move(cpus)}};
    }

    auto service_valid(const MatrixWorkerRow& row, std::size_t experiment_index,
                       std::size_t row_index) -> bool {
        const auto& service = scenario_.memory_placement_resolved().service_cpu_resolved();
        bool valid = true;
        for (const auto& worker : row.workers) {
            if (service.reserved_service_cpus().contains(worker.linux_cpu)) {
                diagnostics_.add("scenario.matrix.service_overlap",
                                 path(experiment_index, "workers_per_node/select"), "same_pu",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"worker_row_index", to_string(row_index)}});
                valid = false;
            } else if (std::ranges::contains(service.reserved_service_cores(), worker.core_id)) {
                diagnostics_.add("scenario.matrix.service_overlap",
                                 path(experiment_index, "workers_per_node/select"), "same_core",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"worker_row_index", to_string(row_index)}});
                valid = false;
            }
        }
        return valid;
    }

    static auto has_constraint(const ExperimentWorkloadBinding& binding_value,
                               DeferredKernelConstraint constraint) -> bool {
        return std::ranges::contains(binding_value.deferred_constraints, constraint);
    }

    auto memory_template(std::size_t experiment_index)
        -> std::expected<MatrixMemoryPlacementTemplate, core::Error> {
        const auto& templates = scenario_.memory_placement_resolved().matrix_memory_templates();
        const auto found = std::ranges::find_if(templates, [experiment_index](const auto& item) {
            return item.experiment_index == experiment_index;
        });
        if (found != templates.end())
            return *found;
        auto threshold = ExactRatio::create(999U, 1000U);
        if (!threshold)
            return std::unexpected{matrix_internal_error("default_ratio_invariant")};
        return MatrixMemoryPlacementTemplate{experiment_index,
                                             MemoryPolicy::bind,
                                             PageMode::base,
                                             base_page_size_,
                                             *threshold,
                                             true,
                                             true,
                                             true,
                                             "",
                                             std::string{memory_placement_resolution_profile}};
    }

    static auto working_set(const DispatchedWorkload& workload_value) -> core::ByteCount {
        if (const auto* bandwidth =
                std::get_if<kernel::SequentialReadBandwidthConfig>(&workload_value.configuration))
            return bandwidth->working_set_per_worker();
        return std::get<kernel::DependentLoadLatencyConfig>(workload_value.configuration)
            .working_set();
    }

    auto comparison(const Json& value)
        -> std::expected<MatrixComparisonConfiguration, core::Error> {
        auto effect = ratio(value.at("practical_effect"));
        if (!effect)
            return std::unexpected{std::move(effect.error())};
        auto confidence = ratio(value.at("confidence"));
        if (!confidence)
            return std::unexpected{std::move(confidence.error())};
        const auto strategy = order(value.at("order").at("strategy"));
        if (!strategy)
            return std::unexpected{matrix_internal_error("comparison_order_invariant")};
        std::optional<std::uint64_t> seed;
        if (value.at("order").contains("seed"))
            seed = value.at("order").at("seed").get<std::uint64_t>();
        return MatrixComparisonConfiguration{*effect, *confidence, *strategy, seed};
    }

    auto retain_comparison(const Json& experiment, std::size_t experiment_index)
        -> std::expected<void, core::Error> {
        if (!core::detail::json_string_equal(experiment.at("type"), "comparison"))
            return std::unexpected{matrix_internal_error("experiment_type_invariant")};

        const auto& resources =
            scenario_.memory_placement_resolved().service_cpu_resolved().resource_resolved();
        const auto worker_reference = std::ranges::find_if(
            resources.experiment_workers(), [experiment_index](const auto& item) {
                return item.experiment_index == experiment_index;
            });
        if (worker_reference == resources.experiment_workers().end())
            return std::unexpected{matrix_internal_error("worker_reference_missing")};
        const auto workers = resources.worker_sets().find(worker_reference->worker_set_name);
        if (workers == resources.worker_sets().end())
            return std::unexpected{matrix_internal_error("worker_set_missing")};

        const auto bound = binding(experiment_index);
        if (!bound)
            return std::unexpected{std::move(bound.error())};
        if ((**bound).workload_id != experiment.at("workload").get<std::string>())
            return std::unexpected{matrix_internal_error("workload_binding_invariant")};

        std::vector<ResolvedComparisonVariant> variants;
        const auto& authored_variants = experiment.at("variants");
        budget_.take(&MatrixExpansionLimits::variants, authored_variants.size());
        budget_.take(&MatrixExpansionLimits::edges, experiment.at("compare").at("candidates").size());
        budget_.take(&MatrixExpansionLimits::worker_associations,
                     workers->second.processing_units.size());
        budget_.bytes(1, sizeof(ResolvedComparisonExperiment));
        budget_.bytes(authored_variants.size(), sizeof(ResolvedComparisonVariant));
        for (const auto& worker : workers->second.processing_units) budget_.worker(worker);
        budget_.bytes(workers->second.linux_cpus.cardinality(), 2 * sizeof(std::uint32_t));
        budget_.text(experiment.at("id").get_ref<const std::string&>());
        budget_.text((**bound).workload_id);
        budget_.text(experiment.at("compare").at("baseline").get_ref<const std::string&>());
        for (const auto& candidate : experiment.at("compare").at("candidates")) {
            budget_.bytes(1, sizeof(std::string));
            budget_.text(candidate.get_ref<const std::string&>());
        }
        variants.reserve(authored_variants.size());
        const auto& placements = scenario_.memory_placement_resolved().comparison_memory();
        for (std::size_t variant_index = 0; variant_index < authored_variants.size();
             ++variant_index) {
            const auto placement = std::ranges::find_if(
                placements, [experiment_index, variant_index](const auto& item) {
                    return item.experiment_index == experiment_index &&
                           item.variant_index == variant_index;
                });
            if (placement == placements.end())
                return std::unexpected{matrix_internal_error("comparison_memory_missing")};
            if (const auto* per_worker =
                    std::get_if<ResolvedPerWorkerMemoryPlacement>(&placement->placement)) {
                if (per_worker->worker_nodes.size() != workers->second.processing_units.size())
                    return std::unexpected{
                        matrix_internal_error("per_worker_memory_mapping_mismatch")};
                for (std::size_t worker_index = 0;
                     worker_index < per_worker->worker_nodes.size(); ++worker_index)
                    if (per_worker->worker_nodes[worker_index].worker !=
                        workers->second.processing_units[worker_index])
                        return std::unexpected{
                            matrix_internal_error("per_worker_memory_mapping_mismatch")};
            }
            budget_.text(authored_variants.at(variant_index).at("id").get_ref<const std::string&>());
            budget_.memory(placement->placement);
            variants.push_back({variant_index,
                                authored_variants.at(variant_index).at("id").get<std::string>(),
                                placement->placement});
        }

        auto configuration = comparison(experiment.at("compare"));
        if (!configuration)
            return std::unexpected{std::move(configuration.error())};
        std::vector<std::string> candidates;
        for (const auto& candidate : experiment.at("compare").at("candidates"))
            candidates.push_back(candidate.get<std::string>());
        const auto origin_pointer = path(experiment_index, "compare/practical_effect");
        const auto origin = default_expanded(scenario_).origins().find(origin_pointer);
        if (origin == default_expanded(scenario_).origins().end())
            return std::unexpected{matrix_internal_error("comparison_origin_missing")};
        if (origin->second.source_pointer) budget_.text(*origin->second.source_pointer);

        comparisons_.push_back({experiment_index, experiment.at("id").get<std::string>(),
                                (**bound).workload_id, workers->second.processing_units,
                                workers->second.linux_cpus, std::move(variants),
                                experiment.at("compare").at("baseline").get<std::string>(),
                                std::move(candidates), std::move(*configuration), origin->second});
        return {};
    }

    auto expand(const Json& experiment, std::size_t experiment_index)
        -> std::expected<void, core::Error> {
        const auto before = diagnostics_.size();
        auto workers = worker_axis(experiment_index);
        auto memories = memory_axis(experiment_index);
        if (diagnostics_.size() != before || workers.empty() || memories.empty())
            return {};

        auto retained_selector = selector_template(experiment_index);
        if (!retained_selector)
            return std::unexpected{std::move(retained_selector.error())};
        auto bound = binding(experiment_index);
        if (!bound)
            return std::unexpected{std::move(bound.error())};
        auto dispatched = workload(**bound);
        if (!dispatched)
            return std::unexpected{std::move(dispatched.error())};

        std::vector<MatrixWorkerRow> rows;
        budget_.take(&MatrixExpansionLimits::worker_rows, workers.size());
        budget_.bytes(workers.size(), sizeof(MatrixWorkerRow));
        rows.reserve(workers.size());
        bool row_failed = false;
        for (std::size_t row_index = 0; row_index < workers.size() && !diagnostics_.capped();
             ++row_index) {
            // Also cover repeated explicit SMT indices before semantic rejection.
            const auto& node = *workers[row_index];
            const auto repeats = std::max(std::size_t{1}, (**retained_selector).smt_indices.size());
            const auto pu_count = node.node->member_pus.cardinality();
            if (pu_count > std::numeric_limits<std::size_t>::max() / repeats)
                throw ExpansionCapacity{};
            budget_.take(&MatrixExpansionLimits::worker_associations, pu_count * repeats);
            budget_.bytes(pu_count * repeats, sizeof(ResolvedProcessingUnit) +
                          2 * sizeof(std::uint32_t));
            budget_.text(node.stable_id.value());
            for (const auto id : node.node->member_pus) {
                const auto stable = context_.stable_ids.processing_unit_id(id);
                if (stable) budget_.bytes(repeats, stable->get().value().size());
                if (const auto pu = context_.topology.processing_unit(id))
                    if (const auto core = context_.stable_ids.core_id(pu->core_id))
                        budget_.bytes(repeats, core->get().value().size());
            }
            auto row = resolve_row(experiment, experiment_index, row_index, *workers[row_index],
                                   **retained_selector);
            if (!row)
                return std::unexpected{std::move(row.error())};
            if (!*row) {
                row_failed = true;
                continue;
            }
            bool valid = service_valid(**row, experiment_index, row_index);
            if (has_constraint(**bound, DeferredKernelConstraint::exactly_one_worker) &&
                (*row)->workers.size() != 1U) {
                diagnostics_.add("scenario.matrix.kernel_constraint_failed",
                                 path(experiment_index, "workers_per_node"), "exactly_one_worker",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"worker_row_index", to_string(row_index)},
                                  {"required_worker_count", "1"}});
                valid = false;
            }
            if (!valid) {
                row_failed = true;
                continue;
            }
            rows.push_back(std::move(**row));
        }
        if (row_failed || diagnostics_.capped())
            return {};
        if (workers.size() > std::numeric_limits<std::size_t>::max() / memories.size())
            return std::unexpected{matrix_internal_error("matrix_size_unrepresentable")};
        const auto cardinality = workers.size() * memories.size();
        std::vector<ResolvedMatrixCell> cells;
        if (cardinality > cells.max_size())
            return std::unexpected{matrix_internal_error("matrix_size_unrepresentable")};
        budget_.take(&MatrixExpansionLimits::cells, cardinality);
        budget_.bytes(cardinality, sizeof(ResolvedMatrixCell));
        // Each row has at most columns - 1 executable edges.
        budget_.take(&MatrixExpansionLimits::edges, workers.size() * (memories.size() - 1));
        budget_.bytes(workers.size() * (memories.size() - 1), sizeof(MatrixComparisonEdge));
        cells.reserve(cardinality);
        auto memory = memory_template(experiment_index);
        if (!memory)
            return std::unexpected{std::move(memory.error())};
        const auto authored_memory = experiment.contains("memory");
        std::set<MatrixCellId> identities;
        bool cell_failed = false;
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
            for (std::size_t column = 0; column < memories.size(); ++column) {
                const auto& node = *memories[column];
                if (!std::ranges::contains(node.node->page_sizes, memory->page_size)) {
                    diagnostics_.add("scenario.matrix.page_size_unavailable",
                                     authored_memory ? path(experiment_index, "memory/page_mode")
                                                     : path(experiment_index, "memory_nodes"),
                                     "base_page_size_unavailable",
                                     {{"experiment_index", to_string(experiment_index)},
                                      {"worker_row_index", to_string(row_index)},
                                      {"memory_column_index", to_string(column)},
                                      {"capability_category", "base_page"}});
                    cell_failed = true;
                    continue;
                }
                if (has_constraint(
                        **bound,
                        DeferredKernelConstraint::working_set_at_least_resolved_page_size) &&
                    working_set(**dispatched) < memory->page_size) {
                    diagnostics_.add("scenario.matrix.kernel_constraint_failed",
                                     authored_memory ? path(experiment_index, "memory")
                                                     : path(experiment_index, "workload"),
                                     "working_set_smaller_than_page",
                                     {{"experiment_index", to_string(experiment_index)},
                                      {"worker_row_index", to_string(row_index)},
                                      {"memory_column_index", to_string(column)}});
                    cell_failed = true;
                    continue;
                }
                const auto from = context_.stable_ids.numa_node(rows[row_index].stable_node_id);
                if (!from)
                    return std::unexpected{matrix_internal_error("stable_id_index_incomplete")};
                const auto distance =
                    context_.topology.numa_distances().distance(*from, node.node->id);
                if (!distance)
                    return std::unexpected{matrix_internal_error("invalid_topology")};
                budget_.bytes(1, sizeof(MatrixCellId) + sizeof(ResolvedMemoryNode) +
                              sizeof(core::LinuxNumaNodeId));
                budget_.text(experiment.at("id").get_ref<const std::string&>());
                budget_.text(experiment.at("id").get_ref<const std::string&>());
                budget_.text(rows[row_index].stable_node_id.value());
                budget_.text(rows[row_index].stable_node_id.value());
                for (unsigned repeat = 0; repeat < 4; ++repeat) budget_.text(node.stable_id.value());
                if (authored_memory) budget_.text(memory->source_pointer);
                MatrixCellId id{experiment.at("id").get<std::string>(),
                                rows[row_index].stable_node_id, node.stable_id};
                if (!identities.insert(id).second)
                    return std::unexpected{matrix_internal_error("duplicate_cell_identity")};
                std::vector<ResolvedMemoryNode> selected{{node.stable_id, node.linux_node}};
                std::set<core::LinuxNumaNodeId> membership{node.linux_node};
                ResolvedMemoryPlacement placement{
                    MemoryPolicy::bind,
                    std::move(selected),
                    std::move(membership),
                    PageMode::base,
                    memory->page_size,
                    memory->placement_threshold,
                    memory->prefault,
                    memory->verify_before,
                    memory->verify_after,
                    authored_memory ? memory->source_pointer : std::string{}};
                cells.push_back({std::move(id),
                                 experiment_index,
                                 row_index,
                                 node.stable_id,
                                 node.linux_node,
                                 std::move(placement),
                                 {*distance}});
            }
        }
        if (cell_failed || diagnostics_.capped())
            return {};

        auto comparison_value = comparison(experiment.at("comparison"));
        if (!comparison_value)
            return std::unexpected{std::move(comparison_value.error())};
        std::vector<MatrixComparisonEdge> edges;
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const auto row_begin = row_index * memories.size();
            std::optional<std::size_t> baseline;
            for (std::size_t column = 0; column < memories.size(); ++column) {
                const auto cell_index = row_begin + column;
                if (cells[cell_index].memory_node_stable_id == rows[row_index].stable_node_id) {
                    if (baseline)
                        return std::unexpected{matrix_internal_error("duplicate_cell_identity")};
                    baseline = cell_index;
                }
            }
            if (!baseline) {
                diagnostics_.add("scenario.matrix.invalid_comparison",
                                 path(experiment_index, "comparison/baseline"),
                                 "local_baseline_missing",
                                 {{"experiment_index", to_string(experiment_index)},
                                  {"worker_row_index", to_string(row_index)}});
                continue;
            }
            for (std::size_t column = 0; column < memories.size(); ++column) {
                const auto candidate = row_begin + column;
                if (candidate != *baseline)
                    edges.push_back({*baseline, candidate});
            }
        }
        if (diagnostics_.size() != before)
            return {};

        std::vector<ResolvedMemoryNode> worker_records;
        std::vector<ResolvedMemoryNode> memory_records;
        budget_.bytes(1, sizeof(ExpandedMatrixExperiment));
        budget_.text(experiment.at("id").get_ref<const std::string&>());
        for (const auto* node : workers) {
            budget_.bytes(1, sizeof(ResolvedMemoryNode)); budget_.text(node->stable_id.value());
            worker_records.push_back({node->stable_id, node->linux_node});
        }
        for (const auto* node : memories) {
            budget_.bytes(1, sizeof(ResolvedMemoryNode)); budget_.text(node->stable_id.value());
            memory_records.push_back({node->stable_id, node->linux_node});
        }
        experiments_.push_back({experiment_index, experiment.at("id").get<std::string>(),
                                std::move(worker_records), std::move(memory_records),
                                std::move(rows), std::move(cells), std::move(*comparison_value),
                                std::move(edges)});
        return {};
    }

    static auto path(std::size_t experiment_index, std::string_view suffix) -> std::string {
        return "/experiments/" + to_string(experiment_index) + '/' + std::string{suffix};
    }

    static auto to_string(std::size_t value) -> std::string { return std::to_string(value); }

    const KernelDispatchedScenario& scenario_;
    const ResourceResolutionContext& context_;
    core::ByteCount base_page_size_;
    Diagnostics diagnostics_;
    std::map<std::uint32_t, NodeView> nodes_;
    std::vector<ExpandedMatrixExperiment> experiments_;
    std::vector<ResolvedComparisonExperiment> comparisons_;
    ExpansionBudget budget_;
};

} // namespace

auto matrix_internal_error(std::string classification, std::string underlying) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!underlying.empty())
        context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.matrix.internal_failure"},
            .message = "Node-pair matrix expansion failed internally",
            .operation = "expand_node_pair_matrices",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto expand_v1_matrices(const KernelDispatchedScenario& scenario,
                        const ResourceResolutionContext& context, core::ByteCount base_page_size)
    -> std::expected<MatrixExpansionPayload, core::Error> {
    try {
        return Expander{scenario, context, base_page_size}.run();
    } catch (const std::exception&) {
        return std::unexpected{matrix_internal_error("normalized_matrix_invariant")};
    } catch (...) {
        return std::unexpected{matrix_internal_error("contained_unknown_failure")};
    }
}

auto expand_v1_matrices(const KernelDispatchedScenario& scenario,
                        const ResourceResolutionContext& context, core::ByteCount base_page_size,
                        MatrixExpansionLimits limits, std::size_t* charged_bytes)
    -> std::expected<MatrixExpansionPayload, core::Error> {
    if (charged_bytes) *charged_bytes = 0;
    try {
        Expander expander{scenario, context, base_page_size, limits};
        auto result = expander.run();
        if (result && result->diagnostics.empty() && charged_bytes)
            *charged_bytes = expander.charged_bytes();
        return result;
    } catch (const ExpansionCapacity&) {
        return std::unexpected{matrix_internal_error("caller_capacity")};
    } catch (const std::exception&) {
        return std::unexpected{matrix_internal_error("normalized_matrix_invariant")};
    } catch (...) {
        return std::unexpected{matrix_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario::detail
