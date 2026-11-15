#include "monitoring/host_q1_validated_public_values.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
class BoundedPublicSax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t, const string_t&) override { return value(); }
    bool string(string_t& text) override { return text.size() <= (1U << 20U) && value(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return start(); }
    bool key(string_t& text) override {
        return depth_ != 0U && text.size() <= 256U && ++members_ <= 1024U;
    }
    bool end_object() override {
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override { return start(); }
    bool end_array() override {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

  private:
    bool value() noexcept { return ++values_ <= 4096U; }
    bool start() noexcept { return value() && ++depth_ <= 16U; }
    std::size_t depth_{}, members_{}, values_{};
};
auto structural_preflight(std::string_view bytes) -> bool {
    BoundedPublicSax sax;
    return nlohmann::json::sax_parse(bytes.begin(), bytes.end(), &sax);
}
auto failure(std::string classification) -> MonitoringQ1GitCommandFailure {
    return {{.category = core::ErrorCategory::monitoring,
             .code = core::StableErrorCode{classification.starts_with("contained_")
                                               ? "monitoring.q1_plan_authority.contained_failure"
                                               : "monitoring.q1_plan_authority.plan_rejected"},
             .message = "Q1 governed public values rejected",
             .operation = "authenticate_monitoring_q1_plan",
             .os_error = std::nullopt,
             .context = {{"classification", std::move(classification)}},
             .severity = core::ErrorSeverity::error,
             .continuation = core::ContinuationPolicy::abort_command},
            {}};
}
auto plan_failure(const core::Error& source) -> MonitoringQ1GitCommandFailure {
    auto source_classification =
        source.context.contains("classification") && source.context.at("classification").is_string()
            ? source.context.at("classification").get<std::string>()
            : std::string{"schema"};
    std::string classification{"schema"};
    if (source_classification.starts_with("capacity"))
        classification = "capacity";
    else if (source_classification.starts_with("deadline"))
        classification = "deadline";
    else if (source_classification == "derivation" ||
             source_classification == "default_derivation" ||
             source_classification == "override_derivation" || source_classification == "mode" ||
             source_classification == "total" || source_classification == "total_overflow")
        classification = "derivation";
    else if (source_classification == "worker_set")
        classification = "worker_set";
    else if (source_classification == "constant")
        classification = "version_chain";
    else if (source_classification == "binding" || source_classification == "digest")
        classification = "binding";
    return failure(std::move(classification));
}
auto parse_canonical(std::string_view bytes) -> std::optional<nlohmann::json> {
    if (bytes.empty() || bytes.size() > (1U << 20U) || bytes.back() != '\n' ||
        bytes.find('\n') != bytes.size() - 1U)
        return std::nullopt;
    const auto body = bytes.substr(0, bytes.size() - 1U);
    if (!structural_preflight(body))
        return std::nullopt;
    auto value = nlohmann::json::parse(body);
    auto canonical = core::canonicalize_json(value);
    if (!canonical || *canonical != body)
        return std::nullopt;
    return value;
}
auto exact(const nlohmann::json& value, std::initializer_list<std::string_view> names) -> bool {
    return value.is_object() && value.size() == names.size() &&
           std::ranges::all_of(names, [&](auto name) { return value.contains(name); });
}
auto qid(std::string_view value) noexcept -> bool {
    constexpr std::string_view prefix{"MON-007.host-v2."};
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > prefix.size() + 64U)
        return false;
    const auto suffix = value.substr(prefix.size());
    return suffix.front() != '-' && suffix.back() != '-' &&
           std::ranges::all_of(suffix, [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'z') || c == '-';
           });
}
auto hex(std::string_view value, bool strong = false) noexcept -> bool {
    if (strong) {
        if (!value.starts_with("sha256:"))
            return false;
        value.remove_prefix(7);
    }
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}
auto safe_privacy(const nlohmann::json& value, std::string path = "$") -> bool {
    static constexpr std::array forbidden{"hostname",
                                          "username",
                                          "home",
                                          "project_path",
                                          "machine_id",
                                          "boot_id",
                                          "pid",
                                          "tid",
                                          "linux_tid",
                                          "perf_fd",
                                          "fd",
                                          "proc_path",
                                          "sysfs_path",
                                          "mmap_address",
                                          "raw_pointer",
                                          "serial",
                                          "serial_number",
                                          "mac",
                                          "mac_address",
                                          "gpu_uuid",
                                          "storage_serial",
                                          "process_listing"};
    if (path == "$.identity_policy.qualification_id_format")
        return value.is_string() &&
               value == R"(^MON-007\.host-v2\.[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$)";
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            auto normalized = key;
            std::ranges::transform(normalized, normalized.begin(), [](unsigned char c) {
                return c == '-' ? '_' : static_cast<char>(std::tolower(c));
            });
            if (std::ranges::find(forbidden, normalized) != forbidden.end() ||
                !safe_privacy(child, path + "." + key))
                return false;
        }
    } else if (value.is_array()) {
        std::size_t index{};
        for (const auto& child : value) {
            if (!safe_privacy(child, path + "[" + std::to_string(index++) + "]"))
                return false;
        }
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.starts_with("/proc/") || text.starts_with("/sys/") || text.starts_with("/home/") ||
            text.contains('\\'))
            return false;
        std::string token;
        for (char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (std::isalnum(c) != 0 || c == '_' || c == '-')
                token += c == '-' ? '_' : static_cast<char>(std::tolower(c));
            else {
                if (std::ranges::find(forbidden, token) != forbidden.end())
                    return false;
                token.clear();
            }
        }
        if (std::ranges::find(forbidden, token) != forbidden.end())
            return false;
    }
    return true;
}
auto profile_schema(const nlohmann::json& value, const nlohmann::json& policy) -> bool {
    constexpr std::array common{"qualification_contract", "tool",
                                "evidence_schema",        "production_authority_schema",
                                "configurations",         "monitoring",
                                "perturbation",           "service_placement",
                                "identity_policy",        "privacy_policy",
                                "input_template"};
    if (!exact(value, {"schema", "identity", "version", "stage", "qualification_contract", "tool",
                       "evidence_schema", "production_authority_schema", "configurations",
                       "kernel_families", "monitoring", "perturbation", "service_placement",
                       "identity_policy", "privacy_policy", "input_template", "q0b_provenance",
                       "qualification_stages"}) ||
        value.value("schema", "") != "epycbench.monitoring-qualification-profile/v2" ||
        value.value("identity", "") != "epycbench.monitoring-qualification.host-generic/v2" ||
        value.value("version", 0) != 2 || value.value("stage", "") != "final_frozen_profile")
        return false;
    for (auto name : common)
        if (!policy.contains(name) || value.at(name) != policy.at(name))
            return false;
    const auto& families = value.at("kernel_families");
    if (!families.is_array() || families.size() != 2)
        return false;
    constexpr std::array ids{"sequential-read-bandwidth", "dependent-load-latency"};
    for (std::size_t i{}; i < 2; ++i) {
        const auto& family = families.at(i);
        if (!exact(family, {"id", "fixed_work"}) || family.value("id", "") != ids[i] ||
            !family.at("fixed_work").is_number_unsigned() ||
            family.at("fixed_work").get<std::uint64_t>() == 0)
            return false;
    }
    const auto& p = value.at("q0b_provenance");
    if (!exact(p, {"qualification_id", "policy", "input_template_sha256", "host_environment",
                   "host_qualification_plan", "characterization", "q0b_production_inputs",
                   "q0b_base_environment", "family_production_admissions", "repository_commit",
                   "build_identity", "initial_manifest_sha256", "initial_checksums_sha256"}) ||
        !p.at("qualification_id").is_string() ||
        !qid(p.at("qualification_id").get_ref<const std::string&>()))
        return false;
    const auto digest_member = [&](std::string_view name) {
        return p.at(name).is_string() && hex(p.at(name).get_ref<const std::string&>());
    };
    if (!digest_member("input_template_sha256") || !digest_member("build_identity") ||
        !digest_member("initial_manifest_sha256") || !digest_member("initial_checksums_sha256") ||
        p.at("input_template_sha256") != value.at("input_template").at("sha256"))
        return false;
    const auto& policy_binding = p.at("policy");
    if (!exact(policy_binding, {"schema", "identity", "sha256"}) ||
        policy_binding.value("schema", "") != "epycbench.monitoring-qualification-policy/v2" ||
        policy_binding.value("identity", "") !=
            "epycbench.monitoring-qualification.host-generic/v2" ||
        !policy_binding.at("sha256").is_string() ||
        !hex(policy_binding.at("sha256").get_ref<const std::string&>()))
        return false;
    const auto& commit = p.at("repository_commit");
    if (!commit.is_string() ||
        !((commit.get_ref<const std::string&>().size() == 40 ||
           commit.get_ref<const std::string&>().size() == 64) &&
          std::ranges::all_of(commit.get_ref<const std::string&>(), [](unsigned char c) {
              return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
          })))
        return false;
    const auto object = [](const nlohmann::json& o, std::initializer_list<std::string_view> names,
                           std::string_view schema, std::string_view digest_name, bool strong) {
        return exact(o, names) && o.value("schema", "") == schema &&
               o.at(digest_name).is_string() &&
               hex(o.at(digest_name).template get_ref<const std::string&>(), strong);
    };
    if (!object(p.at("host_environment"), {"schema", "digest", "artifact_sha256"},
                "epycbench.monitoring-host-environment/v2", "digest", true) ||
        !hex(p.at("host_environment").at("artifact_sha256").get_ref<const std::string&>()) ||
        p.at("host_environment").at("digest").get<std::string>() !=
            "sha256:" + p.at("host_environment").at("artifact_sha256").get<std::string>() ||
        !object(p.at("host_qualification_plan"), {"schema", "digest"},
                "epycbench.monitoring-host-qualification-plan/v2", "digest", true) ||
        !object(p.at("characterization"), {"schema", "artifact_sha256"},
                "epycbench.monitoring-work-characterization/v2", "artifact_sha256", false) ||
        !object(p.at("q0b_production_inputs"), {"schema", "sha256"},
                "epycbench.monitoring-q0b-production-inputs/v2", "sha256", false) ||
        !object(p.at("q0b_base_environment"), {"schema", "sha256"},
                "epycbench.monitoring-q0b-base-environment/v2", "sha256", false))
        return false;
    const auto& admissions = p.at("family_production_admissions");
    if (!admissions.is_array() || admissions.size() != 2)
        return false;
    for (std::size_t i{}; i < 2; ++i) {
        const auto& a = admissions.at(i);
        if (!exact(a, {"family", "schema", "sha256", "variant_role", "base_environment_sha256"}) ||
            a.value("family", "") != ids[i] ||
            a.value("schema", "") != "epycbench.monitoring-admitted-production-facts/v2" ||
            a.value("variant_role", "") != "first" || !a.at("sha256").is_string() ||
            !hex(a.at("sha256").get_ref<const std::string&>()) ||
            a.at("base_environment_sha256") != p.at("q0b_base_environment").at("sha256"))
            return false;
    }
    const auto& stages = value.at("qualification_stages");
    if (!exact(stages, {"Q0", "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7"}) ||
        stages.value("Q0", "") != "complete")
        return false;
    for (int stage = 1; stage <= 7; ++stage)
        if (stages.value("Q" + std::to_string(stage), "") != "pending")
            return false;
    return true;
}
} // namespace
auto MonitoringQ1ValidatedPublicValues::bounded_preflight(std::string_view bytes) -> bool {
    return structural_preflight(bytes);
}
auto MonitoringQ1ValidatedPublicValues::profile_schema_for_test(std::string_view profile_bytes,
                                                                std::string_view policy_bytes)
    -> bool {
    try {
        auto profile = parse_canonical(profile_bytes);
        auto policy = parse_canonical(policy_bytes);
        return profile && policy && safe_privacy(*profile) && profile_schema(*profile, *policy);
    } catch (...) {
        return false;
    }
}
auto MonitoringQ1ValidatedPublicValues::validate(const MonitoringQ1PublicInputAuthority& inputs)
    -> std::expected<MonitoringQ1ValidatedPublicValues, MonitoringQ1GitCommandFailure> {
    try {
        auto policy = parse_canonical(inputs.bytes(0));
        auto profile = parse_canonical(inputs.bytes(1));
        if (!policy || !profile)
            return std::unexpected{failure("schema")};
        if (!safe_privacy(*policy) || !safe_privacy(*profile))
            return std::unexpected{failure("privacy")};
        // The committed policy-v2 schema is a single top-level const.  Its canonical digest is the
        // generated closed validator for that const and rejects every member/value substitution.
        if (inputs.digest(0) !=
                "54105988475a6031462ce4cc8b7d3e192f8481dc6e80bae9222228076c8aac14" ||
            !policy->is_object())
            return std::unexpected{failure("schema")};
        if (!profile_schema(*profile, *policy))
            return std::unexpected{failure("schema")};
        const auto& provenance = profile->at("q0b_provenance");
        if (!provenance.contains("qualification_id") ||
            !provenance.at("qualification_id").is_string() ||
            !qid(provenance.at("qualification_id").get_ref<const std::string&>()) ||
            !provenance.contains("policy") || !provenance.at("policy").is_object())
            return std::unexpected{failure("binding")};
        const auto& policy_binding = provenance.at("policy");
        if (!exact(policy_binding, {"schema", "identity", "sha256"}) ||
            !policy_binding.at("schema").is_string() ||
            !policy_binding.at("identity").is_string() ||
            !policy_binding.at("sha256").is_string() ||
            policy_binding.value("schema", "") != "epycbench.monitoring-qualification-policy/v2" ||
            policy_binding.value("identity", "") !=
                "epycbench.monitoring-qualification.host-generic/v2" ||
            !hex(policy_binding.at("sha256").get_ref<const std::string&>()))
            return std::unexpected{failure("schema")};
        if (policy_binding.value("sha256", "") != inputs.digest(0))
            return std::unexpected{failure("binding")};
        const auto& stages = profile->at("qualification_stages");
        if (!exact(stages, {"Q0", "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7"}) ||
            stages.value("Q0", "") != "complete")
            return std::unexpected{failure("binding")};
        for (int stage = 1; stage <= 7; ++stage)
            if (stages.value("Q" + std::to_string(stage), "") != "pending")
                return std::unexpected{failure("binding")};
        if (inputs.capture_binding_ == nullptr)
            return std::unexpected{failure("binding")};
        auto plan = MonitoringQ1PlanV2Value::decode(inputs.bytes(2), inputs.captured_head_);
        if (!plan)
            return std::unexpected{plan_failure(plan.error())};
        if (plan->qualification_id() != provenance.at("qualification_id").get<std::string>() ||
            plan->profile_sha256().to_hex() != inputs.digest(1) ||
            "sha256:" + plan->qualification_plan_digest().to_hex() !=
                provenance.at("host_qualification_plan").at("digest").get<std::string>())
            return std::unexpected{failure("binding")};
        return MonitoringQ1ValidatedPublicValues{
            std::move(*plan), policy->at("identity").get<std::string>(),
            std::string{inputs.digest(0)}, std::string{inputs.digest(1)},
            MonitoringQ1Q0PublicProjection{
                policy->at("identity_policy").at("private_root").get<std::string>(),
                provenance.at("repository_commit").get<std::string>(),
                *core::Sha256Digest::parse(provenance.at("build_identity").get<std::string>()),
                provenance.at("host_environment").at("schema").get<std::string>(),
                provenance.at("host_environment").at("digest").get<std::string>(),
                *core::Sha256Digest::parse(
                    provenance.at("host_environment").at("artifact_sha256").get<std::string>()),
                provenance.at("host_qualification_plan").at("schema").get<std::string>(),
                provenance.at("host_qualification_plan").at("digest").get<std::string>(),
                provenance.at("characterization").at("schema").get<std::string>(),
                *core::Sha256Digest::parse(
                    provenance.at("characterization").at("artifact_sha256").get<std::string>()),
                provenance.at("q0b_production_inputs").at("schema").get<std::string>(),
                *core::Sha256Digest::parse(
                    provenance.at("q0b_production_inputs").at("sha256").get<std::string>()),
                provenance.at("q0b_base_environment").at("schema").get<std::string>(),
                *core::Sha256Digest::parse(
                    provenance.at("q0b_base_environment").at("sha256").get<std::string>()),
                [&] {
                    const auto admission = [&](std::size_t index) {
                        const auto& item = provenance.at("family_production_admissions").at(index);
                        return MonitoringQ1Q0FamilyAdmission{
                            item.at("family").get<std::string>(),
                            item.at("schema").get<std::string>(),
                            *core::Sha256Digest::parse(item.at("sha256").get<std::string>()),
                            item.at("variant_role").get<std::string>(),
                            *core::Sha256Digest::parse(
                                item.at("base_environment_sha256").get<std::string>())};
                    };
                    return std::array{admission(0), admission(1)};
                }(),
                {profile->at("kernel_families").at(0).at("fixed_work").get<std::uint64_t>(),
                 profile->at("kernel_families").at(1).at("fixed_work").get<std::uint64_t>()},
                *core::Sha256Digest::parse(
                    provenance.at("initial_manifest_sha256").get<std::string>()),
                *core::Sha256Digest::parse(
                    provenance.at("initial_checksums_sha256").get<std::string>())}};
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{failure("schema")};
    } catch (const std::invalid_argument&) {
        return std::unexpected{failure("schema")};
    } catch (const std::out_of_range&) {
        return std::unexpected{failure("schema")};
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_standard_exception")};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_exception")};
    }
}
} // namespace epycbench::monitoring::qualification::detail
