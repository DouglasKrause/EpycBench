#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/runtime/production_admission.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/topology/reference_fixture.hpp"

#include <expected>
#include <string>
#include <vector>

namespace epycbench::monitoring::qualification {

inline constexpr auto monitoring_admitted_production_facts_schema =
    "epycbench.monitoring-admitted-production-facts/v1";

struct AdmittedWorkerFact {
    core::WorkerId worker_id;
    topology::StableNumaNodeId numa;
    topology::StableCoreId core;
    topology::StableProcessingUnitId processing_unit;
    core::LinuxCpuId linux_cpu;
    topology::StableNumaNodeId memory_target_numa;
    auto operator<=>(const AdmittedWorkerFact&) const = default;
};

struct ServiceReservationFact {
    std::vector<topology::StableCoreId> reserved_cores;
    std::vector<topology::StableProcessingUnitId> reserved_processing_units;
    std::vector<core::LinuxCpuId> reserved_linux_cpus;
    auto operator==(const ServiceReservationFact&) const -> bool = default;
};

struct AdmissionInvariantFacts {
    bool one_worker_per_required_numa_node;
    bool unique_workers;
    bool worker_service_core_disjoint;
    bool worker_service_cpu_disjoint;
    bool worker_local_memory;
    bool topology_consistent;
    auto operator<=>(const AdmissionInvariantFacts&) const = default;
};

class MonitoringProductionAdmissionProjection {
  public:
    [[nodiscard]] auto fixture_id() const noexcept -> const std::string& { return fixture_id_; }
    [[nodiscard]] auto variant() const noexcept -> runtime::PairwiseVariant { return variant_; }
    [[nodiscard]] auto fixture_version() const noexcept -> const std::string& {
        return fixture_version_;
    }
    [[nodiscard]] auto manifest_sha256() const noexcept -> const core::Sha256Digest& {
        return manifest_sha256_;
    }
    [[nodiscard]] auto checksums_sha256() const noexcept -> const core::Sha256Digest& {
        return checksums_sha256_;
    }
    [[nodiscard]] auto physical_fingerprint() const noexcept
        -> const topology::PhysicalTopologyFingerprint& {
        return physical_;
    }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& {
        return effective_;
    }
    [[nodiscard]] auto stable_numa_order() const noexcept
        -> const std::vector<topology::StableNumaNodeId>& {
        return stable_numa_order_;
    }
    [[nodiscard]] auto workers() const noexcept -> const std::vector<AdmittedWorkerFact>& {
        return workers_;
    }
    [[nodiscard]] auto service_reservation() const noexcept -> const ServiceReservationFact& {
        return service_;
    }
    [[nodiscard]] auto admitted_placement_identity() const noexcept
        -> const runtime::AdmittedPlacementIdentity& {
        return placement_identity_;
    }
    [[nodiscard]] auto scenario_digest() const noexcept -> const std::string& {
        return scenario_digest_;
    }
    [[nodiscard]] auto resolved_scenario_plan_digest() const noexcept -> const std::string& {
        return resolved_digest_;
    }
    [[nodiscard]] auto invariants() const noexcept -> const AdmissionInvariantFacts& {
        return invariants_;
    }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& {
        return canonical_json_;
    }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }

  private:
    MonitoringProductionAdmissionProjection(
        runtime::PairwiseVariant variant, std::string fixture_id, std::string fixture_version,
        core::Sha256Digest manifest,
        core::Sha256Digest checksums, topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        std::vector<topology::StableNumaNodeId> numa_order, std::vector<AdmittedWorkerFact> workers,
        ServiceReservationFact service, runtime::AdmittedPlacementIdentity placement_identity,
        std::string scenario_digest, std::string resolved_digest,
        AdmissionInvariantFacts invariants, std::string canonical_json, core::Sha256Digest digest)
        : variant_{variant}, fixture_id_{std::move(fixture_id)},
          fixture_version_{std::move(fixture_version)},
          manifest_sha256_{manifest}, checksums_sha256_{checksums}, physical_{std::move(physical)},
          effective_{std::move(effective)}, stable_numa_order_{std::move(numa_order)},
          workers_{std::move(workers)}, service_{std::move(service)},
          placement_identity_{std::move(placement_identity)},
          scenario_digest_{std::move(scenario_digest)},
          resolved_digest_{std::move(resolved_digest)}, invariants_{invariants},
          canonical_json_{std::move(canonical_json)}, digest_{digest} {}

    runtime::PairwiseVariant variant_;
    std::string fixture_id_, fixture_version_;
    core::Sha256Digest manifest_sha256_, checksums_sha256_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    std::vector<topology::StableNumaNodeId> stable_numa_order_;
    std::vector<AdmittedWorkerFact> workers_;
    ServiceReservationFact service_;
    runtime::AdmittedPlacementIdentity placement_identity_;
    std::string scenario_digest_, resolved_digest_;
    AdmissionInvariantFacts invariants_;
    std::string canonical_json_;
    core::Sha256Digest digest_;

    friend auto
    project_monitoring_production_admission(const topology::ReferenceFixtureAuthority&,
                                            const runtime::AdmittedPairwiseProductionInputs&,
                                            runtime::PairwiseVariant) noexcept
        -> std::expected<MonitoringProductionAdmissionProjection, core::Error>;
    template <class PairAuthority>
    friend auto project_impl(const topology::ReferenceFixtureAuthority&,
                             const PairAuthority&, runtime::PairwiseVariant) noexcept
        -> std::expected<MonitoringProductionAdmissionProjection, core::Error>;
    friend auto
    project_monitoring_production_admission(const topology::ReferenceFixtureAuthority&,
                                            const runtime::CalibrationOnlyProductionAdmission&,
                                            runtime::PairwiseVariant) noexcept
        -> std::expected<MonitoringProductionAdmissionProjection, core::Error>;
};

[[nodiscard]] auto
project_monitoring_production_admission(const topology::ReferenceFixtureAuthority& fixture,
                                        const runtime::AdmittedPairwiseProductionInputs& inputs,
                                        runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringProductionAdmissionProjection, core::Error>;

[[nodiscard]] auto
project_monitoring_production_admission(
    const topology::ReferenceFixtureAuthority& fixture,
    const runtime::CalibrationOnlyProductionAdmission& admission,
    runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringProductionAdmissionProjection, core::Error>;

} // namespace epycbench::monitoring::qualification
