#include "cli/results_command.hpp"

#include "core/json_ownership.hpp"
#include "results/result_export.hpp"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <filesystem>
#include <ostream>
#include <string_view>

namespace epycbench::cli {
namespace {
namespace ep = results::event_profile;
namespace fd = results::final_document;
constexpr std::size_t mib = 1024U * 1024U;
constexpr std::size_t input_limit = 64U * mib; // Per-frame/per-child policy is unchanged.
constexpr std::size_t document_limit = results_document_input_bytes;
constexpr std::size_t run_records = 16384;
constexpr std::size_t run_blocks = 2048;
constexpr std::size_t run_observations = 8192;
constexpr std::size_t run_samples = 524288;
constexpr std::string_view help =
    "Usage: epycbench result validate FILE\n"
    "Usage: epycbench result export --profile {complete,shareable,summary,observation-table} FILE\n"
    "Export validates once. Complete retains all evidence; derived profiles are reduced, linkable, not anonymous.\n"
    "Offline, read-only validation of a canonical EpycBench Results document.\n"
    "Supports result revisions 0-2 with matching event profiles v1-v3.\n"
    "A valid document does not imply successful execution or hardware qualification.\n";

enum class Diagnostic { usage, input_io, limit_exceeded, unsupported_profile, invalid_document, output_io, internal_failure };

auto message(Diagnostic diagnostic) noexcept -> std::string_view {
    switch (diagnostic) {
    case Diagnostic::usage: return "result validate: failed\ncode: results.cli.usage\n";
    case Diagnostic::input_io: return "result validate: failed\ncode: results.cli.input_io\n";
    case Diagnostic::limit_exceeded: return "result validate: failed\ncode: results.cli.limit_exceeded\n";
    case Diagnostic::unsupported_profile: return "result validate: failed\ncode: results.cli.unsupported_profile\n";
    case Diagnostic::invalid_document: return "result validate: failed\ncode: results.cli.invalid_document\n";
    case Diagnostic::output_io: return "result validate: failed\ncode: results.cli.output_io\n";
    case Diagnostic::internal_failure: return "result validate: failed\ncode: results.cli.internal_failure\n";
    }
    return "result validate: failed\ncode: results.cli.internal_failure\n";
}

auto export_message(Diagnostic diagnostic) noexcept -> std::string_view {
    switch (diagnostic) {
    case Diagnostic::usage: return "result export: failed\ncode: results.export.usage\n";
    case Diagnostic::input_io: return "result export: failed\ncode: results.export.input_io\n";
    case Diagnostic::limit_exceeded: return "result export: failed\ncode: results.export.limit_exceeded\n";
    case Diagnostic::unsupported_profile: return "result export: failed\ncode: results.export.unsupported_profile\n";
    case Diagnostic::invalid_document: return "result export: failed\ncode: results.export.invalid_document\n";
    case Diagnostic::output_io: return "result export: failed\ncode: results.export.output_io\n";
    case Diagnostic::internal_failure: return "result export: failed\ncode: results.export.internal_failure\n";
    }
    return "result export: failed\ncode: results.export.internal_failure\n";
}

auto fail(std::ostream& error, int status, Diagnostic diagnostic, bool exporting = false) noexcept -> int {
    const auto text = exporting ? export_message(diagnostic) : message(diagnostic);
    try {
        error.write(text.data(), static_cast<std::streamsize>(text.size()));
    } catch (...) {
        // Diagnostics cannot replace the already selected primary failure.
    }
    return status;
}

auto emit(std::ostream& output, std::ostream& error, std::string_view text, bool exporting = false) -> int {
    try {
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (output) return 0;
    } catch (...) {
    }
    return fail(error, 3, Diagnostic::output_io, exporting);
}

constexpr std::size_t retained_limit = 256U * mib;
constexpr std::size_t count_limit = 1U << 20U;
constexpr std::size_t leaf_logical = std::size_t{1} << 30U;
constexpr std::size_t plan_logical = std::size_t{4} << 30U;
constexpr std::size_t creation_logical = std::size_t{8} << 30U;
constexpr std::size_t logical_work = std::size_t{1} << 40U;

auto plan_limits() -> results::private_plan_composition::Limits {
    results::private_plan_composition::Limits result;
    result.document_ancestry.tree_nodes.fill(count_limit);
    result.document_ancestry.tree_depth = 64;
    result.document_ancestry.location_rows = count_limit;
    result.document_ancestry.origin_rows = count_limit;
    result.document_ancestry.source_name_bytes = input_limit;
    result.document_ancestry.tree_octets = input_limit;
    result.document_ancestry.metadata_octets = input_limit;
    result.document_ancestry.index_entries = count_limit;
    result.document_ancestry.pointer_scratch_bytes = input_limit;
    result.document_ancestry.canonical_bytes = input_limit;
    result.document_ancestry.retained_bytes = retained_limit;
    result.document_ancestry.consumer_simultaneous_bytes = leaf_logical;
    result.resource_service.worker_sets = 256;
    result.resource_service.worker_pus = 4096;
    result.resource_service.bindings = 64;
    result.resource_service.templates = 64;
    result.resource_service.selector_numbers = 4096;
    result.resource_service.selector_names = 4096;
    result.resource_service.pools = 4096;
    result.resource_service.service_pus = 4096;
    result.resource_service.reserved_cores = 4096;
    result.resource_service.cpu_members = 4096;
    result.resource_service.text_bytes = input_limit;
    result.resource_service.maximum_text = count_limit;
    result.resource_service.range_components = count_limit;
    result.resource_service.cpu_list_bytes = input_limit;
    result.resource_service.identity_entries = 4096;
    result.resource_service.smt_entries = 4096;
    result.resource_service.canonical_bytes = input_limit;
    result.resource_service.retained_bytes = retained_limit;
    result.resource_service.consumer_simultaneous_bytes = leaf_logical;
    result.memory_placement.comparisons = 64;
    result.memory_placement.templates = 64;
    result.memory_placement.nodes = 4096;
    result.memory_placement.set_members = count_limit;
    result.memory_placement.mappings = count_limit;
    result.memory_placement.text_bytes = input_limit;
    result.memory_placement.maximum_text = count_limit;
    result.memory_placement.range_components = count_limit;
    result.memory_placement.canonical_bytes = input_limit;
    result.memory_placement.staged_bytes = leaf_logical;
    result.memory_placement.retained_bytes = retained_limit;
    result.memory_placement.consumer_simultaneous_bytes = leaf_logical;
    result.kernel_dispatch.workloads = 64;
    result.kernel_dispatch.bindings = 64;
    result.kernel_dispatch.constraints = count_limit;
    result.kernel_dispatch.text_bytes = input_limit;
    result.kernel_dispatch.maximum_text = count_limit;
    result.kernel_dispatch.canonical_bytes = input_limit;
    result.kernel_dispatch.staged_bytes = leaf_logical;
    result.kernel_dispatch.retained_bytes = retained_limit;
    result.kernel_dispatch.consumer_simultaneous_bytes = leaf_logical;
    result.comparison_experiments.experiments = 64;
    result.comparison_experiments.workers = 256;
    result.comparison_experiments.variants = count_limit;
    result.comparison_experiments.nodes = 4096;
    result.comparison_experiments.set_members = count_limit;
    result.comparison_experiments.mappings = count_limit;
    result.comparison_experiments.candidates = 4096;
    result.comparison_experiments.cpu_members = 4096;
    result.comparison_experiments.cpu_ranges = count_limit;
    result.comparison_experiments.maximum_cpu_members = count_limit;
    result.comparison_experiments.cpu_input_bytes = input_limit;
    result.comparison_experiments.text_bytes = input_limit;
    result.comparison_experiments.maximum_text = count_limit;
    result.comparison_experiments.range_components = count_limit;
    result.comparison_experiments.canonical_bytes = input_limit;
    result.comparison_experiments.staged_bytes = leaf_logical;
    result.comparison_experiments.retained_bytes = retained_limit;
    result.comparison_experiments.consumer_simultaneous_bytes = leaf_logical;
    result.matrix_experiments.experiments = 64;
    result.matrix_experiments.worker_axis_nodes = 4096;
    result.matrix_experiments.memory_axis_nodes = 4096;
    result.matrix_experiments.rows = 64;
    result.matrix_experiments.workers = 256;
    result.matrix_experiments.cells = 64;
    result.matrix_experiments.placement_nodes = 4096;
    result.matrix_experiments.set_members = count_limit;
    result.matrix_experiments.edges = 64;
    result.matrix_experiments.cpu_members = 4096;
    result.matrix_experiments.cpu_ranges = count_limit;
    result.matrix_experiments.maximum_cpu_members = count_limit;
    result.matrix_experiments.cpu_input_bytes = input_limit;
    result.matrix_experiments.text_bytes = input_limit;
    result.matrix_experiments.maximum_text = count_limit;
    result.matrix_experiments.range_components = count_limit;
    result.matrix_experiments.canonical_bytes = input_limit;
    result.matrix_experiments.staged_bytes = leaf_logical;
    result.matrix_experiments.retained_bytes = retained_limit;
    result.matrix_experiments.consumer_simultaneous_bytes = leaf_logical;
    result.scenario_projection.nodes = result.resolved_projection.nodes = count_limit;
    result.scenario_projection.objects = result.resolved_projection.objects = count_limit;
    result.scenario_projection.arrays = result.resolved_projection.arrays = count_limit;
    result.scenario_projection.object_members = result.resolved_projection.object_members = count_limit;
    result.scenario_projection.string_values = result.resolved_projection.string_values = count_limit;
    result.scenario_projection.key_bytes = result.resolved_projection.key_bytes = input_limit;
    result.scenario_projection.string_bytes = result.resolved_projection.string_bytes = input_limit;
    result.scenario_projection.depth = result.resolved_projection.depth = 64;
    result.scenario_projection.canonical_bytes = result.resolved_projection.canonical_bytes = input_limit;
    result.scenario_projection.utf32_reserved_elements = result.resolved_projection.utf32_reserved_elements = count_limit;
    result.scenario_projection.utf16_elements = result.resolved_projection.utf16_elements = count_limit;
    result.scenario_projection.canonical_member_elements = result.resolved_projection.canonical_member_elements = count_limit;
    result.scenario_projection.quoted_key_bytes = result.resolved_projection.quoted_key_bytes = input_limit;
    result.scenario_projection.path_bytes = result.resolved_projection.path_bytes = input_limit;
    result.scenario_projection.index_decimal_bytes = result.resolved_projection.index_decimal_bytes = input_limit;
    result.scenario_projection.ordered_staging_bytes = result.resolved_projection.ordered_staging_bytes = leaf_logical;
    result.scenario_projection.unordered_clone_bytes = result.resolved_projection.unordered_clone_bytes = leaf_logical;
    result.scenario_projection.projector_auxiliary_bytes = result.resolved_projection.projector_auxiliary_bytes = leaf_logical;
    result.scenario_projection.fixed_bytes = result.resolved_projection.fixed_bytes = leaf_logical;
    result.scenario_projection.error_bytes = result.resolved_projection.error_bytes = leaf_logical;
    result.scenario_projection.simultaneous_bytes = result.resolved_projection.simultaneous_bytes = leaf_logical;
    result.scenario_projection.work_units = result.resolved_projection.work_units = logical_work;
    result.projection_input = epycbench::scenario::detail::projection_input_supported_limits;
    result.input_bytes = result.canonical_scenario_bytes = result.canonical_resolved_bytes = result.envelope_bytes = input_limit;
    result.association_entries = count_limit;
    result.retained_bytes = retained_limit;
    result.consumer_simultaneous_bytes = plan_logical;
    result.association_work = logical_work;
    return result;
}

auto error_limits() -> results::error_serialization::Limits {
    results::error_serialization::Limits out;
    out.context_nodes = 4096;
    out.context_depth = 16;
    out.string_bytes = input_limit;
    out.binary_bytes = input_limit;
    out.targets = 65536;
    out.nodes = count_limit;
    out.depth = 7;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.retained_bytes = retained_limit;
    out.consumer_simultaneous_bytes = retained_limit;
    return out;
}

auto topology_limits() -> results::topology_serialization::Limits {
    results::topology_serialization::Limits out;
    out.nodes = count_limit;
    out.depth = 4;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.topology_decode_bytes = input_limit;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.child = error_limits();
    out.topology = results::topology_evidence::supported_limits();
    return out;
}

auto embedded_topology_limits() -> results::topology_serialization::Limits {
    auto out = topology_limits();
    // The category reserves this child plus its own decoded/backing stock.
    out.topology_preparation_bytes = 512U * mib;
    out.error_preparation_bytes = 512U * mib;
    out.publication_bytes = 512U * mib;
    return out;
}

auto source_limits() -> results::source_serialization::Limits {
    results::source_serialization::Limits out;
    out.source_bytes = input_limit;
    out.name_bytes = input_limit;
    out.override_count = 4096;
    out.override_bytes = input_limit;
    out.source_retained_bytes = retained_limit;
    out.nodes = count_limit;
    out.depth = 4;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.error_preparation_bytes = leaf_logical;
    out.source_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.child = error_limits();
    return out;
}

auto base_limits() -> results::base_build_serialization::Limits {
    results::base_build_serialization::Limits out;
    out.nodes = count_limit;
    out.depth = 3;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.manifest_decode_bytes = input_limit;
    out.manifest_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.child = error_limits();
    out.manifest = {.features = 4096, .field_bytes = input_limit,
        .string_bytes = input_limit, .preimage_bytes = input_limit,
        .canonical_bytes = input_limit, .total_bytes = retained_limit};
    return out;
}

auto controls_limits() -> results::system_controls_serialization::Limits {
    results::system_controls_serialization::Limits out;
    out.nodes = count_limit;
    out.depth = 5;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.controls_input_bytes = input_limit;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.controls_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.controls = {.observations = 5, .retained_bytes = retained_limit, .canonical_bytes = input_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto kernel_limits() -> results::kernel_identity_serialization::Limits {
    results::kernel_identity_serialization::Limits out;
    out.nodes = count_limit;
    out.depth = 5;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.identity_input_bytes = input_limit;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.identity_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.kernel_identity = {.observations = 3, .field_bytes = 64, .reported_bytes = 133,
        .canonical_bytes = input_limit, .retained_bytes = retained_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto pu_limits() -> results::pu_identity_serialization::Limits {
    results::pu_identity_serialization::Limits out;
    out.rows = 1024;
    out.observations = 2048;
    out.error_children = 2048;
    out.nodes = count_limit;
    out.depth = 6;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.binding_bytes = leaf_logical;
    out.identity_input_bytes = input_limit;
    out.row_staging_bytes = leaf_logical;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.identity_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.identity = {.processing_units = 1024, .observations = 2048,
        .canonical_bytes = input_limit, .retained_bytes = retained_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto frequency_limits() -> results::frequency_policy_serialization::Limits {
    results::frequency_policy_serialization::Limits out;
    out.policies = 4096;
    out.memberships = 16384;
    out.online_memberships = 16384;
    out.uncovered = 4096;
    out.error_children = 16384;
    out.nodes = count_limit;
    out.depth = 5;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.binding_bytes = leaf_logical;
    out.source_input_bytes = input_limit;
    out.row_staging_bytes = leaf_logical;
    out.identity_index_bytes = leaf_logical;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.source_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.source = {.policies = 4096, .memberships = 16384, .uncovered = 4096,
        .label_bytes = 4096, .total_label_bytes = input_limit,
        .canonical_bytes = input_limit, .retained_bytes = retained_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto anonymous_limits() -> results::anonymous_thp_policy_serialization::Limits {
    results::anonymous_thp_policy_serialization::Limits out;
    out.rows = 54;
    out.error_children = 55;
    out.nodes = count_limit;
    out.depth = 5;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.source_input_bytes = input_limit;
    out.row_staging_bytes = leaf_logical;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.source_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.source = {.rows = 54, .canonical_bytes = input_limit, .retained_bytes = retained_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto process_limits() -> results::process_thp_restriction_serialization::Limits {
    results::process_thp_restriction_serialization::Limits out;
    out.observations = 1;
    out.error_children = 1;
    out.nodes = count_limit;
    out.depth = 4;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.source_input_bytes = input_limit;
    out.topology_preparation_bytes = leaf_logical;
    out.error_preparation_bytes = leaf_logical;
    out.source_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.source = {.observations = 1, .canonical_bytes = input_limit, .retained_bytes = retained_limit};
    out.topology = embedded_topology_limits(); out.child = error_limits();
    return out;
}

auto host_limits() -> results::host_serialization::Limits {
    results::host_serialization::Limits out;
    out.categories = 7;
    out.error_references = 65536;
    out.nodes = count_limit;
    out.depth = 3;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.identity_index_bytes = plan_logical;
    out.publication_bytes = plan_logical;
    out.retained_bytes = retained_limit;
    out.child_preparation_bytes.fill(plan_logical);
    out.topology = topology_limits(); out.system_controls = controls_limits();
    out.kernel_identity = kernel_limits(); out.pu_identity = pu_limits();
    out.frequency = frequency_limits(); out.anonymous_thp = anonymous_limits();
    out.process_thp = process_limits();
    return out;
}

auto creation_limits() -> results::run_creation_serialization::Limits {
    results::run_creation_serialization::Limits out;
    out.artifact_slots = 3;
    out.failure_facts = 4096;
    out.targets = 65536;
    out.bindings = 65536;
    out.consumed_references = 65536;
    out.nodes = count_limit;
    out.depth = 5;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.table_preparation_bytes = creation_logical;
    out.error_preparation_bytes = creation_logical;
    out.identity_index_bytes = creation_logical;
    out.publication_bytes = creation_logical;
    out.retained_bytes = retained_limit;
    out.child_preparation_bytes.fill(creation_logical);
    out.source = source_limits(); out.base_build = base_limits();
    out.host = host_limits(); out.error = error_limits();
    return out;
}

auto normalization_limits() -> results::normalization_serialization::Limits {
    results::normalization_serialization::Limits out;
    out.input_bytes = input_limit;
    out.nodes = count_limit;
    out.depth = 64;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.source_preparation_bytes = creation_logical;
    out.plan_preparation_bytes = creation_logical;
    out.topology_preparation_bytes = creation_logical;
    out.association_entries = count_limit;
    out.association_bytes = creation_logical;
    out.association_work = logical_work;
    out.publication_bytes = creation_logical;
    out.retained_bytes = retained_limit;
    out.source = source_limits(); out.plan = plan_limits(); out.topology = topology_limits();
    return out;
}

auto outcome_limits() -> results::source_outcome_serialization::Limits {
    results::source_outcome_serialization::Limits out;
    out.diagnostics = 4096;
    out.diagnostic_bytes = input_limit;
    out.nodes = count_limit;
    out.depth = 3;
    out.text_bytes = input_limit;
    out.canonical_bytes = input_limit;
    out.provenance_bytes = 4096;
    out.source_preparation_bytes = leaf_logical;
    out.upstream_preparation_bytes = leaf_logical;
    out.publication_bytes = leaf_logical;
    out.retained_bytes = retained_limit;
    out.source = source_limits(); out.upstream = error_limits();
    return out;
}

auto composition_limits() -> ep::CompositionLimits {
    ep::CompositionLimits out;
    out.pairs = 64; out.events = run_records;
    out.retained = retained_limit; out.simultaneous = std::size_t{16} << 30U;
    out.planning = {.bytes = input_limit, .nodes = count_limit, .depth = 64,
        .text = input_limit, .workloads = 64, .bindings = 64, .pairs = 64,
        .retained = retained_limit, .simultaneous = plan_logical,
        .base = base_limits(),
        .report = {.input_bytes = input_limit, .string_bytes = input_limit,
            .integer_digits = input_limit, .nodes = count_limit, .entries = 65536,
            .depth = 64, .retained_bytes = retained_limit},
        .approval = {.input_bytes = input_limit, .string_bytes = input_limit,
            .integer_digits = input_limit, .nodes = count_limit, .entries = 65536,
            .depth = 64, .retained_bytes = retained_limit, .scanner_bytes = retained_limit}};
    out.preflight = {.bytes = input_limit, .nodes = count_limit, .depth = 64,
        .text = input_limit, .retained = retained_limit, .simultaneous = leaf_logical,
        .evaluator = {}, .monitoring = {.checks = 4096, .retained_bytes = retained_limit,
            .canonical_bytes = input_limit}};
    auto& evaluator = out.preflight.evaluator;
    evaluator.records = 1; evaluator.scopes = 4096; evaluator.checks = 65536;
    evaluator.worker_associations = 65536; evaluator.cache_memberships = 65536;
    evaluator.target_nodes = 4096; evaluator.requirement_tokens = 65536;
    evaluator.temperatures = 4096; evaluator.timestamps = 4096;
    evaluator.warnings = 65536; evaluator.overrides = 4096; evaluator.failures = 4096;
    evaluator.text_bytes = input_limit; evaluator.canonical_child_bytes = input_limit;
    evaluator.retained_bytes = retained_limit; evaluator.consumer_simultaneous_bytes = leaf_logical;
    out.context = {.bytes = input_limit, .nodes = count_limit, .depth = 64,
        .text = input_limit, .workers = 256, .targets = 256,
        .retained = retained_limit, .simultaneous = leaf_logical,
        .capacity = {.cpus = 4096, .domains = 4096, .workers = 256,
            .text = input_limit, .retained = retained_limit, .simultaneous = leaf_logical,
            .size_max = SIZE_MAX, .byte_container_max = std::vector<std::byte>{}.max_size(),
            .node_container_max = std::vector<std::uint64_t>{}.max_size(), .parser_depth_max = 64},
        .association = {}};
    auto& association = out.context.association;
    association.records = 4096; association.workers = 256; association.node_targets = 256;
    association.service_candidates = 4096; association.compared_bytes = input_limit;
    association.work = logical_work; association.preparation_bytes = leaf_logical;
    auto& pair = out.pair;
    pair.blocks = 1024; pair.observations = 4096; pair.events = 4096;
    pair.retained_bytes = retained_limit; pair.simultaneous_bytes = leaf_logical;
    pair.component = {.events = 6, .canonical_bytes = input_limit, .nodes = count_limit,
        .strings = input_limit, .depth = 64, .workers = 256, .array_elements = count_limit,
        .frequency_cpu_memberships = 65536, .typed_bytes = retained_limit,
        .simultaneous_bytes = leaf_logical};
    pair.root_bytes = input_limit; pair.root_nodes = count_limit;
    pair.root_strings = input_limit; pair.root_elements = count_limit; pair.root_depth = 64;
    pair.root = {.typed = retained_limit, .scratch = retained_limit,
        .ledger = retained_limit, .peak = leaf_logical, .blocks = 1024,
        .workers = 256, .memberships = 65536, .candidates = 4096, .trials = 65536,
        .targets = 256, .pages = 65536};
    pair.terminal_errors = 8;
    pair.finalization = {.workers = 256, .targets = 256, .pages = 65536,
        .typed_payload = retained_limit, .reconstruction_scratch = retained_limit};
    pair.readiness = {.workers = 256, .strings = input_limit};
    pair.memory_preparation = {.workers = 256, .targets = 256, .pages = 65536,
        .strings = input_limit, .typed_payload = retained_limit};
    pair.control_strings = input_limit;
    pair.monitoring_preparation = {.workers = 256, .domains = 4096,
        .memberships = 65536, .strings = input_limit};
    pair.preparation_outcomes = {.candidates = 4096, .trials = 65536,
        .calibration_payload = retained_limit, .calibration_scratch = retained_limit,
        .warmup_payload = retained_limit, .warmup_scratch = retained_limit};
    out.timing = {.records = run_observations, .utc_bytes = ep::detail::multiply(run_observations, 30), .error_nodes = 4096,
        .error_depth = 16, .error_string_bytes = input_limit, .error_binary_bytes = input_limit,
        .error_retained_bytes = retained_limit, .retained_bytes = retained_limit,
        .simultaneous_bytes = leaf_logical};
    out.error = error_limits();
    out.analysis.values = {.records = 4096, .canonical_bytes = input_limit,
        .input_bytes = input_limit, .root_bytes = input_limit, .root_nodes = count_limit,
        .root_strings = input_limit, .root_elements = count_limit, .root_depth = 64,
        .blocks = 1024, .observations = 4096, .historical = 4096,
        .comparison_replicates = 100000, .stopping_replicates = 100000,
        .result_bytes = retained_limit, .workspace_bytes = retained_limit,
        .retained_bytes = retained_limit, .simultaneous_bytes = leaf_logical};
    out.analysis.root = pair.root;
    out.analysis.serialization = {.output_bytes = input_limit, .depth = 64, .nodes = count_limit};

    // These callers reserve configured children plus their own live state. Keep
    // the same checked shape helpers and actual declared cell types as replay.
    const auto add = ep::detail::add;
    const auto multiply = ep::detail::multiply;
    const auto shape = [](const auto& limits) -> ep::detail::Shape {
        return {limits.nodes, limits.nodes, limits.text, limits.depth, limits.bytes};
    };
    auto& planning = out.planning;
    const auto plan_rows = add(add(multiply(planning.workloads, sizeof(results::implementation_selection::Workload)),
        multiply(planning.bindings, sizeof(results::implementation_selection::Binding))),
        multiply(planning.pairs, sizeof(results::execution_plan::Pair)));
    const auto plan_children = add(planning.base.publication_bytes,
        add(multiply(planning.approval.retained_bytes, 2),
        add(multiply(planning.report.retained_bytes, 2), multiply(planning.approval.scanner_bytes, 2))));
    const auto projection_rows = add(add(planning.workloads, planning.bindings), add(planning.pairs, 16));
    const ep::detail::Shape projection{multiply(projection_rows, 64), multiply(projection_rows, 64),
        multiply(planning.bytes, multiply(projection_rows, 2)), planning.depth, 0};
    planning.simultaneous = add(sizeof(ep::PlanFacts),
        add(multiply(ep::detail::workspace(shape(planning)), 8),
        add(plan_rows, add(plan_children, ep::detail::json_stock(projection)))));

    auto& preflight = out.preflight;
    const auto preflight_rows = add(add(evaluator.checks, evaluator.temperatures),
                                   add(evaluator.overrides, evaluator.failures));
    const auto declaration = add(sizeof(results::preflight::TemperatureFact),
        add(sizeof(results::preflight::OverrideAttempt), add(sizeof(results::preflight::CheckDeclaration),
        add(sizeof(results::preflight::FailureReference), sizeof(results::journal::Timestamp)))));
    preflight.simultaneous = add(multiply(ep::detail::workspace(shape(preflight)), 4),
        add(multiply(preflight_rows, declaration), add(evaluator.consumer_simultaneous_bytes,
        add(preflight.monitoring.retained_bytes, preflight.monitoring.canonical_bytes))));

    auto& context = out.context;
    context.simultaneous = add(topology_limits().topology_decode_bytes,
        add(multiply(ep::detail::workspace(shape(context)), 12),
        add(multiply(context.workers, sizeof(results::block_evidence::ExpectedMemoryCell) * 8U),
        add(multiply(context.targets, sizeof(runtime::ipc::PlacementNumaTarget) * 8U),
        add(context.capacity.simultaneous, context.association.preparation_bytes)))));
    const auto allowance = add(sizeof(ep::detail::CompositionState),
        add(multiply(out.pairs, sizeof(std::shared_ptr<const ep::detail::ComposedPair>)),
            add(planning.retained, preflight.retained)));
    // Whole-Run contexts and PairPrefix owners coexist across admitted pairs.
    // This is an explicit aggregate caller stock, independent of leaf limits.
    const auto pair_inventory = multiply(document_limit, 2U);
    out.retained = add(allowance, pair_inventory);
    const auto pair_stock = add(sizeof(ep::detail::ComposedPair), add(context.retained,
        add(context.text, add(pair.retained_bytes, multiply(out.events, sizeof(std::uint64_t) * 4U)))));
    const auto compose_peak = add(allowance, add(std::max(planning.simultaneous, preflight.simultaneous),
        add(pair_stock, add(context.simultaneous, add(pair.simultaneous_bytes, out.analysis.values.simultaneous_bytes)))));
    const auto child_peak = add(out.retained, add(context.simultaneous,
        add(pair.simultaneous_bytes, out.error.consumer_simultaneous_bytes)));
    const auto replay_peak = add(allowance, add(context.retained,
        add(pair.simultaneous_bytes, multiply(add(out.events, 1), sizeof(std::uint64_t) * 4U))));
    out.simultaneous = std::max({compose_peak, child_peak, replay_peak});
    return out;
}

auto select_profile(std::string_view bytes) -> std::optional<ep::Version> {
    ep::Limits limits;
    limits.body_bytes = document_limit;
    limits.nodes = 2U * count_limit;
    limits.depth = 64;
    limits.text_bytes = document_limit;
    limits.canonical_bytes = document_limit;
    limits.candidate_bytes = ep::detail::add(retained_limit,
        ep::detail::add(document_limit, input_limit));
    limits.work = 1U << 30U;
    ep::detail::Work work{0, limits.work, 0, limits.candidate_bytes};
    // Selection needs an ordinary DOM; reduced export keeps its downstream
    // limits. The shared SAX owner is active before any tree insertion,
    // including failure during ordinary untimed allocation.
    auto tree = ep::detail::parse_tree_dom(bytes, limits, work);
    core::detail::JsonCleanup cleanup{tree};
    if (!tree.is_object() || !tree.contains("schema") || !tree.contains("journal") ||
        !tree.contains("extensions")) return {};
    const auto& schema = tree.at("schema");
    const auto& journal = tree.at("journal");
    const auto& extensions = tree.at("extensions");
    if (!schema.is_object() || schema.size() != 4 || !journal.is_object() ||
        !extensions.is_object() || !extensions.contains("epycbench.private_complete")) return {};
    const auto equal = [](const nlohmann::json& object, std::string_view key,
                          std::string_view value) {
        const auto found = object.find(key);
        return found != object.end() && found->is_string() &&
               found->get_ref<const std::string&>() == value;
    };
    if (!equal(schema, "name", "epycbench.result") ||
        !equal(schema, "json_schema_dialect", "https://json-schema.org/draft/2020-12/schema") ||
        !schema.contains("major") || !schema.at("major").is_number_unsigned() ||
        schema.at("major") != 1 || !schema.contains("revision") ||
        !schema.at("revision").is_number_unsigned()) return {};
    const auto& extension = extensions.at("epycbench.private_complete");
    if (!extension.is_object()) return {};
    if (schema.at("revision") == 0 && equal(journal, "profile", ep::schema_name) &&
        equal(extension, "profile", "epycbench.private-complete/v1"))
        return ep::Version::supplied_v1;
    if (schema.at("revision") == 1 && equal(journal, "profile", ep::progress_schema_name) &&
        equal(extension, "profile", "epycbench.private-complete/v2"))
        return ep::Version::progress_v2;
    if (schema.at("revision") == 2 && equal(journal, "profile", ep::compact_schema_name) &&
        equal(extension, "profile", "epycbench.private-complete/v3"))
        return ep::Version::compact_v3;
    return {};
}
} // namespace

auto results_validation_limits(ep::Version version) -> fd::Limits {
    fd::Limits out;
    out.input_bytes = document_limit; out.output_bytes = document_limit;
    out.nodes = 8U * count_limit; out.depth = 64;
    out.text_bytes = ep::detail::multiply(document_limit, 2U);
    out.records = run_records; out.experiments = 64; out.pairs = 64;
    out.blocks = run_blocks; out.observations = run_observations;
    out.samples = run_samples; out.references = run_samples;
    out.retained_bytes = document_limit + mib;
    out.analysis = {.output_bytes = input_limit, .depth = 64, .nodes = count_limit};
    auto& events = out.events;
    events.body_bytes = input_limit; events.nodes = count_limit;
    events.depth = 64; events.text_bytes = document_limit; events.canonical_bytes = document_limit;
    events.origin_entries = run_samples; events.references = run_samples; events.records = run_records;
    events.version = version;
    events.child.creation = creation_limits();
    events.child.normalization = normalization_limits();
    events.child.source_outcome = outcome_limits();
    events.child.topology = topology_limits(); events.child.error = error_limits();
    events.composition = std::make_shared<const ep::CompositionLimits>(composition_limits());
    const auto add = ep::detail::add;
    const auto multiply = ep::detail::multiply;
    const auto workspace = ep::detail::workspace({.nodes = count_limit, .members = count_limit,
        .text = input_limit, .depth = 64, .bytes = input_limit});
    std::size_t allowance{};
    for (const auto kind : {ep::Kind::creation, ep::Kind::normalization,
                           ep::Kind::source_outcome, ep::Kind::topology, ep::Kind::failure})
        allowance = std::max(allowance, ep::detail::child_allowance(kind, events.child));
    events.candidate_bytes = add(add(workspace, allowance), events.composition->simultaneous);
    events.retained_bytes = add(add(multiply(out.records, retained_limit), input_limit),
                                events.composition->retained);
    events.simultaneous_bytes = add(add(multiply(2, events.retained_bytes), events.candidate_bytes),
                                    multiply(2, add(input_limit, 1)));
    events.work = multiply(out.records, events.candidate_bytes);
    const auto document = add(multiply(out.output_bytes, 4),
                              multiply(out.nodes, sizeof(nlohmann::json) * 4));
    out.simultaneous_bytes = add(add(add(events.simultaneous_bytes, out.input_bytes), document),
                                creation_logical);
    // C4 preflights all retained pair populations before each embedded record,
    // then projects the assembly and validation documents. A single-frame tail
    // cannot cover those whole-prefix visits. Keep child replay work separate.
    const auto outer_phase_work = add(events.retained_bytes, document_limit);
    out.work = add(events.work, multiply(add(out.records, 2U), outer_phase_work));
    return out;
}

auto run_results_command(int argc, char* argv[], topology::cli::Environment& environment,
                         std::ostream& output, std::ostream& error) -> int {
    const bool exporting = argc > 2 && argv && argv[2] && std::string_view{argv[2]} == "export";
    const auto reject = [&](int status, Diagnostic diagnostic) { return fail(error, status, diagnostic, exporting); };
    try {
        CLI::App app{"Offline Results validation and export", "epycbench"};
        app.require_subcommand(1, 1);
        auto* result = app.add_subcommand("result");
        result->require_subcommand(1, 1);
        auto* validate = result->add_subcommand("validate");
        auto* export_command = result->add_subcommand("export");
        std::string path, selected_profile;
        validate->add_option("FILE", path)->required();
        export_command->add_option("FILE", path)->required();
        export_command->add_option("--profile", selected_profile)->required()->expected(1);

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& exception) {
            if (exception.get_exit_code() == 0) return emit(output, error, help, exporting);
            return reject(2, Diagnostic::usage);
        }
        const auto selected = results::result_export::profile(selected_profile);
        if ((!*validate && !*export_command) || path.empty() || path == "-" ||
            (*export_command && (!selected || export_command->count("--profile") != 1)))
            return reject(2, Diagnostic::usage);
        if (!environment.read_file) return reject(1, Diagnostic::internal_failure);
        auto bytes = environment.read_file(std::filesystem::path{path});
        if (!bytes) {
            if (bytes.error().code == "topology.serialization.limit_exceeded")
                return reject(9, Diagnostic::limit_exceeded);
            return reject(3, Diagnostic::input_io);
        }
        if (bytes->size() > document_limit) return reject(9, Diagnostic::limit_exceeded);
        const auto profile = select_profile(*bytes);
        if (!profile) return reject(9, Diagnostic::unsupported_profile);
        auto document = fd::validate(*bytes, results_validation_limits(*profile));
        if (!document) {
            const auto kind = document.error().kind;
            if (kind == fd::Error::allocation) return reject(1, Diagnostic::internal_failure);
            if (kind == fd::Error::limits || kind == fd::Error::capacity ||
                kind == fd::Error::overflow) return reject(9, Diagnostic::limit_exceeded);
            return reject(9, Diagnostic::invalid_document);
        }
        if (*validate) return emit(output, error, "result validate: valid\n");
        // Retire the ordinary file copy before parsing canonical validated bytes.
        std::string{}.swap(*bytes);
        if (*selected == results::result_export::Profile::complete)
            return emit(output, error, document->canonical(), true);
        auto rendered = results::result_export::render(*document, *selected);
        if (!rendered) {
            using ExportError = results::result_export::Error;
            if (rendered.error() == ExportError::limits || rendered.error() == ExportError::overflow)
                return reject(9, Diagnostic::limit_exceeded);
            return reject(1, Diagnostic::internal_failure);
        }
        return emit(output, error, *rendered, true);
    } catch (ep::Error failure) {
        return reject(9, failure == ep::Error::limits || failure == ep::Error::capacity
                                  ? Diagnostic::limit_exceeded : Diagnostic::invalid_document);
    } catch (const nlohmann::json::exception&) {
        return reject(9, Diagnostic::invalid_document);
    } catch (...) {
        return reject(1, Diagnostic::internal_failure);
    }
}
} // namespace epycbench::cli
