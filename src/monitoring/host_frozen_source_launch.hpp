#pragma once

#include "epycbench/runtime/runner_control_monitor.hpp"
#include "epycbench/core/digest.hpp"
#include "monitoring/host_q1_git_bounded_child.hpp"
#include "platform_linux/host_frozen_process_launch.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::monitoring::qualification::detail {

enum class HostFrozenSourceLaunchType : std::uint16_t {
    launch = 1, launched, terminal, cancel, grant, permit, git, git_streams, finish, ready
};
enum class HostFrozenSourceLaunchReason : unsigned char {
    malformed, capacity, association, sequence, credentials, rights, clock,
    deadline, cancelled, io, eof, native, uncertain, unqualified, contained
};
enum class HostFrozenSourceBindingOperation : unsigned char {
    guard, receive, send, close, credentials, decode, open, metadata, read, sync
};
struct HostFrozenSourceBindingFact final {
    HostFrozenSourceLaunchReason reason{};
    HostFrozenSourceBindingOperation operation{};
    long native_result{};
    int error_number{}, descriptor{-1};
};
using HostFrozenSourceLaunchBytes = std::array<std::byte, 1024>;
struct HostFrozenSourceLaunchRecord final {
    HostFrozenSourceLaunchType type{};
    platform_linux::host_work::SourceLaunchRole role{};
    std::uint64_t sequence{}, generation{};
    std::array<std::byte, 16> invocation{};
    std::uint64_t origin_ns{}, phase_end_ns{}, entry_end_ns{}, permit_end_ns{}, launch_origin_ns{};
    platform_linux::host_work::SourceLaunchPhase phase{platform_linux::host_work::SourceLaunchPhase::q5};
    std::uint64_t phase_generation{1}, invocation_origin_ns{}, campaign_end_ns{};
    platform_linux::host_work::Resources resources{};
    std::array<std::byte, 32> build{}, source{}, campaign{};
    std::uint64_t stdout_cap{};
    std::uint8_t oid_width{}, secondary_oid_width{};
    std::uint16_t path_bytes{}, rights_count{}, selector{};
    std::uint64_t pid{};
    std::uint32_t wait_status{};
    std::int32_t error_number{};
    std::array<char, 64> oid{}, secondary_oid{};
    std::array<char, 511> path{};
    auto operator==(const HostFrozenSourceLaunchRecord&) const -> bool = default;
};
[[nodiscard]] auto encode_host_frozen_source_launch_record(
    const HostFrozenSourceLaunchRecord&) noexcept
    -> std::expected<HostFrozenSourceLaunchBytes, HostFrozenSourceLaunchReason>;
[[nodiscard]] auto decode_host_frozen_source_launch_record(std::span<const std::byte>) noexcept
    -> std::expected<HostFrozenSourceLaunchRecord, HostFrozenSourceLaunchReason>;
// Timing/accounting handoff from the actual same-PID finite invoker. This is
// not a producer or publication capability. The dispatcher owns all five
// inherited descriptors before calling this ordinary untimed decoder.
struct HostFrozenSourceGovernedHandoff final {
    std::uint64_t window_origin{}, work_end{}, window_end{}, exposure_used{};
    std::uint32_t window_ordinal{}, q4_count{}, original_policy{};
    std::array<char, 40> source{};
    core::Sha256Digest execution_seal, build_manifest;
    std::size_t ready_bytes{};
    bool post_reboot_baseline{};
    bool governed_reboot_baseline{};
    bool environment_recovery_baseline{};
    bool current_source_baseline{};
};
[[nodiscard]] auto decode_host_frozen_source_governed_handoff(std::string_view,
    std::uint64_t q5_origin, std::uint64_t launch_origin, std::uint64_t launch_end,
    std::uint64_t original_main_entry, std::string_view qualification_id = {}) noexcept
    -> std::expected<HostFrozenSourceGovernedHandoff, HostFrozenSourceLaunchReason>;
// The already accepted frozen64-byte control record uses the existing source
// channel; it neither serializes a Session nor supplies producer authority.
inline constexpr std::uint16_t host_frozen_source_control_selector = UINT16_C(0x8000);
inline constexpr std::uint16_t host_frozen_source_root_directory_selector = UINT16_C(0x8001);
inline constexpr std::uint16_t host_frozen_source_leaf_directory_selector = UINT16_C(0x8002);
inline constexpr std::uint16_t host_frozen_source_reobserve_selector = UINT16_C(0x8003);
inline constexpr std::uint16_t host_frozen_source_publication_retired_selector = UINT16_C(0x8004);
inline constexpr std::uint16_t host_frozen_source_input_selector = UINT16_C(0x8005);
inline constexpr std::uint16_t host_frozen_source_output_selector = UINT16_C(0x8006);
// Operational refusal envelope for the controlled qualification host. It is
// independent of storage authority and supplies one initial acquisition plus
// one actual Session revalidation before each Q5/Q6 Run preparation.
[[nodiscard]] auto host_frozen_source_linux_limits(platform_linux::host_work::SourceLaunchPhase) noexcept
    -> std::expected<platform_linux::detail::TopologySourceLimits, HostFrozenSourceLaunchReason>;
[[nodiscard]] auto source_control_envelope(HostFrozenSourceLaunchRecord,
    std::span<const std::byte>) noexcept
    -> std::expected<HostFrozenSourceLaunchRecord, HostFrozenSourceLaunchReason>;
[[nodiscard]] auto source_control_payload(const HostFrozenSourceLaunchRecord&) noexcept
    -> std::expected<std::array<std::byte, 64>, HostFrozenSourceLaunchReason>;

using HostFrozenSourcePeer = platform_linux::host_work::SourceLaunchPeer;
using HostFrozenSourceAncillary = platform_linux::host_work::SourceLaunchAncillary;
struct HostFrozenSourceLaunchIO final {
    void* context{};
    platform_linux::detail::RawStatusResult (*receive)(void*, int,
        std::span<std::byte>, HostFrozenSourceAncillary*) noexcept{};
    platform_linux::detail::RawStatusResult (*send)(void*, int,
        std::span<const std::byte>, std::span<const int>) noexcept{};
    platform_linux::detail::RawStatusResult (*close)(void*, int) noexcept{};
    platform_linux::host_work::GuardOperations guard;
    const platform_linux::host_work::SourceLaunchChildFailure* raw_clock{};
};
class HostFrozenSourceLaunchBinding;
struct FrozenProcessRecord;
struct HostFrozenSourceLaunchState;
struct HostFrozenSourceLaunchTestAccess;

// A borrow of the exact authenticated child binding, not a Resources parameter
// that a caller can manufacture. The parent slot retains the enclosing lease.
class HostFrozenSourceChildQuota final {
  public:
    HostFrozenSourceChildQuota(const HostFrozenSourceChildQuota&) = delete;
    HostFrozenSourceChildQuota(HostFrozenSourceChildQuota&&) noexcept;
    auto operator=(HostFrozenSourceChildQuota&&) -> HostFrozenSourceChildQuota& = delete;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto resources() const noexcept
        -> const platform_linux::host_work::Resources& { return resources_; }
    [[nodiscard]] auto parent_pid() const noexcept -> int;
    [[nodiscard]] auto origin_ns() const noexcept -> std::uint64_t;
    [[nodiscard]] auto phase_end_ns() const noexcept -> std::uint64_t;
    [[nodiscard]] auto guard() const noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    // One ledger inside this exact parent-reserved quota. A failed later
    // construction cannot mint a replacement full budget or reclaim this grant.
    [[nodiscard]] auto claim_ledger() const noexcept
        -> std::expected<platform_linux::host_work::ResourceLedger, HostFrozenSourceLaunchReason>;
  private:
    HostFrozenSourceChildQuota(const HostFrozenSourceLaunchBinding*,
                              platform_linux::host_work::Resources) noexcept;
    const HostFrozenSourceLaunchBinding* binding_;
    platform_linux::host_work::Resources resources_;
    friend class HostFrozenSourceLaunchBinding;
    friend class HostQ0bGovernedWork;
};

// Stable source-only binding precedes SourceAdmission and every Git selector.
// Incoming rights are registered into this fixed stock before record decoding.
// It cannot be moved while Q1/Session/C0 borrow it.
class HostFrozenSourceLaunchBinding final {
  public:
    HostFrozenSourceLaunchBinding(const HostFrozenSourceLaunchBinding&) = delete;
    HostFrozenSourceLaunchBinding(HostFrozenSourceLaunchBinding&&) = delete;
    auto operator=(HostFrozenSourceLaunchBinding&&) -> HostFrozenSourceLaunchBinding& = delete;
    ~HostFrozenSourceLaunchBinding() noexcept;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto supervises_git() const noexcept -> bool { return git_.run != nullptr; }
    [[nodiscard]] auto child_quota() const noexcept
        -> std::expected<HostFrozenSourceChildQuota, HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto guard() const noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto retire() noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto git(const MonitoringQ1GitExecutable&, int repository,
        std::uint16_t selector, std::string_view oid, std::string_view secondary_oid,
        std::string_view path, std::size_t stdout_cap) const
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto raw_primary() const noexcept
        -> const std::optional<HostFrozenSourceLaunchReason>& { return primary_; }
    [[nodiscard]] auto first_raw_fact() const noexcept
        -> const std::optional<HostFrozenSourceBindingFact>& { return first_fact_; }
    [[nodiscard]] auto secondary_facts() const noexcept -> std::span<const HostFrozenSourceBindingFact> {
        return std::span{secondary_}.first(secondary_count_);
    }
    [[nodiscard]] auto owning_cancellation() const noexcept -> runtime::RunnerCancellation& {
        return *cancellation_;
    }
    [[nodiscard]] auto grant_record() const noexcept -> const HostFrozenSourceLaunchRecord& {
        return grant_;
    }
  private:
    struct GitOperations final {
        void* context{};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*run)(
            void*, const MonitoringQ1GitExecutable&, int, std::uint16_t, std::string_view,
            std::string_view, std::string_view, std::size_t){};
    };
    HostFrozenSourceLaunchBinding(int, int, std::uint32_t, runtime::RunnerCancellation&,
        HostFrozenSourceLaunchIO, GitOperations, HostFrozenSourceLaunchRecord,
        std::span<const int> borrowed_descriptors = {}) noexcept;
    [[nodiscard]] auto authenticate() noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto fail(HostFrozenSourceLaunchReason,
        HostFrozenSourceBindingOperation = HostFrozenSourceBindingOperation::guard,
        platform_linux::detail::RawStatusResult = {}, int descriptor = -1) const noexcept
        -> std::unexpected<HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto close_pending() noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    [[nodiscard]] auto write_payload(const FrozenProcessRecord&, std::string_view,
        platform_linux::host_work::SourceLaunchStreamOperations) noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason>;
    int channel_, parent_pid_;
    std::uint32_t uid_;
    runtime::RunnerCancellation* cancellation_;
    HostFrozenSourceLaunchIO io_;
    GitOperations git_;
    HostFrozenSourceLaunchRecord grant_;
    mutable std::uint64_t last_ns_;
    struct PendingRight final {
        int descriptor{-1};
        bool consumed{}, uncertain{};
        platform_linux::detail::RawStatusResult close_result{};
    };
    std::array<PendingRight, 16> pending_rights_{};
    std::array<int, 48> borrowed_descriptors_{};
    std::size_t borrowed_count_{};
    std::size_t rights_count_{};
    mutable std::optional<HostFrozenSourceLaunchReason> primary_;
    mutable std::optional<HostFrozenSourceBindingFact> first_fact_;
    mutable std::array<HostFrozenSourceBindingFact, 32> secondary_{};
    mutable std::size_t secondary_count_{};
    bool authenticated_{}, channel_closed_{};
    mutable bool uncertainty_{};
    mutable bool quota_issued_{}, quota_claimed_{};
    friend class HostFrozenSourceChildQuota;
    friend struct HostFrozenSourceLaunchState;
    friend struct HostFrozenSourceLaunchTestAccess;
    friend class HostQ0bGovernedWork;
};

struct HostFrozenSourceLaunchClock final {
    platform_linux::host_work::SourceLaunchChildFailure current;
    platform_linux::detail::RawStatusResult actual;
    const runtime::RunnerCancellation* cancellation{};
};
[[nodiscard]] auto production_host_frozen_source_launch_io(HostFrozenSourceLaunchClock&) noexcept
    -> HostFrozenSourceLaunchIO;
// Exact internal dispatch only. The actual sole main authority is borrowed.
[[nodiscard]] auto is_host_frozen_source_invocation(int argc, char* argv[]) noexcept -> bool;
[[nodiscard]] auto run_host_frozen_source_invocation(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority&, runtime::RunnerCancellation&,
    HostFrozenSourceLaunchClock&, std::uint64_t original_main_entry_ns) noexcept -> int;

} // namespace epycbench::monitoring::qualification::detail
