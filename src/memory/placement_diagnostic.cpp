#include "epycbench/memory/placement_diagnostic.hpp"

#include "memory/placement_diagnostic_access.hpp"
#include "memory/region_binding_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_placement_query_access.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::memory {
namespace {

[[nodiscard]] auto diagnostic_error(std::string_view code,
                                    std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::memory,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Memory placement diagnostic construction failed",
            .operation = "memory.placement_diagnostic.create",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

[[nodiscard]] auto invalid(std::string_view classification) -> core::Error {
    return diagnostic_error("memory.placement_diagnostic.invalid_argument", classification);
}

[[nodiscard]] auto invariant(std::string_view classification) -> core::Error {
    return diagnostic_error("memory.placement_diagnostic.invariant_failed", classification);
}

[[nodiscard]] auto same_evaluation(const RegionPlacementEvaluation& left,
                                   const RegionPlacementEvaluation& right) noexcept -> bool {
    return left.requested_node_pages() == right.requested_node_pages() &&
           left.unexpected_node_pages() == right.unexpected_node_pages() &&
           left.unresolved_pages() == right.unresolved_pages() &&
           left.observed_ratio() == right.observed_ratio();
}

[[nodiscard]] auto validate_observed(
    const RegionRequest& request, const RegionPlacementObservation& observation,
    const RegionPlacementEvaluation& evaluation,
    PlacementQualificationState qualification, std::string_view classification)
    -> std::optional<core::Error> {
    const auto counts = observation.resident_page_counts();
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index].page_count() == 0U ||
            (index != 0U && !(counts[index - 1].linux_node() < counts[index].linux_node()))) {
            return invariant("node_identity_inconsistent");
        }
    }
    auto expected = detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(
        request, observation);
    if (!expected || !same_evaluation(expected->evaluation, evaluation) ||
        expected->qualification_state != qualification) {
        return invariant(classification);
    }
    return std::nullopt;
}

[[nodiscard]] auto make_pre_run(const PlacementEvaluatedRegion& evaluated)
    -> std::expected<PreRunPlacementDiagnostic, core::Error> {
    const auto& observed = evaluated.region();
    const auto& observation = observed.observation();
    const auto& evaluation = evaluated.evaluation();
    const auto qualification = evaluated.qualification_state();
    if (observed.query_state() == PreRunPlacementQueryState::skipped) {
        if (observation || evaluation || qualification != PlacementQualificationState::skipped ||
            observed.request().verify_before()) {
            return std::unexpected{invalid("pre_run_state_inconsistent")};
        }
        return detail::RegionPlacementDiagnosticAccess::pre_run(
            PreRunDiagnosticState::skipped, std::nullopt, std::nullopt, std::nullopt);
    }
    if (observed.query_state() != PreRunPlacementQueryState::observed || !observation ||
        !evaluation || (qualification != PlacementQualificationState::qualified &&
                        qualification != PlacementQualificationState::not_qualified)) {
        return std::unexpected{invalid("pre_run_state_inconsistent")};
    }
    if (auto error = validate_observed(observed.request(), *observation, *evaluation,
                                       qualification, "pre_run_evaluation_inconsistent")) {
        return std::unexpected{std::move(*error)};
    }
    return detail::RegionPlacementDiagnosticAccess::pre_run(
        PreRunDiagnosticState::observed, *observation, *evaluation, qualification);
}

[[nodiscard]] auto validate_post_run(const RegionRequest& request,
                                     const PostRunPlacementEvidence& post_run)
    -> std::optional<core::Error> {
    const auto state = post_run.state();
    const auto& observation = post_run.observation();
    const auto& evaluation = post_run.evaluation();
    const auto qualification = post_run.qualification_state();
    if (state == PostRunVerificationState::not_applicable ||
        state == PostRunVerificationState::skipped) {
        if (observation || evaluation || qualification) {
            return invalid("post_run_state_inconsistent");
        }
        return std::nullopt;
    }
    if (state != PostRunVerificationState::observed || !observation || !evaluation ||
        !qualification || (*qualification != PlacementQualificationState::qualified &&
                           *qualification != PlacementQualificationState::not_qualified)) {
        return invalid("post_run_state_inconsistent");
    }
    return validate_observed(request, *observation, *evaluation, *qualification,
                             "post_run_evaluation_inconsistent");
}

[[nodiscard]] auto validate_protection(
    RegionFinalizationPath path,
    const std::optional<RegionProtectionEvidence>& protection)
    -> std::optional<core::Error> {
    if (path == RegionFinalizationPath::protected_execution) {
        if (!protection ||
            protection->protection_kind() != RegionProtectionKind::read_only ||
            protection->range() != RegionProtectionRange::full_mapping ||
            !protection->protection_accepted() || protection->writable() ||
            protection->executable()) {
            return invalid("protection_evidence_inconsistent");
        }
        return std::nullopt;
    }
    if (path != RegionFinalizationPath::unprotected_execution_ineligible &&
        path != RegionFinalizationPath::unprotected_protection_failed) {
        return invalid("finalization_path_inconsistent");
    }
    if (protection) return invalid("protection_evidence_inconsistent");
    return std::nullopt;
}

} // namespace

auto RegionPlacementDiagnostic::create(const FinalizedRegion& region) noexcept
    -> std::expected<RegionPlacementDiagnostic, core::Error> {
    try {
        const auto& evaluated = region.evaluated_region();
        const auto& observed = evaluated.region();
        const auto& initialized = observed.region();
        const auto& bound = initialized.region();
        const auto& mapping = detail::RegionMappingAccess::mapping(bound.mapping());
        if (mapping.active() || mapping.size() != 0U || !mapping.bytes().empty() ||
            mapping.protection() != platform_linux::MemoryProtection::none) {
            return std::unexpected{invalid("finalized_region_invalid")};
        }

        const auto path = region.finalization_path();
        const auto protection = region.protection_evidence();
        if (auto error = validate_protection(path, protection)) {
            return std::unexpected{std::move(*error)};
        }
        if (path == RegionFinalizationPath::unprotected_execution_ineligible &&
            evaluated.qualification_state() != PlacementQualificationState::not_qualified) {
            return std::unexpected{invalid("finalization_path_inconsistent")};
        }
        if (path == RegionFinalizationPath::unprotected_protection_failed &&
            !evaluated.execution_eligible()) {
            return std::unexpected{invalid("finalization_path_inconsistent")};
        }
        if (path != RegionFinalizationPath::protected_execution &&
            region.post_run_evidence().state() !=
                PostRunVerificationState::not_applicable) {
            return std::unexpected{invalid("post_run_state_inconsistent")};
        }

        const auto& cleanup = region.cleanup_evidence();
        if (!cleanup.release_attempted() || !cleanup.release_succeeded() ||
            cleanup.range() != RegionCleanupRange::full_mapping ||
            cleanup.mapping_active_after()) {
            return std::unexpected{invalid("cleanup_evidence_inconsistent")};
        }
        auto pre_run = make_pre_run(evaluated);
        if (!pre_run) return std::unexpected{std::move(pre_run.error())};
        if (auto error = validate_post_run(region.request(), region.post_run_evidence())) {
            return std::unexpected{std::move(*error)};
        }

        return detail::RegionPlacementDiagnosticAccess::diagnostic(
            region.request(), bound.binding_evidence(), initialized.initialization_evidence(),
            std::move(*pre_run), protection, region.post_run_evidence(), cleanup, path);
    } catch (const std::exception&) {
        return std::unexpected{diagnostic_error(
            "memory.placement_diagnostic.contained_exception", "contained_exception")};
    } catch (...) {
        return std::unexpected{diagnostic_error(
            "memory.placement_diagnostic.contained_unknown_failure",
            "contained_unknown_failure")};
    }
}

} // namespace epycbench::memory
