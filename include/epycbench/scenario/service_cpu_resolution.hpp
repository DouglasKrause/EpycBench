#pragma once

#include "epycbench/scenario/resource_resolution.hpp"

#include <compare>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::scenario {

inline constexpr std::string_view service_cpu_resolution_profile =
    "epycbench.service-cpu-resolution.v1";

struct ResolvedServiceCpu {
    topology::StableProcessingUnitId stable_id;
    topology::StableCoreId core_id;
    core::LinuxCpuId linux_cpu;
    auto operator<=>(const ResolvedServiceCpu&) const = default;
};

struct ResolvedServiceCpuPool {
    std::string name;
    std::vector<ResolvedServiceCpu> processing_units;
    core::CpuSet linux_cpus;
    std::vector<topology::StableCoreId> reserved_cores;
    std::string source_pointer;
    auto operator==(const ResolvedServiceCpuPool&) const -> bool = default;
};

struct ServiceCpuResolutionResult;

class ServiceCpuResolvedScenario {
  public:
    [[nodiscard]] auto resource_resolved() const noexcept -> const ResourceResolvedScenario&;
    [[nodiscard]] auto service_cpu_pools() const noexcept
        -> const std::map<std::string, ResolvedServiceCpuPool>&;
    [[nodiscard]] auto reserved_service_cpus() const noexcept -> const core::CpuSet&;
    [[nodiscard]] auto reserved_service_cores() const noexcept
        -> const std::vector<topology::StableCoreId>&;
    [[nodiscard]] auto has_dedicated_service_placement() const noexcept -> bool;
    [[nodiscard]] auto resolution_profile() const noexcept -> std::string_view;

  private:
    ServiceCpuResolvedScenario(ResourceResolvedScenario scenario,
                               std::map<std::string, ResolvedServiceCpuPool> pools,
                               core::CpuSet reserved_cpus,
                               std::vector<topology::StableCoreId> reserved_cores);

    ResourceResolvedScenario scenario_;
    std::map<std::string, ResolvedServiceCpuPool> pools_;
    core::CpuSet reserved_cpus_;
    std::vector<topology::StableCoreId> reserved_cores_;

    friend auto resolve_service_cpus(ResourceResolvedScenario,
                                     const ResourceResolutionContext&)
        -> std::expected<ServiceCpuResolutionResult, core::Error>;
};

struct ServiceCpuResolutionResult {
    std::optional<ServiceCpuResolvedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto resolve_service_cpus(ResourceResolvedScenario scenario,
                                        const ResourceResolutionContext& context)
    -> std::expected<ServiceCpuResolutionResult, core::Error>;

} // namespace epycbench::scenario
