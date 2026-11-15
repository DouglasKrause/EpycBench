#include "epycbench/monitoring/q0b_production_inputs.hpp"

#include "epycbench/kernel/kernel_id.hpp"
#include "epycbench/scenario/parser.hpp"
#include "monitoring/q0b_production_inputs_access.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace epycbench::monitoring::qualification {
class detail_MonitoringQ0bProductionInputsFactory {
  public:
    static auto make_family(std::string family, core::Sha256Digest declared_sha,
        core::Sha256Digest actual_sha, core::GovernedRepositoryFile file,
        scenario::StructurallyValidScenario scenario, scenario::ScenarioKernelBindings bindings)
        -> MonitoringQ0bProductionInputFamilyAuthority;
    static auto make_success(core::GovernedRepositoryFile manifest_file,
        core::Sha256Digest manifest_sha_value, Q0bManifest manifest,
        MonitoringQ0bProductionInputFamilyAuthority bandwidth,
        MonitoringQ0bProductionInputFamilyAuthority latency)
        -> MonitoringQ0bProductionInputsLoadResult;
};
namespace {
using Json = nlohmann::json;
constexpr std::string_view manifest_path = "config/monitoring-qualification/q0b/reference-epyc-v1-inputs.json";
constexpr std::string_view manifest_sha = "163a100ad5cd2a3adaca8cfd845427eb50189cbe023283c1244b8476870b4377";
constexpr std::string_view schema = "epycbench.monitoring-q0b-production-inputs/v1";
constexpr std::string_view policy_identity = "epycbench.monitoring-qualification.reference-epyc/v1";
constexpr std::string_view policy_sha = "a25e7d729851c59979806a826d386c76e323570dddd81a17c2c7ea2e8dc86d9a";
constexpr std::string_view bandwidth_family = "sequential-read-bandwidth";
constexpr std::string_view latency_family = "dependent-load-latency";
constexpr std::string_view bandwidth_path = "config/monitoring-qualification/q0b/reference-epyc-v1-bandwidth.json";
constexpr std::string_view latency_path = "config/monitoring-qualification/q0b/reference-epyc-v1-latency.json";
constexpr std::string_view bandwidth_sha = "9ae4dabed964098dd8062decbe7eb4eff8df1226a75364e90afa7325a12ce747";
constexpr std::string_view latency_sha = "f24c81e28a74b34ea0bbf36fa5b2c1e76c098cb1950f66cbf043969d569531f9";

auto error(std::string code, std::string classification, Json context = Json::object()) -> core::Error {
    context["classification"] = std::move(classification);
    return {.category=core::ErrorCategory::monitoring, .code=core::StableErrorCode{std::move(code)},
            .message="Q0b production input authority loading failed",
            .operation="load_q0b_production_inputs", .os_error=std::nullopt,
            .context=std::move(context), .severity=core::ErrorSeverity::error,
            .continuation=core::ContinuationPolicy::abort_command};
}
auto parse_digest(std::string_view value) -> std::optional<core::Sha256Digest> {
    auto parsed = core::Sha256Digest::parse(value);
    if (!parsed) return std::nullopt;
    return std::move(*parsed);
}
auto expected_digest(std::string_view value) -> core::Sha256Digest { return *core::Sha256Digest::parse(value); }
auto shape_error() { return error("monitoring.q0b_production_inputs.manifest_parse_failed", "manifest_shape_invalid"); }
bool exact_keys(const Json& object, std::initializer_list<std::string_view> keys) {
    if (!object.is_object() || object.size() != keys.size()) return false;
    for (auto key : keys) if (!object.contains(key)) return false;
    return true;
}
auto reject(std::string classification, Json context = Json::object()) {
    return error("monitoring.q0b_production_inputs.manifest_rejected", std::move(classification), std::move(context));
}
auto family_token(Q0bManifestFamily family) -> std::string_view {
    return family == Q0bManifestFamily::bandwidth ? bandwidth_family : latency_family;
}
auto expected_kernel(Q0bManifestFamily family) -> kernel::KernelId {
    return family == Q0bManifestFamily::bandwidth ? kernel::KernelId::sequential_read_bandwidth_v1
                                                  : kernel::KernelId::dependent_load_latency_v1;
}
auto validate_consistency_values(Q0bManifestFamily family,
    const std::vector<scenario::ScenarioKernelBinding>& bindings) -> std::expected<void, core::Error> {
    const auto expected=expected_kernel(family); const auto token=family_token(family);
    if (bindings.empty()) return std::unexpected{error("monitoring.q0b_production_inputs.scenario_family_mismatch","projected_bindings_empty",{{"manifest_family",token},{"expected_kernel_family",kernel::kernel_id_token(expected)},{"projected_binding_count",0}})};
    for (std::size_t i=0;i<bindings.size();++i) { const auto& binding=bindings[i]; if(binding.kernel_id!=expected)
        return std::unexpected{error("monitoring.q0b_production_inputs.scenario_family_mismatch","parsed_scenario_family_mismatch",{{"manifest_family",token},{"binding_index",i},{"experiment_id",binding.experiment_id.value()},{"workload_id",binding.workload_id.value()},{"expected_kernel_family",kernel::kernel_id_token(expected)},{"actual_kernel_family",kernel::kernel_id_token(binding.kernel_id)}})}; }
    return {};
}

struct FamilyOutcome {
    std::optional<MonitoringQ0bProductionInputFamilyAuthority> authority;
    std::vector<scenario::ScenarioDiagnostic> diagnostics;
};

auto diagnostic_result(std::vector<scenario::ScenarioDiagnostic> diagnostics)
    -> MonitoringQ0bProductionInputsLoadResult {
    auto first = std::move(diagnostics.front());
    diagnostics.erase(diagnostics.begin());
    return detail_MonitoringQ0bProductionInputsAccess::make_diagnostics(
        std::move(first), std::move(diagnostics));
}

struct AuthenticatedScenarioOutcome {
    std::optional<scenario::StructurallyValidScenario> scenario;
    std::optional<scenario::ScenarioKernelBindings> bindings;
    std::vector<scenario::ScenarioDiagnostic> diagnostics;
};

auto process_authenticated_scenario(Q0bManifestFamily family, std::string source_identity,
    std::string_view bytes) -> std::expected<AuthenticatedScenarioOutcome, core::Error> {
    auto authored = scenario::parse_authored_scenario(
        {std::move(source_identity), bytes, scenario::ScenarioFormat::json});
    if (!authored) return std::unexpected{std::move(authored.error())};
    auto structured = scenario::validate_scenario_structure(std::move(*authored));
    if (!structured) return std::unexpected{std::move(structured.error())};
    if (!structured->diagnostics.empty())
        return AuthenticatedScenarioOutcome{.scenario=std::nullopt, .bindings=std::nullopt,
            .diagnostics=std::move(structured->diagnostics)};
    auto projected = scenario::project_kernel_bindings(*structured->scenario);
    if (!projected) return std::unexpected{std::move(projected.error())};
    if (!projected->diagnostics.empty())
        return AuthenticatedScenarioOutcome{.scenario=std::nullopt, .bindings=std::nullopt,
            .diagnostics=std::move(projected->diagnostics)};
    auto consistent = detail_MonitoringQ0bProductionInputsAccess::validate_consistency(
        family, *projected->bindings);
    if (!consistent) return std::unexpected{std::move(consistent.error())};
    return AuthenticatedScenarioOutcome{.scenario=std::move(structured->scenario),
        .bindings=std::move(projected->bindings), .diagnostics={}};
}

auto process_family(const core::GovernedRepositoryRoot& root, Q0bManifestFamily family,
                    const Q0bManifestEntry& entry) -> std::expected<FamilyOutcome, core::Error> {
    const auto path = family == Q0bManifestFamily::bandwidth ? bandwidth_path : latency_path;
    const auto sha = family == Q0bManifestFamily::bandwidth ? bandwidth_sha : latency_sha;
    auto file = root.read_regular_file(std::filesystem::path{path});
    if (!file) return std::unexpected{std::move(file.error())};
    const auto actual = core::sha256(file->bytes());
    if (actual != expected_digest(sha))
        return std::unexpected{error("monitoring.q0b_production_inputs.scenario_authentication_failed",
            "scenario_sha256_mismatch", {{"manifest_family", family_token(family)},
             {"expected_sha256", std::string{sha}}, {"actual_sha256", actual.to_hex()}})};
    auto processed = process_authenticated_scenario(family,
        file->repository_relative_path().generic_string(), file->bytes());
    if (!processed) return std::unexpected{std::move(processed.error())};
    if (!processed->diagnostics.empty())
        return FamilyOutcome{.authority=std::nullopt,
                             .diagnostics=std::move(processed->diagnostics)};
    return FamilyOutcome{.authority=detail_MonitoringQ0bProductionInputsFactory::make_family(
        std::string{family_token(family)}, entry.scenario_sha256, actual, std::move(*file),
        std::move(*processed->scenario), std::move(*processed->bindings)), .diagnostics={}};
}
} // namespace

auto detail_MonitoringQ0bProductionInputsFactory::make_family(std::string family,
    core::Sha256Digest declared_sha, core::Sha256Digest actual_sha,
    core::GovernedRepositoryFile file, scenario::StructurallyValidScenario scenario,
    scenario::ScenarioKernelBindings bindings) -> MonitoringQ0bProductionInputFamilyAuthority {
    return {std::move(family), declared_sha, actual_sha, std::move(file),
            std::move(scenario), std::move(bindings)};
}

auto detail_MonitoringQ0bProductionInputsFactory::make_success(
    core::GovernedRepositoryFile manifest_file, core::Sha256Digest manifest_sha_value,
    Q0bManifest manifest, MonitoringQ0bProductionInputFamilyAuthority bandwidth,
    MonitoringQ0bProductionInputFamilyAuthority latency) -> MonitoringQ0bProductionInputsLoadResult {
    return MonitoringQ0bProductionInputsLoadResult{MonitoringQ0bProductionInputsAuthority{
        std::move(manifest_file), manifest_sha_value, std::move(manifest.schema),
        std::move(manifest.policy_identity), manifest.policy_sha256, std::move(bandwidth),
        std::move(latency)}};
}

auto detail_MonitoringQ0bProductionInputsAccess::decode_manifest(std::string_view bytes) noexcept
    -> std::expected<Q0bManifest, core::Error> {
    try {
        Json value;
        try { value = Json::parse(bytes); }
        catch (const Json::parse_error&) { return std::unexpected{error("monitoring.q0b_production_inputs.manifest_parse_failed", "malformed_json")}; }
        if (!exact_keys(value,{"schema","policy","families"}) || !value["schema"].is_string() ||
            !exact_keys(value["policy"],{"identity","sha256"}) || !value["policy"]["identity"].is_string() ||
            !value["policy"]["sha256"].is_string() || !value["families"].is_array()) return std::unexpected{shape_error()};
        auto policy = parse_digest(value["policy"]["sha256"].get<std::string>());
        if (!policy) return std::unexpected{shape_error()};
        Q0bManifest result{value["schema"].get<std::string>(), value["policy"]["identity"].get<std::string>(), std::move(*policy), {}};
        for (const auto& item : value["families"]) {
            if (!exact_keys(item,{"family","scenario_path","scenario_sha256"}) || !item["family"].is_string() ||
                !item["scenario_path"].is_string() || !item["scenario_sha256"].is_string()) return std::unexpected{shape_error()};
            auto digest = parse_digest(item["scenario_sha256"].get<std::string>());
            if (!digest) return std::unexpected{shape_error()};
            result.families.push_back({item["family"].get<std::string>(), item["scenario_path"].get<std::string>(), std::move(*digest)});
        }
        return result;
    } catch (const std::exception&) { return std::unexpected{shape_error()}; }
    catch (...) { return std::unexpected{shape_error()}; }
}

auto detail_MonitoringQ0bProductionInputsAccess::validate_manifest(const Q0bManifest& value) noexcept -> std::expected<void, core::Error> {
    try {
        if (value.schema != schema) return std::unexpected{reject("schema_mismatch")};
        if (value.policy_identity != policy_identity) return std::unexpected{reject("policy_identity_mismatch")};
        if (value.policy_sha256 != expected_digest(policy_sha)) return std::unexpected{reject("policy_sha256_mismatch")};
        if (value.families.size()!=2U) return std::unexpected{reject("entry_count_mismatch")};
        const auto& a=value.families[0]; const auto& b=value.families[1];
        if (a.family==latency_family && b.family==bandwidth_family) return std::unexpected{reject("entry_order_mismatch")};
        if (a.family!=bandwidth_family || b.family!=latency_family) return std::unexpected{reject("manifest_family_mismatch")};
        if (a.scenario_path!=bandwidth_path) return std::unexpected{reject("path_binding_mismatch",{{"entry_index",0},{"expected_repository_relative_path",bandwidth_path},{"actual_repository_relative_path",a.scenario_path}})};
        if (b.scenario_path!=latency_path) return std::unexpected{reject("path_binding_mismatch",{{"entry_index",1},{"expected_repository_relative_path",latency_path},{"actual_repository_relative_path",b.scenario_path}})};
        if (a.scenario_sha256!=expected_digest(bandwidth_sha)) return std::unexpected{reject("declared_sha256_mismatch",{{"entry_index",0}})};
        if (b.scenario_sha256!=expected_digest(latency_sha)) return std::unexpected{reject("declared_sha256_mismatch",{{"entry_index",1}})};
        return {};
    } catch (const std::exception&) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_exception")}; }
    catch (...) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_unknown_failure")}; }
}

auto detail_MonitoringQ0bProductionInputsAccess::validate_consistency(Q0bManifestFamily family, const scenario::ScenarioKernelBindings& bindings) noexcept -> std::expected<void, core::Error> {
    try {
        return validate_consistency_values(family, bindings.bindings());
    } catch (const std::exception&) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_exception")}; }
    catch (...) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_unknown_failure")}; }
}

auto detail_MonitoringQ0bProductionInputsAccess::validate_consistency_for_test(
    Q0bManifestFamily family, const std::vector<scenario::ScenarioKernelBinding>& bindings) noexcept
    -> std::expected<void, core::Error> {
    try { return validate_consistency_values(family, bindings); }
    catch (const std::exception&) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_exception")}; }
    catch (...) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_unknown_failure")}; }
}

auto detail_MonitoringQ0bProductionInputsAccess::make_diagnostics(
    scenario::ScenarioDiagnostic first, std::vector<scenario::ScenarioDiagnostic> remaining)
    -> MonitoringQ0bProductionInputsLoadResult {
    return MonitoringQ0bProductionInputsLoadResult{std::move(first), std::move(remaining)};
}

auto detail_MonitoringQ0bProductionInputsAccess::process_authenticated_scenario_for_test(
    Q0bManifestFamily family, std::string source_identity, std::string bytes) noexcept
    -> std::expected<std::optional<MonitoringQ0bProductionInputsLoadResult>, core::Error> {
    try {
        auto result = process_authenticated_scenario(family, std::move(source_identity), bytes);
        if (!result) return std::unexpected{std::move(result.error())};
        if (result->diagnostics.empty()) return std::nullopt;
        return diagnostic_result(std::move(result->diagnostics));
    } catch (const std::exception&) {
        return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure",
                                     "contained_exception")};
    } catch (...) {
        return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure",
                                     "contained_unknown_failure")};
    }
}

auto load_q0b_production_inputs(const std::filesystem::path& repository_root) noexcept -> std::expected<MonitoringQ0bProductionInputsLoadResult, core::Error> {
    try {
        auto root=core::GovernedRepositoryRoot::create(repository_root); if(!root) return std::unexpected{std::move(root.error())};
        auto file=root->read_regular_file(std::filesystem::path{manifest_path}); if(!file) return std::unexpected{std::move(file.error())};
        const auto actual=core::sha256(file->bytes());
        if(actual!=expected_digest(manifest_sha)) return std::unexpected{error("monitoring.q0b_production_inputs.manifest_authentication_failed","manifest_sha256_mismatch",{{"expected_sha256",manifest_sha},{"actual_sha256",actual.to_hex()}})};
        auto manifest=detail_MonitoringQ0bProductionInputsAccess::decode_manifest(file->bytes()); if(!manifest) return std::unexpected{std::move(manifest.error())};
        auto accepted=detail_MonitoringQ0bProductionInputsAccess::validate_manifest(*manifest); if(!accepted) return std::unexpected{std::move(accepted.error())};
        auto bandwidth=process_family(*root,Q0bManifestFamily::bandwidth,manifest->families[0]); if(!bandwidth) return std::unexpected{std::move(bandwidth.error())};
        if(!bandwidth->diagnostics.empty()) return diagnostic_result(std::move(bandwidth->diagnostics));
        auto latency=process_family(*root,Q0bManifestFamily::latency,manifest->families[1]); if(!latency) return std::unexpected{std::move(latency.error())};
        if(!latency->diagnostics.empty()) return diagnostic_result(std::move(latency->diagnostics));
        return detail_MonitoringQ0bProductionInputsFactory::make_success(std::move(*file), actual,
            std::move(*manifest), std::move(*bandwidth->authority), std::move(*latency->authority));
    } catch(const std::exception&) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_exception")}; }
    catch(...) { return std::unexpected{error("monitoring.q0b_production_inputs.contained_failure","contained_unknown_failure")}; }
}
} // namespace epycbench::monitoring::qualification
