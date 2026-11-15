#include "runtime/finalized_evidence_wire_typed_stock.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
#include "monitoring/frequency_classifier_kernel.hpp"
#include "runtime/finalized_evidence_wire_schema.generated.hpp"
namespace epycbench::runtime::wire_v1::detail {
auto typed_preparation_outcome_stocks(EvidenceComponentKind kind, const nlohmann::json& value)
    -> PreparationOutcomeStocks {
    PreparationOutcomeStocks result;
    if (kind == EvidenceComponentKind::calibration) {
        result.candidates = value.at("candidate_trials").size();
        result.owned.add(5U, sizeof(measurement::EmptyHarnessTrial));
        result.owned.add(result.candidates, core::detail::json_string_equal(value.at("kind"), "bandwidth")
            ? sizeof(measurement::SequentialReadBandwidthCalibrationTrial)
            : sizeof(measurement::DependentLoadLatencyCalibrationTrial));
        result.scratch.add(result.candidates, sizeof(std::uint64_t));
    } else if (kind == EvidenceComponentKind::warmup) {
        result.trials = value.at("trials").size();
        result.owned.add(result.trials, sizeof(measurement::WarmupTrial));
        const auto& text = value.at("policy").at("stability_window").get_ref<const std::string&>();
        std::uint64_t window{};
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), window);
        if (code != std::errc{} || end != text.data() + text.size()) {
            result.scratch.valid = false;
        } else {
            const auto count = std::min(window, static_cast<std::uint64_t>(result.trials));
            if (count > SIZE_MAX) result.scratch.valid = false;
            else result.scratch.add(static_cast<std::size_t>(count), sizeof(std::uint64_t));
        }
    } else {
        result.owned.valid = false;
        result.scratch.valid = false;
    }
    return result;
}
auto typed_finalization_stocks(const nlohmann::json& finalization) -> FinalizationStocks {
    FinalizationStocks result;
    std::size_t largest_stable{};
    for (const auto side : {"first", "second"}) {
        const auto& final_workers = finalization.at(side).at("workers");
        result.workers.add(final_workers.size());
        result.owned.add(final_workers.size(), sizeof(FinalizedWorkerEvidence));
        result.scratch.add(final_workers.size(), 2U * sizeof(std::uint64_t));
        for (const auto& worker : final_workers) {
            const auto& region = worker.at("region");
            if (!core::detail::json_string_equal(region.at("kind"), "available")) continue;
            const auto& numa = region.at("request").at("numa_targets");
            result.targets.add(numa.size()); result.owned.add(numa.size(), sizeof(memory::RegionNumaTarget));
            for (const auto& target : numa) {
                const auto length = target.at("stable_node").get_ref<const std::string&>().size();
                result.owned.add(length); largest_stable = std::max(largest_stable, length);
            }
            for (const auto phase : {"pre_run", "post_run"}) {
                const auto& diagnostic = region.at(phase);
                if (diagnostic.is_null() || diagnostic.at("observation").is_null()) continue;
                const auto count = diagnostic.at("observation").at("resident_page_counts").size();
                result.pages.add(count); result.owned.add(count, sizeof(memory::LinuxNodePageCount));
            }
        }
    }
    result.scratch.add(largest_stable);
    return result;
}
auto typed_component_stocks(EvidenceComponentKind kind, const nlohmann::json& value) -> TypedStocks {
    TypedStocks result;
    if (kind == EvidenceComponentKind::block_plan) return result;
    if (kind == EvidenceComponentKind::failed_observation) {
        result.owned.add(value.at("worker_errors").size(), sizeof(runtime::FinalizedEvidenceError));
        return result;
    }
    const auto& measurement = value.at("measurement");
    result.workers = measurement.at("workers").size();
    const bool bandwidth = measurement.at("kind").get<std::string_view>() == "bandwidth";
    result.owned.add(result.workers, bandwidth ? sizeof(measurement::SequentialReadBandwidthWorkerObservation)
                                               : sizeof(measurement::DependentLoadLatencyWorkerObservation));
    result.owned.add(value.at("placements").size(), sizeof(measurement::WorkerPlacementValidityEvidence));
    result.owned.add(value.at("migrations").size(), sizeof(measurement::WorkerMigrationValidityEvidence));
    const auto& monitoring = value.at("monitoring");
    if (monitoring.is_null()) return result;
    result.scratch.add(result.workers, sizeof(core::WorkerId));
    const auto& classified = monitoring.at("evidence");
    const auto& raw = monitoring.at("raw");
    const auto& page = classified.at("page_faults");
    if (!page.is_null()) {
        result.owned.add(page.at("workers").size(), sizeof(monitoring::WorkerPageFaultEvidence));
        result.scratch.add(result.workers, sizeof(monitoring::PageFaultAcquisitionPair) +
            sizeof(ClassifierWorkerTuple<monitoring::PageFaultCount>));
    }
    const auto& context = classified.at("context_switches");
    if (!context.is_null()) {
        result.owned.add(context.at("workers").size(), sizeof(monitoring::WorkerContextSwitchEvidence));
        result.scratch.add(result.workers, sizeof(monitoring::ContextSwitchWorkerAcquisitionView) +
            sizeof(ClassifierWorkerTuple<monitoring::ContextSwitchCount>) +
            std::string_view{"linux-perf-context-switch-records"}.size());
    }
    const auto& frequency = classified.at("frequency");
    if (!frequency.is_null())
        result.owned.add(frequency.at("domains").size(), sizeof(monitoring::FrequencyDomainEvidence));
    using Raw = monitoring::MonitoringRawCaptureEvidence;
    result.owned.add(raw.at("page_fault_before").size(), sizeof(Raw::PageAcquisition));
    result.owned.add(raw.at("page_fault_after").size(), sizeof(Raw::PageAcquisition));
    result.owned.add(raw.at("context_switches").size(), sizeof(Raw::ContextAcquisition));
    result.owned.add(raw.at("temperature").size(), sizeof(Raw::TemperatureAcquisition));
    result.owned.add(raw.at("frequency").size(), sizeof(std::vector<monitoring::FrequencyAcquisition>) +
        sizeof(monitoring::FrequencyDomainAcquisitions));
    result.owned.add(raw.at("frequency_metadata").size(), sizeof(monitoring::FrequencyPolicyMetadata));
    if (!classified.at("temperature").is_null())
        result.scratch.add(raw.at("temperature").size(), sizeof(const Raw::TemperatureAcquisition*));
    std::size_t maximum_samples{};
    for (const auto& group : raw.at("frequency")) {
        const auto samples = group.at("acquisitions").size();
        result.owned.add(samples, sizeof(monitoring::FrequencyAcquisition));
        result.scratch.add(group.at("domain_id").at("cpus").size(), sizeof(core::LinuxCpuId));
        maximum_samples = std::max(maximum_samples, samples);
    }
    if (!frequency.is_null()) {
        result.scratch.add(maximum_samples, sizeof(const monitoring::FrequencyAcquisition*));
        result.scratch.add(raw.at("frequency").size(), sizeof(std::optional<monitoring::detail::FrequencyDomainClassification>));
    }
    // Closed registry shape identifies every repeated owned occurrence. No
    // deduplication of labels or domain vectors is permissible here.
    auto nested = [&](auto&& self, const nlohmann::json& node) -> void {
        if (node.is_object()) {
            for (auto i = node.begin(); i != node.end(); ++i) {
                if (i.key() == "domain_id") {
                    const auto count = i.value().at("cpus").size();
                    result.memberships.add(count);
                    result.owned.add(count, sizeof(core::LinuxCpuId));
                } else if (i.key() == "implementation_id" || i.key() == "scaling_driver" ||
                           i.key() == "scaling_governor") {
                    result.owned.add(i.value().get_ref<const std::string&>().size());
                } else if (i.key() == "events") {
                    result.owned.add(i.value().size(), sizeof(monitoring::ContextSwitchEvent));
                }
                self(self, i.value());
            }
        } else if (node.is_array()) for (const auto& child : node) self(self, child);
    };
    nested(nested, monitoring);
    return result;
}
namespace {
auto shape_failure(ReconstructionPath path, TypedReconstructionErrorCode code)
    -> std::unexpected<RootReconstructionFailure> {
    const auto overflow = path.overflowed();
    return std::unexpected{RootReconstructionFailure{
        overflow ? RootReconstructionFailureCode::diagnostic_overflow : RootReconstructionFailureCode::shape,
        {code, std::move(path)}}};
}
auto shape_node(std::uint32_t index, const nlohmann::json& value,
                ReconstructionPath path, std::size_t depth)
    -> std::expected<void, RootReconstructionFailure> {
    if (path.overflowed() || depth > 64U)
        return shape_failure(std::move(path), TypedReconstructionErrorCode::invalid_scalar);
    const auto& node = schema_nodes[index];
    if (node.kind == SchemaKind::scalar) {
        if (node.scalar_class == ScalarClass::boolean ? value.is_boolean() : value.is_string()) return {};
        return shape_failure(std::move(path), TypedReconstructionErrorCode::invalid_scalar);
    }
    if (node.kind == SchemaKind::option)
        return value.is_null() ? std::expected<void, RootReconstructionFailure>{}
            : shape_node(node.child, value, std::move(path), depth);
    if (node.kind == SchemaKind::array) {
        if (!value.is_array()) return shape_failure(std::move(path), TypedReconstructionErrorCode::invalid_scalar);
        std::size_t ordinal{};
        for (const auto& child : value) {
            auto result = shape_node(node.child, child, path + "[" + ReconstructionPath::ordinal(ordinal++) + "]", depth + 1U);
            if (!result) return result;
        }
        return {};
    }
    if (!value.is_object()) return shape_failure(std::move(path), TypedReconstructionErrorCode::invalid_scalar);
    if (node.kind == SchemaKind::sum) {
        const auto selected = value.find(node.discriminator);
        if (selected == value.end()) return shape_failure(path + "." + node.discriminator, TypedReconstructionErrorCode::missing_member);
        if (!selected->is_string()) return shape_failure(path + "." + node.discriminator, TypedReconstructionErrorCode::invalid_scalar);
        const auto& token = selected->get_ref<const std::string&>();
        for (std::uint32_t i{}; i < node.edge_count; ++i) {
            const auto& edge = schema_edges[node.edge_begin + i];
            if (edge.token == token) return shape_node(edge.child, value, std::move(path), depth);
        }
        return shape_failure(path + "." + node.discriminator, TypedReconstructionErrorCode::invalid_scalar);
    }
    for (auto i = value.begin(); i != value.end(); ++i) {
        bool known{};
        for (std::uint32_t j{}; j < node.edge_count; ++j)
            known = known || schema_edges[node.edge_begin + j].key == i.key();
        if (!known) return shape_failure(path, TypedReconstructionErrorCode::invalid_scalar);
    }
    for (std::uint32_t i{}; i < node.edge_count; ++i) {
        const auto& edge = schema_edges[node.edge_begin + i];
        const auto child = value.find(edge.key);
        if (child == value.end()) {
            if (index == evidence_root && edge.key == "schema") continue;
            return shape_failure(path + "." + edge.key, TypedReconstructionErrorCode::missing_member);
        }
        if (index == evidence_root && edge.key == "finalization" && child->is_null()) continue;
        auto result = shape_node(edge.child, *child, path + "." + edge.key, depth + 1U);
        if (!result) return result;
    }
    return {};
}
} // namespace
auto check_root_reconstruction_shape(const nlohmann::json& value) noexcept
    -> std::expected<void, RootReconstructionFailure> {
    return shape_node(evidence_root, value, "root", 1U);
}
auto check_root_reconstruction_limits(const RootReconstructionUsage& u,
    const RootReconstructionLimits& l) noexcept -> std::expected<void, RootReconstructionFailure> {
    const std::array pairs{std::pair{u.typed,l.typed}, std::pair{u.scratch,l.scratch},
        std::pair{u.ledger,l.ledger}, std::pair{u.peak,l.peak}, std::pair{u.blocks,l.blocks},
        std::pair{u.workers,l.workers}, std::pair{u.memberships,l.memberships},
        std::pair{u.candidates,l.candidates}, std::pair{u.trials,l.trials},
        std::pair{u.targets,l.targets}, std::pair{u.pages,l.pages}};
    for (const auto& [observed, limit] : pairs) if (observed > limit)
        return std::unexpected{RootReconstructionFailure{RootReconstructionFailureCode::capacity,
            {TypedReconstructionErrorCode::invalid_scalar, "root.stock"}, limit, observed}};
    return {};
}
auto count_root_reconstruction(const nlohmann::json& root)
    -> std::expected<RootReconstructionUsage, RootReconstructionFailure> {
    if (auto shape = check_root_reconstruction_shape(root); !shape)
        return std::unexpected{std::move(shape.error())};
    RootReconstructionUsage result;
    TypedStockSum u, x, a, memberships, workers, targets, pages;
    u.add(sizeof(ReconstructedEvidenceHeader) + sizeof(ReconstructedCalibrationWarmup) +
        sizeof(ReconstructedObservationBlocks) + sizeof(ReconstructedStoppingAndFailures) +
        sizeof(ReconstructedTerminalAndFinalization));
    auto number = [](const nlohmann::json& value) -> std::optional<std::size_t> {
        const auto& text = value.get_ref<const std::string&>();
        std::size_t n{};
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), n);
        if (code != std::errc{} || end != text.data() + text.size()) return std::nullopt;
        return n;
    };
    auto arithmetic = [] { return std::unexpected{RootReconstructionFailure{
        RootReconstructionFailureCode::arithmetic,
        {TypedReconstructionErrorCode::invalid_scalar, "root.stock"}}}; };
    for (const auto key : {"scenario_profile", "scenario_digest", "resolved_plan_digest",
                           "workload_id", "first_variant_id", "second_variant_id"})
        u.add(root.at("provenance").at(key).get_ref<const std::string&>().size());
    for (const auto& [key, kind] : {
             std::pair{"calibration", EvidenceComponentKind::calibration},
             std::pair{"warmup", EvidenceComponentKind::warmup}}) {
        const auto& value = root.at(key);
        if (value.is_null()) continue;
        const auto stock = typed_preparation_outcome_stocks(kind, value);
        if (!stock.owned.valid || !stock.scratch.valid) return arithmetic();
        u.add(stock.owned.value); x.add(stock.scratch.value);
        result.candidates += stock.candidates;
        result.trials += stock.trials;
    }
    const auto& blocks = root.at("blocks"); result.blocks = blocks.size();
    u.add(result.blocks, sizeof(ReconstructedObservationBlock));
    for (const auto& block : blocks) {
        u.add(block.at("observations").size(), sizeof(FactualObservationEvidence));
        for (const auto& observation : block.at("observations")) {
            const auto stock = typed_component_stocks(EvidenceComponentKind::factual_observation, observation);
            u.add(stock.owned.value); x.add(stock.scratch.value);
            memberships.add(stock.memberships.value); workers.add(stock.workers);
            if (!stock.owned.valid || !stock.scratch.valid || !stock.memberships.valid) return arithmetic();
        }
        if (!block.at("failed_observation").is_null()) {
            u.add(sizeof(ReconstructedFailedObservation));
            u.add(block.at("failed_observation").at("worker_errors").size(), sizeof(FinalizedEvidenceError));
        }
    }
    u.add(root.at("stopping_evaluations").size(), sizeof(RunnerStoppingEvaluationEvidence));
    std::size_t error_count{};
    const auto& terminal = root.at("terminal");
    if (core::detail::json_string_equal(terminal.at("kind"), "operational")) {
        const auto secondary = terminal.at("secondary_errors").size();
        const auto count = add_wire_json_count(secondary, 1U);
        if (!count) return arithmetic();
        error_count = *count;
        u.add(secondary, sizeof(FinalizedEvidenceError));
    }
    const auto& finalization = root.at("finalization");
    if (!finalization.is_null()) {
        const auto stock = typed_finalization_stocks(finalization);
        if (!stock.owned.valid || !stock.scratch.valid || !stock.workers.valid ||
            !stock.targets.valid || !stock.pages.valid) return arithmetic();
        u.add(stock.owned.value); x.add(stock.scratch.value);
        x.add(error_count, sizeof(std::uint8_t));
        workers.add(stock.workers.value); targets.add(stock.targets.value); pages.add(stock.pages.value);
    }
    x.add(sizeof(FixedTypedReconstructionError) + sizeof(RootReconstructionUsage));
    const auto maximum = number(root.at("repetition_policy").at("maximum_blocks"));
    if (!maximum || *maximum > std::vector<PairwiseBlockAttemptEvidence>{}.max_size() ||
        *maximum > std::vector<RunnerStoppingEvaluationEvidence>{}.max_size()) return arithmetic();
    a.add(sizeof(PairwiseRunEvidenceLedger) + sizeof(FinalizedPairwiseRunEvidence));
    a.add(*maximum, sizeof(PairwiseBlockAttemptEvidence) + sizeof(RunnerStoppingEvaluationEvidence));
    TypedStockSum peak; peak.add(u.value); peak.add(x.value); peak.add(a.value);
    if (!u.valid || !x.valid || !a.valid || !peak.valid || !memberships.valid ||
        !workers.valid || !targets.valid || !pages.valid) return arithmetic();
    result.typed=u.value; result.scratch=x.value; result.ledger=a.value; result.peak=peak.value;
    result.memberships=memberships.value; result.workers=workers.value;
    result.targets=targets.value; result.pages=pages.value;
    return result;
}
} // namespace epycbench::runtime::wire_v1::detail
