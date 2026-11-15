#pragma once
#include "epycbench/core/digest.hpp"
#include "monitoring/host_q4_source_ready.hpp"
#include "platform_linux/authenticated_monitoring_capacity_authority_test_access.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4SourceReadyTestAccess final {
  public:
    static void set_capacity_projection(
        MonitoringQ4KernelMaximumReady& value, std::string environment,
        std::string plan, std::string physical, std::string effective,
        std::uint64_t page) {
        auto& projection = value.projection_;
        projection.host_environment_digest = std::move(environment);
        projection.qualification_plan_digest = std::move(plan);
        projection.physical_fingerprint = std::move(physical);
        projection.effective_fingerprint = std::move(effective);
        projection.page_size_bytes = MonitoringQ4BasePageSizeBytes{page};
        projection.authenticated_host_environment_digest.reset();
        if (projection.host_environment_digest.starts_with("sha256:")) {
            const auto parsed = core::Sha256Digest::parse(
                std::string_view{projection.host_environment_digest}.substr(7));
            if (parsed) projection.authenticated_host_environment_digest = *parsed;
        }
        projection.authenticated_qualification_plan_digest.reset();
        if (projection.qualification_plan_digest.starts_with("sha256:")) {
            const auto parsed = core::Sha256Digest::parse(
                std::string_view{projection.qualification_plan_digest}.substr(7));
            if (parsed) projection.authenticated_qualification_plan_digest = *parsed;
        }
        projection.authenticated_physical_fingerprint.reset();
        if (const auto parsed = topology::PhysicalTopologyFingerprint::parse(
                projection.physical_fingerprint))
            projection.authenticated_physical_fingerprint = *parsed;
        projection.authenticated_effective_fingerprint.reset();
        if (const auto parsed = topology::EffectiveTopologyFingerprint::parse(
                projection.effective_fingerprint))
            projection.authenticated_effective_fingerprint = *parsed;
    }
    static void set_plan_sha256(MonitoringQ4SourceReady& value,
                                core::Sha256Digest digest) noexcept {
        value.authority_.governed_.plan_sha256_ = digest;
    }
    static void set_qualification_id(MonitoringQ4SourceReady& value, std::string text) {
        value.authority_.qid_ = std::move(text);
    }
    static void set_captured_head(MonitoringQ4SourceReady& value, std::string text) {
        value.authority_.head_ = std::move(text);
    }
    static void set_build_identity(MonitoringQ4SourceReady& value,
                                   core::Sha256Digest digest) noexcept {
        value.authority_.build_identity_ = digest;
    }
    static void set_build_attestation(MonitoringQ4SourceReady& value, std::string bytes) {
        value.authority_.build_attestation_ = std::move(bytes);
    }
    static void set_evidence_capacity(MonitoringQ4SourceReady& value,
                                      std::uint64_t bytes) noexcept {
        value.derived_.capacity.evidence_.value = bytes;
    }
    using Operations = MonitoringQ4SourceReadyTransition::Operations;
    static auto derive(const MonitoringQ4PlanValue& plan, MonitoringQ4BasePageSizeBytes page,
                       std::span<const MonitoringQ4AdmittedLinuxCpuId> cpus,
                       MonitoringQ4KernelMaximum maximum, long runtime_page,
                       Operations operations = {}) noexcept {
        return MonitoringQ4SourceReadyTransition::derive(plan, page, cpus, maximum, runtime_page,
                                                         operations);
    }
    static auto inactive(bool consumed = false) noexcept {
        return MonitoringQ4SourceReadyTransition{consumed};
    }
    static auto finalize(MonitoringQ4SourceReadyTransition& value,
                         MonitoringQ4KernelMaximum maximum) noexcept {
        return value.finalize(MonitoringQ4AuthenticatedKernelMaximum{maximum, {}});
    }
    static auto authenticated(const MonitoringQ4KernelMaximumReady& ready,
                              MonitoringQ4KernelMaximum maximum) noexcept {
        auto physical = *topology::PhysicalTopologyFingerprint::parse(
            ready.predecessor_projection().physical_fingerprint);
        auto effective = *topology::EffectiveTopologyFingerprint::parse(
            ready.predecessor_projection().effective_fingerprint);
        using CapacityAccess =
            platform_linux::detail::LinuxMonitoringCapacityAuthorityTestAccess;
        auto provenance = CapacityAccess::host_plan_provenance(
            core::sha256("environment"), core::sha256("plan"));
        std::vector<core::LinuxCpuId> cpus{core::LinuxCpuId{0}};
        auto facts = CapacityAccess::host_facts(
            core::ByteCount::from_base_units(ready.base_page_size_bytes().value),
            std::move(cpus), std::move(physical), std::move(effective), std::move(provenance));
        auto prepared = CapacityAccess::prepare(std::move(facts));
        auto authority = CapacityAccess::seal(std::move(*prepared), maximum.value);
        return MonitoringQ4AuthenticatedKernelMaximum{
            maximum, ready.kernel_maximum_binding_, std::move(*authority)};
    }
    static auto finalize(MonitoringQ4SourceReadyTransition& value,
                         MonitoringQ4AuthenticatedKernelMaximum&& maximum, long runtime_page,
                         Operations operations = {}) noexcept {
        return value.finalize_with(std::move(maximum), runtime_page, operations);
    }
    static auto ready(MonitoringQ4PlanValue plan, MonitoringQ4ResolvedSchemaBundle schemas,
                      unsigned char marker, std::vector<MonitoringQ4AdmittedLinuxCpuId> cpus,
                      std::uint64_t page = 4096) {
        const auto binding = std::shared_ptr<const void>{std::make_shared<unsigned char>(marker)};
        const std::string head(40, static_cast<char>('a' + marker));
        std::array<core::Sha256Digest, 5> schema_digests{core::sha256("s0"), core::sha256("s1"),
                                                         core::sha256("s2"), core::sha256("s3"),
                                                         core::sha256("s4")};
        MonitoringQ4GovernedPlanSchemaBinding governed{
            std::move(plan),      std::move(schemas), head,
            core::sha256("plan"), schema_digests,     binding};
        MonitoringQ4HistoricalGitValue history{{std::string(40, '0'), std::string(40, '1'),
                                                std::string(40, '2'), std::string(40, '3'), head},
                                               binding};
        MonitoringQ4AuthenticatedHostProjection projection{
            "environment-digest", "qualification-plan-digest",
            "sha256:" + core::sha256("physical").to_hex(),
            "sha256:" + core::sha256("effective").to_hex(),
            MonitoringQ4BasePageSizeBytes{page},
            MonitoringQ4BandwidthWorkerCount{cpus.size()}, std::move(cpus), {}, {}, {}, {}};
        projection.authenticated_host_environment_digest = core::sha256("environment");
        projection.authenticated_qualification_plan_digest = core::sha256("plan");
        projection.authenticated_physical_fingerprint =
            *topology::PhysicalTopologyFingerprint::parse(projection.physical_fingerprint);
        projection.authenticated_effective_fingerprint =
            *topology::EffectiveTopologyFingerprint::parse(projection.effective_fingerprint);
        return MonitoringQ4KernelMaximumReady{std::move(governed),
                                              std::move(history),
                                              std::move(projection),
                                              "MON-007.host-v2.20260906-05",
                                              head,
                                              core::sha256("build"),
                                              "{\"build\":true}",
                                              MonitoringQ4EvidenceProvenance{
                                                  core::sha256("profile"),
                                                  {core::sha256("environment"), core::sha256("q0b"),
                                                   core::sha256("q1"), core::sha256("q2"),
                                                   core::sha256("q3"), core::sha256("manifest"),
                                                   core::sha256("checksums")}},
                                              std::make_shared<unsigned char>(marker)};
    }
    static void invalidate_plan(MonitoringQ4PlanValue& value, unsigned index) noexcept {
        switch (index) {
        case 0:
            value.backend_stderr_bytes_.value = 0;
            break;
        case 1:
            value.build_manifest_bytes_.value = 0;
            break;
        case 2:
            value.cleanup_projection_count_.value = 0;
            break;
        case 3:
            value.deadline_quantum_seconds_.value = 0;
            break;
        case 4:
            value.deadline_quantum_units_.value = 0;
            break;
        case 5:
            value.kernel_max_input_bytes_.value = 0;
            break;
        case 6:
            value.public_cleanup_wrapper_bytes_.value = 0;
            break;
        case 7:
            value.agreement_rounds_per_cycle_.value = 0;
            break;
        case 8:
            value.discovery_cycles_.value = 0;
            break;
        case 9:
            value.raw_reads_per_domain_round_.value = 0;
            break;
        case 10:
            value.typed_reads_per_domain_round_.value = 0;
            break;
        case 11:
            value.raw_agreement_count_.value = 0;
            break;
        case 12:
            value.typed_read_count_.value = 0;
            break;
        case 13:
            value.deadline_operation_units_per_domain_.value = 0;
            break;
        case 14:
            value.collection_formula_kind_ = static_cast<MonitoringQ4CollectionFormula>(1);
            break;
        case 15:
            value.publication_formula_kind_ = static_cast<MonitoringQ4PublicationFormula>(1);
            break;
        case 16:
            value.structural_formula_kind_ = static_cast<MonitoringQ4StructuralFormula>(1);
            break;
        case 17:
            value.page_binding_kind_ = static_cast<MonitoringQ4PageBinding>(1);
            break;
        case 18:
            value.agreement_rule_kind_ = static_cast<MonitoringQ4AgreementRule>(1);
            break;
        case 19:
            value.oracle_protocol_kind_ = static_cast<MonitoringQ4OracleProtocol>(1);
            break;
        case 20:
            value.production_protocol_kind_ = static_cast<MonitoringQ4ProductionProtocol>(1);
            break;
        case 21:
            value.source_preference_kind_ = static_cast<MonitoringQ4SourcePreference>(1);
            break;
        case 22:
            value.unit_kind_ = static_cast<MonitoringQ4FrequencyUnit>(1);
            break;
        case 23:
            value.privacy_validation_kind_ = static_cast<MonitoringQ4PrivacyValidation>(1);
            break;
        }
    }
};
} // namespace epycbench::monitoring::qualification::detail
