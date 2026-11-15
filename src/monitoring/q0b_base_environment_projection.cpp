#include "epycbench/monitoring/q0b_base_environment_projection.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <string_view>

namespace epycbench::monitoring::qualification {
namespace {
auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b_base_environment.invalid_input"},
            .message = "MON-007 Q0b base environment projection failed",
            .operation = "monitoring.project_q0b_base_environment",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
} // namespace

auto project_monitoring_q0b_base_environment(
    const MonitoringProductionAdmissionProjection& projection) noexcept
    -> std::expected<MonitoringQ0bBaseEnvironmentProjection, core::Error> {
    try {
        if (projection.variant() != runtime::PairwiseVariant::first)
            return std::unexpected{error("variant_not_first")};

        nlohmann::json json{
            {"schema", monitoring_q0b_base_environment_schema},
            {"variant_role", "first"},
            {"topology",
             {{"fixture_id", projection.fixture_id()},
              {"fixture_version", projection.fixture_version()},
              {"manifest_sha256", projection.manifest_sha256().to_hex()},
              {"checksums_sha256", projection.checksums_sha256().to_hex()},
              {"physical_fingerprint", projection.physical_fingerprint().to_string()},
              {"effective_fingerprint", projection.effective_fingerprint().to_string()}}}};
        auto& topology = json["topology"];
        topology["stable_numa_order"] = nlohmann::json::array();
        for (const auto& id : projection.stable_numa_order())
            topology["stable_numa_order"].push_back(id.value());

        const auto& service = projection.service_reservation();
        auto& service_json = json["service_reservation"];
        service_json = {{"reserved_stable_core_ids", nlohmann::json::array()},
                        {"reserved_stable_pu_ids", nlohmann::json::array()},
                        {"reserved_linux_cpus", nlohmann::json::array()}};
        for (const auto& id : service.reserved_cores)
            service_json["reserved_stable_core_ids"].push_back(id.value());
        for (const auto& id : service.reserved_processing_units)
            service_json["reserved_stable_pu_ids"].push_back(id.value());
        for (const auto id : service.reserved_linux_cpus)
            service_json["reserved_linux_cpus"].push_back(id.value());

        json["topology_consistent"] = projection.invariants().topology_consistent;

        auto canonical = core::canonicalize_json(json);
        if (!canonical)
            return std::unexpected{canonical.error()};
        auto digest = core::sha256(*canonical);
        return MonitoringQ0bBaseEnvironmentProjection{
            projection.fixture_id(), projection.fixture_version(), projection.manifest_sha256(),
            projection.checksums_sha256(), projection.physical_fingerprint(),
            projection.effective_fingerprint(), projection.stable_numa_order(),
            projection.service_reservation(), projection.invariants().topology_consistent,
            std::move(*canonical), digest};
    } catch (const std::exception&) {
        return std::unexpected{error("contained_exception")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure")};
    }
}
} // namespace epycbench::monitoring::qualification
