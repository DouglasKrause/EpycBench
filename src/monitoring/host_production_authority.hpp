#pragma once

#include "monitoring/host_frozen_observation_admission.hpp"
#include "monitoring/host_running_build_authority.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

enum class HostProductionAuthorityFailure { unavailable, binding, capacity, allocation, contained };
struct HostProductionAuthorityView final {
    std::string_view canonical_json;
    std::string_view canonical_payload;
    std::string_view canonical_common;
    std::string_view canonical_association;
};
struct HostProductionAuthorityTestAccess;
// Untimed projection only. The enclosing collector must establish confirmed
// native-owner destruction before using this allocation-owning serializer.
class HostProductionAuthority final {
  public:
    static constexpr std::size_t hard_limit = 64U * 1024U * 1024U;
    HostProductionAuthority(const HostProductionAuthority&) = delete;
    HostProductionAuthority(HostProductionAuthority&&) noexcept;
    auto operator=(HostProductionAuthority&&) -> HostProductionAuthority& = delete;
    ~HostProductionAuthority() noexcept;
    [[nodiscard]] static auto create(const HostFrozenObservationAdmission&,
        const MonitoringQ1ValidatedPublicValues&, const MonitoringRunningBuildAuthority&,
        std::size_t allowance = hard_limit) noexcept
        -> std::expected<HostProductionAuthority, HostProductionAuthorityFailure>;
    [[nodiscard]] auto view() const noexcept -> std::optional<HostProductionAuthorityView>;
  private:
    struct State;
    explicit HostProductionAuthority(std::unique_ptr<State>) noexcept;
    static auto ascii_json(std::string_view) -> std::string;
    // The optional seam is private and production always leaves it null.
    static auto binding_json(std::string_view, const nlohmann::json&,
                             void (*after_association_clone)() noexcept = nullptr) -> nlohmann::json;
    std::unique_ptr<State> state_;
    friend struct HostProductionAuthorityTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
