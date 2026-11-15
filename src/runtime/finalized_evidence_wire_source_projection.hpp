#pragma once

#include "runtime/finalized_evidence_wire_capacity.hpp"
#include "runtime/pairwise_monitoring_host_capacity_startup.hpp"

#include <expected>
#include <optional>

namespace epycbench::runtime::wire_v1::detail {

inline constexpr std::string_view capacity_projection_context_profile =
    "epycbench.finalized-pairwise-evidence-capacity-projection-context/v1";

// Pre-codec contextual value. This is deliberately not the registry's
// startup_capacity object: session_id, request_id, literal profile tokens and
// offer/ack body hashes belong to the following startup-codec slice.
struct ProjectedCapacityContext final {
    core::Sha256Digest projection_context_profile_sha256;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest logical_profile_sha256;
    core::Sha256Digest capacity_profile_sha256;
    core::Sha256Digest registry_sha256;
    core::Sha256Digest source_manifest_sha256;
    core::Sha256Digest finalized_error_profile_sha256;
    core::Sha256Digest monitoring_capacity_profile_sha256;
    CalculatedEvidenceCapacity capacity;
    friend auto operator==(const ProjectedCapacityContext&,
                           const ProjectedCapacityContext&) noexcept -> bool = default;
};

[[nodiscard]] auto derive_monitoring_round_capacity(
    bool context, bool temperature, bool frequency, std::uint64_t duration,
    std::optional<std::uint64_t> interval) noexcept
    -> std::expected<MonitoringRounds, CapacityError>;

// Both entry points derive every semantic parameter from already-admitted
// request/topology/Monitoring authority. There is intentionally no candidate
// or field-by-field public constructor.
[[nodiscard]] auto project_controller_startup_capacity(
    const runtime::detail::PairwiseHostCapacityRequestTranscript&,
    const RunnerContext&,
    const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority&, LocalResourceLimits = {}) noexcept
    -> std::expected<ProjectedCapacityContext, CapacityError>;

[[nodiscard]] auto project_runner_startup_capacity(
    const runtime::detail::PairwiseHostCapacityRequestTranscript&,
    const AdmittedPairwiseProductionInputs&,
    const runtime::detail::PairwiseMonitoringHostCapacityAdmission&, LocalResourceLimits = {}) noexcept
    -> std::expected<ProjectedCapacityContext, CapacityError>;

[[nodiscard]] auto validate_exact_projected_capacity_context(
    const ProjectedCapacityContext&, const ProjectedCapacityContext&) noexcept
    -> std::expected<void, CapacityError>;

} // namespace epycbench::runtime::wire_v1::detail
