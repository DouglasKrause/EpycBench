#pragma once

#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "monitoring/host_q4_complete_q3_predecessor_authority.hpp"
#include "monitoring/host_q4_governed_input_authority.hpp"
#include "monitoring/host_running_build_authority.hpp"

#include <array>
#include <expected>
#include <memory>
#include <optional>

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ4AuthoritySession;
class MonitoringQ4CompositionOwnerTestAccess;

enum class MonitoringQ4CompositionSource : unsigned char {
    repository,
    governed,
    predecessor,
    build,
    contained
};
enum class MonitoringQ4CompositionFactKind : unsigned char {
    primary,
    cleanup,
    changed,
    inactive,
    allocation,
    contained
};
struct MonitoringQ4CompositionFact final {
    MonitoringQ4CompositionSource source{};
    MonitoringQ4CompositionFactKind kind{};
    unsigned int detail{};
    int native_error{};
    auto operator==(const MonitoringQ4CompositionFact&) const -> bool = default;
};
struct MonitoringQ4CompositionRetirement final {
    static constexpr std::size_t component_capacity =
        1U + MonitoringQ4CompleteQ3Retirement::cleanup_capacity + 1U +
        MonitoringGovernedTrackedBundleCore::descriptor_count +
        MonitoringQ1BoundedRetirement::capacity;
    static constexpr std::size_t capacity = component_capacity * 2U;
    std::array<MonitoringQ4CompositionFact, capacity> facts{};
    std::size_t count{};
    bool overflowed{};
};
struct MonitoringQ4CompositionFailure final {
    MonitoringQ4CompositionFact primary{};
    MonitoringQ4CompositionRetirement cleanup{};
};

class MonitoringQ4CompositionOwner final {
  public:
    MonitoringQ4CompositionOwner(const MonitoringQ4CompositionOwner&) = delete;
    MonitoringQ4CompositionOwner(MonitoringQ4CompositionOwner&&) noexcept;
    auto operator=(const MonitoringQ4CompositionOwner&) -> MonitoringQ4CompositionOwner& = delete;
    auto operator=(MonitoringQ4CompositionOwner&&) -> MonitoringQ4CompositionOwner& = delete;
    ~MonitoringQ4CompositionOwner() noexcept;

    [[nodiscard]] static auto create() noexcept
        -> std::expected<MonitoringQ4CompositionOwner, MonitoringQ4CompositionFailure>;

  private:
    class Impl;
    explicit MonitoringQ4CompositionOwner(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    void adopt_repository(MonitoringQ1PlanV2AuthoritySession) noexcept;
    void adopt_governed(MonitoringQ4GovernedInputAuthority,
                        MonitoringQ4GovernedInputCandidate) noexcept;
    void adopt_predecessor(MonitoringQ4CompleteQ3PredecessorAuthority) noexcept;
    void adopt_build(MonitoringRunningBuildAuthority) noexcept;
    [[nodiscard]] auto rollback(MonitoringQ4CompositionFact) noexcept
        -> MonitoringQ4CompositionFailure;
    [[nodiscard]] auto retire(std::optional<MonitoringQ4GovernedInputValue>* = nullptr) noexcept
        -> MonitoringQ4CompositionRetirement;
    [[nodiscard]] auto take_cleanup() noexcept -> MonitoringQ4CompositionRetirement;
    void seed_cleanup(MonitoringQ4CompositionFact) noexcept;
    void note_cleanup_overflow() noexcept;
    [[nodiscard]] auto repository() noexcept -> MonitoringQ1PlanV2AuthoritySession&;
    [[nodiscard]] auto governed() noexcept -> MonitoringQ4GovernedInputAuthority&;
    [[nodiscard]] auto candidate() noexcept -> MonitoringQ4GovernedInputCandidate&;
    [[nodiscard]] auto predecessor() noexcept -> MonitoringQ4CompleteQ3PredecessorAuthority&;
    [[nodiscard]] auto build() noexcept -> MonitoringRunningBuildAuthority&;
    void append_governed_failure(const MonitoringQ4GovernedInputFailure&) noexcept;
    void append_governed_inactive() noexcept;
    void append_predecessor_retirement(const MonitoringQ4CompleteQ3Retirement&) noexcept;
    void append_repository_retirement(const MonitoringQ1BoundedRetirement&) noexcept;
    static void set_create_fault(unsigned char) noexcept;
    void adopt_test(std::size_t, void*,
                    MonitoringQ4CompositionRetirement (*)(void*) noexcept) noexcept;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4CompositionOwnerTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
