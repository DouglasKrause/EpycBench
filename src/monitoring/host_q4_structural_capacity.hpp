#pragma once

#include "monitoring/host_q4_capacity_reason.hpp"

#include <cstdint>
#include <expected>

namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ4KernelMaximum {
    std::uint32_t value{};
};
struct MonitoringQ4BasePageBytes {
    std::uint64_t value{};
};
struct MonitoringQ4AdmittedCpuCount {
    std::uint64_t value{};
};
struct MonitoringQ4CapacityCount {
    std::uint64_t value{};
};
struct MonitoringQ4CapacityBytes {
    std::uint64_t value{};
};
struct MonitoringQ4DeadlineSeconds {
    std::uint64_t value{};
};
enum class MonitoringQ4CapacityFailureKind { invalid_input, arithmetic_overflow, allocation_failure };
struct MonitoringQ4CapacityFailure final {
    MonitoringQ4CapacityFailureKind kind;
    MonitoringQ4CapacityReason reason;
};
static_assert(std::is_trivially_copyable_v<MonitoringQ4CapacityFailure>);

class MonitoringQ4StructuralCapacity {
  public:
    MonitoringQ4StructuralCapacity(const MonitoringQ4StructuralCapacity&) = default;
    MonitoringQ4StructuralCapacity(MonitoringQ4StructuralCapacity&&) = default;
    auto operator=(const MonitoringQ4StructuralCapacity&)
        -> MonitoringQ4StructuralCapacity& = delete;
    auto operator=(MonitoringQ4StructuralCapacity&&) -> MonitoringQ4StructuralCapacity& = delete;
    [[nodiscard]] constexpr auto maximum_cpu_id() const noexcept { return maximum_cpu_id_; }
    [[nodiscard]] constexpr auto maximum_cpu_count() const noexcept { return maximum_cpu_count_; }
    [[nodiscard]] constexpr auto admitted_cpu_count() const noexcept { return admitted_cpu_count_; }
    [[nodiscard]] constexpr auto maximum_policy_directories() const noexcept {
        return maximum_policy_directories_;
    }
    [[nodiscard]] constexpr auto maximum_membership_entries() const noexcept {
        return maximum_membership_entries_;
    }
    [[nodiscard]] constexpr auto maximum_root_entries() const noexcept { return root_entry_slots_; }
    [[nodiscard]] constexpr auto root_entry_slots() const noexcept { return root_entry_slots_; }
    [[nodiscard]] constexpr auto maximum_cpu_decimal_digits() const noexcept {
        return maximum_cpu_decimal_digits_;
    }
    [[nodiscard]] constexpr auto policy_name_bytes() const noexcept { return policy_name_bytes_; }
    [[nodiscard]] constexpr auto root_entry_name_bytes() const noexcept {
        return root_entry_name_bytes_;
    }
    [[nodiscard]] constexpr auto per_file_content_bytes() const noexcept {
        return per_file_content_bytes_;
    }
    [[nodiscard]] constexpr auto per_file_probe_bytes() const noexcept {
        return per_file_probe_bytes_;
    }
    [[nodiscard]] constexpr auto root_identity_slots() const noexcept {
        return root_identity_slots_;
    }
    [[nodiscard]] constexpr auto policy_identity_slots() const noexcept {
        return policy_identity_slots_;
    }
    [[nodiscard]] constexpr auto attribute_presence_slots() const noexcept {
        return attribute_presence_slots_;
    }
    [[nodiscard]] constexpr auto attribute_identity_slots() const noexcept {
        return attribute_identity_slots_;
    }
    [[nodiscard]] constexpr auto attribute_owned_content_slots() const noexcept {
        return attribute_owned_content_slots_;
    }
    [[nodiscard]] constexpr auto membership_value_slots() const noexcept {
        return membership_value_slots_;
    }
    [[nodiscard]] constexpr auto attribute_owned_bytes() const noexcept {
        return attribute_owned_bytes_;
    }
    [[nodiscard]] constexpr auto owned_content_bytes() const noexcept {
        return owned_content_bytes_;
    }
    [[nodiscard]] constexpr auto simultaneous_live_roles() const noexcept {
        return simultaneous_live_roles_;
    }
    [[nodiscard]] constexpr auto cleanup_events() const noexcept { return cleanup_events_; }
    [[nodiscard]] constexpr auto enumeration_operation_units() const noexcept {
        return enumeration_operation_units_;
    }
    [[nodiscard]] constexpr auto total_operation_units() const noexcept {
        return total_operation_units_;
    }
    [[nodiscard]] constexpr auto deadline_quanta() const noexcept { return deadline_quanta_; }
    [[nodiscard]] constexpr auto deadline_seconds() const noexcept { return deadline_seconds_; }

  private:
    friend auto monitoring_q4_structural_capacity(MonitoringQ4KernelMaximum,
                                                  MonitoringQ4BasePageBytes,
                                                  MonitoringQ4AdmittedCpuCount)
        -> std::expected<MonitoringQ4StructuralCapacity, MonitoringQ4CapacityFailure>;
    constexpr MonitoringQ4StructuralCapacity() = default;
    MonitoringQ4CapacityCount maximum_cpu_id_{};
    MonitoringQ4CapacityCount maximum_cpu_count_{};
    MonitoringQ4CapacityCount admitted_cpu_count_{};
    MonitoringQ4CapacityCount maximum_policy_directories_{};
    MonitoringQ4CapacityCount maximum_membership_entries_{};
    MonitoringQ4CapacityCount root_entry_slots_{};
    MonitoringQ4CapacityCount maximum_cpu_decimal_digits_{};
    MonitoringQ4CapacityBytes policy_name_bytes_{};
    MonitoringQ4CapacityBytes root_entry_name_bytes_{};
    MonitoringQ4CapacityBytes per_file_content_bytes_{};
    MonitoringQ4CapacityBytes per_file_probe_bytes_{};
    MonitoringQ4CapacityCount root_identity_slots_{};
    MonitoringQ4CapacityCount policy_identity_slots_{};
    MonitoringQ4CapacityCount attribute_presence_slots_{};
    MonitoringQ4CapacityCount attribute_identity_slots_{};
    MonitoringQ4CapacityCount attribute_owned_content_slots_{};
    MonitoringQ4CapacityCount membership_value_slots_{};
    MonitoringQ4CapacityBytes attribute_owned_bytes_{};
    MonitoringQ4CapacityBytes owned_content_bytes_{};
    MonitoringQ4CapacityCount simultaneous_live_roles_{};
    MonitoringQ4CapacityCount cleanup_events_{};
    MonitoringQ4CapacityCount enumeration_operation_units_{};
    MonitoringQ4CapacityCount total_operation_units_{};
    MonitoringQ4CapacityCount deadline_quanta_{};
    MonitoringQ4DeadlineSeconds deadline_seconds_{};
};

[[nodiscard]] auto monitoring_q4_structural_capacity(MonitoringQ4KernelMaximum,
                                                     MonitoringQ4BasePageBytes,
                                                     MonitoringQ4AdmittedCpuCount)
    -> std::expected<MonitoringQ4StructuralCapacity, MonitoringQ4CapacityFailure>;
} // namespace epycbench::monitoring::qualification::detail
