#include "epycbench/monitoring/host_input_template.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_input_template_access.hpp"

#include <array>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace epycbench::monitoring::qualification {

class detail_MonitoringHostInputTemplateFactory {
  public:
    static auto make(core::GovernedRepositoryFile manifest, core::GovernedRepositoryFile policy,
                     core::GovernedRepositoryFile input_template, core::Sha256Digest manifest_sha,
                     core::Sha256Digest policy_sha, core::Sha256Digest template_sha,
                     core::Duration minimum_duration, core::Duration target_duration,
                     core::Duration maximum_duration) noexcept
        -> MonitoringHostInputTemplateAuthority {
        return {std::move(manifest), std::move(policy), std::move(input_template),
                manifest_sha,        policy_sha,        template_sha,
                minimum_duration,    target_duration,   maximum_duration};
    }
};

namespace {
using Json = nlohmann::json;
using SourceOperations = detail_MonitoringHostInputTemplateAccess::SourceOperations;
static_assert(std::is_nothrow_move_constructible_v<core::Error>);

constexpr std::string_view manifest_path =
    "config/monitoring-qualification/q0b/host-v2-inputs.json";
constexpr std::string_view policy_path = "config/monitoring-qualification/host-v2-policy.json";
constexpr std::string_view template_path =
    "config/monitoring-qualification/q0b/host-v2-input-template.json";
constexpr std::string_view manifest_digest =
    "0b09c37a97fbf34fb91a0c10a3ef684a734b81df5cfcb4a1a6730fc208f694b2";
constexpr std::string_view policy_digest =
    "54105988475a6031462ce4cc8b7d3e192f8481dc6e80bae9222228076c8aac14";
constexpr std::string_view template_digest =
    "1da436e3a7369d521b66e81fa66e00d3a8f6ab306b38d72b42d5ca72efa37fcd";
constexpr std::string_view policy_identity_value =
    "epycbench.monitoring-qualification.host-generic/v2";
constexpr std::string_view expected_template_bytes =
    R"({"comparison":{"baseline":"first","candidate":"second","order":{"seed":0,"strategy":"randomized-abba"}},"families":[{"family":"sequential-read-bandwidth","kernel":"memory.bandwidth.read/v1","workload":{"working_set":"1 GiB"}},{"family":"dependent-load-latency","kernel":"memory.latency.dependent-load/v1","workload":{"working_set":"1 GiB"}}],"measurement":{"calibration":{"enabled":true,"strategy":"geometric-refine"},"observation":{"maximum_duration":"1 s","maximum_harness_overhead":"1%","minimum_duration":"250 ms","target_duration":"500 ms"},"repetition":{"maximum_blocks":20,"minimum_blocks":8,"minimum_valid_observations_per_variant":16},"stopping":{"confidence":"95%","enabled":false,"ratio_interval_relative_half_width":"2%"},"warmup":{"maximum":10,"minimum":3,"stability_threshold":"3%","stability_window":3}},"memory":{"nodes":{"local-to-each-worker":true},"policy":"bind"},"monitoring":{"configuration":"C0","optional":[],"required":[],"sampling":{"after_observation":false,"before_observation":false,"during_observation":{"enabled":false}},"unavailable_optional_source":"fail"},"policy":{"identity":"epycbench.monitoring-qualification.host-generic/v2"},"privacy_validation":"passed","random":{"seed":0},"schema":"epycbench.monitoring-host-input-template/v2","template_id":"monitoring-q0b-host-generic"})";

auto expected_digest(std::string_view text) -> core::Sha256Digest {
    return *core::Sha256Digest::parse(text);
}

auto failure(std::string code, std::string classification) -> core::Error {
    auto context = Json::object();
    core::detail::JsonCleanup context_cleanup{context};
    Json value(std::move(classification));
    core::detail::JsonCleanup value_cleanup{value};
    core::detail::append_object(context, "classification", value);
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Host-generic Monitoring input loading failed",
            .operation = "load_monitoring_host_input_template",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto project_failure(core::Error& actual, SourceOperations operations,
                     std::optional<core::Error>* raw_failure) -> core::Error {
    const core::Error* original = &actual;
    if (raw_failure && !raw_failure->has_value()) {
        raw_failure->emplace(std::move(actual));
        original = &**raw_failure;
    }
    // Retention is a nonallocating move into the caller's preexisting cell.
    // Its original cause survives a checkpoint or any later copy failure.
    if (operations.before_projection)
        operations.before_projection(operations.context);
    if (!raw_failure)
        return core::Error(std::move(actual));
    auto context = core::detail::clone_json(original->context);
    core::detail::JsonCleanup cleanup{context};
    return {.category = original->category,
            .code = original->code,
            .message = original->message,
            .operation = original->operation,
            .os_error = original->os_error,
            .context = std::move(context),
            .severity = original->severity,
            .continuation = original->continuation};
}

auto check_source_guard(SourceOperations operations,
                        std::optional<core::Error>* raw_failure)
    -> std::expected<void, core::Error> {
    if (!operations.guard)
        return {};
    auto guarded = operations.guard(operations.context);
    core::detail::ExpectedErrorCleanup cleanup{guarded};
    if (!guarded)
        return std::unexpected{project_failure(guarded.error(), operations, raw_failure)};
    return {};
}

auto read_authenticated(SourceOperations operations, std::optional<core::Error>* raw_failure,
                        std::string_view path, std::string_view digest, std::string_view kind)
    -> std::expected<core::GovernedRepositoryFile, core::Error> {
    auto guarded = check_source_guard(operations, raw_failure);
    core::detail::ExpectedErrorCleanup guarded_cleanup{guarded};
    if (!guarded)
        return std::unexpected{std::move(guarded.error())};
    auto file = operations.read(operations.context, path);
    core::detail::ExpectedErrorCleanup file_cleanup{file};
    if (!file) {
        return std::unexpected{project_failure(file.error(), operations, raw_failure)};
    }
    auto after = check_source_guard(operations, raw_failure);
    core::detail::ExpectedErrorCleanup after_cleanup{after};
    if (!after)
        return std::unexpected{std::move(after.error())};
    if (file->repository_relative_path().native() != path ||
        core::sha256(file->bytes()) != expected_digest(digest)) {
        auto error = failure("monitoring.host_input.authentication_failed",
                             std::string{kind} + "_sha256_mismatch");
        core::detail::JsonCleanup cleanup{error.context};
        return std::unexpected{project_failure(error, operations, raw_failure)};
    }
    return file;
}

auto parse_canonical(const core::GovernedRepositoryFile& file, std::string_view kind,
                     SourceOperations operations, std::optional<core::Error>* raw_failure)
    -> std::expected<Json, core::Error> {
    Json value = Json::parse(file.bytes(), nullptr, false);
    core::detail::JsonCleanup value_cleanup{value};
    if (value.is_discarded()) {
        return std::unexpected{failure("monitoring.host_input.document_rejected",
                                       std::string{kind} + "_malformed_json")};
    }
    auto canonical = core::canonicalize_json(value);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical && raw_failure && !raw_failure->has_value()) {
        raw_failure->emplace(std::move(canonical.error()));
        if (operations.before_projection)
            operations.before_projection(operations.context);
    }
    if (!canonical || *canonical + "\n" != file.bytes()) {
        return std::unexpected{failure("monitoring.host_input.document_rejected",
                                       std::string{kind} + "_noncanonical")};
    }
    return Json(std::move(value));
}

auto validate_bindings(const Json& manifest, const Json& policy, const Json& input)
    -> std::expected<void, core::Error> {
    try {
        if (manifest.size() != 3U ||
            manifest.at("schema") != "epycbench.monitoring-q0b-production-inputs/v2" ||
            manifest.at("policy").size() != 2U ||
            manifest.at("policy").at("identity") != policy_identity_value ||
            manifest.at("policy").at("sha256") != policy_digest ||
            manifest.at("template").size() != 2U ||
            manifest.at("template").at("path") != template_path ||
            manifest.at("template").at("sha256") != template_digest) {
            return std::unexpected{
                failure("monitoring.host_input.binding_rejected", "manifest_binding_mismatch")};
        }
        if (policy.at("schema") != "epycbench.monitoring-qualification-policy/v2" ||
            policy.at("identity") != policy_identity_value ||
            policy.at("input_template").at("path") != template_path ||
            policy.at("input_template").at("sha256") != template_digest) {
            return std::unexpected{
                failure("monitoring.host_input.binding_rejected", "policy_binding_mismatch")};
        }
        if (input.at("schema") != "epycbench.monitoring-host-input-template/v2" ||
            input.at("policy").at("identity") != policy_identity_value) {
            return std::unexpected{
                failure("monitoring.host_input.binding_rejected", "template_binding_mismatch")};
        }
        return {};
    } catch (const Json::exception&) {
        return std::unexpected{
            failure("monitoring.host_input.document_rejected", "closed_shape_invalid")};
    }
}

auto validate_template(const Json& value)
    -> std::expected<std::array<core::Duration, 3>, core::Error> {
    try {
        auto expected = Json::parse(expected_template_bytes);
        core::detail::JsonCleanup expected_cleanup{expected};
        if (value != expected) {
            return std::unexpected{
                failure("monitoring.host_input.template_rejected", "template_value_mismatch")};
        }
        return std::array{core::Duration::from_base_units(250'000'000),
                          core::Duration::from_base_units(500'000'000),
                          core::Duration::from_base_units(1'000'000'000)};
    } catch (const Json::exception&) {
        return std::unexpected{
            failure("monitoring.host_input.document_rejected", "closed_shape_invalid")};
    }
}

auto load_algorithm(SourceOperations operations, std::optional<core::Error>* raw_failure)
    -> std::expected<MonitoringHostInputTemplateAuthority, core::Error> {
    if (!operations.read) {
        auto error = failure("monitoring.host_input.contained_failure", "contained_exception");
        core::detail::JsonCleanup cleanup{error.context};
        return std::unexpected{project_failure(error, operations, raw_failure)};
    }
    auto manifest = read_authenticated(operations, raw_failure, manifest_path, manifest_digest, "manifest");
    core::detail::ExpectedErrorCleanup manifest_cleanup{manifest};
    if (!manifest)
        return std::unexpected{std::move(manifest.error())};
    auto policy = read_authenticated(operations, raw_failure, policy_path, policy_digest, "policy");
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy)
        return std::unexpected{std::move(policy.error())};
    auto input = read_authenticated(operations, raw_failure, template_path, template_digest, "template");
    core::detail::ExpectedErrorCleanup input_cleanup{input};
    if (!input)
        return std::unexpected{std::move(input.error())};
    auto manifest_json = parse_canonical(*manifest, "manifest", operations, raw_failure);
    core::detail::ExpectedErrorCleanup manifest_error_cleanup{manifest_json};
    core::detail::JsonPointerCleanup manifest_json_cleanup{manifest_json ? &*manifest_json : nullptr};
    if (!manifest_json)
        return std::unexpected{project_failure(manifest_json.error(), operations, raw_failure)};
    auto policy_json = parse_canonical(*policy, "policy", operations, raw_failure);
    core::detail::ExpectedErrorCleanup policy_error_cleanup{policy_json};
    core::detail::JsonPointerCleanup policy_json_cleanup{policy_json ? &*policy_json : nullptr};
    if (!policy_json)
        return std::unexpected{project_failure(policy_json.error(), operations, raw_failure)};
    auto input_json = parse_canonical(*input, "template", operations, raw_failure);
    core::detail::ExpectedErrorCleanup input_error_cleanup{input_json};
    core::detail::JsonPointerCleanup input_json_cleanup{input_json ? &*input_json : nullptr};
    if (!input_json)
        return std::unexpected{project_failure(input_json.error(), operations, raw_failure)};
    auto binding = validate_bindings(*manifest_json, *policy_json, *input_json);
    core::detail::ExpectedErrorCleanup binding_cleanup{binding};
    if (!binding)
        return std::unexpected{project_failure(binding.error(), operations, raw_failure)};
    auto durations = validate_template(*input_json);
    core::detail::ExpectedErrorCleanup durations_cleanup{durations};
    if (!durations)
        return std::unexpected{project_failure(durations.error(), operations, raw_failure)};
    auto guarded = check_source_guard(operations, raw_failure);
    core::detail::ExpectedErrorCleanup guarded_cleanup{guarded};
    if (!guarded)
        return std::unexpected{std::move(guarded.error())};
    // The actual caller's guard precedes this nonallocating ownership transfer.
    return detail_MonitoringHostInputTemplateFactory::make(
        std::move(*manifest), std::move(*policy), std::move(*input),
        expected_digest(manifest_digest), expected_digest(policy_digest),
        expected_digest(template_digest), (*durations)[0], (*durations)[1], (*durations)[2]);
}
} // namespace

MonitoringHostInputTemplateAuthority::MonitoringHostInputTemplateAuthority(
    core::GovernedRepositoryFile manifest, core::GovernedRepositoryFile policy,
    core::GovernedRepositoryFile input_template, core::Sha256Digest manifest_sha,
    core::Sha256Digest policy_sha, core::Sha256Digest template_sha, core::Duration minimum_duration,
    core::Duration target_duration, core::Duration maximum_duration) noexcept
    : manifest_{std::move(manifest)}, policy_{std::move(policy)},
      template_{std::move(input_template)}, manifest_sha_{manifest_sha}, policy_sha_{policy_sha},
      template_sha_{template_sha}, minimum_duration_{minimum_duration},
      target_duration_{target_duration}, maximum_duration_{maximum_duration} {}

auto MonitoringHostInputTemplateAuthority::manifest_file() const noexcept
    -> const core::GovernedRepositoryFile& {
    return manifest_;
}
auto MonitoringHostInputTemplateAuthority::policy_file() const noexcept
    -> const core::GovernedRepositoryFile& {
    return policy_;
}
auto MonitoringHostInputTemplateAuthority::template_file() const noexcept
    -> const core::GovernedRepositoryFile& {
    return template_;
}
auto MonitoringHostInputTemplateAuthority::manifest_sha256() const noexcept
    -> const core::Sha256Digest& {
    return manifest_sha_;
}
auto MonitoringHostInputTemplateAuthority::policy_sha256() const noexcept
    -> const core::Sha256Digest& {
    return policy_sha_;
}
auto MonitoringHostInputTemplateAuthority::template_sha256() const noexcept
    -> const core::Sha256Digest& {
    return template_sha_;
}
auto MonitoringHostInputTemplateAuthority::policy_identity() const noexcept -> std::string_view {
    return policy_identity_value;
}
auto MonitoringHostInputTemplateAuthority::bandwidth_kernel() const noexcept -> kernel::KernelId {
    return kernel::KernelId::sequential_read_bandwidth_v1;
}
auto MonitoringHostInputTemplateAuthority::latency_kernel() const noexcept -> kernel::KernelId {
    return kernel::KernelId::dependent_load_latency_v1;
}
auto MonitoringHostInputTemplateAuthority::working_set_bytes() const noexcept -> core::ByteCount {
    return core::ByteCount::from_base_units(1'073'741'824);
}
auto MonitoringHostInputTemplateAuthority::minimum_duration() const noexcept -> core::Duration {
    return minimum_duration_;
}
auto MonitoringHostInputTemplateAuthority::target_duration() const noexcept -> core::Duration {
    return target_duration_;
}
auto MonitoringHostInputTemplateAuthority::maximum_duration() const noexcept -> core::Duration {
    return maximum_duration_;
}
auto MonitoringHostInputTemplateAuthority::uses_worker_local_memory() const noexcept -> bool {
    return true;
}
auto MonitoringHostInputTemplateAuthority::monitoring_configuration() const noexcept
    -> std::string_view {
    return "C0";
}

auto load_monitoring_host_input_template(const std::filesystem::path& repository_root) noexcept
    -> std::expected<MonitoringHostInputTemplateAuthority, core::Error> {
    try {
        auto root = core::GovernedRepositoryRoot::create(repository_root);
        core::detail::ExpectedErrorCleanup root_cleanup{root};
        if (!root)
            return std::unexpected{std::move(root.error())};
        SourceOperations operations{};
        operations.context = &*root;
        operations.read = [](void* context, std::string_view path)
            -> std::expected<core::GovernedRepositoryFile, core::Error> {
            return static_cast<core::GovernedRepositoryRoot*>(context)->read_regular_file(
                std::filesystem::path{path});
        };
        return load_algorithm(operations, nullptr);
    } catch (const std::exception&) {
        return std::unexpected{
            failure("monitoring.host_input.contained_failure", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            failure("monitoring.host_input.contained_failure", "contained_unknown_failure")};
    }
}

auto detail_MonitoringHostInputTemplateAccess::load_source_fallible(
    SourceOperations operations, std::optional<core::Error>& raw_failure)
    -> std::expected<MonitoringHostInputTemplateAuthority, core::Error> {
    if (!operations.guard) {
        auto error = failure("monitoring.host_input.contained_failure", "contained_exception");
        core::detail::JsonCleanup cleanup{error.context};
        return std::unexpected{project_failure(error, operations, &raw_failure)};
    }
    return load_algorithm(operations, &raw_failure);
}

auto detail_MonitoringHostInputTemplateAccess::validate_template_json_fallible(
    std::string_view bytes, std::optional<core::Error>* raw_failure, void* context,
    void (*before_projection)(void*)) -> std::expected<void, core::Error> {
    SourceOperations operations{};
    operations.context = context;
    operations.before_projection = before_projection;
    auto value = Json::parse(bytes, nullptr, false);
    core::detail::JsonCleanup value_cleanup{value};
    if (value.is_discarded()) {
        auto error = failure("monitoring.host_input.document_rejected", "template_malformed_json");
        core::detail::JsonCleanup cleanup{error.context};
        return std::unexpected{project_failure(error, operations, raw_failure)};
    }
    auto result = validate_template(value);
    core::detail::ExpectedErrorCleanup result_cleanup{result};
    if (!result)
        return std::unexpected{project_failure(result.error(), operations, raw_failure)};
    return {};
}

auto detail_MonitoringHostInputTemplateAccess::validate_template_json(
    std::string_view bytes) noexcept -> std::expected<void, core::Error> {
    try {
        return validate_template_json_fallible(bytes);
    } catch (const std::exception&) {
        return std::unexpected{
            failure("monitoring.host_input.contained_failure", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            failure("monitoring.host_input.contained_failure", "contained_unknown_failure")};
    }
}

} // namespace epycbench::monitoring::qualification
