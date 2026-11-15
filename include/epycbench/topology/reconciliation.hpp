#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"
#include "epycbench/topology/source_evidence.hpp"

#include <expected>
#include <optional>
#include <vector>

namespace epycbench::topology {

struct ReconciliationDiagnostic {
    core::Error error;
    bool material{};

    auto operator==(const ReconciliationDiagnostic&) const -> bool = default;
};

struct ReconciliationResult {
    std::optional<TopologySnapshot> snapshot;
    std::vector<ReconciliationDiagnostic> diagnostics;
};

[[nodiscard]] auto reconcile_topology(const TopologySourceEvidence& hwloc_evidence,
                                      const TopologySourceEvidence& linux_evidence)
    -> std::expected<ReconciliationResult, core::Error>;

} // namespace epycbench::topology
