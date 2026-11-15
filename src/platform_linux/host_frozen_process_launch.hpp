#pragma once

#include "platform_linux/host_work_admission.hpp"
#include "platform_linux/process_ipc_operations.hpp"
#include "platform_linux/topology_source_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::platform_linux::host_work {

enum class SourceLaunchRole : unsigned char { controller, collector, validator, git };
enum class SourceLaunchPhase : std::uint16_t { q0b = 0, q5 = 5, q6 = 6, q7 = 7 };
enum class SourceLaunchStage : unsigned char { startup, bootstrap, collection, publication, terminal };
enum class SourceLaunchReason : unsigned char {
    invalid_input, capacity, association, sequence, state, clock, deadline,
    cancelled, pipe, fork, group, gate, child_setup, wait, signal, close,
    ancillary, uncertainty, input
};
enum class SourceLaunchOperation : unsigned char {
    guard, reserve, pipe, fork, group, gate, child_setup, wait, signal, close,
    receive_rights, send_rights, directory_open, directory_stat, directory_read, directory_parse,
    input_open, input_stat, input_read
};
struct SourceLaunchFact final {
    SourceLaunchReason reason{};
    SourceLaunchOperation operation{};
    SourceLaunchRole role{};
    std::uint64_t generation{};
    long native_result{};
    int error_number{};
    int descriptor{-1};
};
struct SourceLaunchDescriptorMapping final { int source{-1}, target{-1}; };
struct SourceLaunchDeadline final {
    std::uint64_t origin_ns{}, end_ns{};
    auto operator==(const SourceLaunchDeadline&) const -> bool = default;
};
struct SourceLaunchPhaseDeadline final {
    SourceLaunchPhase phase{SourceLaunchPhase::q5};
    std::uint64_t generation{1}, origin_ns{}, end_ns{}, invocation_origin_ns{}, campaign_end_ns{};
    auto operator==(const SourceLaunchPhaseDeadline&) const -> bool = default;
};
struct SourceLaunchSpec final {
    int image{-1};
    char* const* arguments{};
    char* const* environment{};
    std::span<const SourceLaunchDescriptorMapping> mappings;
    std::span<const int> inherited_descriptors;
    LimitDemand limits;
    int expected_parent{};
    std::uint64_t origin_ns{}, entry_end_ns{};
    SourceLaunchPhase phase{SourceLaunchPhase::q5};
    std::uint64_t phase_generation{1};
    // Borrowed from the genuine charged inherited inventory. Defaults retain
    // the null-input/current-directory behavior of existing launch callers.
    int working_directory{-1}, standard_input{-1};
};
enum class SourceLaunchChildOperation : std::uint16_t {
    clock, parent_death, parent, group, gate, gate_wait, limit_identity, limit_get, limit_set,
    standard_close, duplicate, descriptor_flags, descriptor_close, execute, limits, working_directory
};
struct SourceLaunchChildFailure final {
    std::uint32_t magic{UINT32_C(0x45534243)};
    std::uint16_t version{1};
    SourceLaunchReason reason{};
    bool lower_present{};
    Reason lower{};
    SourceLaunchChildOperation operation{};
    std::int64_t native_result{};
    std::int32_t error_number{};
    std::int32_t native_argument{-1};
    bool raw_present{};
    std::array<std::byte, 7> reserved{};
};
using SourceLaunchChildFailureBytes = std::array<std::byte, 32>;
[[nodiscard]] auto encode_source_launch_child_failure(const SourceLaunchChildFailure&) noexcept
    -> SourceLaunchChildFailureBytes;
[[nodiscard]] auto decode_source_launch_child_failure(std::span<const std::byte>) noexcept
    -> std::expected<SourceLaunchChildFailure, SourceLaunchReason>;
// A source-owned current sample. Checked conversion can fail after a successful
// clock_gettime; keep the actual native result/errno before that projection.
// This cell is overwritten at the next sample. The enclosing owner separately
// retains its immutable primary and ordered later facts before sampling again.
[[nodiscard]] auto decode_source_launch_raw_clock(detail::RawStatusResult,
    std::int64_t seconds, std::int64_t nanoseconds, std::uint64_t*,
    SourceLaunchChildFailure&, detail::RawStatusResult* actual_native = nullptr) noexcept
    -> detail::RawStatusResult;
[[nodiscard]] auto source_launch_raw_now(SourceLaunchChildFailure&,
    std::uint64_t*, detail::RawStatusResult* actual_native = nullptr) noexcept
    -> detail::RawStatusResult;
// Source-local wrapping of the existing admitted limits algorithm. All raw
// failures are captured before its typed projection; no shared contract changes.
// The optional last-observed cell is borrowed synchronously. The actual child
// shares its one clock history across both helpers and all surrounding guards.
// Standalone callers retain a local history beginning at the original origin.
[[nodiscard]] auto apply_source_launch_child_limits(const SourceLaunchSpec&,
    const LimitOperations&, const SourceLaunchChildFailure* original_clock = nullptr,
    std::uint64_t* last_observed = nullptr) noexcept
    -> std::expected<void, SourceLaunchChildFailure>;
[[nodiscard]] auto apply_source_launch_working_directory(const SourceLaunchSpec&, void*,
    detail::RawStatusResult (*)(void*, int) noexcept, GuardOperations,
    const SourceLaunchChildFailure* original_clock = nullptr,
    std::uint64_t* last_observed = nullptr) noexcept
    -> std::expected<void, SourceLaunchChildFailure>;
struct SourceLaunchPeer final { int pid{}; std::uint32_t uid{}, gid{}; };
struct SourceLaunchAncillary final {
    std::array<int, 16> descriptors{};
    std::size_t count{};
    SourceLaunchPeer peer;
    bool credentials{}, truncated{}, malformed{};
};
struct SourceLaunchChannelOperations final {
    void* context{};
    detail::RawStatusResult (*socket_pair)(void*, int[2]) noexcept{};
    detail::RawStatusResult (*enable_credentials)(void*, int) noexcept{};
    detail::RawStatusResult (*receive)(void*, int, std::span<std::byte>,
                                       SourceLaunchAncillary*) noexcept{};
    detail::RawStatusResult (*send)(void*, int, std::span<const std::byte>,
                                    std::span<const int>) noexcept{};
    detail::RawStatusResult (*close)(void*, int) noexcept{};
};
// The source dispatcher owns every supplied descriptor. These leaves make one
// native attempt; the same actual token and absolute fence bound dispatcher
// retries and transport polling. Retirement waiting retains the entire actor.
struct SourceLaunchStreamOperations final {
    detail::RawStatusResult (*write)(int, std::span<const std::byte>) noexcept{};
    detail::RawStatusResult (*read_at)(int, std::span<std::byte>, std::uint64_t) noexcept{};
    detail::RawStatusResult (*pause)(int) noexcept{};
    std::uint32_t (*effective_uid)() noexcept{};
    void (*retain_until_outer_termination)() noexcept{};
    int (*process_id)() noexcept{};
    detail::RawStatusResult (*subreaper)(bool set, int* value) noexcept{};
    detail::RawStatusResult (*entropy)(std::span<std::byte>) noexcept{};
    detail::RawStatusResult (*make_directory)(int, const char*) noexcept{};
    detail::RawStatusResult (*output_flags)(int, bool set, int flags) noexcept{};
    detail::RawStatusResult (*output_wait)(int, int milliseconds, short* events) noexcept{};
};
[[nodiscard]] auto production_source_launch_stream_operations() noexcept
    -> SourceLaunchStreamOperations;
// Parse one actual getdents64 refill from the closed source snapshot bank.
// The owning caller retains its FD, guard and cumulative refill/record bounds.
[[nodiscard]] auto source_launch_directory_names(std::span<const char>, SourceLaunchPhase,
    bool root, std::uint64_t& mask, std::size_t& visited) noexcept
    -> std::expected<void, SourceLaunchReason>;
// The original source actor refuses an inherited FD outside its actual fixed
// inventory. This parses one /proc/self/fd refill, including its own held FD;
// it does not close unknown owners or assume lowering RLIMIT_NOFILE closes them.
[[nodiscard]] auto source_launch_descriptor_names(std::span<const char>,
    std::span<const int> known, std::size_t& visited) noexcept
    -> std::expected<void, SourceLaunchReason>;
struct SourceLaunchOperations final {
    void* context{};
    detail::RawStatusResult (*pipe)(void*, int[2]) noexcept{};
    detail::RawStatusResult (*gate_pair)(void*, int[2]) noexcept{};
    detail::RawStatusResult (*open_null)(void*) noexcept{};
    detail::RawStatusResult (*fork)(void*) noexcept{};
    detail::RawStatusResult (*group)(void*, int) noexcept{};
    detail::RawStatusResult (*gate)(void*, int) noexcept{};
    detail::RawStatusResult (*close)(void*, int) noexcept{};
    // Called only in the child branch. Actual production never returns.
    void (*child_entry)(void*, const SourceLaunchSpec&, int, int) noexcept{};
    detail::ProcessOperations process;
};
struct SourceLaunchSlotView final {
    // Preserve any positive raw fork return before narrowing. An injected or
    // unsupported value outside pid_t's admitted domain remains an owned,
    // uncertain launch; no conversion failure erases the acquisition.
    long raw_pid{};
    int pid{};
    std::uint64_t generation{};
    SourceLaunchPhase phase{SourceLaunchPhase::q5};
    std::uint64_t phase_generation{1};
    bool occupied{}, terminal{}, uncertainty{}, group_confirmed{}, gate_released{};
    std::optional<ProcessExitStatus> status;
    std::optional<int> native_wait_status;
    std::array<int, 3> streams{-1, -1, -1}; // stdout, stderr, actual setup read
};
struct SourceLaunchDescriptorView final {
    int descriptor{-1};
    SourceLaunchRole role{};
    std::uint64_t generation{};
    bool acquired{}, consumed{}, uncertain{};
    int close_error{};
};

enum class SourceInputPath : unsigned char { manifest, policy, input_template, campaign, post_reboot_campaign, governed_reboot_campaign, environment_recovery_campaign, current_source_campaign };
struct SourceInputRootIdentity final { std::uint64_t device{}, inode{}; };
// Direct fixed-input native reader. The source State owns/charges the borrowed
// 64KiB scratch and authenticated root FD before constructing this owner. This
// owner reserves its three walking/file roles before the first open. It creates
// no file/input/publication capability and uses no legacy filesystem wrapper.
class SourceInputFileOwner final {
  public:
    static constexpr std::size_t input_capacity = 65536;
    SourceInputFileOwner(ResourceLedger&, GuardOperations, detail::TopologySourceOperations,
        int authenticated_root, SourceInputRootIdentity, std::span<char> charged_scratch,
        std::uint64_t original_origin, std::uint64_t original_end,
        std::uint64_t& actual_last, const SourceLaunchChildFailure* current_clock = nullptr,
        std::span<const int> borrowed_descriptors = {}) noexcept;
    SourceInputFileOwner(const SourceInputFileOwner&) = delete;
    SourceInputFileOwner(SourceInputFileOwner&&) = delete;
    auto operator=(SourceInputFileOwner&&) -> SourceInputFileOwner& = delete;
    ~SourceInputFileOwner() noexcept;
    [[nodiscard]] auto read(SourceInputPath) noexcept
        -> std::expected<std::string_view, SourceLaunchReason>;
    [[nodiscard]] auto retire() noexcept -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto primary() const noexcept -> const std::optional<SourceLaunchFact>& {
        return primary_;
    }
    [[nodiscard]] auto cleanup() const noexcept -> std::span<const SourceLaunchFact> {
        return std::span{cleanup_}.first(cleanup_count_);
    }
    [[nodiscard]] auto uncertain() const noexcept -> bool { return uncertain_; }
  private:
    [[nodiscard]] auto guard() noexcept -> bool;
    [[nodiscard]] auto fail(SourceLaunchReason, SourceLaunchOperation,
        detail::RawStatusResult = {}, int descriptor = -1) noexcept -> bool;
    [[nodiscard]] auto close_owned() noexcept -> bool;
    [[nodiscard]] auto close_cell(std::size_t) noexcept -> bool;
    [[nodiscard]] auto charge_native() noexcept -> bool;
    [[nodiscard]] auto root_matches() noexcept -> bool;
    GuardOperations guard_;
    detail::TopologySourceOperations operations_;
    int root_;
    SourceInputRootIdentity identity_;
    std::span<char> scratch_;
    std::span<const int> borrowed_;
    std::uint64_t origin_, end_;
    std::uint64_t* last_;
    const SourceLaunchChildFailure* clock_;
    std::optional<ResourceLease> lease_;
    std::array<long, 3> owned_{-1, -1, -1};
    std::optional<SourceLaunchFact> primary_;
    std::array<SourceLaunchFact, 32> cleanup_{};
    std::size_t cleanup_count_{}, native_calls_{}, close_calls_{}, reads_{};
    bool uncertain_{}, retired_{};
};

// This is the enclosing invocation's actual owner. It cannot be returned or
// moved out of main's stable storage. Every post-acquisition registration is a
// fixed assignment. The enclosing source loop must retain it after uncertainty
// rather than return/destruct it or release an unresolved role lease.
class SourceLaunchInvocation final {
  public:
    static constexpr std::size_t role_count = 4;
    static constexpr std::size_t descriptor_capacity = 48;
    static constexpr std::size_t failure_capacity = 4096;
    SourceLaunchInvocation(ResourceLedger&, GuardOperations, SourceLaunchOperations,
                           std::uint64_t invocation_origin_ns, std::uint64_t campaign_end_ns,
                           const SourceLaunchChildFailure* current_clock = nullptr,
                           SourceLaunchPhase initial_phase = SourceLaunchPhase::q5,
                           std::optional<SourceLaunchDeadline> inherited_launch = std::nullopt) noexcept;
    SourceLaunchInvocation(const SourceLaunchInvocation&) = delete;
    auto operator=(const SourceLaunchInvocation&) -> SourceLaunchInvocation& = delete;
    SourceLaunchInvocation(SourceLaunchInvocation&&) = delete;
    auto operator=(SourceLaunchInvocation&&) -> SourceLaunchInvocation& = delete;
    ~SourceLaunchInvocation() noexcept;

    [[nodiscard]] auto set_stage(SourceLaunchStage) noexcept
        -> std::expected<void, SourceLaunchReason>;
    // The source publication caller must already have accepted the genuine
    // predecessor FINISH/output/validator transfer and publication. This lower
    // owner checks resource/deadline chronology and conveys no source authority.
    [[nodiscard]] auto advance_phase(SourceLaunchPhase next, std::uint64_t previous_generation) noexcept
        -> std::expected<SourceLaunchPhaseDeadline, SourceLaunchReason>;
    [[nodiscard]] auto phase_deadline() const noexcept -> SourceLaunchPhaseDeadline;
    // Mint once before launch-specific allocation/image/argv/FD preparation.
    // The actual owner and original phase end bind this scope; delays never
    // replace its origin. Only one launch scope may be outstanding.
    [[nodiscard]] auto begin_launch(SourceLaunchRole) noexcept
        -> std::expected<SourceLaunchDeadline, SourceLaunchReason>;
    // This single invocation bank precedes every native receive. The16
    // possible receiver copies and one independent directory description stay
    // reserved through malformed messages, in-flight transfers and uncertainty.
    [[nodiscard]] auto prepare_receive_bank(std::span<const int> borrowed,
        const SourceLaunchChannelOperations&) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto receive_channel(int, std::span<std::byte>, SourceLaunchAncillary&) noexcept
        -> std::expected<detail::RawStatusResult, SourceLaunchReason>;
    [[nodiscard]] auto received_descriptors() const noexcept -> std::span<const int>;
    [[nodiscard]] auto close_received() noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto publication_directory(bool root,
        const detail::TopologySourceOperations&) noexcept
        -> std::expected<std::uint64_t, SourceLaunchReason>;
    [[nodiscard]] auto retire_receive_bank() noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto launch(SourceLaunchRole, Resources, const SourceLaunchSpec&) noexcept
        -> std::expected<int, SourceLaunchReason>;
    // The trusted source Python actor uses its existing Popen owner. The
    // enclosing invocation reserves these fixed role slots before replying to
    // its launch request, then adopts the actual returned PID before any
    // fallible registration. It never launches a duplicate native child.
    [[nodiscard]] auto reserve_external(SourceLaunchRole, Resources,
        std::optional<Resources> serial_bootstrap = std::nullopt) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto adopt_external(SourceLaunchRole, long actual_pid) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto confirm_external_terminal(SourceLaunchRole, int actual_pid,
        int actual_wait_status) noexcept -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto retire_serial_bootstrap() noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto request_group_stop(SourceLaunchRole, ProcessSignal) noexcept
        -> std::expected<void, SourceLaunchReason>;
    // After the actual controller has been waited, this native subreaper owns
    // its orphan descendants. One wait per call; ECHILD certifies the terminal
    // process graph, never a signal or an elapsed timer. Unknown children are
    // reaped but make the attempt fail rather than qualifying extra roles.
    [[nodiscard]] auto reap_orphans() noexcept -> std::expected<bool, SourceLaunchReason>;
    // One actual nonblocking wait. EINTR is a retained fact, not an implicit
    // retry or a fresh cleanup deadline. An actual terminal wait precedes close
    // and lease retirement. Errors retain this same slot/lease.
    [[nodiscard]] auto observe_terminal(SourceLaunchRole) noexcept
        -> std::expected<bool, SourceLaunchReason>;
    [[nodiscard]] auto request_stop(SourceLaunchRole, ProcessSignal) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto retire(SourceLaunchRole) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto close_descriptor(SourceLaunchRole, int) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto slot(SourceLaunchRole) const noexcept -> SourceLaunchSlotView;
    [[nodiscard]] auto descriptors(SourceLaunchRole) const noexcept
        -> std::span<const SourceLaunchDescriptorView>;
    [[nodiscard]] auto primary() const noexcept -> const std::optional<SourceLaunchFact>&;
    [[nodiscard]] auto secondary() const noexcept -> std::span<const SourceLaunchFact>;
    [[nodiscard]] auto has_owned_resources() const noexcept -> bool;
    [[nodiscard]] auto uncertain() const noexcept -> bool;
    [[nodiscard]] auto origin_ns() const noexcept -> std::uint64_t { return phase_origin_ns_; }
    [[nodiscard]] auto phase_end_ns() const noexcept -> std::uint64_t { return phase_end_ns_; }

  private:
    struct Slot final {
        SourceLaunchSlotView view;
        std::optional<ResourceLease> lease;
        std::optional<ResourceLease> bootstrap_lease;
        bool external{};
        std::array<SourceLaunchDescriptorView, descriptor_capacity> descriptors{};
        std::size_t descriptor_count{};
    };
    [[nodiscard]] auto check_guard(bool retirement) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto refuse(SourceLaunchReason, SourceLaunchOperation,
                              SourceLaunchRole, detail::RawStatusResult = {}, int descriptor = -1) noexcept
        -> std::unexpected<SourceLaunchReason>;
    [[nodiscard]] auto acquire_pair(SourceLaunchRole, std::array<int, 2>&, bool gate,
                                   std::span<const int> borrowed) noexcept
        -> std::expected<void, SourceLaunchReason>;
    [[nodiscard]] auto allowed(SourceLaunchRole) const noexcept -> bool;
    void retain_fact(SourceLaunchFact) noexcept;
    ResourceLedger* ledger_;
    GuardOperations guard_;
    SourceLaunchOperations operations_;
    const SourceLaunchChildFailure* current_clock_;
    const std::uint64_t invocation_origin_ns_, campaign_end_ns_;
    std::uint64_t phase_origin_ns_, phase_end_ns_;
    std::uint64_t last_ns_, cleanup_end_ns_{};
    SourceLaunchStage stage_{SourceLaunchStage::startup};
    SourceLaunchPhase phase_{SourceLaunchPhase::q5};
    std::uint64_t phase_generation_{1};
    std::optional<SourceLaunchDeadline> launch_deadline_;
    SourceLaunchRole launch_role_{};
    bool inherited_controller_launch_{};
    std::array<Slot, role_count> slots_{};
    std::optional<SourceLaunchFact> primary_;
    std::array<SourceLaunchFact, failure_capacity> secondary_{};
    std::size_t secondary_count_{}, native_calls_{};
    std::optional<ResourceLease> receive_lease_;
    SourceLaunchChannelOperations receive_operations_;
    detail::TopologySourceOperations directory_operations_;
    std::array<int, descriptor_capacity> receive_borrowed_{};
    std::size_t receive_borrowed_count_{};
    std::array<int, 16> received_{};
    std::array<detail::RawStatusResult, 16> received_close_{};
    std::array<bool, 16> received_consumed_{};
    std::size_t received_count_{};
    int directory_descriptor_{-1};
    long directory_raw_descriptor_{-1};
    bool directory_consumed_{}, receive_uncertain_{};
    std::array<char, 4096> directory_bytes_{};
    bool valid_{}, uncertain_{};
};

[[nodiscard]] auto production_source_launch_operations() noexcept -> SourceLaunchOperations;
[[nodiscard]] auto production_source_launch_channel_operations() noexcept
    -> SourceLaunchChannelOperations;

} // namespace epycbench::platform_linux::host_work
