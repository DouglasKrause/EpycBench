#pragma once

#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/reconciliation.hpp"
#include "epycbench/topology/source_evidence.hpp"

#include <expected>
#include <optional>
#include "platform_linux/topology_source_io.hpp"

namespace epycbench::topology::detail {

enum class CheckedTopologyStage { hwloc_discovery, linux_discovery, reconciliation, validation };

struct CheckedTopologyFailure {
    CheckedTopologyStage stage;
    core::Error error;
};

struct CheckedTopologyOperations {
    void* context;
    std::expected<TopologySourceEvidence, core::Error> (*discover_hwloc)(void*);
    std::expected<TopologySourceEvidence, core::Error> (*discover_linux)(void*);
    std::expected<ReconciliationResult, core::Error> (*reconcile)(
        void*, const TopologySourceEvidence&, const TopologySourceEvidence&);
    ValidationReport (*validate)(void*, const TopologySnapshot&);
};

[[nodiscard]] auto discover_checked_topology_with(CheckedTopologyOperations operations) noexcept
    -> std::expected<TopologySnapshot, CheckedTopologyFailure>;

// Source composition owns containment and retains the first actual diagnostic
// before an allocating compatibility projection can be attempted.
[[nodiscard]] auto discover_checked_topology_with_fallible(
    CheckedTopologyOperations, std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<TopologySnapshot, CheckedTopologyFailure>;
[[nodiscard]] auto discover_checked_topology_with_prepared_linux_fallible(
    CheckedTopologyOperations, platform_linux::detail::PreparedTopologySourceIO*,
    std::uint64_t original_absolute_end, std::optional<core::Error>* raw_failure = nullptr)
    -> std::expected<TopologySnapshot, CheckedTopologyFailure>;
[[nodiscard]] auto discover_checked_topology_fallible(std::optional<core::Error>* raw_failure,
    platform_linux::detail::PreparedTopologySourceIO* prepared = nullptr,
    std::uint64_t original_absolute_end = 0)
    -> std::expected<TopologySnapshot, core::Error>;

} // namespace epycbench::topology::detail
