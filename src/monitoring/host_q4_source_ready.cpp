#include "monitoring/host_q4_source_ready.hpp"

#include <new>
#include <unistd.h>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
using FailureKind = MonitoringQ4SourceReadyFailureKind;
auto valid_plan(const MonitoringQ4PlanValue& plan) noexcept -> bool {
    return plan.backend_stderr_bytes().value == 792302U &&
           plan.build_manifest_bytes().value == 1048576U &&
           plan.cleanup_projection_count().value == 32U &&
           plan.deadline_quantum_seconds().value == 60U &&
           plan.deadline_quantum_units().value == 99U &&
           plan.kernel_max_input_bytes().value == 11U &&
           plan.public_cleanup_wrapper_bytes().value == 796659U &&
           plan.agreement_rounds_per_cycle().value == 16U && plan.discovery_cycles().value == 3U &&
           plan.raw_reads_per_domain_round().value == 1U &&
           plan.typed_reads_per_domain_round().value == 1U &&
           plan.raw_agreement_count().value == 48U && plan.typed_read_count().value == 48U &&
           plan.deadline_operation_units_per_domain().value == 99U &&
           plan.collection_formula_kind() == MonitoringQ4CollectionFormula::v1 &&
           plan.publication_formula_kind() == MonitoringQ4PublicationFormula::v1 &&
           plan.structural_formula_kind() == MonitoringQ4StructuralFormula::v1 &&
           plan.page_binding_kind() ==
               MonitoringQ4PageBinding::authenticated_host_base_page_size_v1 &&
           plan.agreement_rule_kind() ==
               MonitoringQ4AgreementRule::exact_domain_metadata_and_uint64_hz_equality &&
           plan.oracle_protocol_kind() ==
               MonitoringQ4OracleProtocol::identity_bound_inventory_independent_parser_v1 &&
           plan.production_protocol_kind() ==
               MonitoringQ4ProductionProtocol::linux_cpufreq_policy_source_v1 &&
           plan.source_preference_kind() ==
               MonitoringQ4SourcePreference::cpuinfo_cur_freq_then_scaling_cur_freq &&
           plan.unit_kind() == MonitoringQ4FrequencyUnit::hertz &&
           plan.privacy_validation_kind() == MonitoringQ4PrivacyValidation::passed;
}
auto native_page_size(void*) noexcept -> long { return ::sysconf(_SC_PAGESIZE); }
} // namespace

MonitoringQ4SourceReadyTransition::MonitoringQ4SourceReadyTransition(
    MonitoringQ4SourceReadyTransition&& other) noexcept
    : authority_{std::move(other.authority_)}, consumed_{other.consumed_} {
    other.authority_.reset();
    other.consumed_ = true;
}

auto MonitoringQ4SourceReadyTransition::begin(MonitoringQ4KernelMaximumReady authority) noexcept
    -> MonitoringQ4SourceReadyTransition {
    return MonitoringQ4SourceReadyTransition{std::move(authority)};
}

auto MonitoringQ4SourceReadyTransition::derive(const MonitoringQ4PlanValue& plan,
                                               MonitoringQ4BasePageSizeBytes page,
                                               std::span<const MonitoringQ4AdmittedLinuxCpuId> cpus,
                                               MonitoringQ4KernelMaximum maximum, long runtime_page,
                                               Operations operations) noexcept
    -> std::expected<MonitoringQ4SourceReadyDerivation, MonitoringQ4SourceReadyFailure> {
    auto allocation_reason = Reason::ready_admitted_allocation;
    try {
        if (!valid_plan(plan))
            return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::plan_invalid, Reason::none}};
        if (runtime_page <= 0)
            return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::page_query_failed, Reason::ready_page_query}};
        if (static_cast<std::uint64_t>(runtime_page) != page.value)
            return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::page_size_mismatch, Reason::ready_page_mismatch}};
        if (cpus.empty())
            return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::admitted_cpus_invalid, Reason::ready_cpus_empty}};
        std::vector<core::LinuxCpuId> admitted;
        if (operations.fault)
            operations.fault(operations.context, 1U);
        admitted.reserve(cpus.size());
        std::optional<std::uint32_t> previous;
        for (const auto cpu : cpus) {
            if (cpu.value > maximum.value)
                return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::admitted_cpus_invalid, Reason::ready_cpu_range}};
            if (previous && cpu.value <= *previous)
                return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::admitted_cpus_invalid, Reason::ready_cpu_order}};
            admitted.push_back(core::LinuxCpuId{cpu.value});
            previous = cpu.value;
        }
        allocation_reason = Reason::ready_structural_allocation;
        if (operations.fault)
            operations.fault(operations.context, 2U);
        auto structural = monitoring_q4_structural_capacity(
            maximum, MonitoringQ4BasePageBytes{page.value},
            MonitoringQ4AdmittedCpuCount{static_cast<std::uint64_t>(admitted.size())});
        if (!structural)
            return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::capacity, structural.error().reason}};
        allocation_reason = Reason::ready_inputs_allocation;
        auto inputs = monitoring_q4_collection_capacity_inputs(*structural, std::move(admitted));
        if (!inputs)
            return std::unexpected{MonitoringQ4SourceReadyFailure{
                inputs.error().kind == MonitoringQ4CapacityFailureKind::allocation_failure
                    ? FailureKind::allocation : FailureKind::capacity,
                inputs.error().reason}};
        allocation_reason = Reason::ready_collection_allocation;
        if (operations.fault)
            operations.fault(operations.context, 3U);
        auto capacity = monitoring_q4_collection_capacity(*inputs);
        if (!capacity)
            return std::unexpected{MonitoringQ4SourceReadyFailure{
                capacity.error().kind == MonitoringQ4CapacityFailureKind::allocation_failure
                    ? FailureKind::allocation : FailureKind::capacity,
                capacity.error().reason}};
        return MonitoringQ4SourceReadyDerivation{std::move(*inputs), std::move(*capacity)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::allocation, allocation_reason}};
    } catch (...) {
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::contained, Reason::none}};
    }
}

auto MonitoringQ4SourceReadyTransition::finalize(
    MonitoringQ4AuthenticatedKernelMaximum&& authenticated) noexcept
    -> std::expected<MonitoringQ4SourceReady, MonitoringQ4SourceReadyFailure> {
    return finalize_with(std::move(authenticated), native_page_size(nullptr), {nullptr, nullptr});
}

auto MonitoringQ4SourceReadyTransition::finalize_with(
    MonitoringQ4AuthenticatedKernelMaximum&& authenticated, long runtime_page,
    Operations operations) noexcept
    -> std::expected<MonitoringQ4SourceReady, MonitoringQ4SourceReadyFailure> {
    if (consumed_ || !authority_) {
        static_cast<void>(authenticated.take({}));
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::inactive, Reason::none}};
    }
    consumed_ = true;
    auto payload = authenticated.take(authority_->kernel_maximum_binding_);
    if (!payload)
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::inactive, Reason::none}};
    auto derived = derive(authority_->governed().plan(), authority_->base_page_size_bytes(),
                          authority_->admitted_linux_cpus(), payload->maximum, runtime_page, operations);
    if (!derived)
        return std::unexpected{derived.error()};
    std::shared_ptr<const void> binding;
    try {
        binding = std::make_shared<unsigned char>(0);
    } catch (const std::bad_alloc&) {
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::allocation, Reason::ready_binding_allocation}};
    } catch (...) {
        return std::unexpected{MonitoringQ4SourceReadyFailure{FailureKind::contained, Reason::none}};
    }
    auto authority = std::move(*authority_);
    authority_.reset();
    return MonitoringQ4SourceReady{std::move(authority), payload->maximum,
                                   std::move(payload->authority), std::move(*derived),
                                   std::move(binding)};
}

} // namespace epycbench::monitoring::qualification::detail
