#include "runtime/placement_provenance_access.hpp"

#include "core/json_ownership.hpp"
#include <cctype>

namespace epycbench::runtime {
namespace {
auto wrong(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.placement_provenance.wrong_alternative"},
            .message = "Placement provenance has the wrong alternative",
            .operation = "access_placement_provenance",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json(std::move(classification)));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto invalid() -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.ipc.invalid_construction"},
            .message = "Placement provenance is invalid",
            .operation = "construct_placement_provenance",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json("provenance_invalid"));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto policy_valid(std::string_view s) -> bool {
    if (s.empty() || s.size()>128 || s.front()<'a' || s.front()>'z') return false;
    bool separator=false;
    for (char c : s.substr(1)) {
        if (c=='.' || c=='-' || c=='/') { if(separator) return false; separator=true; continue; }
        if (!((c>='a'&&c<='z')||(c>='0'&&c<='9'))) return false;
        separator=false;
    }
    if (separator) return false;
    const auto slash=s.find('/');
    if (slash!=std::string_view::npos) {
        if (s.find('/',slash+1)!=std::string_view::npos) return false;
        auto suffix=s.substr(slash+1);
        if (suffix.size()<2 || suffix[0]!='v' || suffix[1]<'1'||suffix[1]>'9') return false;
        for(char c:suffix.substr(2)) if(c<'0'||c>'9') return false;
    }
    return true;
}
auto prefixed(std::string_view text) -> std::expected<core::Sha256Digest,core::Error> {
    if (!text.starts_with("sha256:")) return std::unexpected{invalid()};
    auto parsed=core::Sha256Digest::parse(text.substr(7));
    core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
    if(!parsed) return std::unexpected{invalid()};
    return *parsed;
}
}
auto scenario_provenance(const PlacementProvenance& p) noexcept
 -> std::expected<std::reference_wrapper<const ScenarioPlacementProvenance>,core::Error> {
    if(auto* v=std::get_if<ScenarioPlacementProvenance>(&p)) return std::cref(*v);
    return std::unexpected{wrong("scenario_provenance_required")};
}
auto host_qualification_plan_provenance(const PlacementProvenance& p) noexcept
    -> std::expected<std::reference_wrapper<const HostQualificationPlanPlacementProvenance>,
                     core::Error> {
    return detail::host_qualification_plan_provenance_fallible(p);
}
namespace detail {
auto host_qualification_plan_provenance_fallible(const PlacementProvenance& p)
    -> std::expected<std::reference_wrapper<const HostQualificationPlanPlacementProvenance>,
                     core::Error> {
    if(auto* v=std::get_if<HostQualificationPlanPlacementProvenance>(&p)) return std::cref(*v);
    return std::unexpected{wrong("host_qualification_plan_provenance_required")};
}
auto PlacementProvenanceConstructionAccess::scenario(scenario::ScenarioDigest s,
 scenario::ResolvedScenarioPlanDigest r)->PlacementProvenance {
    return ScenarioPlacementProvenance{std::move(s),std::move(r)};
}
auto PlacementProvenanceConstructionAccess::policy_identity(std::string_view value)
 -> std::expected<HostQualificationPolicyIdentity,core::Error> {
    if(!policy_valid(value)) return std::unexpected{invalid()};
    return HostQualificationPolicyIdentity{std::string{value}};
}
auto PlacementProvenanceConstructionAccess::environment_digest(std::string_view value)
 -> std::expected<HostEnvironmentDigest,core::Error> {
    auto parsed = prefixed(value);
    core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
    if (!parsed)
        return std::unexpected{std::move(parsed.error())};
    return HostEnvironmentDigest{*parsed};
}
auto PlacementProvenanceConstructionAccess::plan_digest(std::string_view value)
 -> std::expected<HostQualificationPlanDigest,core::Error> {
    auto parsed = prefixed(value);
    core::detail::ExpectedErrorCleanup parsed_cleanup{parsed};
    if (!parsed)
        return std::unexpected{std::move(parsed.error())};
    return HostQualificationPlanDigest{*parsed};
}
auto PlacementProvenanceConstructionAccess::host(HostQualificationPolicyIdentity policy,
 core::Sha256Digest templ, HostEnvironmentDigest environment, HostQualificationPlanDigest plan)
 -> PlacementProvenance {
    return HostQualificationPlanPlacementProvenance{std::move(policy),templ,
        std::move(environment),std::move(plan)};
}
auto placement_provenance_json(const PlacementProvenance& p)->nlohmann::ordered_json {
    using Json = nlohmann::ordered_json;
    return core::detail::json_object<Json>([&](auto& value) {
        if (auto* scenario = std::get_if<ScenarioPlacementProvenance>(&p)) {
            core::detail::set_json(value, "kind", Json("scenario"));
            core::detail::set_json(value, "resolved_scenario_plan_digest",
                                   Json(scenario->resolved_scenario_plan_digest().text()));
            core::detail::set_json(value, "scenario_digest",
                                   Json(scenario->scenario_digest().text()));
        } else {
            const auto& host = std::get<HostQualificationPlanPlacementProvenance>(p);
            core::detail::set_json(value, "environment_digest",
                                   Json(host.environment_digest().text()));
            core::detail::set_json(value, "host_qualification_plan_digest",
                                   Json(host.host_qualification_plan_digest().text()));
            core::detail::set_json(value, "kind", Json("host_qualification_plan"));
            core::detail::set_json(value, "policy_identity", Json(host.policy_identity().text()));
            core::detail::set_json(value, "template_sha256", Json(host.template_sha256().to_hex()));
        }
    });
}
} // namespace detail
}
