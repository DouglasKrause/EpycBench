#pragma once

#include "monitoring/host_q4_collection_capacity.hpp"
#include "monitoring/host_q4_kernel_maximum_authority.hpp"

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4SourceReadyFailureKind : unsigned char {
    inactive,
    plan_invalid,
    page_query_failed,
    page_size_mismatch,
    admitted_cpus_invalid,
    capacity,
    allocation,
    contained
};

struct MonitoringQ4SourceReadyFailure final {
    MonitoringQ4SourceReadyFailureKind kind;
    MonitoringQ4CapacityReason reason;
};
static_assert(std::is_trivially_copyable_v<MonitoringQ4SourceReadyFailure>);

struct MonitoringQ4SourceReadyDerivation final {
    MonitoringQ4CollectionCapacityInputs inputs;
    MonitoringQ4CollectionCapacity capacity;
};

class MonitoringQ4SourceReady final {
  public:
    MonitoringQ4SourceReady(const MonitoringQ4SourceReady&) = delete;
    MonitoringQ4SourceReady(MonitoringQ4SourceReady&&) noexcept = default;
    auto operator=(const MonitoringQ4SourceReady&) -> MonitoringQ4SourceReady& = delete;
    auto operator=(MonitoringQ4SourceReady&&) -> MonitoringQ4SourceReady& = delete;

    [[nodiscard]] auto authority() const noexcept -> const MonitoringQ4KernelMaximumReady& {
        return authority_;
    }
    [[nodiscard]] auto kernel_maximum() const noexcept -> MonitoringQ4KernelMaximum {
        return kernel_maximum_;
    }
    [[nodiscard]] auto monitoring_capacity_authority() const noexcept
        -> const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& {
        return monitoring_capacity_authority_;
    }
    [[nodiscard]] auto inputs() const noexcept -> const MonitoringQ4CollectionCapacityInputs& {
        return derived_.inputs;
    }
    [[nodiscard]] auto capacity() const noexcept -> const MonitoringQ4CollectionCapacity& {
        return derived_.capacity;
    }
    [[nodiscard]] auto deadline() const noexcept -> MonitoringQ4DeadlineSeconds {
        return derived_.inputs.structural().deadline_seconds();
    }

  private:
    MonitoringQ4SourceReady(MonitoringQ4KernelMaximumReady authority,
                            MonitoringQ4KernelMaximum maximum,
                            platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority capacity_authority,
                            MonitoringQ4SourceReadyDerivation derived,
                            std::shared_ptr<const void> binding) noexcept
        : authority_{std::move(authority)}, kernel_maximum_{maximum},
          monitoring_capacity_authority_{std::move(capacity_authority)}, derived_{std::move(derived)},
          binding_{std::move(binding)} {
    }
    MonitoringQ4KernelMaximumReady authority_;
    MonitoringQ4KernelMaximum kernel_maximum_{};
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority monitoring_capacity_authority_;
    MonitoringQ4SourceReadyDerivation derived_;
    std::shared_ptr<const void> binding_;
    friend class MonitoringQ4SourceReadyTransition;
    friend class MonitoringQ4SourceReadyTestAccess;
    friend class MonitoringQ4CpuFreqCollector;
    friend class MonitoringQ4CpuFreqEvidenceBuilder;
};

class MonitoringQ4SourceReadyTestAccess;

class MonitoringQ4SourceReadyTransition final {
  public:
    MonitoringQ4SourceReadyTransition(const MonitoringQ4SourceReadyTransition&) = delete;
    MonitoringQ4SourceReadyTransition(MonitoringQ4SourceReadyTransition&&) noexcept;
    auto operator=(const MonitoringQ4SourceReadyTransition&)
        -> MonitoringQ4SourceReadyTransition& = delete;
    auto operator=(MonitoringQ4SourceReadyTransition&&)
        -> MonitoringQ4SourceReadyTransition& = delete;

    [[nodiscard]] static auto begin(MonitoringQ4KernelMaximumReady) noexcept
        -> MonitoringQ4SourceReadyTransition;
    [[nodiscard]] auto finalize(MonitoringQ4AuthenticatedKernelMaximum&&) noexcept
        -> std::expected<MonitoringQ4SourceReady, MonitoringQ4SourceReadyFailure>;

  private:
    struct Operations final {
        void* context{};
        void (*fault)(void*, unsigned){};
    };
    static auto derive(const MonitoringQ4PlanValue&, MonitoringQ4BasePageSizeBytes,
                       std::span<const MonitoringQ4AdmittedLinuxCpuId>, MonitoringQ4KernelMaximum,
                       long, Operations) noexcept
        -> std::expected<MonitoringQ4SourceReadyDerivation, MonitoringQ4SourceReadyFailure>;
    auto finalize_with(MonitoringQ4AuthenticatedKernelMaximum&&, long, Operations) noexcept
        -> std::expected<MonitoringQ4SourceReady, MonitoringQ4SourceReadyFailure>;
    explicit MonitoringQ4SourceReadyTransition(MonitoringQ4KernelMaximumReady authority) noexcept
        : authority_{std::move(authority)} {}
    explicit MonitoringQ4SourceReadyTransition(bool consumed) noexcept : consumed_{consumed} {}
    std::optional<MonitoringQ4KernelMaximumReady> authority_;
    bool consumed_{};
    friend class MonitoringQ4SourceReadyTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
