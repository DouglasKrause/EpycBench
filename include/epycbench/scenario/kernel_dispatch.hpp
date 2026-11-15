#pragma once

#include "epycbench/kernel/configuration.hpp"
#include "epycbench/scenario/memory_placement_resolution.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::scenario {

inline constexpr std::string_view kernel_dispatch_profile =
    "epycbench.kernel-dispatch.v1";

enum class PointerCycleSeedOrigin { explicit_value, derived_value };
enum class DeferredKernelConstraint {
    working_set_at_least_resolved_page_size,
    exactly_one_worker,
};

struct DispatchedWorkload {
    std::string workload_id;
    kernel::KernelId kernel_id;
    std::string kernel_token;
    kernel::KernelConfiguration configuration;
    std::string source_pointer;
    auto operator<=>(const DispatchedWorkload&) const = default;
};

struct ExperimentWorkloadBinding {
    std::size_t experiment_index{};
    std::string workload_id;
    std::optional<std::uint64_t> pointer_cycle_seed;
    std::optional<PointerCycleSeedOrigin> pointer_cycle_seed_origin;
    std::vector<DeferredKernelConstraint> deferred_constraints;
    std::string source_pointer;
    auto operator<=>(const ExperimentWorkloadBinding&) const = default;
};

struct KernelDispatchResult;

class KernelDispatchedScenario {
  public:
    [[nodiscard]] auto memory_placement_resolved() const noexcept
        -> const MemoryPlacementResolvedScenario&;
    [[nodiscard]] auto workloads() const noexcept
        -> const std::map<std::string, DispatchedWorkload>&;
    [[nodiscard]] auto experiment_workloads() const noexcept
        -> const std::vector<ExperimentWorkloadBinding>&;
    [[nodiscard]] auto dispatch_profile() const noexcept -> std::string_view;

  private:
    KernelDispatchedScenario(MemoryPlacementResolvedScenario scenario,
                             std::map<std::string, DispatchedWorkload> workloads,
                             std::vector<ExperimentWorkloadBinding> bindings);

    MemoryPlacementResolvedScenario scenario_;
    std::map<std::string, DispatchedWorkload> workloads_;
    std::vector<ExperimentWorkloadBinding> bindings_;

    friend auto dispatch_kernel_parameters(MemoryPlacementResolvedScenario)
        -> std::expected<KernelDispatchResult, core::Error>;
};

struct KernelDispatchResult {
    std::optional<KernelDispatchedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto dispatch_kernel_parameters(MemoryPlacementResolvedScenario scenario)
    -> std::expected<KernelDispatchResult, core::Error>;

} // namespace epycbench::scenario
