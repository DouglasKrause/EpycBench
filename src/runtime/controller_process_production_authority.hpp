#pragma once

#include "epycbench/runtime/pairwise_production_authority.hpp"

namespace epycbench::runtime {

namespace detail {
class ControllerProcessBootstrapAccess;
struct ProductionAuthorityTestAccess;
}

class ControllerProcessProductionAuthority final {
  public:
    ControllerProcessProductionAuthority(const ControllerProcessProductionAuthority&) = delete;
    auto operator=(const ControllerProcessProductionAuthority&)
        -> ControllerProcessProductionAuthority& = delete;
    ControllerProcessProductionAuthority(ControllerProcessProductionAuthority&&) = delete;
    auto operator=(ControllerProcessProductionAuthority&&)
        -> ControllerProcessProductionAuthority& = delete;

    [[nodiscard]] auto request_ids() noexcept -> ProductionRequestIdSource& {
        return request_ids_;
    }

  private:
    ControllerProcessProductionAuthority() noexcept = default;

    ProductionRequestIdSource request_ids_;

    friend class detail::ControllerProcessBootstrapAccess;
    friend struct detail::ProductionAuthorityTestAccess;
};

} // namespace epycbench::runtime
