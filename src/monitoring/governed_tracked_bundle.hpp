#pragma once

#include "epycbench/core/digest.hpp"
#include "monitoring/current_revision_tracked_capture.hpp"

#include <array>
#include <bitset>
#include <expected>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringGovernedTrackedBundleFailureKind : unsigned char {
    operations,
    tracked_entry,
    directory,
    leaf,
    identity,
    read,
    growth,
    mutation,
    contained_failure,
    cleanup_failure,
    inactive
};

struct MonitoringGovernedTrackedBundleRetirement final {
    std::bitset<22> confirmed{};
    std::bitset<22> uncertain{};
    std::bitset<22> active{};
    std::array<std::uint8_t, 22> cleanup_roles{};
    std::array<int, 22> cleanup_errors{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
};

struct MonitoringGovernedTrackedBundleFailure final {
    MonitoringGovernedTrackedBundleFailureKind kind{};
    int native_error{};
    MonitoringGovernedTrackedBundleRetirement retirement{};
    MonitoringQ1GitCommandFailure diagnostic{};
};

struct MonitoringGovernedTrackedIdentity final {
    std::uint64_t device{}, inode{}, size{};
    std::uint32_t mode{}, owner{}, links{};
    std::int64_t modification_seconds{}, modification_nanoseconds{};
    std::int64_t change_seconds{}, change_nanoseconds{};
    auto operator==(const MonitoringGovernedTrackedIdentity&) const -> bool = default;
};

class MonitoringGovernedTrackedBundleCore final {
  public:
    static constexpr std::size_t directory_count = 4;
    static constexpr std::size_t leaf_count = 18;
    static constexpr std::size_t descriptor_count = directory_count + leaf_count;

    MonitoringGovernedTrackedBundleCore(const MonitoringGovernedTrackedBundleCore&) = delete;
    MonitoringGovernedTrackedBundleCore(MonitoringGovernedTrackedBundleCore&&) noexcept;
    auto operator=(const MonitoringGovernedTrackedBundleCore&)
        -> MonitoringGovernedTrackedBundleCore& = delete;
    auto operator=(MonitoringGovernedTrackedBundleCore&&)
        -> MonitoringGovernedTrackedBundleCore& = delete;
    ~MonitoringGovernedTrackedBundleCore() noexcept;

    [[nodiscard]] auto recheck() const
        -> std::expected<bool, MonitoringGovernedTrackedBundleFailure>;
    [[nodiscard]] auto retire() noexcept
        -> std::expected<void, MonitoringGovernedTrackedBundleFailure>;
    [[nodiscard]] auto retirement_outcome() const noexcept
        -> const MonitoringGovernedTrackedBundleRetirement& {
        return retirement_;
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
        void (*fault)(void*, unsigned int){};
    };
    static auto retain(int, MonitoringCurrentRevisionTrackedCapture&&, Operations)
        -> std::expected<MonitoringGovernedTrackedBundleCore,
                         MonitoringGovernedTrackedBundleFailure>;
    MonitoringGovernedTrackedBundleCore(
        std::array<int, descriptor_count>,
        std::array<MonitoringGovernedTrackedIdentity, descriptor_count>,
        std::array<std::string, leaf_count>, std::vector<core::Sha256Digest>, std::string,
        std::shared_ptr<const void>, Operations, MonitoringQ1GitCommandFailure,
        std::array<core::Error, descriptor_count>) noexcept;

    std::array<int, descriptor_count> descriptors_{};
    std::array<MonitoringGovernedTrackedIdentity, descriptor_count> identities_{};
    std::array<std::string, leaf_count> bytes_{};
    std::vector<core::Sha256Digest> digests_;
    std::string head_;
    std::shared_ptr<const void> capture_binding_;
    Operations operations_{};
    MonitoringQ1GitCommandFailure retirement_diagnostic_{};
    std::array<core::Error, descriptor_count> retirement_cleanup_{};
    std::bitset<descriptor_count> active_{};
    std::bitset<descriptor_count> confirmed_{};
    std::bitset<descriptor_count> uncertain_{};
    MonitoringGovernedTrackedBundleRetirement retirement_{};
    friend class MonitoringGovernedTrackedBundleTestAccess;
    friend class MonitoringQ4GovernedInputAuthority;
};

} // namespace epycbench::monitoring::qualification::detail
