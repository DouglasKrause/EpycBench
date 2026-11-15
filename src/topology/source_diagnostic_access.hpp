#pragma once

#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/reconciliation.hpp"

#include <optional>

namespace epycbench::topology::detail {

// Original-main borrowed sink. Only an actual failure populates it; completed
// public reports retain their existing order and ownership. Stable identity
// validation has exactly its existing optional package/die os_id exemptions.
enum class SourceValidationEligibility { all_invariants, stable_identity };
[[nodiscard]] auto validate_topology_with_failure_sink(const TopologySnapshot&,
    std::optional<core::Error>*,
    SourceValidationEligibility = SourceValidationEligibility::all_invariants)
    -> ValidationReport;
[[nodiscard]] auto reconcile_topology_with_failure_sink(const TopologySourceEvidence&,
    const TopologySourceEvidence&, std::optional<core::Error>*)
    -> std::expected<ReconciliationResult, core::Error>;
[[nodiscard]] auto derive_topology_fingerprints_with_failure_sink(const TopologySnapshot&,
    const StableTopologyIds&, std::optional<core::Error>*)
    -> std::expected<TopologyFingerprints, core::Error>;

} // namespace epycbench::topology::detail
