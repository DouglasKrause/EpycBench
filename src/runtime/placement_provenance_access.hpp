#pragma once
#include "epycbench/runtime/placement_provenance.hpp"
#include <string_view>

namespace epycbench::runtime::detail {
struct PlacementProvenanceConstructionAccess final {
    static auto scenario(scenario::ScenarioDigest, scenario::ResolvedScenarioPlanDigest)
        -> PlacementProvenance;
    static auto policy_identity(std::string_view)
        -> std::expected<HostQualificationPolicyIdentity, core::Error>;
    static auto environment_digest(std::string_view)
        -> std::expected<HostEnvironmentDigest, core::Error>;
    static auto plan_digest(std::string_view)
        -> std::expected<HostQualificationPlanDigest, core::Error>;
    static auto host(HostQualificationPolicyIdentity policy_identity,
                     core::Sha256Digest template_digest,
                     HostEnvironmentDigest environment_digest,
                     HostQualificationPlanDigest plan_digest)
        -> PlacementProvenance;
};
[[nodiscard]] auto host_qualification_plan_provenance_fallible(const PlacementProvenance&)
    -> std::expected<std::reference_wrapper<const HostQualificationPlanPlacementProvenance>,
                     core::Error>;
[[nodiscard]] auto placement_provenance_json(const PlacementProvenance&)
    -> nlohmann::ordered_json;
}
