#include "monitoring/host_q4_complete_q3_predecessor_authenticator.hpp"
#include "monitoring/host_q1_private_q0_value.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "monitoring/host_q3_complete_q2_predecessor_authenticator.hpp"
#include "monitoring/host_q3_temperature_evidence_json.hpp"
#include "monitoring/privacy_validation.hpp"
#include "core/json_ownership.hpp"

#include <nlohmann/json.hpp>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Json = nlohmann::json;
thread_local void (*prefix_test_hook)(unsigned char) noexcept {};
// Unvalidated parser trees have no depth precondition. Retire only leaves,
// then rescan the exclusively owned root; no recursion or auxiliary stock.
struct ParsedJsonCleanup final {
    Json& value;
    ~ParsedJsonCleanup() noexcept {
        while (value.is_structured()) {
            auto* leaf = &value;
            for (;;) {
                Json* next{};
                for (auto& child : *leaf)
                    if (child.is_structured()) { next = &child; break; }
                if (!next) break;
                leaf = next;
            }
            leaf->clear();
            *leaf = nullptr;
        }
    }
};
constexpr std::string_view qid{"MON-007.host-v2.20260906-05"};
constexpr std::array<std::string_view, 7> frozen{
    "60a9367baca9edb8dcc143470cb76ed6a3912c7e7701cbf4082d31a37425f8b4",
    "6fa0f12980ea1da0c94898080e0ca77ec8b4c22385d592acc37434c29eb41d54",
    "7e910596d08f05446b0ac1cd9ade11e51977cafb8c6397b177d60a0c08f92022",
    "016c774dec1cd45a058693066f5751332dab3ba84f4346b1509a4de0aafe3e21",
    "985ad325fe93db1fcf747201cf4b50d03bc1625f624edc9cac90ac5b33aa9925",
    "5bc584c2f263465532616e69c6fe17418117e107c0d3b979ed807bdbd6837bf1",
    "33b6ee7c981323ef3fe5162649502cd9de0726cb9c586f0f3ff5e9adc26846d4"};
auto rejected(std::string classification) -> MonitoringQ4CompleteQ3PredecessorFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{"monitoring.q4.complete_q3_predecessor.invalid"},
             .message = "Q4 complete Q3 predecessor rejected",
             .operation = "authenticate_monitoring_q4_complete_q3_predecessor",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command}};
}
auto parse(std::string_view bytes, std::size_t cap) -> std::optional<Json> {
    if (bytes.empty() || bytes.size() > cap || bytes.back() != '\n') return std::nullopt;
    try {
        auto result = Json::parse(bytes.begin(), bytes.end() - 1);
        auto encoded = core::canonicalize_json(result);
        if (!encoded || *encoded + "\n" != bytes || !privacy_safe(result)) return std::nullopt;
        return result;
    } catch (...) { return std::nullopt; }
}
auto canonical(Json value) -> std::optional<std::string> {
    auto result = core::canonicalize_json(value);
    if (!result) return std::nullopt;
    result->push_back('\n');
    return std::move(*result);
}
auto exact_worker(const Json& left, const Json& right) -> bool {
    return left == right;
}
auto exact(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}
auto u32(const Json& value) -> bool {
    return value.is_number_unsigned() &&
           value.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
}
auto nonempty(const Json& value) -> bool { return value.is_string() && !value.get_ref<const std::string&>().empty(); }
auto string_array(const Json& value) -> bool {
    if (!value.is_array() || value.empty()) return false;
    std::set<std::string> seen;
    for (const auto& member : value)
        if (!nonempty(member) || !seen.insert(member.get<std::string>()).second) return false;
    return true;
}
auto cpu_array(const Json& value) -> bool {
    if (!value.is_array() || value.empty()) return false;
    std::set<std::uint64_t> seen;
    for (const auto& member : value)
        if (!u32(member) || !seen.insert(member.get<std::uint64_t>()).second) return false;
    return true;
}
auto worker_shape(const Json& worker) -> bool {
    return exact(worker, {"linux_cpu", "stable_core_id", "stable_numa_node_id",
                          "stable_pu_id", "worker_id"}) &&
           u32(worker["linux_cpu"]) && u32(worker["worker_id"]) &&
           nonempty(worker["stable_core_id"]) && nonempty(worker["stable_numa_node_id"]) &&
           nonempty(worker["stable_pu_id"]);
}
auto service_shape(const Json& service, std::string_view role) -> bool {
    return exact(service, {"execution_linux_cpu", "execution_stable_pu_id", "linux_cpus",
                           "role", "stable_core_id", "stable_pu_ids"}) &&
           service["role"] == role && u32(service["execution_linux_cpu"]) &&
           nonempty(service["execution_stable_pu_id"]) && nonempty(service["stable_core_id"]) &&
           cpu_array(service["linux_cpus"]) && string_array(service["stable_pu_ids"]);
}
auto environment_shape(const Json& environment) -> bool {
    constexpr std::array capability_keys{"authentic_amd", "bandwidth_kernel", "base_page_size",
        "calibration", "eligible_numa_nonempty", "epyc_model", "fingerprints",
        "latency_kernel", "linux", "stable_ids", "topology_valid", "two_service_cores", "x86_64"};
    if (!exact(environment, {"bandwidth_workers", "capabilities", "latency_workers",
              "platform", "policy_identity", "privacy_validation", "schema",
              "selection_policy", "service_reservation", "topology"}) ||
        environment["schema"] != "epycbench.monitoring-host-environment/v2" ||
        environment["policy_identity"] != "epycbench.monitoring-qualification.host-generic/v2" ||
        environment["privacy_validation"] != "passed" ||
        !exact(environment["capabilities"], {"authentic_amd", "bandwidth_kernel", "base_page_size",
          "calibration", "eligible_numa_nonempty", "epyc_model", "fingerprints", "latency_kernel",
          "linux", "stable_ids", "topology_valid", "two_service_cores", "x86_64"})) return false;
    for (const auto key : capability_keys) if (environment["capabilities"][key] != true) return false;
    if (!exact(environment["platform"], {"architecture", "cpu_model", "cpu_vendor", "os"}) ||
        environment["platform"]["architecture"] != "x86_64" ||
        environment["platform"]["cpu_vendor"] != "AuthenticAMD" ||
        environment["platform"]["os"] != "Linux" || !nonempty(environment["platform"]["cpu_model"]) ||
        environment["selection_policy"] != Json{{"id", "epycbench.monitoring-host-selection"}, {"version", 1}} ||
        !exact(environment["service_reservation"], {"cores"}) ||
        !environment["service_reservation"]["cores"].is_array() ||
        environment["service_reservation"]["cores"].size() != 2U ||
        !service_shape(environment["service_reservation"]["cores"][0], "control") ||
        !service_shape(environment["service_reservation"]["cores"][1], "measurement")) return false;
    const auto& topology=environment["topology"];
    if (!exact(topology, {"base_page_size_bytes", "effective_fingerprint", "eligible_nodes", "physical_fingerprint"}) ||
        !topology["base_page_size_bytes"].is_number_unsigned() || topology["base_page_size_bytes"] == 0 ||
        !topology["eligible_nodes"].is_array() || topology["eligible_nodes"].empty()) return false;
    for (const auto key : {"effective_fingerprint", "physical_fingerprint"}) {
        if (!topology[key].is_string()) return false;
        const auto& text=topology[key].get_ref<const std::string&>();
        if (text.size()!=71U || !text.starts_with("sha256:") ||
            !std::ranges::all_of(text.substr(7), [](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
    }
    for (const auto& node : topology["eligible_nodes"])
        if (!exact(node, {"installed_memory_bytes", "stable_numa_node_id"}) ||
            !node["installed_memory_bytes"].is_number_unsigned() || node["installed_memory_bytes"] == 0 ||
            !nonempty(node["stable_numa_node_id"])) return false;
    if (!environment["bandwidth_workers"].is_array() || environment["bandwidth_workers"].empty() ||
        !environment["latency_workers"].is_array() || environment["latency_workers"].size()!=1U) return false;
    return std::ranges::all_of(environment["bandwidth_workers"], worker_shape) &&
           worker_shape(environment["latency_workers"][0]);
}
struct HistoryContext final {
    const MonitoringQ4HistoricalGitValue* history{};
    const MonitoringQ4GovernedInputCandidate* candidate{};
    static auto authenticate(void* raw, std::string_view q0, std::string_view q1,
                             std::string_view q2, std::string_view current,
                             std::string_view profile, std::string_view q1_plan,
                             std::string_view q2_plan, std::string_view q3_plan)
        -> std::expected<void, MonitoringQ3HistoricalGitFailure> {
        const auto& self = *static_cast<HistoryContext*>(raw);
        if (q0 != self.history->q0_commit() || q1 != self.history->q1_commit() ||
            q2 != self.history->q2_commit() || current != self.history->q3_commit() ||
            profile != self.candidate->profile().bytes ||
            q1_plan != self.candidate->q1_plan().bytes ||
            q2_plan != self.candidate->q2_plan().bytes ||
            q3_plan != self.candidate->q3_plan().bytes)
            return std::unexpected{MonitoringQ3HistoricalGitFailure{}};
        return {};
    }
};
} // namespace

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_environment(
    std::string_view bytes) noexcept -> bool {
    try {
        auto value=parse(bytes,1U<<20U);
        return value && environment_shape(*value);
    } catch (...) { return false; }
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_outer_canonical(
    CanonicalArtifactRole role, std::string_view bytes) noexcept -> bool {
    std::size_t cap{};
    switch (role) {
    case CanonicalArtifactRole::host_environment: cap=1U<<20U; break;
    case CanonicalArtifactRole::q1_evidence: cap=8U<<20U; break;
    case CanonicalArtifactRole::q2_evidence: cap=16U<<20U; break;
    case CanonicalArtifactRole::q3_evidence: cap=512U<<10U; break;
    default: return false;
    }
    try { return parse(bytes,cap).has_value(); } catch (...) { return false; }
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate(
    MonitoringQ4CompleteQ3PredecessorInput input,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                     MonitoringQ4CompleteQ3PredecessorFailure> {
    static const ExpectedDigests expected{
        {std::string{frozen[0]}, std::string{frozen[1]}, std::string{frozen[2]},
         std::string{frozen[3]}, std::string{frozen[4]}, std::string{frozen[5]},
         std::string{frozen[6]}},
        {std::string{frozen[0]}, std::string{frozen[1]}, std::string{frozen[2]},
         std::string{frozen[3]},
         "2febf924b43c4c1a46f047e31645a27b0f939451e0d686bba13091b161ca1a26",
         "159dbfb9452becd49d664a7cb72c1d108e2871a0daca5734ee95c0f83287fd7a"},
        nullptr, nullptr};
    return authenticate_with(std::move(input), candidate, history, expected);
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::candidate_active(
    const MonitoringQ4GovernedInputCandidate& candidate) noexcept -> bool {
    return candidate.valid_ && static_cast<bool>(candidate.binding_);
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_with(
    MonitoringQ4CompleteQ3PredecessorInput input,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history,
    const ExpectedDigests& expected)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                     MonitoringQ4CompleteQ3PredecessorFailure> {
    return authenticate_impl(std::move(input), candidate, history, &expected, nullptr);
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_campaign(
    MonitoringQ4CompleteQ3PredecessorInput input,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history,
    const MonitoringQ1ValidatedPublicValues& values)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                     MonitoringQ4CompleteQ3PredecessorFailure> {
    return authenticate_impl(std::move(input), candidate, history, nullptr, &values);
}

void MonitoringQ4CompleteQ3PredecessorAuthenticator::set_prefix_test_hook(
    void (*hook)(unsigned char) noexcept) noexcept {
    prefix_test_hook = hook;
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::retained_depth_valid(
    std::string_view bytes, std::size_t maximum) noexcept -> bool {
    std::size_t depth{};
    bool quoted{}, escaped{};
    for (const char byte : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (byte == '\\') escaped = true;
            else if (byte == '"') quoted = false;
        } else if (byte == '"') quoted = true;
        else if (byte == '{' || byte == '[') {
            if (depth == maximum) return false;
            ++depth;
        } else if (byte == '}' || byte == ']') {
            if (depth == 0U) return false;
            --depth;
        }
    }
    // Syntax, UTF-8, exact schema and canonical bytes remain the parser's work.
    return !quoted && depth == 0U;
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::completed_prefix(
    const std::array<std::string, 7>& bytes, std::string_view q4_bytes,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ1ValidatedPublicValues& values)
    -> std::expected<CompletedPrefix, MonitoringQ4CompleteQ3PredecessorFailure> {
    auto contained = rejected("completed_prefix_contained");
    try {
        if (!candidate_active(candidate) ||
            (values.qualification_id() != "MON-007.host-v2.campaign-20261005-01" && values.qualification_id() != "MON-007.host-v2.campaign-20261018-01" && values.qualification_id() != "MON-007.host-v2.campaign-20261020-01" && values.qualification_id() != "MON-007.host-v2.campaign-20261024-01" && values.qualification_id() != "MON-007.host-v2.campaign-20261102-01") ||
            values.profile_sha256() != candidate.profile().sha256.to_hex() ||
            q4_bytes.empty() || q4_bytes.size() > 268435456U || q4_bytes.back() != '\n' ||
            bytes[5].empty() || bytes[5].size() > (1U << 20U))
            return std::unexpected{rejected("completed_prefix_binding")};
        // Existing closed grammar: manifest/artifacts/artifact is depth three;
        // Q4/result/domains/domain/linux_cpus is depth five. This refuses only
        // eventual-schema-invalid nesting before recursive untimed validation.
        if (!retained_depth_valid(bytes[5], 3U) || !retained_depth_valid(q4_bytes, 5U))
            return std::unexpected{rejected("completed_manifest")};
        auto manifest = Json::parse(bytes[5]);
        ParsedJsonCleanup manifest_cleanup{manifest};
        auto q4 = Json::parse(q4_bytes);
        ParsedJsonCleanup q4_cleanup{q4};
        if (prefix_test_hook) prefix_test_hook(1U);
        const auto manifest_encoded = core::canonicalize_json(manifest);
        const auto q4_encoded = core::canonicalize_json(q4);
        constexpr std::array paths{"host-environment.json", "q0b/evidence.json",
            "q1/evidence.json", "q2/evidence.json", "q3/evidence.json", "q4/evidence.json"};
        if (!manifest_encoded || *manifest_encoded + "\n" != bytes[5] ||
            !q4_encoded || *q4_encoded + "\n" != q4_bytes ||
            !privacy_safe(manifest) || !privacy_safe(q4) ||
            !exact(manifest, {"artifacts", "environment_digest", "profile",
                             "qualification_id", "schema", "tool"}) ||
            manifest["qualification_id"] != values.qualification_id() ||
            manifest["schema"] != "epycbench.monitoring-qualification-manifest/v3" ||
            !exact(manifest["tool"], {"identity", "version"}) ||
            manifest["tool"]["identity"] != "epycbench.monitoring-qualification-tool/v1" ||
            manifest["tool"]["version"] != "1.0.0" ||
            !exact(manifest["profile"], {"identity", "sha256", "stage"}) ||
            manifest["profile"]["identity"] != "epycbench.monitoring-qualification.host-generic/v2" ||
            manifest["profile"]["sha256"] != candidate.profile().sha256.to_hex() ||
            manifest["profile"]["stage"] != "final_frozen_profile" ||
            manifest["environment_digest"] != "sha256:" + core::sha256(bytes[0]).to_hex() ||
            !manifest["artifacts"].is_array() || manifest["artifacts"].size() != paths.size() ||
            !q4.is_object() || !q4.contains("provenance") ||
            !q4["provenance"].is_object())
            return std::unexpected{rejected("completed_manifest")};
        std::string checksums = core::sha256(bytes[5]).to_hex() + "  manifest.json\n";
        for (std::size_t i{}; i < paths.size(); ++i) {
            const auto digest = core::sha256(i < 5U ? std::string_view{bytes[i]} : q4_bytes).to_hex();
            const auto& artifact = manifest["artifacts"][i];
            if (!exact(artifact, {"path", "sha256"}) || artifact["path"] != paths[i] ||
                artifact["sha256"] != digest)
                return std::unexpected{rejected("completed_inventory")};
            checksums += digest + "  " + paths[i] + "\n";
        }
        if (checksums != bytes[6])
            return std::unexpected{rejected("completed_checksums")};
        if (prefix_test_hook) prefix_test_hook(2U);
        // This child now has the exact flat artifact shape. Erase must never
        // invoke nlohmann's allocating structured destructor under denial.
        core::detail::retire_json(manifest["artifacts"][5]);
        manifest["artifacts"].erase(manifest["artifacts"].begin() + 5);
        auto prefix = core::canonicalize_json(manifest);
        if (!prefix) return std::unexpected{rejected("completed_prefix")};
        prefix->push_back('\n');
        std::string prefix_checksums = core::sha256(*prefix).to_hex() + "  manifest.json\n";
        for (std::size_t i{}; i < 5U; ++i)
            prefix_checksums += core::sha256(bytes[i]).to_hex() + "  " + paths[i] + "\n";
        const auto& provenance = q4["provenance"];
        if (provenance.at("predecessor_manifest_sha256") != core::sha256(*prefix).to_hex() ||
            provenance.at("predecessor_checksums_sha256") != core::sha256(prefix_checksums).to_hex() ||
            !provenance.at("repository_commit").is_string())
            return std::unexpected{rejected("completed_predecessor_association")};
        return CompletedPrefix{{std::move(*prefix), std::move(prefix_checksums)},
            provenance.at("repository_commit").get<std::string>()};
    } catch (...) { return std::unexpected{std::move(contained)}; }
}

auto MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_impl(
    MonitoringQ4CompleteQ3PredecessorInput input,
    const MonitoringQ4GovernedInputCandidate& candidate,
    const MonitoringQ4HistoricalGitValue& history, const ExpectedDigests* expected,
    const MonitoringQ1ValidatedPublicValues* values)
    -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                     MonitoringQ4CompleteQ3PredecessorFailure> {
    auto contained = rejected("contained");
    try {
        if (expected && expected->fault) expected->fault(expected->fault_context, 0U);
        if (!candidate.valid_ || !candidate.binding_ ||
            candidate.binding_ != history.binding_ ||
            candidate.head_ != history.commits_[4])
            return std::unexpected{rejected("authority_binding")};
        const auto qualification_id = values ? values->qualification_id() : qid;
        if (values && ((qualification_id != "MON-007.host-v2.campaign-20261005-01" && qualification_id != "MON-007.host-v2.campaign-20261018-01" && qualification_id != "MON-007.host-v2.campaign-20261020-01" && qualification_id != "MON-007.host-v2.campaign-20261024-01" && qualification_id != "MON-007.host-v2.campaign-20261102-01") ||
            values->profile_sha256() != candidate.profile().sha256.to_hex() ||
            !MonitoringQ1PrivateQ0Value::validate_successor_subset(
                input.host_environment, input.q0b_evidence, *values)))
            return std::unexpected{rejected("campaign_original_q0")};
        const std::array<std::string_view, 7> bytes{
            input.host_environment, input.q0b_evidence, input.q1_evidence,
            input.q2_evidence, input.q3_evidence, input.manifest, input.checksums};
        const auto empty_digest = core::sha256(std::string_view{});
        std::array<core::Sha256Digest, 7> digests{
            empty_digest, empty_digest, empty_digest, empty_digest,
            empty_digest, empty_digest, empty_digest};
        for (std::size_t index{}; index < bytes.size(); ++index) {
            digests[index] = core::sha256(bytes[index]);
            if (expected && digests[index].to_hex() != expected->artifacts[index])
                return std::unexpected{rejected("frozen_digest")};
        }
        auto environment = parse(input.host_environment, 1U << 20U);
        auto q1 = parse(input.q1_evidence, 8U << 20U);
        auto q2 = parse(input.q2_evidence, 16U << 20U);
        auto q3 = parse(input.q3_evidence, 512U << 10U);
        auto manifest = parse(input.manifest, 1U << 20U);
        if (!environment || !q1 || !q2 || !q3 || !manifest)
            return std::unexpected{rejected("canonical_json_privacy")};
        auto decoded_q3 = validate_monitoring_q3_temperature_evidence_json(input.q3_evidence);
        if (!decoded_q3)
            return std::unexpected{rejected("q3_evidence")};
        constexpr std::array paths{"host-environment.json", "q0b/evidence.json",
                                   "q1/evidence.json", "q2/evidence.json",
                                   "q3/evidence.json"};
        if (!exact(*manifest, {"artifacts", "environment_digest", "profile",
                               "qualification_id", "schema", "tool"}) ||
            !exact((*manifest)["tool"], {"identity", "version"}) ||
            !exact((*manifest)["profile"], {"identity", "sha256", "stage"}) ||
            (*manifest)["tool"] != Json{{"identity", "epycbench.monitoring-qualification-tool/v1"},
                                         {"version", "1.0.0"}} ||
            (*manifest)["profile"] != Json{{"identity", "epycbench.monitoring-qualification.host-generic/v2"},
                                            {"sha256", candidate.profile().sha256.to_hex()},
                                            {"stage", "final_frozen_profile"}} ||
            (*manifest)["environment_digest"] != "sha256:" + digests[0].to_hex() ||
            (*manifest)["schema"] !=
                "epycbench.monitoring-qualification-manifest/v3" ||
            (*manifest)["qualification_id"] != qualification_id || !(*manifest)["artifacts"].is_array() ||
            (*manifest)["artifacts"].size() != 5U)
            return std::unexpected{rejected("manifest")};
        for (std::size_t index{}; index < paths.size(); ++index)
            if ((*manifest)["artifacts"][index] !=
                Json{{"path", paths[index]}, {"sha256", digests[index].to_hex()}})
                return std::unexpected{rejected("manifest_inventory")};
        std::string checksum = digests[5].to_hex() + "  manifest.json\n";
        for (std::size_t index{}; index < paths.size(); ++index)
            checksum += digests[index].to_hex() + "  " + paths[index] + "\n";
        if (checksum != input.checksums)
            return std::unexpected{rejected("checksums")};

        auto predecessor_manifest = *manifest;
        predecessor_manifest["artifacts"].erase(
            predecessor_manifest["artifacts"].begin() + 4);
        auto predecessor_manifest_bytes = canonical(std::move(predecessor_manifest));
        if (!predecessor_manifest_bytes)
            return std::unexpected{rejected("predecessor_manifest")};
        std::string predecessor_checksums =
            core::sha256(*predecessor_manifest_bytes).to_hex() + "  manifest.json\n";
        for (std::size_t index{}; index < 4U; ++index)
            predecessor_checksums += digests[index].to_hex() + "  " + paths[index] + "\n";
        HistoryContext history_context{&history, &candidate};
        MonitoringQ3CompleteQ2PredecessorInput q2_input{
            std::string{qualification_id}, history.commits_[3], candidate.profile().bytes,
            candidate.q1_plan().bytes, candidate.q2_plan().bytes, candidate.q3_plan().bytes,
            input.host_environment, input.q0b_evidence, input.q1_evidence, input.q2_evidence,
            std::move(*predecessor_manifest_bytes), std::move(predecessor_checksums)};
        auto q2_value = values
            ? MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_campaign(
                  std::move(q2_input), {&history_context, HistoryContext::authenticate}, *values)
            : MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_with(
                  std::move(q2_input), {&history_context, HistoryContext::authenticate},
                  expected->complete_q2);
        if (!q2_value)
            return std::unexpected{rejected("complete_q2_predecessor")};
        if (q2_value->q2_unsupported != (values == nullptr))
            return std::unexpected{rejected("q2_disposition")};

        const auto& binding = decoded_q3->binding;
        if (binding.qualification_id != qualification_id || binding.repository_commit != history.commits_[3] ||
            binding.profile_sha256 != candidate.profile().sha256.to_hex() ||
            binding.host_environment_digest != "sha256:" + digests[0].to_hex() ||
            binding.q0b_sha256 != digests[1].to_hex() || binding.q0_commit != history.commits_[0] ||
            binding.q1_sha256 != digests[2].to_hex() || binding.q1_commit != history.commits_[1] ||
            binding.q2_sha256 != digests[3].to_hex() || binding.q2_commit != history.commits_[2] ||
            binding.manifest_sha256 != q2_value->manifest.to_hex() ||
            binding.checksums_sha256 != q2_value->checksums.to_hex() ||
            binding.plan_sha256 != candidate.q3_plan().sha256.to_hex())
            return std::unexpected{rejected("q3_provenance")};

        if (!environment_shape(*environment) ||
            !(*environment)["topology"]["base_page_size_bytes"].is_number_unsigned() ||
            (*environment)["topology"]["base_page_size_bytes"].get<std::uint64_t>() == 0U ||
            !environment->contains("bandwidth_workers") ||
            !(*environment)["bandwidth_workers"].is_array() ||
            (*environment)["bandwidth_workers"].empty() ||
            !environment->contains("latency_workers") ||
            !(*environment)["latency_workers"].is_array() ||
            (*environment)["latency_workers"].size() != 1U ||
            !exact_worker((*environment)["latency_workers"][0],
                          (*environment)["bandwidth_workers"][0]))
            return std::unexpected{rejected("host_workers")};
        const auto& runtime = (*q2)["runtime_binding"];
        if (!exact(runtime, {"page_size_bytes", "qualification_plan_digest",
                             "worker_count", "workers"}) ||
            !runtime["page_size_bytes"].is_number_unsigned() ||
            runtime["page_size_bytes"] != (*environment)["topology"]["base_page_size_bytes"] ||
            !runtime["worker_count"].is_number_unsigned() ||
            runtime["worker_count"] != runtime["workers"].size() ||
            !runtime["workers"].is_array() ||
            runtime["workers"].size() != (*environment)["bandwidth_workers"].size() ||
            runtime["qualification_plan_digest"] != (*q1)["qualification_plan"]["sha256"])
            return std::unexpected{rejected("runtime_binding")};
        MonitoringQ4AuthenticatedHostProjection projection;
        projection.host_environment_digest = "sha256:" + digests[0].to_hex();
        projection.qualification_plan_digest =
            runtime["qualification_plan_digest"].get<std::string>();
        projection.physical_fingerprint =
            (*environment)["topology"]["physical_fingerprint"].get<std::string>();
        projection.effective_fingerprint =
            (*environment)["topology"]["effective_fingerprint"].get<std::string>();
        projection.authenticated_host_environment_digest = digests[0];
        const auto plan_authority = core::Sha256Digest::parse(
            std::string_view{projection.qualification_plan_digest}.substr(7));
        const auto physical_authority =
            topology::PhysicalTopologyFingerprint::parse(projection.physical_fingerprint);
        const auto effective_authority =
            topology::EffectiveTopologyFingerprint::parse(projection.effective_fingerprint);
        if (!plan_authority || !physical_authority || !effective_authority)
            return std::unexpected{rejected("authenticated_host_projection")};
        projection.authenticated_qualification_plan_digest = *plan_authority;
        projection.authenticated_physical_fingerprint = *physical_authority;
        projection.authenticated_effective_fingerprint = *effective_authority;
        projection.page_size_bytes = MonitoringQ4BasePageSizeBytes{
            runtime["page_size_bytes"].get<std::uint64_t>()};
        projection.bandwidth_worker_count = MonitoringQ4BandwidthWorkerCount{runtime["workers"].size()};
        for (std::size_t index{}; index < runtime["workers"].size(); ++index) {
            const auto& source = (*environment)["bandwidth_workers"][index];
            const auto& worker = runtime["workers"][index];
            if (!exact(source, {"linux_cpu", "stable_core_id", "stable_numa_node_id",
                                "stable_pu_id", "worker_id"}) ||
                !exact(worker, {"linux_cpu", "stable_core_id", "stable_pu_id", "worker_id"}) ||
                !source["worker_id"].is_number_unsigned() ||
                source["worker_id"].get<std::uint64_t>() != index ||
                !source["stable_core_id"].is_string() || !source["stable_numa_node_id"].is_string() ||
                !source["stable_pu_id"].is_string())
                return std::unexpected{rejected("worker_shape")};
            for (const auto key : {"worker_id", "linux_cpu", "stable_core_id", "stable_pu_id"})
                if (worker[key] != source[key])
                    return std::unexpected{rejected("runtime_workers")};
            if (!source["linux_cpu"].is_number_unsigned())
                return std::unexpected{rejected("linux_cpu")};
            const auto cpu = source["linux_cpu"].get<std::uint64_t>();
            if (cpu > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected{rejected("linux_cpu")};
            projection.admitted_linux_cpus.push_back(
                MonitoringQ4AdmittedLinuxCpuId{static_cast<std::uint32_t>(cpu)});
        }
        std::ranges::sort(projection.admitted_linux_cpus);
        if (std::ranges::adjacent_find(projection.admitted_linux_cpus) !=
            projection.admitted_linux_cpus.end())
            return std::unexpected{rejected("linux_cpu_duplicate")};
        const std::array<std::uint64_t, 3> q3_counts{
            decoded_q3->discovery_count, decoded_q3->agreement_count,
            decoded_q3->typed_count};
        const bool resources_retired =
            decoded_q3->authority == MonitoringQ3ResourceFinalization::retired &&
            decoded_q3->source == MonitoringQ3ResourceFinalization::retired &&
            decoded_q3->oracle == MonitoringQ3ResourceFinalization::retired;
        if (decoded_q3->disposition != MonitoringQ3TemperatureDisposition::pass ||
            q3_counts != std::array<std::uint64_t, 3>{3U, 48U, 48U} || !resources_retired)
            return std::unexpected{rejected("q3_completion")};
        std::array<std::string, 5> commits{
            std::string{history.q0_commit()}, std::string{history.q1_commit()},
            std::string{history.q2_commit()}, std::string{history.q3_commit()},
            std::string{history.current_commit()}};
        return MonitoringQ4CompleteQ3PredecessorValue{
            std::move(input), std::move(digests), std::move(projection),
            binding.build_identity, std::move(commits), q2_value->q2_unsupported,
            q3_counts, resources_retired, candidate.binding_, std::string{qualification_id}};
    } catch (...) { return std::unexpected{std::move(contained)}; }
}

} // namespace epycbench::monitoring::qualification::detail
