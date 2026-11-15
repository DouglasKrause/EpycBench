#pragma once

#include "epycbench/monitoring/host_qualification_plan.hpp"

namespace epycbench::monitoring::qualification {

class detail_HostQualificationCompositionAccess final {
  public:
    enum class Fault { none,descriptor_id_observation,descriptor_implementation_observation,
      duration_observation,projection_1,projection_2,projection_3,projection_4,
      allocation_1,allocation_2,allocation_3,allocation_4,
      first_single_admission,second_single_admission,bandwidth_compatibility,
      latency_compatibility,bandwidth_admission,latency_admission,
      standard_exception,unknown_exception };
    static auto binding(const MonitoringHostQualificationPlan&) noexcept
        -> const std::shared_ptr<const detail_HostSessionBinding>&;
    static auto policy_identity(const MonitoringHostQualificationPlan&) noexcept
        -> const std::string&;
    static void mutate_policy(MonitoringHostQualificationPlan&,std::string);
    static void mutate_template(MonitoringHostQualificationPlan&,core::Sha256Digest);
    static void mutate_environment(MonitoringHostQualificationPlan&,std::string);
    static void mutate_physical(MonitoringHostQualificationPlan&,topology::PhysicalTopologyFingerprint);
    static void mutate_effective(MonitoringHostQualificationPlan&,topology::EffectiveTopologyFingerprint);
    static void mutate_working_set(MonitoringHostQualificationPlan&,core::ByteCount);
    static void mutate_base_page(MonitoringHostQualificationPlan&,core::ByteCount);
    static void mutate_first_bandwidth_node(MonitoringHostQualificationPlan&,
                                            core::LinuxNumaNodeId);
    static void drop_latency_worker(MonitoringHostQualificationPlan&);
    static void mutate_first_bandwidth_worker_id(MonitoringHostQualificationPlan&,std::uint32_t);
    static void drop_last_bandwidth_worker(MonitoringHostQualificationPlan&);
    static void mutate_first_bandwidth_stable_pu(MonitoringHostQualificationPlan&,
                                                 topology::StableProcessingUnitId);
    static void drop_service_core(MonitoringHostQualificationPlan&);
    static void mutate_canonical(MonitoringHostQualificationPlan&,std::string);
    static void mutate_digest(MonitoringHostQualificationPlan&,core::Sha256Digest);
    static void swap_first_bandwidth_variants(MonitoringHostQualificationPlan&);
    static void mutate_first_bandwidth_linux_cpu(MonitoringHostQualificationPlan&,core::LinuxCpuId);
    static void mutate_first_bandwidth_worker_and_binding_cpu(MonitoringHostQualificationPlan&,
                                                              core::LinuxCpuId);
    static void mutate_first_bandwidth_binding_cpu(MonitoringHostQualificationPlan&,core::LinuxCpuId);
    static void mutate_first_bandwidth_stable_core(MonitoringHostQualificationPlan&,topology::StableCoreId);
    static void mutate_first_bandwidth_stable_node(MonitoringHostQualificationPlan&,topology::StableNumaNodeId);
    static void mutate_first_service_role(MonitoringHostQualificationPlan&,std::string);
    static void mutate_first_service_core(MonitoringHostQualificationPlan&,topology::StableCoreId);
    static void drop_first_service_stable_pu(MonitoringHostQualificationPlan&);
    static void drop_first_service_linux_cpu(MonitoringHostQualificationPlan&);
    static void mutate_active_service_role(MonitoringHostQualificationPlan&,HostPlanServiceRole);
    static void set_fault(Fault) noexcept;
    static void reset_admission_attempt_counts() noexcept;
    static auto admission_attempt_counts() noexcept->std::pair<std::size_t,std::size_t>;
};
}
