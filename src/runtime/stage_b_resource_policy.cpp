#include "runtime/stage_b_resource_policy.hpp"
#include "runtime/pairwise_preparation_graph.hpp"
#include "runtime/finalized_evidence_prepared_resources.generated.hpp"

#include <array>
#include <limits>

namespace epycbench::runtime::detail::stage_b_policy {
namespace {
auto sum(std::uint64_t& total, std::uint64_t value) noexcept -> Admission {
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        return std::unexpected(Error::overflow);
    total += value;
    return {};
}
} // namespace

auto admit_startup_retention(std::span<const std::uint64_t> sizes) noexcept -> Admission {
    std::uint64_t total{};
    for (const auto size : sizes)
        if (auto result = sum(total, size); !result) return result;
    if (total > startup) return std::unexpected(Error::limit);
    return {};
}

auto admit_bootstrap(std::string_view bytes) noexcept -> Admission {
    if (bytes.empty() || bytes.size() > frame) return std::unexpected(Error::limit);
    std::uint64_t nesting{}, nodes{}, string_bytes{};
    bool quoted{}, escaped{}, atom{};
    for (const char ch : bytes) {
        if (quoted) {
            if (++string_bytes > boot_text) return std::unexpected(Error::limit);
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            atom = false;
            if (++nodes > boot_nodes) return std::unexpected(Error::limit);
        } else if (ch == '{' || ch == '[') {
            atom = false;
            if (++nesting > boot_depth || ++nodes > boot_nodes)
                return std::unexpected(Error::limit);
        } else if (ch == '}' || ch == ']') {
            atom = false;
            if (nesting == 0) return std::unexpected(Error::shape);
            --nesting;
        } else if (ch == ',' || ch == ':' || ch == ' ' || ch == '\n' ||
                   ch == '\r' || ch == '\t') {
            atom = false;
        } else if (!atom) {
            atom = true;
            if (++nodes > boot_nodes) return std::unexpected(Error::limit);
        }
    }
    if (quoted || nesting != 0) return std::unexpected(Error::shape);
    return {};
}

auto admit(const ipc::PlacementRequest& request) noexcept -> Admission {
    if (request.workers().size() > workers) return std::unexpected(Error::limit);
    const auto& measurement = request.execution_inputs().measurement();
    if (measurement.repetition_maximum_blocks() > blocks ||
        measurement.warmup_minimum() > trials || measurement.warmup_maximum() > trials ||
        measurement.warmup_stability_window() > trials)
        return std::unexpected(Error::limit);
    if (const auto* latency = std::get_if<ipc::PlacementLatencyExecutionInputs>(
            &request.execution_inputs().kernel())) {
        const auto bytes = latency->configuration().working_set().value();
        // Each retained initializer successor, verification permutation and
        // inverse-position entry corresponds to one 64-byte latency node.
        const auto nodes = bytes / 64U;
        std::uint64_t destinations = nodes;
        if (auto result = sum(destinations, nodes); !result) return result;
        if (auto result = sum(destinations, nodes); !result) return result;
        if (destinations > verification) return std::unexpected(Error::limit);
    }
    return {};
}

auto admit(const PreparationGraphLimits& limits) noexcept -> Admission {
    if (limits.workers > workers || limits.rows > graph_rows ||
        limits.targets > graph_rows || limits.page_counts > graph_rows ||
        limits.domains > monitoring_domains || limits.memberships > monitoring_members ||
        limits.strings > graph_text || limits.labels > graph_text)
        return std::unexpected(Error::limit);
    return {};
}

auto admit(const AdmittedPairwiseProductionInputs& inputs) noexcept -> Admission {
    const auto& request = inputs.pairwise_request();
    if (auto result = admit(request.first().request()); !result) return result;
    if (auto result = admit(request.second().request()); !result) return result;
    if (inputs.statistics_policy().comparison_bootstrap_policy().replicate_count().value() > replicates ||
        inputs.statistics_policy().stopping_bootstrap_policy().replicate_count().value() > replicates)
        return std::unexpected(Error::limit);
    std::uint64_t pages{};
    for (const auto* side : {&request.first(), &request.second()}) {
        if (side->workers().size() > regions) return std::unexpected(Error::limit);
        const auto& present = side->context().topology().machine().present_nodes;
        std::uint64_t present_count{};
        for (const auto node : present)
            if (++present_count > node_entries || node.value() >= node_bits)
                return std::unexpected(Error::limit);
        for (const auto& worker : side->workers()) {
            const auto& region = worker.request().region();
            const auto page = region.page_size_bytes().value();
            if (page == 0 || region.bytes().value() % page != 0)
                return std::unexpected(Error::shape);
            if (auto result = sum(pages, region.bytes().value() / page); !result) return result;
            if (pages > page_entries || region.nodes().size() > node_entries)
                return std::unexpected(Error::limit);
            for (const auto& node : region.nodes())
                if (node.linux_numa_node().value() >= node_bits)
                    return std::unexpected(Error::limit);
        }
    }
    return {};
}

auto admit(const PairwiseHostCapacityRequestTranscript& request) noexcept -> Admission {
    if (auto result = admit(request.first()); !result) return result;
    if (auto result = admit(request.second()); !result) return result;
    if (request.statistics_source().bootstrap_samples() > replicates)
        return std::unexpected(Error::limit);
    return {};
}

auto admit(const prepared_resources_v1::PreparedScalarStocks& s) noexcept -> Admission {
    if (s.logical_bytes > logical || s.logical_probe_bytes > logical ||
        s.parser_depth > depth || s.array_elements > elements ||
        s.results_raw_graph_elements > elements || s.decoded_string_bytes > text ||
        s.results_raw_graph_string_bytes > text)
        return std::unexpected(Error::limit);
    const std::array bounded{
        s.parser_nodes, s.builder_slots, s.builder_initialized_bits, s.array_owners,
        s.string_owners, s.results_raw_graph_slots, s.results_first_metrics,
        s.results_second_metrics, s.results_first_observation_ids,
        s.results_second_observation_ids, s.results_block_ids, s.results_paired_effects,
        s.results_first_outlier_states, s.results_second_outlier_states,
        s.statistics_metric_scratch, s.statistics_double_scratch_primary,
        s.statistics_double_scratch_secondary, s.statistics_effect_resample,
        s.statistics_stopping_prefix, s.results_stopping_replays,
        s.results_mismatch_diagnostics, s.active_primary_entries, s.active_secondary_entries};
    for (const auto value : bounded)
        if (value > count) return std::unexpected(Error::limit);
    return {};
}

auto admit(const prepared_resources_v1::PreparedResourceValue& value) noexcept -> Admission {
    if (auto result = admit(value.stocks); !result) return result;
    if (value.comparison_bootstrap_replicates > replicates ||
        value.stopping_bootstrap_replicates > replicates ||
        value.definitions.size() > definitions)
        return std::unexpected(Error::limit);
    const auto& expected = prepared_resources_v1::detail::definition_stocks;
    if (value.definitions.size() != expected.size()) return std::unexpected(Error::shape);
    std::uint64_t owners{}, arrays{}, utf8{};
    for (std::size_t index{}; index < value.definitions.size(); ++index) {
        const auto& item = value.definitions[index];
        const auto& definition = expected[index];
        if (item.definition_id != definition.definition_id ||
            static_cast<unsigned>(item.kind) != static_cast<unsigned>(definition.kind) ||
            item.local_array_cardinality.has_value() != definition.local_array_formula.has_value() ||
            item.decoded_utf8_bytes.has_value() != definition.decoded_utf8_formula.has_value())
            return std::unexpected(Error::shape);
        if (item.owner_occurrences > count ||
            item.local_array_cardinality.value_or(0) > elements ||
            item.decoded_utf8_bytes.value_or(0) > text)
            return std::unexpected(Error::limit);
        if (auto result = sum(owners, item.owner_occurrences); !result) return result;
        if (auto result = sum(arrays, item.local_array_cardinality.value_or(0)); !result)
            return result;
        if (auto result = sum(utf8, item.decoded_utf8_bytes.value_or(0)); !result)
            return result;
    }
    if (owners > count || arrays > elements || utf8 > logical)
        return std::unexpected(Error::limit);
    return {};
}

auto admit_hwmon(platform_linux::detail::BoundedHwmonCapacity capacity) noexcept -> Admission {
    const auto shape = platform_linux::detail::bounded_hwmon_storage_shape(capacity);
    if (!shape) return std::unexpected(Error::overflow);
    if (shape->token_storage > monitoring_text || shape->entry_storage > hwmon_entry_bytes ||
        shape->operation_capacity > hwmon_operation_slots ||
        shape->retirement_capacity > hwmon_operation_slots)
        return std::unexpected(Error::limit);
    return {};
}

auto admit(const wire_v1::CalculatedEvidenceCapacity& capacity) noexcept -> Admission {
    const auto& shape = capacity.logical_shape;
    const auto& parameters = capacity.prerequisite.parameters;
    if (shape.canonical_logical_bytes.value > logical || shape.typed_nodes.value > count ||
        shape.parser_depth.value > depth || parameters.workers.value > workers ||
        parameters.maximum_blocks.value > blocks || parameters.maximum_warmup_trials.value > trials ||
        parameters.present_cpu_ids.size() > monitoring_domains ||
        parameters.maximum_cpu_index.value >= node_bits ||
        parameters.numa_domain_maxnode.value > node_bits)
        return std::unexpected(Error::limit);
    const auto& c = capacity.prerequisite.components;
    const std::array captures{c.raw_page_acquisitions.value, c.raw_context_acquisitions.value,
        c.context_events.value, c.context_offsets.value, c.raw_temperature_acquisitions.value,
        c.frequency_acquisitions.value, c.frequency_cpu_memberships.value,
        c.frequency_metadata.value, c.classified_page_workers.value,
        c.classified_context_workers.value, c.classified_frequency_domains.value};
    std::uint64_t total{};
    for (auto value : captures) {
        if (value > monitoring_members) return std::unexpected(Error::limit);
        if (auto result = sum(total, value); !result) return result;
    }
    if (total > elements || c.frequency_domains.value > monitoring_domains)
        return std::unexpected(Error::limit);
    // Legacy wire maxima describe potential discovery, not the selected owner.
    // The real dormant preparation omits hwmon entirely when not requested.
    if (parameters.channels.temperature) {
        auto selected = admit_hwmon({parameters.present_cpu_ids.size(),
            parameters.maximum_cpu_index.value + 2U, parameters.base_page_bytes.value});
        if (!selected) return selected;
    }
    const auto& stocks = capacity.prerequisite.monitoring_lower_stocks;
    const std::array lower{
        stocks.persistent_worker_scopes.value, stocks.page_sources.value,
        stocks.context_sources.value, stocks.context_coverage_slots.value,
        stocks.temperature_sources.value, stocks.frequency_sources.value,
        stocks.frequency_metadata_slots.value, stocks.persistent_frequency_cpu_ids.value,
        stocks.preflight_channel_dispositions.value, stocks.page_pair_scratch.value,
        stocks.context_array_scratch.value, stocks.temperature_view_scratch.value,
        stocks.frequency_view_scratch.value, stocks.frequency_result_scratch.value,
        stocks.whole_stop_cleanup.value, stocks.preparation_rollback_cleanup.value,
        stocks.completion_abort_cleanup.value, stocks.cpufreq_paths.value,
        stocks.cpufreq_directory_entries.value, stocks.cpufreq_operation_slots.value};
    for (const auto value : lower) {
        if (value > monitoring_members) return std::unexpected(Error::limit);
        if (auto result = sum(total, value); !result) return result;
    }
    if (total > elements || stocks.dynamic_frequency_raw_bytes.value > component ||
        stocks.dynamic_frequency_escaped_bytes.value > component)
        return std::unexpected(Error::limit);
    std::uint64_t buffers = stocks.dynamic_frequency_raw_bytes.value;
    if (auto result = sum(buffers, stocks.dynamic_frequency_escaped_bytes.value); !result)
        return result;
    if (buffers > logical) return std::unexpected(Error::limit);
    return {};
}

auto derive_progress(const PairwiseHostCapacityRequestTranscript& request,
    const wire_v1::CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<ProgressPreparation, Error> {
    if (auto result = admit(request); !result) return std::unexpected(result.error());
    if (auto result = admit(capacity); !result) return std::unexpected(result.error());
    const auto& parameters = capacity.prerequisite.parameters;
    if (request.first().workers().empty() ||
        request.first().workers().size() != request.second().workers().size() ||
        parameters.workers.value != request.first().workers().size())
        return std::unexpected(Error::shape);
    std::uint64_t strings = std::string_view{"epycbench.scenario/v0.1"}.size() + 142U + 13U;
    for (auto length : {request.first().experiment().workload_id().value().size(),
                       request.first().experiment().id().value().size(),
                       request.first().experiment().id().value().size()})
        if (auto result = sum(strings, length); !result) return std::unexpected(result.error());
    std::uint64_t targets{}, pages{}, physical_pages{};
    for (const auto& worker : request.first().workers()) {
        for (auto length : {worker.cpu().stable_pu_id().value().size(),
                           worker.cpu().stable_core_id().value().size()})
            if (auto result = sum(strings, length); !result) return std::unexpected(result.error());
    }
    for (const auto* side : {&request.first(), &request.second()}) {
        for (const auto& worker : side->workers()) {
            const auto& region = worker.region();
            const auto page = region.page_size_bytes().value();
            if (page == 0 || region.bytes().value() % page != 0) return std::unexpected(Error::shape);
            const auto total_pages = region.bytes().value() / page;
            if (auto result = sum(physical_pages, total_pages); !result)
                return std::unexpected(result.error());
            // The graph retains one count per possible resident node, matching
            // PairwiseMemoryWorkspace::node_capacity(), not one per queried page.
            if (auto result = sum(pages, std::min(total_pages,
                    parameters.numa_domain_maxnode.value)); !result)
                return std::unexpected(result.error());
            if (auto result = sum(targets, region.nodes().size()); !result)
                return std::unexpected(result.error());
            for (const auto& target : region.nodes())
                if (auto result = sum(strings, target.stable_numa_node_id().value().size()); !result)
                    return std::unexpected(result.error());
        }
    }
    // Same unconditional domain/member admission as PairwiseAdmittedMonitoringCapacity,
    // including when frequency capture is disabled.
    const auto domains = std::min<std::uint64_t>(parameters.workers.value,
        parameters.present_cpu_ids.size());
    const auto members = parameters.present_cpu_ids.size();
    const auto page = parameters.base_page_bytes.value;
    if (page > monitoring_text || strings > graph_text || targets > graph_rows ||
        physical_pages > page_entries || pages > graph_rows ||
        domains > monitoring_domains || members > monitoring_members)
        return std::unexpected(Error::limit);
    const auto labels = domains * page * 2U; // factors admitted above
    if (labels > graph_text) return std::unexpected(Error::limit);
    const PreparationGraphLimits graph{request.first().workers().size(),
        request.first().workers().size() * 2U, static_cast<std::size_t>(targets),
        static_cast<std::size_t>(pages), static_cast<std::size_t>(domains), members,
        static_cast<std::size_t>(strings), static_cast<std::size_t>(labels),
        std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max()};
    const PreparationGraphUsage usage{0, 0, graph.strings, graph.targets, graph.page_counts};
    auto preparation = progress::preparation_ceiling(usage, graph.workers, graph);
    if (!preparation) return std::unexpected(Error::overflow);
    // The placed control service row has fixed closed fields and stable SHA256 IDs.
    if (*preparation > UINT64_MAX - 768U) return std::unexpected(Error::overflow);
    std::array<std::uint64_t, progress::kind_count> ceilings{};
    ceilings[static_cast<std::size_t>(progress::Kind::preparation)] = *preparation + 768U;
    ceilings[static_cast<std::size_t>(progress::Kind::block_closed)] = progress::block_closed_ceiling();
    constexpr std::array mapping{
        std::pair{progress::Kind::calibration, wire_v1::EvidenceComponentKind::calibration},
        std::pair{progress::Kind::warmup, wire_v1::EvidenceComponentKind::warmup},
        std::pair{progress::Kind::block_opened, wire_v1::EvidenceComponentKind::block_plan},
        std::pair{progress::Kind::observation, wire_v1::EvidenceComponentKind::factual_observation},
        std::pair{progress::Kind::failed_observation, wire_v1::EvidenceComponentKind::failed_observation},
        std::pair{progress::Kind::stopping, wire_v1::EvidenceComponentKind::stopping_evaluation},
        std::pair{progress::Kind::finalization, wire_v1::EvidenceComponentKind::finalization},
        std::pair{progress::Kind::terminal, wire_v1::EvidenceComponentKind::terminal},
        std::pair{progress::Kind::failure, wire_v1::EvidenceComponentKind::finalized_error}};
    for (auto [kind, child] : mapping)
        for (auto side : {wire_v1::EvidenceComponentSide::first, wire_v1::EvidenceComponentSide::second}) {
            auto shape = wire_v1::maximum_component_shape(child, side, capacity);
            if (!shape) return std::unexpected(Error::shape);
            auto& ceiling = ceilings[static_cast<std::size_t>(kind)];
            ceiling = std::max(ceiling, shape->canonical_logical_bytes.value);
        }
    auto result = progress::limits(parameters.maximum_blocks.value, ceilings);
    if (!result) return std::unexpected(Error::overflow);
    if (auto bounded = admit(*result); !bounded) return std::unexpected(bounded.error());
    return ProgressPreparation{*result, graph};
}

auto admit(const progress::Limits& value) noexcept -> Admission {
    if (value.transitions > count || value.arena_bytes > arena || value.wire_bytes > wire)
        return std::unexpected(Error::limit);
    std::array<std::uint64_t, progress::kind_count> ceilings{};
    for (std::size_t index{}; index < value.components.size(); ++index) {
        const auto& item = value.components[index];
        if (item.count > count || item.bytes > component)
            return std::unexpected(Error::limit);
        ceilings[index] = item.bytes;
    }
    const auto derived = progress::limits(
        value.components[static_cast<std::size_t>(progress::Kind::block_opened)].count,
        ceilings);
    if (!derived) return std::unexpected(derived.error() == progress::Error::overflow
        ? Error::overflow : Error::shape);
    if (*derived != value) return std::unexpected(Error::shape);
    return {};
}
} // namespace epycbench::runtime::detail::stage_b_policy
