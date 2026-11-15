#include "epycbench/monitoring/q0b_backend_result.hpp"
#include "monitoring/q0b_backend_result_access.hpp"
#include "monitoring/privacy_validation.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/build_manifest.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::monitoring::qualification {
namespace {
auto contained_error() -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b.backend.contained_failure"},
            .message = "Q0b backend failure could not be safely projected",
            .operation = "monitoring.q0b.backend.project_failure",
            .os_error = std::nullopt,
            .context = {{"classification", "contained_failure"}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto safe_projection(const core::Error& value)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    auto projected = runtime::ipc::project_failure(value);
    if (projected) return projected;
    return runtime::ipc::project_failure(contained_error());
}
auto project(const core::Error& primary, const std::vector<core::Error>& secondary)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    auto first = safe_projection(primary);
    if (!first) return std::unexpected{std::move(first.error())};
    auto remaining = nlohmann::ordered_json::array();
    for (const auto& item : secondary) {
        auto projected = safe_projection(item);
        if (!projected) return std::unexpected{std::move(projected.error())};
        remaining.push_back(std::move(*projected));
    }
    nlohmann::ordered_json envelope{{"schema", q0b_backend_error_schema},
                                    {"failure", std::move(*first)},
                                    {"secondary_failures", std::move(remaining)}};
    auto canonical = core::canonicalize_json(envelope);
    if (!canonical) return std::unexpected{std::move(canonical.error())};
    return envelope;
}
auto boundary_error(std::string classification) -> core::Error {
    auto value = contained_error();
    value.context = {{"classification", std::move(classification)}};
    return value;
}
auto diagnostic_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b.backend.unsafe_diagnostic"},
            .message = "Q0b authored diagnostic is not safe for transport",
            .operation = "monitoring.q0b.backend.project_authored_diagnostics",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto payload_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b.backend.invalid_payload_input"},
            .message = "Q0b characterization payload input is inconsistent",
            .operation = "monitoring.q0b.backend.build_characterization_payload",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto safe_pointer(std::string_view value) -> bool {
    if ((!value.empty() && value.front() != '/') || value.contains('\\') ||
        std::ranges::any_of(value, [](char character) {
            return static_cast<unsigned char>(character) < 0x20U;
        })) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '~') continue;
        if (index + 1U == value.size() ||
            (value[index + 1U] != '0' && value[index + 1U] != '1')) return false;
        ++index;
    }
    return true;
}
auto safe_code(std::string_view value) -> bool {
    return !value.empty() && value.size() <= 128U && value.front() >= 'a' &&
           value.front() <= 'z' &&
           std::ranges::all_of(value, [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '.' ||
                      character == '_' || character == '-';
           });
}
auto valid_qualification_id(std::string_view value) -> bool {
    constexpr std::string_view prefix = "MON-007.reference-epyc-v1.";
    if (!value.starts_with(prefix)) return false;
    const auto suffix = value.substr(prefix.size());
    if (suffix.empty() || suffix.size() > 64U)
        return false;
    return std::ranges::all_of(suffix, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}
auto valid_timestamp(std::string_view value) -> bool {
    if (value.size() < 20U || value.back() != 'Z') return false;
    const auto digit = [&](std::size_t index) { return value[index] >= '0' && value[index] <= '9'; };
    for (const auto index : {0U,1U,2U,3U,5U,6U,8U,9U,11U,12U,14U,15U,17U,18U})
        if (!digit(index)) return false;
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':') return false;
    if (value.size() == 21U || (value.size() > 20U && (value[19] != '.' ||
        !std::ranges::all_of(value.substr(20U, value.size() - 21U), [](char character) {
            return character >= '0' && character <= '9';
        })))) return false;
    const auto number = [&](std::size_t offset, std::size_t count) {
        unsigned result{};
        for (std::size_t index = offset; index < offset + count; ++index)
            result = result * 10U + static_cast<unsigned>(value[index] - '0');
        return result;
    };
    const auto date = std::chrono::year_month_day{
        std::chrono::year{static_cast<int>(number(0U, 4U))},
        std::chrono::month{number(5U, 2U)}, std::chrono::day{number(8U, 2U)}};
    return date.ok() && number(11U, 2U) <= 23U && number(14U, 2U) <= 59U &&
           number(17U, 2U) <= 59U;
}
} // namespace

auto project_q0b_backend_error(const core::Error& failure) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try { return project(failure, {}); }
    catch (const std::exception&) { return std::unexpected{boundary_error("contained_exception")}; }
    catch (...) { return std::unexpected{boundary_error("contained_unknown_failure")}; }
}
auto project_q0b_backend_error(const Q0bC0DriverFailure& failure) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try { return project(failure.primary, failure.secondary); }
    catch (const std::exception&) { return std::unexpected{boundary_error("contained_exception")}; }
    catch (...) { return std::unexpected{boundary_error("contained_unknown_failure")}; }
}

auto project_q0b_backend_authored_diagnostics(
    std::span<const scenario::ScenarioDiagnostic> diagnostics) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        if (diagnostics.empty())
            return std::unexpected{diagnostic_error("empty_diagnostics")};
        auto projected = nlohmann::ordered_json::array();
        for (const auto& diagnostic : diagnostics) {
            if (!safe_code(diagnostic.code.value()) || !safe_pointer(diagnostic.json_pointer))
                return std::unexpected{diagnostic_error("invalid_identity")};
            if (detail::privacy_prohibited(diagnostic.code.value()) ||
                detail::privacy_prohibited(diagnostic.json_pointer))
                return std::unexpected{diagnostic_error("privacy_rejected")};
            auto context = nlohmann::ordered_json::object();
            for (const auto& [key, value] : diagnostic.safe_context) {
                if (key.empty() || detail::privacy_prohibited(key) ||
                    detail::privacy_prohibited(value))
                    return std::unexpected{diagnostic_error("privacy_rejected")};
                context[key] = value;
            }
            nlohmann::ordered_json location = nullptr;
            if (diagnostic.location)
                location = {{"byte_offset", diagnostic.location->byte_offset},
                            {"line", diagnostic.location->line},
                            {"column", diagnostic.location->column}};
            projected.push_back({{"code", diagnostic.code.value()},
                                 {"json_pointer", diagnostic.json_pointer},
                                 {"location", std::move(location)},
                                 {"safe_context", std::move(context)}});
        }
        nlohmann::ordered_json envelope{
            {"schema", q0b_backend_authored_diagnostics_schema},
            {"diagnostics", std::move(projected)}};
        auto canonical = core::canonicalize_json(envelope);
        if (!canonical) return std::unexpected{std::move(canonical.error())};
        return envelope;
    } catch (const std::exception&) {
        return std::unexpected{boundary_error("contained_exception")};
    } catch (...) {
        return std::unexpected{boundary_error("contained_unknown_failure")};
    }
}

auto detail::build_q0b_characterization_payload_with_manifest(
    std::string_view qualification_id, std::string_view collected_at,
    const MonitoringQ0bProductionInputsAuthority& authority,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const Q0bC0Calibrations& calibrations, nlohmann::json build) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        if (!valid_qualification_id(qualification_id))
            return std::unexpected{payload_error("qualification_id")};
        if (!valid_timestamp(collected_at))
            return std::unexpected{payload_error("collected_at")};
        if (!build.is_object() || !build.contains("git") || !build["git"].is_object() ||
            !build["git"].contains("revision") || !build["git"]["revision"].is_string() ||
            !build["git"].contains("dirty") || !build["git"]["dirty"].is_boolean() ||
            build["git"]["dirty"].get<bool>())
            return std::unexpected{payload_error("build_git")};
        const auto revision = build["git"]["revision"].get<std::string>();
        if ((revision.size() != 40U && revision.size() != 64U) ||
            !std::ranges::all_of(revision, [](char character) {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f');
            })) return std::unexpected{payload_error("build_revision")};
        if (authority.policy_identity() != "epycbench.monitoring-qualification.reference-epyc/v1" ||
            authority.schema() != "epycbench.monitoring-q0b-production-inputs/v1")
            return std::unexpected{payload_error("production_input_identity")};
        if (base.fixture_id() != "reference-epyc" || base.fixture_version() != "1")
            return std::unexpected{payload_error("fixture_identity")};
        const auto& families = calibrations.families();
        if (!families.is_array() || families.size() != 2U ||
            families[0].value("family", "") != "sequential-read-bandwidth" ||
            families[1].value("family", "") != "dependent-load-latency")
            return std::unexpected{payload_error("family_order")};
        for (const auto& family : families)
            if (!family.contains("production_admission") ||
                family["production_admission"].value("base_environment_sha256", "") !=
                    base.digest().to_hex())
                return std::unexpected{payload_error("family_base_binding")};
        const auto& assignment = calibrations.control_monitor_assignment();
        const auto& reservation = base.service_reservation();
        if (std::ranges::find(reservation.reserved_cores, assignment.stable_core) ==
                reservation.reserved_cores.end() ||
            std::ranges::find(reservation.reserved_processing_units, assignment.stable_pu) ==
                reservation.reserved_processing_units.end() ||
            std::ranges::find(reservation.reserved_linux_cpus, assignment.linux_cpu) ==
                reservation.reserved_linux_cpus.end())
            return std::unexpected{payload_error("service_assignment")};
        nlohmann::ordered_json cores = nlohmann::ordered_json::array();
        nlohmann::ordered_json pus = nlohmann::ordered_json::array();
        nlohmann::ordered_json cpus = nlohmann::ordered_json::array();
        for (const auto& id : reservation.reserved_cores) cores.push_back(id.value());
        for (const auto& id : reservation.reserved_processing_units) pus.push_back(id.value());
        for (const auto id : reservation.reserved_linux_cpus) cpus.push_back(id.value());
        nlohmann::ordered_json payload{
            {"schema", monitoring_work_characterization_schema},
            {"qualification_id", qualification_id},
            {"policy", {{"identity", authority.policy_identity()},
                         {"sha256", authority.policy_sha256().to_hex()}}},
            {"q0b_production_inputs", {{"schema", authority.schema()},
                                        {"sha256", authority.manifest_raw_sha256().to_hex()}}},
            {"q0b_base_environment", {{"schema", monitoring_q0b_base_environment_schema},
                                       {"sha256", base.digest().to_hex()}}},
            {"tool", {{"identity", "epycbench.monitoring-qualification-tool/v1"},
                       {"version", "1.0.0"}}},
            {"repository", {{"commit", revision}, {"dirty", false}}},
            {"build", std::move(build)},
            {"collected_at", collected_at},
            {"reference_fixture", {{"fixture_id", base.fixture_id()},
                                    {"fixture_version", base.fixture_version()},
                                    {"manifest_sha256", base.manifest_sha256().to_hex()},
                                    {"checksums_sha256", base.checksums_sha256().to_hex()},
                                    {"physical_topology_fingerprint", base.physical_fingerprint().to_string()},
                                    {"effective_topology_fingerprint", base.effective_fingerprint().to_string()}}},
            {"service_reservation", {{"stable_core_ids", std::move(cores)},
                                      {"stable_pu_ids", std::move(pus)},
                                      {"linux_cpus", std::move(cpus)}}},
            {"monitoring", {{"configuration", "C0"},
                            {"service_plan", {{"assignments", nlohmann::ordered_json::array({
                                {{"role", "control_monitor"},
                                 {"stable_core_id", assignment.stable_core.value()},
                                 {"stable_pu_id", assignment.stable_pu.value()},
                                 {"linux_cpu", assignment.linux_cpu.value()}}})}}}}},
            {"families", families}, {"provenance", "preliminary_characterization"},
            {"pass_fail_eligible", false},
            {"checksums", {{"manifest", "manifest.json"}, {"checksums", "checksums.sha256"},
                            {"artifact", "q0b/evidence.json"}}},
            {"privacy_validation", "passed"}};
        if (!detail::privacy_safe(payload))
            return std::unexpected{payload_error("privacy_rejected")};
        auto canonical = core::canonicalize_json(payload);
        if (!canonical) return std::unexpected{std::move(canonical.error())};
        return payload;
    } catch (const std::exception&) { return std::unexpected{boundary_error("contained_exception")}; }
    catch (...) { return std::unexpected{boundary_error("contained_unknown_failure")}; }
}

auto build_q0b_characterization_payload(
    std::string_view qualification_id, std::string_view collected_at,
    const MonitoringQ0bProductionInputsAuthority& authority,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const Q0bC0Calibrations& calibrations) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        return detail::build_q0b_characterization_payload_with_manifest(
            qualification_id, collected_at, authority, base, calibrations,
            build_manifest_json());
    } catch (const std::exception&) {
        return std::unexpected{boundary_error("contained_exception")};
    } catch (...) {
        return std::unexpected{boundary_error("contained_unknown_failure")};
    }
}
} // namespace epycbench::monitoring::qualification
