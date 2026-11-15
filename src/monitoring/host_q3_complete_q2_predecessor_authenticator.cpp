#include "monitoring/host_q3_complete_q2_predecessor_authenticator.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
#include "monitoring/host_q1_private_q0_value.hpp"
#include "monitoring/host_q2_context_switch_evidence.hpp"
#include "monitoring/privacy_validation.hpp"

#include <nlohmann/json.hpp>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::json;
auto failure(std::string classification) -> MonitoringQ3CompleteQ2PredecessorFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q3.predecessor_invalid"},
             .message = "Q3 complete Q2 predecessor rejected",
             .operation = "authenticate_monitoring_q3_predecessor",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}};
}
auto parse(std::string_view bytes, std::size_t limit) -> std::optional<Json> {
    if (bytes.empty() || bytes.size() > limit || bytes.back() != '\n') return std::nullopt;
    try {
        auto value = Json::parse(bytes.begin(), bytes.end() - 1);
        auto canonical = core::canonicalize_json(value);
        if (!canonical || *canonical + "\n" != bytes) return std::nullopt;
        return value;
    } catch (...) { return std::nullopt; }
}
auto exact(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}
auto canonical(Json value) -> std::optional<std::string> {
    auto bytes = core::canonicalize_json(value);
    if (!bytes) return std::nullopt;
    bytes->push_back('\n');
    return std::move(*bytes);
}
auto environment_runtime_binding_matches(const Json& environment, const Json& binding) -> bool {
    if (!environment.contains("topology") || !environment["topology"].is_object() ||
        !environment["topology"].contains("base_page_size_bytes") ||
        !environment["topology"]["base_page_size_bytes"].is_number_unsigned() ||
        environment["topology"]["base_page_size_bytes"].get<std::uint64_t>() == 0U ||
        !environment.contains("bandwidth_workers") ||
        !environment["bandwidth_workers"].is_array() ||
        environment["bandwidth_workers"].empty() ||
        !exact(binding, {"qualification_plan_digest", "page_size_bytes", "worker_count", "workers"}) ||
        !binding["workers"].is_array() ||
        binding["page_size_bytes"] != environment["topology"]["base_page_size_bytes"] ||
        binding["worker_count"] != environment["bandwidth_workers"].size() ||
        binding["workers"].size() != environment["bandwidth_workers"].size())
        return false;
    for (std::size_t index{}; index < environment["bandwidth_workers"].size(); ++index) {
        const auto& authority = environment["bandwidth_workers"][index];
        const auto& worker = binding["workers"][index];
        if (!exact(authority, {"linux_cpu", "stable_core_id", "stable_numa_node_id",
                               "stable_pu_id", "worker_id"}) ||
            !exact(worker, {"worker_id", "linux_cpu", "stable_core_id", "stable_pu_id"}) ||
            worker["worker_id"] != authority["worker_id"] ||
            worker["linux_cpu"] != authority["linux_cpu"] ||
            worker["stable_core_id"] != authority["stable_core_id"] ||
            worker["stable_pu_id"] != authority["stable_pu_id"])
            return false;
    }
    return true;
}
auto q1_subset(const Json& full_manifest, const MonitoringQ3CompleteQ2PredecessorInput& input)
    -> std::optional<std::pair<std::string, std::string>> {
    if (!full_manifest.contains("artifacts") || !full_manifest["artifacts"].is_array() ||
        full_manifest["artifacts"].size() != 4U) return std::nullopt;
    auto manifest = full_manifest;
    manifest["artifacts"].erase(manifest["artifacts"].begin() + 3);
    auto manifest_bytes = canonical(std::move(manifest));
    if (!manifest_bytes) return std::nullopt;
    const std::array paths{"host-environment.json", "q0b/evidence.json", "q1/evidence.json"};
    const std::array digests{core::sha256(input.host_environment_bytes).to_hex(),
                             core::sha256(input.q0b_evidence_bytes).to_hex(),
                             core::sha256(input.q1_evidence_bytes).to_hex()};
    std::string checksums = core::sha256(*manifest_bytes).to_hex() + "  manifest.json\n";
    for (std::size_t index{}; index < paths.size(); ++index)
        checksums += digests[index] + "  " + paths[index] + "\n";
    return std::pair{std::move(*manifest_bytes), std::move(checksums)};
}
struct NoHistory final {
    static auto authenticate(void*, std::string_view, std::string_view, std::string_view,
                             std::string_view, std::string_view)
        -> std::expected<void, MonitoringQ2HistoricalFailure> { return {}; }
    static auto retire(void*) noexcept -> std::expected<void, core::Error> { return {}; }
};
} // namespace

auto MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate(
    MonitoringQ3CompleteQ2PredecessorInput input, Operations operations) noexcept
    -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                     MonitoringQ3CompleteQ2PredecessorFailure> {
    static const ExpectedDigests expected{
        "60a9367baca9edb8dcc143470cb76ed6a3912c7e7701cbf4082d31a37425f8b4",
        "6fa0f12980ea1da0c94898080e0ca77ec8b4c22385d592acc37434c29eb41d54",
        "7e910596d08f05446b0ac1cd9ade11e51977cafb8c6397b177d60a0c08f92022",
        "016c774dec1cd45a058693066f5751332dab3ba84f4346b1509a4de0aafe3e21",
        "2febf924b43c4c1a46f047e31645a27b0f939451e0d686bba13091b161ca1a26",
        "159dbfb9452becd49d664a7cb72c1d108e2871a0daca5734ee95c0f83287fd7a"};
    return authenticate_with(std::move(input), operations, expected);
}

auto MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_with(
    MonitoringQ3CompleteQ2PredecessorInput input, Operations operations,
    const ExpectedDigests& expected) noexcept
    -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                     MonitoringQ3CompleteQ2PredecessorFailure> {
    return authenticate_impl(std::move(input), operations, &expected, nullptr);
}

auto MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_campaign(
    MonitoringQ3CompleteQ2PredecessorInput input, Operations operations,
    const MonitoringQ1ValidatedPublicValues& values) noexcept
    -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                     MonitoringQ3CompleteQ2PredecessorFailure> {
    return authenticate_impl(std::move(input), operations, nullptr, &values);
}

auto MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_impl(
    MonitoringQ3CompleteQ2PredecessorInput input, Operations operations,
    const ExpectedDigests* expected, const MonitoringQ1ValidatedPublicValues* values) noexcept
    -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                     MonitoringQ3CompleteQ2PredecessorFailure> {
    try {
        if (values && ((input.qualification_id != "MON-007.host-v2.campaign-20261005-01" && input.qualification_id != "MON-007.host-v2.campaign-20261018-01" && input.qualification_id != "MON-007.host-v2.campaign-20261020-01" && input.qualification_id != "MON-007.host-v2.campaign-20261024-01" && input.qualification_id != "MON-007.host-v2.campaign-20261102-01") ||
            values->qualification_id() != input.qualification_id ||
            values->plan().qualification_id() != input.qualification_id ||
            values->profile_sha256() != core::sha256(input.profile_bytes).to_hex() ||
            !MonitoringQ1PrivateQ0Value::validate_successor_subset(
                input.host_environment_bytes, input.q0b_evidence_bytes, *values)))
            return std::unexpected{failure("campaign_original_q0")};
        if (values) {
            auto plan = parse(input.q1_plan_bytes, 1U << 20U);
            if (!plan || (*plan)["qualification_id"] != values->qualification_id() ||
                (*plan)["profile"]["sha256"] != values->plan().profile_sha256().to_hex() ||
                (*plan)["worker_set"]["qualification_plan_digest"] !=
                    "sha256:" + values->plan().qualification_plan_digest().to_hex() ||
                (*plan)["worker_set"]["worker_ids"].size() != values->plan().worker_ids().size() ||
                (*plan)["collection"]["observations_per_worker"] != values->plan().observations_per_worker() ||
                (*plan)["collection"]["execution_deadline_seconds"] != values->plan().execution_deadline_seconds() ||
                (*plan)["evidence_limits"]["conservative_observation_capacity_per_worker"] != values->plan().capacity() ||
                (*plan)["evidence_limits"]["maximum_successor_tree_bytes"] != values->plan().maximum_successor_tree_bytes() ||
                (*plan)["derivation"]["total_observations"] != values->plan().total_observations() ||
                (*plan)["derivation"]["per_observation_allowance_ms"] != values->plan().per_observation_allowance_ms() ||
                (*plan)["derivation"]["fixed_reserve_seconds"] != values->plan().fixed_reserve_seconds() ||
                (*plan)["derivation"]["mode"] !=
                    (values->plan().derivation_mode() == MonitoringQ1PlanDerivationMode::default_mode
                         ? "default" : "governed_override"))
                return std::unexpected{failure("campaign_q1_plan")};
            for (std::size_t index{}; index < values->plan().worker_ids().size(); ++index)
                if ((*plan)["worker_set"]["worker_ids"][index] != values->plan().worker_ids()[index].value())
                    return std::unexpected{failure("campaign_q1_plan")};
        }
        auto manifest = parse(input.manifest_bytes, 1U << 20U);
        auto environment = parse(input.host_environment_bytes, 1U << 20U);
        auto q0 = parse(input.q0b_evidence_bytes, 8U << 20U);
        auto q1 = parse(input.q1_evidence_bytes, 8U << 20U);
        auto q2 = parse(input.q2_evidence_bytes, 16U << 20U);
        if (!manifest || !environment || !q0 || !q1 || !q2 || !privacy_safe(*manifest) ||
            !privacy_safe(*environment) || !privacy_safe(*q0) || !privacy_safe(*q1) ||
            !privacy_safe(*q2))
            return std::unexpected{failure("canonical_json_privacy")};
        if (!validate_monitoring_q2_context_switch_evidence(input.q2_evidence_bytes))
            return std::unexpected{failure("q2_evidence")};
        const std::array paths{"host-environment.json", "q0b/evidence.json", "q1/evidence.json",
                               "q2/evidence.json"};
        const std::array digests{core::sha256(input.host_environment_bytes).to_hex(),
                                 core::sha256(input.q0b_evidence_bytes).to_hex(),
                                 core::sha256(input.q1_evidence_bytes).to_hex(),
                                 core::sha256(input.q2_evidence_bytes).to_hex()};
        if (expected) {
            const std::array frozen{expected->host_environment, expected->q0b, expected->q1, expected->q2};
            if (digests != frozen || core::sha256(input.manifest_bytes).to_hex() != expected->manifest ||
                core::sha256(input.checksums_bytes).to_hex() != expected->checksums)
                return std::unexpected{failure("frozen_digest")};
        }
        if (values && (*q2)["disposition"] != "pass")
            return std::unexpected{failure("campaign_q2_disposition")};
        if (!exact(*manifest, {"schema", "qualification_id", "tool", "profile",
                               "environment_digest", "artifacts"}) ||
            (*manifest)["schema"] != "epycbench.monitoring-qualification-manifest/v3" ||
            (*manifest)["qualification_id"] != input.qualification_id ||
            !(*manifest)["artifacts"].is_array() || (*manifest)["artifacts"].size() != 4U)
            return std::unexpected{failure("manifest")};
        for (std::size_t index{}; index < paths.size(); ++index)
            if ((*manifest)["artifacts"][index] !=
                Json{{"path", paths[index]}, {"sha256", digests[index]}})
                return std::unexpected{failure("manifest_inventory")};
        std::string checksums = core::sha256(input.manifest_bytes).to_hex() + "  manifest.json\n";
        for (std::size_t index{}; index < paths.size(); ++index)
            checksums += digests[index] + "  " + paths[index] + "\n";
        if (input.checksums_bytes != checksums) return std::unexpected{failure("checksums")};
        auto predecessor = q1_subset(*manifest, input);
        if (!predecessor) return std::unexpected{failure("q1_subset")};
        const auto& provenance = (*q2)["provenance"];
        const auto q2_commit = provenance["repository_commit"].get<std::string>();
        const auto q1_commit = provenance["q1_live_repository_commit"].get<std::string>();
        const auto q0_commit = provenance["q0_profile_repository_commit"].get<std::string>();
        if ((*q2)["qualification_id"] != input.qualification_id ||
            provenance["profile_sha256"] != core::sha256(input.profile_bytes).to_hex() ||
            provenance["host_environment_digest"] != (*manifest)["environment_digest"] ||
            provenance["q0b_evidence_sha256"] != digests[1] ||
            provenance["q1_evidence_sha256"] != digests[2] ||
            provenance["predecessor_manifest_sha256"] != core::sha256(predecessor->first).to_hex() ||
            provenance["predecessor_checksums_sha256"] != core::sha256(predecessor->second).to_hex() ||
            (*q2)["collection_plan"]["sha256"] != core::sha256(input.q2_plan_bytes).to_hex())
            return std::unexpected{failure("q2_provenance")};
        if ((*q2)["runtime_binding"]["qualification_plan_digest"] !=
                (*q1)["qualification_plan"]["sha256"] ||
            !(*q1)["workers"].is_array() ||
            (*q2)["runtime_binding"]["workers"].size() != (*q1)["workers"].size())
            return std::unexpected{failure("q1_q2_runtime_binding")};
        for (std::size_t index{}; index < (*q1)["workers"].size(); ++index)
            if ((*q2)["runtime_binding"]["workers"][index]["worker_id"] !=
                (*q1)["workers"][index]["worker_id"])
                return std::unexpected{failure("q1_q2_runtime_binding")};
        if (!environment_runtime_binding_matches(*environment, (*q2)["runtime_binding"]))
            return std::unexpected{failure("environment_q2_runtime_binding")};
        std::array<std::optional<core::Error>,
                   MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures> stock{};
        for (auto& item : stock) {
            core::Error value{};
            value.category = core::ErrorCategory::monitoring;
            value.code = core::StableErrorCode{"monitoring.q3.predecessor_invalid"};
            value.operation = "authenticate_monitoring_q3_predecessor";
            value.severity = core::ErrorSeverity::error;
            value.continuation = core::ContinuationPolicy::abort_command;
            item.emplace(std::move(value));
        }
        MonitoringQ2CompleteQ1PredecessorInput q1_input{
            input.qualification_id, q2_commit, input.profile_bytes, input.q1_plan_bytes,
            input.host_environment_bytes, input.q0b_evidence_bytes, input.q1_evidence_bytes,
            predecessor->first, predecessor->second};
        auto q1_authority = MonitoringQ2CompleteQ1PredecessorAuthority::authenticate(
            std::move(q1_input), {nullptr, NoHistory::authenticate, NoHistory::retire}, stock);
        if (!q1_authority) return std::unexpected{failure("q1_predecessor")};
        if (!q1_authority->retire()) return std::unexpected{failure("q1_retirement")};
        if (!operations.authenticate_history)
            return std::unexpected{failure("history_operations")};
        auto history = operations.authenticate_history(
            operations.context, q0_commit, q1_commit, q2_commit, input.current_commit,
            input.profile_bytes, input.q1_plan_bytes, input.q2_plan_bytes, input.q3_plan_bytes);
        if (!history) {
            MonitoringQ3CompleteQ2PredecessorFailure result{std::move(history.error().primary)};
            result.cleanup_count = history.error().cleanup_count;
            for (std::size_t index{}; index < result.cleanup_count; ++index)
                result.cleanup[index] = std::move(history.error().cleanup[index]);
            return std::unexpected{std::move(result)};
        }
        return MonitoringQ3CompleteQ2PredecessorValue{
            q0_commit, q1_commit, q2_commit,
            provenance["host_environment_digest"].get<std::string>(),
            (*q2)["runtime_binding"]["qualification_plan_digest"].get<std::string>(),
            core::sha256(input.q0b_evidence_bytes), core::sha256(input.q1_evidence_bytes),
            core::sha256(input.q2_evidence_bytes), core::sha256(input.manifest_bytes),
            core::sha256(input.checksums_bytes), (*q2)["disposition"] == "unsupported"};
    } catch (...) { return std::unexpected{failure("contained")}; }
}

} // namespace epycbench::monitoring::qualification::detail
