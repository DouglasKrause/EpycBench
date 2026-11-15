#pragma once
#include "epycbench/core/digest.hpp"
#include "epycbench/core/id.hpp"
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace epycbench::monitoring::qualification {
class detail_MonitoringQ1PlanV2ValueTestAccess;
namespace detail {
class MonitoringQ1ValidatedPublicValues;
class MonitoringQ2CompleteQ1PredecessorAuthority;
}
enum class MonitoringQ1PlanDerivationMode : std::uint8_t { default_mode, governed_override };
class MonitoringQ1PlanV2Value final {
  public:
    MonitoringQ1PlanV2Value(MonitoringQ1PlanV2Value&&) noexcept = default;
    MonitoringQ1PlanV2Value(const MonitoringQ1PlanV2Value&) = delete;
    auto operator=(MonitoringQ1PlanV2Value&&) -> MonitoringQ1PlanV2Value& = delete;
    auto operator=(const MonitoringQ1PlanV2Value&) -> MonitoringQ1PlanV2Value& = delete;
    static constexpr std::string_view schema =
        "epycbench.monitoring-q1-page-fault-collection-plan/v2";
    static constexpr std::string_view profile_schema =
        "epycbench.monitoring-qualification-profile/v2";
    static constexpr std::string_view profile_path =
        "config/monitoring-qualification/host-v2-profile.json";
    static constexpr std::string_view evidence_schema =
        "epycbench.monitoring-qualification-evidence/v3";
    static constexpr std::string_view backend_schema =
        "epycbench.monitoring-host-q1-backend-result/v2";
    static constexpr std::string_view algorithm =
        "epycbench.monitoring-q1-plan-derivation/deterministic-v1";
    static constexpr std::uint64_t target_total = 4096, envelope_limit = 10485760,
                                   artifact_limit = 8388608;
    auto qualification_id() const noexcept -> std::string_view { return qid_; }
    auto profile_sha256() const noexcept -> const core::Sha256Digest& { return profile_; }
    auto qualification_plan_digest() const noexcept -> const core::Sha256Digest& { return plan_; }
    auto worker_ids() const noexcept -> std::span<const core::WorkerId> { return workers_; }
    auto observations_per_worker() const noexcept -> std::uint64_t { return n_; }
    auto capacity() const noexcept -> std::uint64_t { return capacity_; }
    auto total_observations() const noexcept -> std::uint64_t { return total_; }
    auto execution_deadline_seconds() const noexcept -> std::uint64_t { return deadline_; }
    auto maximum_successor_tree_bytes() const noexcept -> std::uint64_t { return tree_; }
    auto derivation_mode() const noexcept -> MonitoringQ1PlanDerivationMode { return mode_; }
    auto per_observation_allowance_ms() const noexcept -> std::uint64_t { return allowance_; }
    auto fixed_reserve_seconds() const noexcept -> std::uint64_t { return reserve_; }

  private:
    static auto decode(std::string_view, std::string_view)
        -> std::expected<MonitoringQ1PlanV2Value, core::Error>;
    MonitoringQ1PlanV2Value(std::string, core::Sha256Digest, core::Sha256Digest,
                            std::vector<core::WorkerId>, std::uint64_t, std::uint64_t,
                            std::uint64_t, std::uint64_t, std::uint64_t,
                            MonitoringQ1PlanDerivationMode, std::uint64_t, std::uint64_t) noexcept;
    std::string qid_;
    core::Sha256Digest profile_, plan_;
    std::vector<core::WorkerId> workers_;
    std::uint64_t n_, capacity_, total_, deadline_, tree_;
    MonitoringQ1PlanDerivationMode mode_;
    std::uint64_t allowance_, reserve_;
    friend class detail_MonitoringQ1PlanV2ValueTestAccess;
    friend class detail::MonitoringQ1ValidatedPublicValues;
    friend class detail::MonitoringQ2CompleteQ1PredecessorAuthority;
};
} // namespace epycbench::monitoring::qualification
