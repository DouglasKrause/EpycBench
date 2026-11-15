#pragma once

#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"
#include "epycbench/runtime/pairwise_evidence_failure.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>

namespace epycbench::runtime::detail {

enum class PairwiseMonitoringCapacitySlot : std::uint8_t {
    raw_page_before,
    raw_page_after,
    raw_context,
    context_offsets,
    context_events,
    raw_temperature,
    raw_frequency_domains,
    raw_frequency_acquisitions,
    classified_page_workers,
    classified_context_workers,
    classified_frequency_domains,
    frequency_nested_cpu_ids,
    page_pair_scratch,
    context_worker_scratch,
    context_view_scratch,
    context_membership_scratch,
    context_lookup_scratch,
    temperature_view_scratch,
    frequency_view_scratch,
    frequency_result_scratch,
    persistent_worker_scopes,
    persistent_page_sources,
    persistent_context_sources,
    persistent_context_state,
    persistent_temperature_sources,
    persistent_frequency_sources,
    persistent_frequency_metadata,
    persistent_frequency_cpu_ids,
    retained_frequency_raw_bytes,
    retained_frequency_escaped_bytes,
    count,
};

class PairwiseAdmittedMonitoringCapacity final {
  public:
    PairwiseAdmittedMonitoringCapacity(const PairwiseAdmittedMonitoringCapacity&) = delete;
    PairwiseAdmittedMonitoringCapacity(PairwiseAdmittedMonitoringCapacity&&) noexcept = default;
    auto operator=(const PairwiseAdmittedMonitoringCapacity&)
        -> PairwiseAdmittedMonitoringCapacity& = delete;
    auto operator=(PairwiseAdmittedMonitoringCapacity&&)
        -> PairwiseAdmittedMonitoringCapacity& = delete;

    [[nodiscard]] auto admission() const noexcept
        -> const PairwiseMonitoringHostCapacityAdmission& { return admission_; }
    [[nodiscard]] auto workers() const noexcept -> std::uint64_t { return workers_; }
    [[nodiscard]] auto blocks() const noexcept -> std::uint64_t { return blocks_; }
    [[nodiscard]] auto capture_slots() const noexcept -> std::uint64_t { return captures_; }
    [[nodiscard]] auto round_capacity() const noexcept -> std::uint64_t { return rounds_; }
    [[nodiscard]] auto base_page_bytes() const noexcept -> std::uint64_t { return base_page_; }
    [[nodiscard]] auto kernel_cpu_count() const noexcept -> std::uint64_t { return cpu_count_; }
    [[nodiscard]] auto enumeration_capacity() const noexcept -> std::uint64_t {
        return enumeration_;
    }
    [[nodiscard]] auto present_cpu_count() const noexcept -> std::uint64_t { return present_; }
    [[nodiscard]] auto maximum_domain_count() const noexcept -> std::uint64_t {
        return domain_maximum_;
    }
    [[nodiscard]] auto maximum_membership_count() const noexcept -> std::uint64_t {
        return membership_maximum_;
    }
    [[nodiscard]] auto perf_ring_bytes() const noexcept -> std::uint64_t { return ring_; }
    [[nodiscard]] auto perf_mapping_bytes() const noexcept -> std::uint64_t { return mapping_; }
    [[nodiscard]] auto perf_record_capacity() const noexcept -> std::uint64_t { return events_; }
    [[nodiscard]] auto page_requested() const noexcept -> bool { return page_; }
    [[nodiscard]] auto context_requested() const noexcept -> bool { return context_; }
    [[nodiscard]] auto temperature_requested() const noexcept -> bool { return temperature_; }
    [[nodiscard]] auto frequency_requested() const noexcept -> bool { return frequency_; }
    [[nodiscard]] auto asynchronous_requested() const noexcept -> bool { return asynchronous_; }
    [[nodiscard]] auto service_cpu() const noexcept
        -> std::optional<core::LinuxCpuId> { return admission_.service_cpu(); }
    [[nodiscard]] auto slot(PairwiseMonitoringCapacitySlot slot) const noexcept
        -> std::uint64_t { return slots_[static_cast<std::size_t>(slot)]; }
    [[nodiscard]] auto preparation_rollback_values() const noexcept -> std::uint64_t {
        return preparation_rollback_values_;
    }
    [[nodiscard]] auto completion_abort_values() const noexcept -> std::uint64_t {
        return completion_abort_values_;
    }
    [[nodiscard]] auto whole_stop_values() const noexcept -> std::uint64_t {
        return whole_stop_values_;
    }

  private:
    PairwiseAdmittedMonitoringCapacity(
        PairwiseMonitoringHostCapacityAdmission admission,
        std::uint64_t workers, std::uint64_t blocks, std::uint64_t captures,
        std::uint64_t rounds, std::uint64_t base_page, std::uint64_t cpu_count,
        std::uint64_t enumeration, std::uint64_t present,
        std::uint64_t domain_maximum, std::uint64_t membership_maximum,
        std::uint64_t ring, std::uint64_t mapping, std::uint64_t events,
        bool page, bool context, bool temperature, bool frequency,
        std::array<std::uint64_t,
                   static_cast<std::size_t>(PairwiseMonitoringCapacitySlot::count)> slots,
        std::uint64_t preparation_rollback_values,
        std::uint64_t completion_abort_values,
        std::uint64_t whole_stop_values) noexcept;

    PairwiseMonitoringHostCapacityAdmission admission_;
    std::uint64_t workers_{};
    std::uint64_t blocks_{};
    std::uint64_t captures_{};
    std::uint64_t rounds_{};
    std::uint64_t base_page_{};
    std::uint64_t cpu_count_{};
    std::uint64_t enumeration_{};
    std::uint64_t present_{};
    std::uint64_t domain_maximum_{};
    std::uint64_t membership_maximum_{};
    std::uint64_t ring_{};
    std::uint64_t mapping_{};
    std::uint64_t events_{};
    bool page_{};
    bool context_{};
    bool temperature_{};
    bool frequency_{};
    bool asynchronous_{};
    std::array<std::uint64_t,
               static_cast<std::size_t>(PairwiseMonitoringCapacitySlot::count)> slots_{};
    std::uint64_t preparation_rollback_values_{};
    std::uint64_t completion_abort_values_{};
    std::uint64_t whole_stop_values_{};
    friend struct PairwiseMonitoringCapacityAccess;
};

struct PairwiseMonitoringCapacityAccess final {
    [[nodiscard]] static auto project(PairwiseMonitoringHostCapacityAdmission admission) noexcept
        -> std::expected<PairwiseAdmittedMonitoringCapacity, PairwiseRuntimeFailure>;
};

} // namespace epycbench::runtime::detail
