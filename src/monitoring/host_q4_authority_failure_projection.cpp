#include "monitoring/host_q4_authority_failure_projection.hpp"

#include <system_error>

namespace epycbench::monitoring::qualification::detail {
namespace {
thread_local unsigned char fault{};

struct Identity final {
    const char* code;
    const char* operation;
};
auto identity(MonitoringQ4CompositionSource source) noexcept -> std::optional<Identity> {
    switch (source) {
    case MonitoringQ4CompositionSource::repository:
        return Identity{"monitoring.q1_plan_authority.cleanup_failed",
                        "authenticate_monitoring_q1_plan"};
    case MonitoringQ4CompositionSource::governed:
        return Identity{"monitoring.q4_plan_authority.repository_rejected",
                        "authenticate_monitoring_q4_plan"};
    case MonitoringQ4CompositionSource::predecessor:
        return Identity{"monitoring.q4.complete_q3_predecessor_authority.invalid",
                        "authenticate_monitoring_q4_complete_q3_predecessor_authority"};
    case MonitoringQ4CompositionSource::build:
        return Identity{"monitoring.q2.running_build.invalid",
                        "authenticate_monitoring_running_build"};
    case MonitoringQ4CompositionSource::contained:
        return std::nullopt;
    }
    return std::nullopt;
}
auto valid_cleanup(const MonitoringQ4CompositionFact& value) noexcept -> bool {
    if (value.source == MonitoringQ4CompositionSource::governed &&
        value.kind == MonitoringQ4CompositionFactKind::inactive)
        return value.detail == 0U && value.native_error == 0;
    if (value.kind != MonitoringQ4CompositionFactKind::cleanup || value.native_error < 0)
        return false;
    switch (value.source) {
    case MonitoringQ4CompositionSource::repository:
    case MonitoringQ4CompositionSource::build:
        return value.detail == 0U;
    case MonitoringQ4CompositionSource::predecessor:
        return value.detail < 20U;
    case MonitoringQ4CompositionSource::governed:
        return value.detail <= static_cast<unsigned int>(
                                   MonitoringGovernedTrackedBundleFailureKind::inactive) ||
               value.detail < MonitoringGovernedTrackedBundleCore::descriptor_count;
    case MonitoringQ4CompositionSource::contained:
        return false;
    }
    return false;
}
auto error(Identity value, int native) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{value.code},
            .message = "Monitoring Q4 authority cleanup failed",
            .operation = value.operation,
            .os_error = native > 0 ? std::optional{core::OsError::capture(
                                         std::error_code{native, std::generic_category()})}
                                   : std::nullopt,
            .context = {},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto stage_identity(MonitoringQ4AuthorityStage stage, MonitoringQ4CompositionSource source) noexcept
    -> std::optional<std::pair<MonitoringQ4ProjectedAuthorityPhase, const char*>> {
    switch (stage) {
    case MonitoringQ4AuthorityStage::repository:
    case MonitoringQ4AuthorityStage::current_revision:
    case MonitoringQ4AuthorityStage::history:
        return std::pair{MonitoringQ4ProjectedAuthorityPhase::repository,
                         "monitoring.q4.repository_invalid"};
    case MonitoringQ4AuthorityStage::governed_input:
    case MonitoringQ4AuthorityStage::plan_schema:
        return std::pair{MonitoringQ4ProjectedAuthorityPhase::collection_plan,
                         "monitoring.q4.collection_plan_invalid"};
    case MonitoringQ4AuthorityStage::predecessor:
        return std::pair{MonitoringQ4ProjectedAuthorityPhase::predecessor,
                         "monitoring.q4.predecessor_invalid"};
    case MonitoringQ4AuthorityStage::running_build:
        return std::pair{MonitoringQ4ProjectedAuthorityPhase::build_attestation,
                         "monitoring.q4.build_attestation_invalid"};
    case MonitoringQ4AuthorityStage::final_recheck:
        switch (source) {
        case MonitoringQ4CompositionSource::repository:
            return std::pair{MonitoringQ4ProjectedAuthorityPhase::repository,
                             "monitoring.q4.repository_invalid"};
        case MonitoringQ4CompositionSource::governed:
            return std::pair{MonitoringQ4ProjectedAuthorityPhase::collection_plan,
                             "monitoring.q4.collection_plan_invalid"};
        case MonitoringQ4CompositionSource::predecessor:
            return std::pair{MonitoringQ4ProjectedAuthorityPhase::predecessor,
                             "monitoring.q4.predecessor_invalid"};
        case MonitoringQ4CompositionSource::build:
            return std::pair{MonitoringQ4ProjectedAuthorityPhase::build_attestation,
                             "monitoring.q4.build_attestation_invalid"};
        case MonitoringQ4CompositionSource::contained:
            return std::nullopt;
        }
        return std::nullopt;
    case MonitoringQ4AuthorityStage::retirement:
    case MonitoringQ4AuthorityStage::contained:
        return std::pair{MonitoringQ4ProjectedAuthorityPhase::finalization,
                         "monitoring.q4.finalization_failed"};
    }
    return std::nullopt;
}
auto valid_primary(const MonitoringQ4AuthorityFailure& value) noexcept -> bool {
    if (value.primary.native_error < 0)
        return false;
    const auto exact = [&](MonitoringQ4CompositionSource source) {
        return value.primary.source == source &&
               value.primary.kind == MonitoringQ4CompositionFactKind::primary;
    };
    switch (value.stage) {
    case MonitoringQ4AuthorityStage::repository:
    case MonitoringQ4AuthorityStage::current_revision:
    case MonitoringQ4AuthorityStage::history:
        return exact(MonitoringQ4CompositionSource::repository) && value.primary.detail == 0U;
    case MonitoringQ4AuthorityStage::governed_input:
        return exact(MonitoringQ4CompositionSource::governed) &&
               value.primary.detail <=
                   static_cast<unsigned int>(MonitoringGovernedTrackedBundleFailureKind::inactive);
    case MonitoringQ4AuthorityStage::plan_schema:
        return exact(MonitoringQ4CompositionSource::governed) && value.primary.detail == 0U;
    case MonitoringQ4AuthorityStage::predecessor:
        return exact(MonitoringQ4CompositionSource::predecessor) &&
               value.primary.detail <=
                   static_cast<unsigned int>(MonitoringQ4CompleteQ3PrimaryKind::projection_failed);
    case MonitoringQ4AuthorityStage::running_build:
        return exact(MonitoringQ4CompositionSource::build) && value.primary.detail == 0U;
    case MonitoringQ4AuthorityStage::final_recheck:
        if (value.primary.kind == MonitoringQ4CompositionFactKind::changed)
            return value.primary.source != MonitoringQ4CompositionSource::contained &&
                   value.primary.detail == 0U && value.primary.native_error == 0;
        if (value.primary.kind != MonitoringQ4CompositionFactKind::primary)
            return false;
        switch (value.primary.source) {
        case MonitoringQ4CompositionSource::repository:
        case MonitoringQ4CompositionSource::build:
            return value.primary.detail == 0U;
        case MonitoringQ4CompositionSource::governed:
            return value.primary.detail <=
                   static_cast<unsigned int>(MonitoringGovernedTrackedBundleFailureKind::inactive);
        case MonitoringQ4CompositionSource::predecessor:
            return value.primary.detail <=
                   static_cast<unsigned int>(MonitoringQ4CompleteQ3PrimaryKind::projection_failed);
        case MonitoringQ4CompositionSource::contained:
            return false;
        }
        return false;
    case MonitoringQ4AuthorityStage::retirement:
        return exact(MonitoringQ4CompositionSource::contained) && value.primary.detail == 0U &&
               value.primary.native_error == 0;
    case MonitoringQ4AuthorityStage::contained:
        return value.primary.source == MonitoringQ4CompositionSource::contained &&
               (value.primary.kind == MonitoringQ4CompositionFactKind::contained ||
                value.primary.kind == MonitoringQ4CompositionFactKind::allocation ||
                value.primary.kind == MonitoringQ4CompositionFactKind::inactive) &&
               value.primary.detail == 0U && value.primary.native_error == 0;
    }
    return false;
}
} // namespace

void MonitoringQ4AuthorityFailureProjector::set_fault(unsigned char value) noexcept {
    fault = value;
}
auto MonitoringQ4AuthorityFailureProjector::project(MonitoringQ4AuthorityFailure value) noexcept
    -> MonitoringQ4ProjectedAuthorityFailure {
    const auto terminal = []() noexcept {
        return MonitoringQ4ProjectedAuthorityFailure{
            .primary_kind = MonitoringQ4ProjectedPrimaryKind::terminal_containment,
            .phase = MonitoringQ4ProjectedAuthorityPhase::finalization};
    };
    try {
        if (fault == 1U)
            throw std::bad_alloc{};
        const auto stage = stage_identity(value.stage, value.primary.source);
        if (!stage || !valid_primary(value) || value.cleanup.count > value.cleanup.facts.size())
            return terminal();
        const auto [phase, underlying] = *stage;
        std::array<std::optional<core::Error>, MonitoringQ4ProjectedAuthorityFailure::capacity>
            cleanup{};
        std::size_t cleanup_count{};
        const auto ordinary =
            std::min(value.cleanup.count, MonitoringQ4ProjectedAuthorityFailure::ordinary_capacity);
        for (std::size_t index{}; index < ordinary; ++index) {
            if (fault == 2U)
                throw std::bad_alloc{};
            const auto& fact = value.cleanup.facts[index];
            const auto item_identity = identity(fact.source);
            if (!item_identity || !valid_cleanup(fact))
                return terminal();
            cleanup[cleanup_count++].emplace(error(*item_identity, fact.native_error));
        }
        if (value.cleanup.overflowed || value.cleanup.count > ordinary) {
            cleanup[cleanup_count++].emplace(
                core::Error{.category = core::ErrorCategory::monitoring,
                            .code = core::StableErrorCode{"monitoring.q4.cleanup_overflow"},
                            .message = "Monitoring Q4 authority cleanup ledger overflowed",
                            .operation = "run_monitoring_host_q4_backend",
                            .os_error = std::nullopt,
                            .context = {},
                            .severity = core::ErrorSeverity::error,
                            .continuation = core::ContinuationPolicy::abort_command});
        }
        std::optional<core::Error> primary;
        const auto kind = value.stage == MonitoringQ4AuthorityStage::contained
                              ? MonitoringQ4ProjectedPrimaryKind::terminal_containment
                              : MonitoringQ4ProjectedPrimaryKind::stage_failure;
        if (kind == MonitoringQ4ProjectedPrimaryKind::stage_failure)
            primary.emplace(core::Error{
                .category = core::ErrorCategory::monitoring,
                .code = core::StableErrorCode{"monitoring.q4.backend.stage_failure"},
                .message = "Monitoring Q4 backend stage failed",
                .operation = "run_monitoring_host_q4_backend",
                .os_error = value.primary.native_error > 0
                                ? std::optional{core::OsError::capture(std::error_code{
                                      value.primary.native_error, std::generic_category()})}
                                : std::nullopt,
                .context = {{"underlying_code", underlying}},
                .severity = core::ErrorSeverity::error,
                .continuation = core::ContinuationPolicy::abort_command});
        return {.primary_kind = kind,
                .primary = std::move(primary),
                .phase = phase,
                .cleanup = std::move(cleanup),
                .cleanup_count = cleanup_count};
    } catch (...) {
        return terminal();
    }
}

} // namespace epycbench::monitoring::qualification::detail
