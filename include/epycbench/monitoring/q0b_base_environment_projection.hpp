#pragma once

#include "epycbench/monitoring/production_admission_projection.hpp"

#include <expected>
#include <string>

namespace epycbench::monitoring::qualification {

inline constexpr auto monitoring_q0b_base_environment_schema =
    "epycbench.monitoring-q0b-base-environment/v1";

class MonitoringQ0bBaseEnvironmentProjection {
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
        -> const topology::PhysicalTopologyFingerprint& { return physical_; }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& { return effective_; }
    [[nodiscard]] auto stable_numa_order() const noexcept
        -> const std::vector<topology::StableNumaNodeId>& { return stable_numa_order_; }
    [[nodiscard]] auto service_reservation() const noexcept -> const ServiceReservationFact& {
        return service_;
    }
    [[nodiscard]] auto topology_consistent() const noexcept -> bool { return topology_consistent_; }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& {
        return canonical_json_;
    }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }

  private:
    MonitoringQ0bBaseEnvironmentProjection(
        std::string fixture_id, std::string fixture_version, core::Sha256Digest manifest,
        core::Sha256Digest checksums, topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        std::vector<topology::StableNumaNodeId> numa_order, ServiceReservationFact service,
        bool topology_consistent, std::string canonical_json, core::Sha256Digest digest)
        : fixture_id_{std::move(fixture_id)}, fixture_version_{std::move(fixture_version)},
          manifest_sha256_{manifest}, checksums_sha256_{checksums}, physical_{std::move(physical)},
          effective_{std::move(effective)}, stable_numa_order_{std::move(numa_order)},
          service_{std::move(service)}, topology_consistent_{topology_consistent},
          canonical_json_{std::move(canonical_json)}, digest_{digest} {}

    runtime::PairwiseVariant variant_{runtime::PairwiseVariant::first};
    std::string fixture_id_, fixture_version_;
    core::Sha256Digest manifest_sha256_, checksums_sha256_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    std::vector<topology::StableNumaNodeId> stable_numa_order_;
    ServiceReservationFact service_;
    bool topology_consistent_{};
    std::string canonical_json_;
    core::Sha256Digest digest_;

    friend auto project_monitoring_q0b_base_environment(
        const MonitoringProductionAdmissionProjection&) noexcept
        -> std::expected<MonitoringQ0bBaseEnvironmentProjection, core::Error>;
};

[[nodiscard]] auto project_monitoring_q0b_base_environment(
    const MonitoringProductionAdmissionProjection& projection) noexcept
    -> std::expected<MonitoringQ0bBaseEnvironmentProjection, core::Error>;

} // namespace epycbench::monitoring::qualification
