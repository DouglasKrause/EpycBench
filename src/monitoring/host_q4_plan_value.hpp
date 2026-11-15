#pragma once

#include "epycbench/core/error.hpp"
#include "monitoring/governed_plan_decode.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

using MonitoringQ4AuthoredDiagnostic = MonitoringGovernedPlanAuthoredDiagnostic;

struct MonitoringQ4PlanCount final {
    std::uint64_t value{};
};
struct MonitoringQ4PlanBytes final {
    std::uint64_t value{};
};
struct MonitoringQ4PlanSeconds final {
    std::uint64_t value{};
};
enum class MonitoringQ4CollectionFormula : unsigned char { v1 };
enum class MonitoringQ4PublicationFormula : unsigned char { v1 };
enum class MonitoringQ4StructuralFormula : unsigned char { v1 };
enum class MonitoringQ4PageBinding : unsigned char { authenticated_host_base_page_size_v1 };
enum class MonitoringQ4AgreementRule : unsigned char {
    exact_domain_metadata_and_uint64_hz_equality
};
enum class MonitoringQ4OracleProtocol : unsigned char {
    identity_bound_inventory_independent_parser_v1
};
enum class MonitoringQ4ProductionProtocol : unsigned char { linux_cpufreq_policy_source_v1 };
enum class MonitoringQ4SourcePreference : unsigned char { cpuinfo_cur_freq_then_scaling_cur_freq };
enum class MonitoringQ4FrequencyUnit : unsigned char { hertz };
enum class MonitoringQ4PrivacyValidation : unsigned char { passed };
class MonitoringQ4PlanValueTestAccess;
class MonitoringQ4SourceReadyTestAccess;

class MonitoringQ4PlanValue final {
  public:
    static constexpr std::string_view schema =
        "epycbench.monitoring-q4-frequency-collection-plan/v1";
    static constexpr std::string_view path =
        "config/monitoring-qualification/host-v2-q4-frequency-collection-plan.json";
    static constexpr std::string_view collection_formula =
        "epycbench.monitoring-q4-collection-capacity/v1";
    static constexpr std::string_view publication_formula =
        "epycbench.monitoring-q4-publication-capacity/v1";
    static constexpr std::string_view structural_formula =
        "epycbench.monitoring-q4-structural-capacity/v1";
    static constexpr std::string_view sysfs_page_binding = "authenticated-host-base-page-size/v1";
    static constexpr std::string_view agreement_rule =
        "exact-domain-metadata-and-uint64-hz-equality";
    static constexpr std::string_view oracle_protocol =
        "identity-bound-inventory-independent-parser/v1";
    static constexpr std::string_view production_source_protocol = "linux-cpufreq-policy-source/v1";
    static constexpr std::string_view source_preference = "cpuinfo_cur_freq-then-scaling_cur_freq";
    static constexpr std::string_view unit = "hertz";
    static constexpr std::string_view privacy_validation = "passed";

    MonitoringQ4PlanValue(const MonitoringQ4PlanValue&) = default;
    MonitoringQ4PlanValue(MonitoringQ4PlanValue&&) noexcept = default;
    auto operator=(const MonitoringQ4PlanValue&) -> MonitoringQ4PlanValue& = delete;
    auto operator=(MonitoringQ4PlanValue&&) -> MonitoringQ4PlanValue& = delete;

#define EPYCBENCH_Q4_PLAN_GETTER(name)                                                             \
    [[nodiscard]] constexpr auto name() const noexcept { return name##_; }
    EPYCBENCH_Q4_PLAN_GETTER(backend_stderr_bytes)
    EPYCBENCH_Q4_PLAN_GETTER(build_manifest_bytes)
    EPYCBENCH_Q4_PLAN_GETTER(cleanup_projection_count)
    EPYCBENCH_Q4_PLAN_GETTER(deadline_quantum_seconds)
    EPYCBENCH_Q4_PLAN_GETTER(deadline_quantum_units)
    EPYCBENCH_Q4_PLAN_GETTER(kernel_max_input_bytes)
    EPYCBENCH_Q4_PLAN_GETTER(public_cleanup_wrapper_bytes)
    EPYCBENCH_Q4_PLAN_GETTER(agreement_rounds_per_cycle)
    EPYCBENCH_Q4_PLAN_GETTER(discovery_cycles)
    EPYCBENCH_Q4_PLAN_GETTER(raw_reads_per_domain_round)
    EPYCBENCH_Q4_PLAN_GETTER(typed_reads_per_domain_round)
    EPYCBENCH_Q4_PLAN_GETTER(raw_agreement_count)
    EPYCBENCH_Q4_PLAN_GETTER(typed_read_count)
    EPYCBENCH_Q4_PLAN_GETTER(deadline_operation_units_per_domain)
    EPYCBENCH_Q4_PLAN_GETTER(collection_formula_kind)
    EPYCBENCH_Q4_PLAN_GETTER(publication_formula_kind)
    EPYCBENCH_Q4_PLAN_GETTER(structural_formula_kind)
    EPYCBENCH_Q4_PLAN_GETTER(page_binding_kind)
    EPYCBENCH_Q4_PLAN_GETTER(agreement_rule_kind)
    EPYCBENCH_Q4_PLAN_GETTER(oracle_protocol_kind)
    EPYCBENCH_Q4_PLAN_GETTER(production_protocol_kind)
    EPYCBENCH_Q4_PLAN_GETTER(source_preference_kind)
    EPYCBENCH_Q4_PLAN_GETTER(unit_kind)
    EPYCBENCH_Q4_PLAN_GETTER(privacy_validation_kind)
#undef EPYCBENCH_Q4_PLAN_GETTER

  private:
    friend auto decode_monitoring_q4_plan_authored(std::string_view)
        -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure>;
    constexpr MonitoringQ4PlanValue() = default;
    MonitoringQ4PlanBytes backend_stderr_bytes_{};
    MonitoringQ4PlanBytes build_manifest_bytes_{};
    MonitoringQ4PlanCount cleanup_projection_count_{};
    MonitoringQ4PlanSeconds deadline_quantum_seconds_{};
    MonitoringQ4PlanCount deadline_quantum_units_{};
    MonitoringQ4PlanBytes kernel_max_input_bytes_{};
    MonitoringQ4PlanBytes public_cleanup_wrapper_bytes_{};
    MonitoringQ4PlanCount agreement_rounds_per_cycle_{};
    MonitoringQ4PlanCount discovery_cycles_{};
    MonitoringQ4PlanCount raw_reads_per_domain_round_{};
    MonitoringQ4PlanCount typed_reads_per_domain_round_{};
    MonitoringQ4PlanCount raw_agreement_count_{};
    MonitoringQ4PlanCount typed_read_count_{};
    MonitoringQ4PlanCount deadline_operation_units_per_domain_{};
    MonitoringQ4CollectionFormula collection_formula_kind_{MonitoringQ4CollectionFormula::v1};
    MonitoringQ4PublicationFormula publication_formula_kind_{MonitoringQ4PublicationFormula::v1};
    MonitoringQ4StructuralFormula structural_formula_kind_{MonitoringQ4StructuralFormula::v1};
    MonitoringQ4PageBinding page_binding_kind_{
        MonitoringQ4PageBinding::authenticated_host_base_page_size_v1};
    MonitoringQ4AgreementRule agreement_rule_kind_{
        MonitoringQ4AgreementRule::exact_domain_metadata_and_uint64_hz_equality};
    MonitoringQ4OracleProtocol oracle_protocol_kind_{
        MonitoringQ4OracleProtocol::identity_bound_inventory_independent_parser_v1};
    MonitoringQ4ProductionProtocol production_protocol_kind_{
        MonitoringQ4ProductionProtocol::linux_cpufreq_policy_source_v1};
    MonitoringQ4SourcePreference source_preference_kind_{
        MonitoringQ4SourcePreference::cpuinfo_cur_freq_then_scaling_cur_freq};
    MonitoringQ4FrequencyUnit unit_kind_{MonitoringQ4FrequencyUnit::hertz};
    MonitoringQ4PrivacyValidation privacy_validation_kind_{MonitoringQ4PrivacyValidation::passed};
    friend class MonitoringQ4PlanValueTestAccess;
    friend class MonitoringQ4SourceReadyTestAccess;
};

[[nodiscard]] auto decode_monitoring_q4_plan_authored(std::string_view)
    -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure>;
[[nodiscard]] auto decode_monitoring_q4_plan(std::string_view)
    -> std::expected<MonitoringQ4PlanValue, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
