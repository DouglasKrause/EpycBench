#pragma once

#include "monitoring/governed_plan_input_core.hpp"

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ2PlanPublicInputAuthorityTestAccess;

class MonitoringQ2PlanPublicInputAuthority final {
  public:
    MonitoringQ2PlanPublicInputAuthority(const MonitoringQ2PlanPublicInputAuthority&) = delete;
    MonitoringQ2PlanPublicInputAuthority(MonitoringQ2PlanPublicInputAuthority&&) noexcept = default;
    auto operator=(const MonitoringQ2PlanPublicInputAuthority&)
        -> MonitoringQ2PlanPublicInputAuthority& = delete;
    auto operator=(MonitoringQ2PlanPublicInputAuthority&&)
        -> MonitoringQ2PlanPublicInputAuthority& = delete;
    ~MonitoringQ2PlanPublicInputAuthority() noexcept = default;

    [[nodiscard]] static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                     const MonitoringQ1GitFixedResults&)
        -> std::expected<MonitoringQ2PlanPublicInputAuthority, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto recheck() const { return core_.recheck(); }
    [[nodiscard]] auto retire() { return core_.retire(); }
    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return core_.bytes(); }
    [[nodiscard]] auto sha256() const noexcept -> const core::Sha256Digest& { return core_.sha256(); }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return core_.captured_head(); }

  private:
    using Operations = MonitoringGovernedPlanInputCore::Operations;
    static auto retain_with(int, const MonitoringQ1GitFixedResults&, Operations)
        -> std::expected<MonitoringQ2PlanPublicInputAuthority, MonitoringQ1GitCommandFailure>;
    explicit MonitoringQ2PlanPublicInputAuthority(MonitoringGovernedPlanInputCore core) noexcept
        : core_{std::move(core)} {}
    MonitoringGovernedPlanInputCore core_;
    friend class MonitoringQ2PlanPublicInputAuthorityTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
