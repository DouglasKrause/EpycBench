#pragma once

#include "core/governed_repository_root_authority_access.hpp"
#include "epycbench/core/digest.hpp"
#include "monitoring/host_q1_git_fixed_results.hpp"

#include <array>
#include <memory>

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ2PlanPublicInputAuthority;
class MonitoringQ3PlanPublicInputAuthority;

struct MonitoringGovernedPlanInputIdentity final {
    std::uint64_t device{}, inode{}, size{};
    std::uint32_t mode{}, owner{}, links{};
    std::int64_t modification_seconds{}, modification_nanoseconds{};
    std::int64_t change_seconds{}, change_nanoseconds{};
    auto operator==(const MonitoringGovernedPlanInputIdentity&) const -> bool = default;
};

class MonitoringGovernedPlanInputCore final {
  public:
    MonitoringGovernedPlanInputCore(const MonitoringGovernedPlanInputCore&) = delete;
    MonitoringGovernedPlanInputCore(MonitoringGovernedPlanInputCore&&) noexcept;
    auto operator=(const MonitoringGovernedPlanInputCore&)
        -> MonitoringGovernedPlanInputCore& = delete;
    auto operator=(MonitoringGovernedPlanInputCore&&)
        -> MonitoringGovernedPlanInputCore& = delete;
    ~MonitoringGovernedPlanInputCore() noexcept;

    [[nodiscard]] auto recheck() const -> std::expected<bool, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto retire() -> std::expected<void, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return bytes_; }
    [[nodiscard]] auto sha256() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto projection() const noexcept -> const std::shared_ptr<const void>& {
        return projection_;
    }

  private:
    struct Operations final {
        void* context{};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept {};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept {};
        int (*close_fd)(void*, int) noexcept {};
        unsigned int (*effective_user)(void*) noexcept {};
        void (*fault)(void*, unsigned int) {};
        std::optional<MonitoringQ1GitCommandFailure> (*recheck_failure)(void*) {};
    };
    struct Specification final {
        std::string_view tracked_path;
        std::string_view leaf;
        std::string_view stable_code;
        std::string_view message;
        std::string_view operation;
        std::size_t record_count;
        std::size_t record_index;
        std::array<std::string_view, 5> exact_record_paths;
        bool require_exact_record_order;
        std::expected<std::shared_ptr<const void>, MonitoringGovernedPlanDecodeFailure> (*decode)(std::string_view);
        unsigned int leaf_mode{0600U};
    };
    static auto retain(int, const MonitoringQ1GitFixedResults&, Operations, Specification)
        -> std::expected<MonitoringGovernedPlanInputCore, MonitoringQ1GitCommandFailure>;
    MonitoringGovernedPlanInputCore(
        std::array<int, 4>, std::array<MonitoringGovernedPlanInputIdentity, 4>, std::string,
        core::Sha256Digest, std::string, std::shared_ptr<const void>, std::shared_ptr<const void>,
        Operations, Specification, MonitoringQ1GitCommandFailure,
        std::array<core::Error, 4>) noexcept;

    std::array<int, 4> descriptors_{};
    std::array<MonitoringGovernedPlanInputIdentity, 4> identities_{};
    std::string bytes_;
    core::Sha256Digest digest_;
    std::string head_;
    std::shared_ptr<const void> capture_binding_{};
    std::shared_ptr<const void> projection_{};
    Operations operations_{};
    Specification specification_{};
    MonitoringQ1GitCommandFailure retirement_failure_;
    std::array<core::Error, 4> retirement_cleanup_{};
    bool active_{true};

    friend class MonitoringQ2PlanPublicInputAuthority;
    friend class MonitoringQ3PlanPublicInputAuthority;
};

} // namespace epycbench::monitoring::qualification::detail
