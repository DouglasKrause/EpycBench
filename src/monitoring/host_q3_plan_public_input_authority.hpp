#pragma once

#include "monitoring/governed_plan_input_core.hpp"
#include "monitoring/host_q3_plan_value.hpp"

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ3PlanPublicInputAuthorityTestAccess;

class MonitoringQ3PlanPublicInputAuthority final {
  public:
    MonitoringQ3PlanPublicInputAuthority(const MonitoringQ3PlanPublicInputAuthority&) = delete;
    MonitoringQ3PlanPublicInputAuthority(MonitoringQ3PlanPublicInputAuthority&&) noexcept = default;
    auto operator=(const MonitoringQ3PlanPublicInputAuthority&)
        -> MonitoringQ3PlanPublicInputAuthority& = delete;
    auto operator=(MonitoringQ3PlanPublicInputAuthority&&)
        -> MonitoringQ3PlanPublicInputAuthority& = delete;
    ~MonitoringQ3PlanPublicInputAuthority() noexcept = default;

    [[nodiscard]] static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                     const MonitoringQ1GitFixedResults&)
        -> std::expected<MonitoringQ3PlanPublicInputAuthority, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto recheck() const { return core_.recheck(); }
    [[nodiscard]] auto retire() { return core_.retire(); }
    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return core_.bytes(); }
    [[nodiscard]] auto sha256() const noexcept -> const core::Sha256Digest& { return core_.sha256(); }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return core_.captured_head(); }
    [[nodiscard]] auto value() const noexcept -> const MonitoringQ3PlanValue& {
        return *static_cast<const MonitoringQ3PlanValue*>(core_.projection().get());
    }

  private:
    using Operations = MonitoringGovernedPlanInputCore::Operations;
    static auto retain_with(int, const MonitoringQ1GitFixedResults&, Operations)
        -> std::expected<MonitoringQ3PlanPublicInputAuthority, MonitoringQ1GitCommandFailure>;
    static auto retain_with_decoder(int, const MonitoringQ1GitFixedResults&, Operations,
        std::expected<std::shared_ptr<const void>, MonitoringGovernedPlanDecodeFailure>
            (*)(std::string_view))
        -> std::expected<MonitoringQ3PlanPublicInputAuthority, MonitoringQ1GitCommandFailure>;
    explicit MonitoringQ3PlanPublicInputAuthority(MonitoringGovernedPlanInputCore core) noexcept
        : core_{std::move(core)} {}
    MonitoringGovernedPlanInputCore core_;
    friend class MonitoringQ3PlanPublicInputAuthorityTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
