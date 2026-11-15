#include "runtime/pairwise_monitoring_preparation_facts.hpp"
#include <type_traits>

namespace epycbench::runtime::detail {
static_assert(std::is_nothrow_copy_constructible_v<monitoring::MonitoringExecutionPolicy>);
auto MonitoringPreparationCheckpoint::service_placed(core::LinuxCpuId assigned,
    core::LinuxCpuId singleton, core::LinuxCpuId observed) noexcept -> bool {
    if (completed_ || service_ || assigned != singleton || assigned != observed) return false;
    service_.emplace(MonitoringPreparationServiceFact{assigned, singleton, observed});
    return true;
}
auto MonitoringPreparationCheckpoint::complete(const monitoring::MonitoringExecutionPolicy& policy,
    std::size_t workers, std::size_t page_sources, std::size_t context_sources,
    bool temperature_supported, bool frequency_supported, std::size_t domains) noexcept -> bool {
    using namespace monitoring;
    if (completed_ || workers == 0U) return false;
    const std::array requested{policy.disposition(MonitoringChannel::page_faults) != MonitoringChannelDisposition::disabled,
        policy.disposition(MonitoringChannel::context_switches) != MonitoringChannelDisposition::disabled,
        policy.disposition(MonitoringChannel::temperature) != MonitoringChannelDisposition::disabled,
        policy.disposition(MonitoringChannel::frequency) != MonitoringChannelDisposition::disabled};
    if (page_sources != (requested[0] ? workers : 0U) ||
        context_sources != (requested[1] ? workers : 0U) ||
        (!requested[2] && temperature_supported) || (!requested[3] && frequency_supported) ||
        (frequency_supported ? domains == 0U : domains != 0U)) return false;
    const std::array supported{requested[0], requested[1], temperature_supported, frequency_supported};
    std::array<MonitoringPreparationChannelFact,4> result{};
    for (std::size_t i{}; i < result.size(); ++i) {
        if (!requested[i]) result[i] = {MonitoringPreparationSupport::not_requested, MonitoringEffectiveChannelDisposition::disabled};
        else if (supported[i]) result[i] = {MonitoringPreparationSupport::supported, MonitoringEffectiveChannelDisposition::active};
        else {
            if (policy.disposition(static_cast<MonitoringChannel>(i)) != MonitoringChannelDisposition::optional ||
                policy.optional_unsupported() == OptionalUnavailableSourcePolicy::fail) return false;
            result[i] = {MonitoringPreparationSupport::unsupported,
                policy.optional_unsupported() == OptionalUnavailableSourcePolicy::warn
                    ? MonitoringEffectiveChannelDisposition::unsupported_warning : MonitoringEffectiveChannelDisposition::disabled};
        }
    }
    if (service_.has_value() != (supported[1] || supported[2] || supported[3])) return false;
    policy_.emplace(policy);
    channels_ = result;
    workers_ = workers; domains_ = domains;
    completed_ = true;
    return true;
}
auto MonitoringPreparationFactView::project(MonitoringPreparationFactDomain domain,
    const MonitoringPreparationCheckpoint& checkpoint, bool retired,
    const PairwiseMonitoringHostCapacityAdmission& admission,
    std::span<const monitoring::MonitoringWorkerScope> workers, std::size_t domains,
    const void* backing, FrequencyAccessor accessor) noexcept -> std::optional<MonitoringPreparationFactView> {
    if (!retired || !checkpoint.completed() || !checkpoint.policy() ||
        *checkpoint.policy() != admission.monitoring_policy() || workers.size() != admission.worker_count() || workers.size() != checkpoint.worker_count() ||
        domains != checkpoint.domain_count() ||
        (domains != 0U && (!backing || !accessor))) return std::nullopt;
    if (checkpoint.service() &&
        (!admission.service_cpu() || *admission.service_cpu() != checkpoint.service()->assigned)) return std::nullopt;
    return MonitoringPreparationFactView{domain, checkpoint, admission, workers, domains, backing, accessor};
}
} // namespace epycbench::runtime::detail
