#include "monitoring/host_q4_plan_value.hpp"
#include "monitoring/host_q4_plan_value_test_access.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <array>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr std::size_t plan_byte_cap = 65'536;
auto fault = MonitoringQ4PlanValueTestAccess::Fault::none;

auto outer_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q4_plan.invalid"},
            .message = "Monitoring Q4 governed plan is invalid",
            .operation = "validate_monitoring_q4_plan",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <std::size_t Size>
auto exact_object(const nlohmann::json& value, const std::array<std::string_view, Size>& keys)
    -> bool {
    if (!value.is_object() || value.size() != keys.size()) {
        return false;
    }
    for (const auto key : keys) {
        if (!value.contains(key)) {
            return false;
        }
    }
    return true;
}

auto valid_pointer(const std::string_view pointer) noexcept -> bool {
    if (pointer.size() > 4096 || (!pointer.empty() && pointer.front() != '/')) {
        return false;
    }
    for (std::size_t index = 0; index < pointer.size(); ++index) {
        const auto byte = static_cast<unsigned char>(pointer[index]);
        if (byte == '~' &&
            (++index >= pointer.size() || (pointer[index] != '0' && pointer[index] != '1'))) {
            return false;
        }
        if (byte < 0x80U) {
            continue;
        }
        std::size_t continuation = 0;
        if (byte >= 0xC2U && byte <= 0xDFU)
            continuation = 1;
        else if (byte >= 0xE0U && byte <= 0xEFU)
            continuation = 2;
        else if (byte >= 0xF0U && byte <= 0xF4U)
            continuation = 3;
        else
            return false;
        if (index + continuation >= pointer.size())
            return false;
        const auto second = static_cast<unsigned char>(pointer[index + 1]);
        if ((byte == 0xE0U && second < 0xA0U) || (byte == 0xEDU && second > 0x9FU) ||
            (byte == 0xF0U && second < 0x90U) || (byte == 0xF4U && second > 0x8FU)) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            if ((static_cast<unsigned char>(pointer[index + offset]) & 0xC0U) != 0x80U)
                return false;
        }
        index += continuation;
    }
    return true;
}

auto diagnostic(std::string code, std::string pointer,
                std::optional<std::uint64_t> byte = std::nullopt,
                std::optional<std::uint64_t> line = std::nullopt,
                std::optional<std::uint64_t> column = std::nullopt)
    -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure> {
    if (!valid_pointer(pointer)) {
        return std::unexpected{
            MonitoringGovernedPlanDecodeFailure{outer_error("diagnostic_invariant")}};
    }
    return std::unexpected{MonitoringGovernedPlanDecodeFailure{
        MonitoringQ4AuthoredDiagnostic{std::move(code), std::move(pointer), byte, line, column}}};
}

auto parse_location(const std::string_view bytes, const std::size_t parser_byte)
    -> std::array<std::uint64_t, 3> {
    const auto bounded = std::min(parser_byte, bytes.size());
    std::uint64_t line = 1;
    std::uint64_t column = 1;
    for (std::size_t index = 0; index + 1 < bounded; ++index) {
        if (bytes[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {static_cast<std::uint64_t>(parser_byte), line, column};
}
} // namespace

auto decode_monitoring_q4_plan_authored(const std::string_view bytes)
    -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure> {
    if (bytes.empty() || bytes.size() > plan_byte_cap || bytes.back() != '\n') {
        return diagnostic("monitoring.q4.plan.canonical", "");
    }
    try {
        const auto value = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (fault == MonitoringQ4PlanValueTestAccess::Fault::canonical_core) {
            return std::unexpected{MonitoringGovernedPlanDecodeFailure{outer_error("canonical")}};
        }
        if (fault == MonitoringQ4PlanValueTestAccess::Fault::canonical_allocation) {
            throw std::bad_alloc{};
        }
        if (fault == MonitoringQ4PlanValueTestAccess::Fault::canonical_unknown) {
            throw 1;
        }
        const auto canonical = core::canonicalize_json(value);
        if (!canonical) {
            const auto& code = canonical.error().code.value();
            if (code == "core.canonical_json.floating_point_unsupported" ||
                code == "core.canonical_json.invalid_utf8" ||
                code == "core.canonical_json.invalid_unicode_scalar" ||
                code == "core.canonical_json.unsupported_type") {
                return diagnostic("monitoring.q4.plan.canonical", "");
            }
            return std::unexpected{
                MonitoringGovernedPlanDecodeFailure{std::move(canonical.error())}};
        }
        if (*canonical + "\n" != bytes) {
            return diagnostic("monitoring.q4.plan.canonical", "");
        }
        if (!exact_object(value, std::array<std::string_view, 4>{"capacity_policy", "collection",
                                                                 "privacy_validation", "schema"})) {
            return diagnostic("monitoring.q4.plan.structure", "");
        }
        if (!value.at("schema").is_string() ||
            value.at("schema") != MonitoringQ4PlanValue::schema) {
            return diagnostic("monitoring.q4.plan.schema", "/schema");
        }
        if (!value.at("privacy_validation").is_string() ||
            value.at("privacy_validation") != MonitoringQ4PlanValue::privacy_validation) {
            return diagnostic("monitoring.q4.plan.value", "/privacy_validation");
        }
        const auto& policy = value.at("capacity_policy");
        const auto& collection = value.at("collection");
        constexpr std::array policy_keys{std::string_view{"backend_stderr_bytes"},
                                         std::string_view{"build_manifest_bytes"},
                                         std::string_view{"cleanup_projection_count"},
                                         std::string_view{"collection_formula"},
                                         std::string_view{"deadline_quantum_seconds"},
                                         std::string_view{"deadline_quantum_units"},
                                         std::string_view{"kernel_max_input_bytes"},
                                         std::string_view{"public_cleanup_wrapper_bytes"},
                                         std::string_view{"publication_formula"},
                                         std::string_view{"structural_formula"},
                                         std::string_view{"sysfs_page_binding"}};
        constexpr std::array collection_keys{std::string_view{"agreement_rounds_per_cycle"},
                                             std::string_view{"agreement_rule"},
                                             std::string_view{"discovery_cycles"},
                                             std::string_view{"oracle_protocol"},
                                             std::string_view{"production_source_protocol"},
                                             std::string_view{"raw_reads_per_domain_round"},
                                             std::string_view{"source_preference"},
                                             std::string_view{"typed_reads_per_domain_round"},
                                             std::string_view{"unit"}};
        if (!exact_object(policy, policy_keys)) {
            return diagnostic("monitoring.q4.plan.structure", "/capacity_policy");
        }
        if (!exact_object(collection, collection_keys)) {
            return diagnostic("monitoring.q4.plan.structure", "/collection");
        }
        const std::array policy_strings{
            std::pair{"collection_formula", MonitoringQ4PlanValue::collection_formula},
            std::pair{"publication_formula", MonitoringQ4PlanValue::publication_formula},
            std::pair{"structural_formula", MonitoringQ4PlanValue::structural_formula},
            std::pair{"sysfs_page_binding", MonitoringQ4PlanValue::sysfs_page_binding}};
        for (const auto& [key, expected] : policy_strings) {
            if (!policy.at(key).is_string() || policy.at(key) != expected) {
                return diagnostic("monitoring.q4.plan.value",
                                  "/capacity_policy/" + std::string{key});
            }
        }
        const std::array collection_strings{
            std::pair{"agreement_rule", MonitoringQ4PlanValue::agreement_rule},
            std::pair{"oracle_protocol", MonitoringQ4PlanValue::oracle_protocol},
            std::pair{"production_source_protocol",
                      MonitoringQ4PlanValue::production_source_protocol},
            std::pair{"source_preference", MonitoringQ4PlanValue::source_preference},
            std::pair{"unit", MonitoringQ4PlanValue::unit}};
        for (const auto& [key, expected] : collection_strings) {
            if (!collection.at(key).is_string() || collection.at(key) != expected) {
                return diagnostic("monitoring.q4.plan.value", "/collection/" + std::string{key});
            }
        }
        MonitoringQ4PlanValue result;
        const auto assign = [&](const nlohmann::json& object, const char* key,
                                const std::uint64_t expected, auto& destination,
                                const std::string_view prefix)
            -> std::optional<MonitoringGovernedPlanDecodeFailure> {
            const auto& item = object.at(key);
            if (!item.is_number_unsigned() || item.get<std::uint64_t>() != expected) {
                return MonitoringQ4AuthoredDiagnostic{
                    "monitoring.q4.plan.value", std::string{prefix} + key, {}, {}, {}};
            }
            destination.value = expected;
            return std::nullopt;
        };
#define EPYCBENCH_Q4_ASSIGN(object, key, expected, destination, prefix)                            \
    if (auto failed = assign(object, key, expected, destination, prefix)) {                        \
        return std::unexpected{std::move(*failed)};                                                \
    }
        EPYCBENCH_Q4_ASSIGN(policy, "backend_stderr_bytes", 792302U, result.backend_stderr_bytes_,
                            "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "build_manifest_bytes", 1048576U, result.build_manifest_bytes_,
                            "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "cleanup_projection_count", 32U,
                            result.cleanup_projection_count_, "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "deadline_quantum_seconds", 60U,
                            result.deadline_quantum_seconds_, "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "deadline_quantum_units", 99U, result.deadline_quantum_units_,
                            "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "kernel_max_input_bytes", 11U, result.kernel_max_input_bytes_,
                            "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(policy, "public_cleanup_wrapper_bytes", 796659U,
                            result.public_cleanup_wrapper_bytes_, "/capacity_policy/")
        EPYCBENCH_Q4_ASSIGN(collection, "agreement_rounds_per_cycle", 16U,
                            result.agreement_rounds_per_cycle_, "/collection/")
        EPYCBENCH_Q4_ASSIGN(collection, "discovery_cycles", 3U, result.discovery_cycles_,
                            "/collection/")
        EPYCBENCH_Q4_ASSIGN(collection, "raw_reads_per_domain_round", 1U,
                            result.raw_reads_per_domain_round_, "/collection/")
        EPYCBENCH_Q4_ASSIGN(collection, "typed_reads_per_domain_round", 1U,
                            result.typed_reads_per_domain_round_, "/collection/")
#undef EPYCBENCH_Q4_ASSIGN
        if (fault == MonitoringQ4PlanValueTestAccess::Fault::arithmetic_core) {
            return std::unexpected{MonitoringGovernedPlanDecodeFailure{outer_error("arithmetic")}};
        }
        const auto checked_multiply =
            [](const std::uint64_t left,
               const std::uint64_t right) -> std::optional<std::uint64_t> {
            if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
                return {};
            return left * right;
        };
        const auto agreements = checked_multiply(result.discovery_cycles_.value,
                                                 result.agreement_rounds_per_cycle_.value);
        const auto raw =
            agreements ? checked_multiply(*agreements, result.raw_reads_per_domain_round_.value)
                       : std::nullopt;
        const auto typed =
            agreements ? checked_multiply(*agreements, result.typed_reads_per_domain_round_.value)
                       : std::nullopt;
        if (!raw || !typed || *raw > std::numeric_limits<std::uint64_t>::max() - *typed ||
            result.discovery_cycles_.value >
                std::numeric_limits<std::uint64_t>::max() - *raw - *typed ||
            result.discovery_cycles_.value + *raw + *typed !=
                result.deadline_quantum_units_.value) {
            return std::unexpected{MonitoringGovernedPlanDecodeFailure{outer_error("arithmetic")}};
        }
        result.raw_agreement_count_.value = *raw;
        result.typed_read_count_.value = *typed;
        result.deadline_operation_units_per_domain_.value =
            result.discovery_cycles_.value + *raw + *typed;
        return result;
    } catch (const nlohmann::json::parse_error& error) {
        const auto location = parse_location(bytes, error.byte);
        return diagnostic("monitoring.q4.plan.schema", "", location[0], location[1], location[2]);
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringGovernedPlanDecodeFailure{outer_error("allocation")}};
    } catch (...) {
        return std::unexpected{MonitoringGovernedPlanDecodeFailure{outer_error("contained")}};
    }
}

auto decode_monitoring_q4_plan(const std::string_view bytes)
    -> std::expected<MonitoringQ4PlanValue, core::Error> {
    auto result = decode_monitoring_q4_plan_authored(bytes);
    if (result) {
        return std::move(*result);
    }
    if (auto* outer = std::get_if<core::Error>(&result.error())) {
        return std::unexpected(std::move(*outer));
    }
    return std::unexpected(outer_error("authored"));
}

void MonitoringQ4PlanValueTestAccess::set_fault(const Fault value) noexcept { fault = value; }

auto MonitoringQ4PlanValueTestAccess::authored(std::string pointer)
    -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure> {
    return diagnostic("monitoring.q4.plan.value", std::move(pointer));
}

} // namespace epycbench::monitoring::qualification::detail
