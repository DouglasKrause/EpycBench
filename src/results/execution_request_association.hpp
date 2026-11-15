#pragma once
#include "epycbench/runtime/production_admission.hpp"
#include <array>
#include <span>

namespace epycbench::results::process_subject { struct Entry; }
namespace epycbench::results::private_plan_composition { class Owner; }
namespace epycbench::results::implementation_selection { struct Workload; struct Binding; }
namespace epycbench::results::execution_plan { struct Pair; }

namespace epycbench::results::request_association {
enum class Error { state, reference, identity, placement, measurement, statistics, monitoring, service, capacity };
struct ServiceLocator {
    runtime::ServiceRole role{};
    std::size_t reserved_cpu{}, reserved_core{}, pool{}, pool_cpu{};
    auto operator==(const ServiceLocator&) const -> bool = default;
};
struct Evidence {
    std::uint64_t planning_sequence, pair_ordinal, first_request, second_request;
    runtime::AdmittedMeasurementInputs measurement;
    runtime::StatisticsComparisonPolicySource statistics_source;
    runtime::StatisticsProductionPolicy statistics;
    monitoring::MonitoringExecutionPolicy monitoring;
    std::array<ServiceLocator,2> services;
    std::size_t service_count;
};
struct Quantities {
    std::size_t records{}, workers{}, node_targets{}, service_candidates{}, compared_bytes{}, work{}, preparation_bytes{};
    auto operator==(const Quantities&) const -> bool = default;
};
using Limits=Quantities;
using Usage=Quantities;
namespace detail {
struct PlanView {
    const private_plan_composition::Owner& plan;
    std::span<const implementation_selection::Workload> workloads;
    std::span<const implementation_selection::Binding> bindings;
    const execution_plan::Pair& pair;
    const topology::TopologySnapshot& topology;
    const topology::StableTopologyIds& stable_ids;
    const topology::TopologyFingerprints& fingerprints;
};
struct RequestView {
    const runtime::ipc::PlacementRequest& first;
    const runtime::ipc::PlacementRequest& second;
    std::uint64_t first_request, second_request, comparison_seed;
    const runtime::StatisticsComparisonPolicySource& statistics_source;
    const runtime::StatisticsProductionPolicy& statistics;
    const monitoring::MonitoringExecutionPolicy& monitoring;
    std::span<const runtime::ServiceThreadAssignment> services;
    bool measurement_monitor_required;
};
// Consistency of retained facts only; this does not yield Session membership.
[[nodiscard]] auto compare(const PlanView&, const RequestView&, std::uint64_t root_seed,
    const Limits&, Usage&) noexcept -> std::expected<std::array<ServiceLocator,2>, Error>;
[[nodiscard]] auto checked_add(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto checked_multiply(std::size_t,std::size_t) noexcept -> std::optional<std::size_t>;
[[nodiscard]] auto fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto prepare(std::span<const process_subject::Entry>,std::uint64_t,std::uint64_t,
    const runtime::AdmittedPairwiseProductionInputs&,std::uint64_t,std::uint64_t,
    const Limits&,Usage&) noexcept -> std::expected<Evidence,Error>;
}
} // namespace epycbench::results::request_association
