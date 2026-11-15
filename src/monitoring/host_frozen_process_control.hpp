#pragma once
#include "platform_linux/topology_source_io.hpp"

#include "monitoring/host_frozen_collection.hpp"
#include "monitoring/host_frozen_source_admission.hpp"
#include "monitoring/host_q2_native_thread_owner.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "platform_linux/host_frozen_control_preparation.hpp"
#include "platform_linux/host_work_admission.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::monitoring::qualification::detail {
enum class FrozenProcessType : unsigned char {
    ready=1, begin, permit, cell, failure, ack, cancel, terminal, grant, finish
};
enum class FrozenProcessReason : unsigned char {
    malformed, capacity, sequence, state, progress, eof, io, exception, clock, deadline, cancelled
};
struct FrozenProcessRecord final {
    FrozenProcessType type{};
    std::uint8_t phase{};
    std::uint32_t sequence{}, ordinal{};
    std::uint16_t comparison{}, block{};
    std::uint64_t deadline_ns{}, payload_bytes{};
    std::array<std::byte,32> commitment{};
    auto operator==(const FrozenProcessRecord&) const -> bool = default;
};
using FrozenProcessBytes = std::array<std::byte,64>;
[[nodiscard]] auto encode_frozen_process_record(const FrozenProcessRecord&) noexcept
    -> std::expected<FrozenProcessBytes,FrozenProcessReason>;
[[nodiscard]] auto decode_frozen_process_record(std::span<const std::byte>) noexcept
    -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
[[nodiscard]] auto frozen_process_grant(std::uint8_t phase, std::size_t workers,
    std::uint64_t deadline) noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
[[nodiscard]] auto check_frozen_process_grant(const FrozenProcessRecord&,std::uint8_t phase,
    std::size_t actual_workers) noexcept
    -> std::expected<platform_linux::host_work::Resources,FrozenProcessReason>;

enum class FrozenProcessTransferKind : unsigned char { progress, interrupted, backpressure, eof, failure };
struct FrozenProcessTransfer final { FrozenProcessTransferKind kind; std::size_t bytes{}; };
struct FrozenProcessIO final {
    void* context{};
    FrozenProcessTransfer (*read)(void*,std::span<std::byte>){};
    FrozenProcessTransfer (*write)(void*,std::span<const std::byte>){};
    bool (*guard)(void*) noexcept{};
};
// One fixed frame. Refusal permanently consumes it; no buffer growth or retry loop.
class FrozenProcessCursor final {
  public:
    FrozenProcessCursor() noexcept = default;
    explicit FrozenProcessCursor(FrozenProcessBytes bytes) noexcept : bytes_{bytes}, sending_{true} {}
    FrozenProcessCursor(FrozenProcessCursor&&) noexcept;
    FrozenProcessCursor(const FrozenProcessCursor&)=delete;
    [[nodiscard]] auto step(FrozenProcessIO) noexcept -> std::expected<bool,FrozenProcessReason>;
    [[nodiscard]] auto record() const noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
  private:
    FrozenProcessBytes bytes_{};
    std::size_t offset_{};
    bool sending_{}, terminal_{};
};

// Child control state consumes one grant and one actual outstanding cell at a time.
class FrozenProcessChild final {
  public:
    FrozenProcessChild(std::uint8_t phase,std::size_t workers) noexcept : phase_{phase},workers_{workers} {}
    FrozenProcessChild(FrozenProcessChild&&) noexcept;
    FrozenProcessChild(const FrozenProcessChild&)=delete;
    [[nodiscard]] auto grant(const FrozenProcessRecord&) noexcept
        -> std::expected<platform_linux::host_work::Resources,FrozenProcessReason>;
    [[nodiscard]] auto ready() noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    [[nodiscard]] auto begin() noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    [[nodiscard]] auto permit(const FrozenProcessRecord&) noexcept -> std::expected<void,FrozenProcessReason>;
    [[nodiscard]] auto payload(FrozenProcessType,std::uint64_t,std::array<std::byte,32>) noexcept
        -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    [[nodiscard]] auto acknowledge(const FrozenProcessRecord&) noexcept -> std::expected<void,FrozenProcessReason>;
    [[nodiscard]] auto consume(std::size_t actual_consumed) noexcept -> std::expected<void,FrozenProcessReason>;
    [[nodiscard]] auto finish(std::uint64_t,std::array<std::byte,32>) noexcept
        -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    [[nodiscard]] auto terminal(std::size_t actual_consumed) noexcept
        -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    void cancel() noexcept { state_=State::dead; }
  private:
    enum class State : unsigned char { initial, granted, idle, permission, observing, acknowledgment, consumed, finishing, finished, dead };
    [[nodiscard]] auto make(FrozenProcessType) noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason>;
    [[nodiscard]] auto refuse(FrozenProcessReason) noexcept -> std::unexpected<FrozenProcessReason>;
    State state_{State::initial};
    std::uint8_t phase_{};
    std::size_t workers_{};
    std::uint32_t output_sequence_{},input_sequence_{},ordinal_{};
    std::uint64_t phase_deadline_{},deadline_{};
    FrozenProcessRecord outstanding_{};
    friend struct FrozenSourceControlState;
    friend class FrozenSourceControlCapability;
    friend class FrozenSourceControlOwner;
};

struct FrozenSourceControlState;
class FrozenSourceControlTestAccess;
enum class FrozenSourceControlReason : unsigned char {
    invalid_input, construction, capacity, native, session, placement, deadline, cancelled,
    protocol, busy, mailbox, collection, retirement
};
enum class FrozenSourceControlStatus : unsigned char { idle, pending, complete, failed, retired };
struct FrozenSourceControlNativeFacts final {
    std::optional<platform_linux::host_work::ControlFailure> main_clock, control_clock;
    std::optional<MonitoringQ2CompactDiagnostic> reobservation_wait, command_wait;
};
struct FrozenSourceControlDiagnostics final {
    std::optional<platform_linux::host_work::ControlFailure> original_main, control;
    std::span<const platform_linux::host_work::ControlFailure> linux_cleanup;
    const MonitoringQ2NativeThreadCreateFailure* creation{};
    const MonitoringQ2CompactLedger<8>* thread_cleanup{};
    bool cleanup_attempts_exhausted{};
    FrozenSourceControlNativeFacts native_facts;
};
struct FrozenSourceControlFailureProbe final {
    FrozenSourceControlNativeFacts native_facts;
    MonitoringQ2CompactLedger<8> cleanup;
    std::optional<FrozenSourceControlReason> primary;
};
enum class FrozenSourceControlFailureProbeFirst : unsigned char {
    main_clock, control_clock, reobservation_wait, command_wait, session, regression
};
// A borrow of the actual stable owner, never caller-created placement/deadline
// callbacks. Collection uses it only on the actual owning control thread.
class FrozenSourceControlCapability final {
  public:
    FrozenSourceControlCapability(const FrozenSourceControlCapability&) = delete;
    [[nodiscard]] auto phase_deadline(const HostFrozenSourceInputs&, HostFrozenPhase,
        runtime::RunnerCancellation&) const noexcept -> std::expected<std::uint64_t, HostFrozenCollectionReason>;
    [[nodiscard]] auto before_prepare(const HostFrozenSourceInputs&, const HostFrozenCellIdentity&,
        runtime::RunnerCancellation&) const noexcept -> std::expected<void, HostFrozenCollectionReason>;
    [[nodiscard]] auto before_activate(const HostFrozenSourceInputs&, const HostFrozenCellIdentity&,
        runtime::RunnerCancellation&) const noexcept -> std::expected<void, HostFrozenCollectionReason>;
    [[nodiscard]] auto permit_deadline() const noexcept -> std::uint64_t;
  private:
    explicit FrozenSourceControlCapability(FrozenSourceControlState* state) noexcept : state_{state} {}
    FrozenSourceControlState* state_{};
    friend struct FrozenSourceControlState;
    [[nodiscard]] auto phase_deadline_owned(const HostFrozenSourceInputs&, HostFrozenPhase,
        runtime::RunnerCancellation&) const noexcept -> std::expected<std::uint64_t, HostFrozenCollectionReason>;
    [[nodiscard]] auto before_activate_owned(const HostFrozenSourceInputs&, const HostFrozenCellIdentity&,
        runtime::RunnerCancellation&) const noexcept -> std::expected<void, HostFrozenCollectionReason>;
    [[nodiscard]] auto before_prepare_owned(const HostFrozenSourceInputs&, const HostFrozenCellIdentity&,
        runtime::RunnerCancellation&) const noexcept -> std::expected<void, HostFrozenCollectionReason>;
    friend class HostFrozenCollection;
    friend struct HostFrozenCollectionState;
};
// Main keeps this owner alive while servicing requests. Fallible retirement is
// non-consuming: failures retain the same State, recovery owner and reservations.
struct FrozenSourceLinuxIOProbe;
struct FrozenSourceInitialCallProbe;
enum class FrozenSourceInitialCallProbeMode : unsigned char;
struct FrozenSourceRegistrationProbe;
enum class FrozenSourceRegistrationProbeMode : unsigned char;
class FrozenSourceControlOwner final {
  public:
    FrozenSourceControlOwner(FrozenSourceControlOwner&&) noexcept;
    FrozenSourceControlOwner(const FrozenSourceControlOwner&) = delete;
    auto operator=(FrozenSourceControlOwner&&) -> FrozenSourceControlOwner& = delete;
    ~FrozenSourceControlOwner() noexcept;
    [[nodiscard]] static auto acquire_on_original_main(MonitoringHostInputTemplateAuthority,
        std::uint8_t phase, std::size_t workers, const FrozenProcessRecord& original_grant,
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
        std::size_t affinity_capacity,
        const platform_linux::detail::TopologySourceLimits* source_limits = nullptr) noexcept
        -> std::expected<FrozenSourceControlOwner, FrozenSourceControlReason>;
    [[nodiscard]] static auto
    acquire_source_on_original_main(HostFrozenSourceAdmission, MonitoringHostInputTemplateAuthority,
                                    runtime::ProductionRequestIdSource&,
                                    const HostFrozenSourceLaunchBinding&, std::uint8_t phase,
                                    std::size_t workers, const FrozenProcessRecord& original_grant,
                                    platform_linux::host_work::ResourceLedger&,
                                    runtime::RunnerCancellation&, std::size_t affinity_capacity,
                                    const platform_linux::detail::TopologySourceLimits&) noexcept
        -> std::expected<FrozenSourceControlOwner, FrozenSourceControlReason>;
    [[nodiscard]] auto start_source_control(HostFrozenCollectionDependencies) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto session() const noexcept -> const RunnerContextAcquisitionSession*;
    [[nodiscard]] auto start_control(HostFrozenSourceInputs, HostFrozenCollectionDependencies) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    // Enqueue operations never block main. Borrowed output ends at the next
    // enqueue/retirement, before control is allowed to mutate or destroy it.
    [[nodiscard]] auto enqueue_prepare() noexcept -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto ready() noexcept -> std::expected<FrozenProcessRecord, FrozenSourceControlReason>;
    [[nodiscard]] auto begin() noexcept -> std::expected<FrozenProcessRecord, FrozenSourceControlReason>;
    [[nodiscard]] auto enqueue_next(const FrozenProcessRecord& permit) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto payload(FrozenProcessType, std::uint64_t, std::array<std::byte,32>) noexcept
        -> std::expected<FrozenProcessRecord, FrozenSourceControlReason>;
    [[nodiscard]] auto enqueue_consume(const FrozenProcessRecord& ack) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto snapshot() const noexcept -> std::optional<HostFrozenCollectionSnapshot>;
    [[nodiscard]] auto finish(std::uint64_t, std::array<std::byte,32>) noexcept
        -> std::expected<FrozenProcessRecord, FrozenSourceControlReason>;
    [[nodiscard]] auto finish_acknowledgment(const FrozenProcessRecord&) noexcept
        -> std::expected<FrozenProcessRecord, FrozenSourceControlReason>;
    [[nodiscard]] auto service_once() noexcept -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto status() const noexcept -> FrozenSourceControlStatus;
    [[nodiscard]] auto cell() const noexcept -> const HostFrozenCell*;
    // Original-main borrow for untimed FINISH/failure serialization. It ends
    // before owner retirement and cannot observe an executing command or Run.
    [[nodiscard]] auto quiescent_collection() const noexcept -> const HostFrozenCollection*;
    // Genuine completed-phase input owner. The original main may encode its
    // owning producer packet only after FINISH acknowledgment, all cells have
    // been consumed, and no mailbox/Run/failure remains. The borrow ends before
    // this owner's retirement/destruction; a packet is not native authority.
    [[nodiscard]] auto source_admission() const noexcept -> const HostFrozenSourceAdmission*;
    [[nodiscard]] auto collection_failure() const noexcept -> const HostFrozenCollectionFailure*;
    [[nodiscard]] auto failure_reason() const noexcept -> std::optional<FrozenSourceControlReason>;
    [[nodiscard]] auto diagnostics() const noexcept -> std::optional<FrozenSourceControlDiagnostics>;
    [[nodiscard]] auto raw_session_failure() const noexcept -> const core::Error*;
    // A published provider cause after terminal timeout remains separate from
    // the immutable primary and borrowed until this owner's destruction.
    [[nodiscard]] auto raw_late_session_failure() const noexcept -> const core::Error*;
    [[nodiscard]] auto raw_linux_source_failure() const noexcept
        -> const platform_linux::detail::TopologySourceFailure*;
    [[nodiscard]] auto raw_linux_source_cleanup() const noexcept
        -> std::span<const platform_linux::detail::TopologySourceCloseFact>;
    [[nodiscard]] auto retire(MonitoringQ2RawDeadline) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
  private:
    [[nodiscard]] auto start_owned_synthetic_for_testing(
        std::span<const HostFrozenCollection::Endpoint>, HostFrozenCollectionDependencies,
        core::LinuxCpuId actual_allowed_control_cpu) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto collection_inputs_for_testing() const noexcept
        -> std::optional<HostFrozenSourceInputs>;
    [[nodiscard]] static auto
    acquire_impl(MonitoringHostInputTemplateAuthority,
                 std::optional<RunnerContextAcquisitionOperations>, std::uint8_t phase,
                 std::size_t workers, const FrozenProcessRecord&,
                 platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
                 std::size_t affinity_capacity,
                 const platform_linux::detail::TopologySourceLimits* source_limits = nullptr,
                 std::optional<HostFrozenSourceAdmission> = std::nullopt,
                 runtime::ProductionRequestIdSource* = nullptr,
                 const HostFrozenSourceLaunchBinding* = nullptr) noexcept
        -> std::expected<FrozenSourceControlOwner, FrozenSourceControlReason>;
    [[nodiscard]] auto enqueue_probe(const FrozenProcessRecord&) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] auto probe_result() const noexcept -> std::optional<HostFrozenCollectionReason>;
    [[nodiscard]] auto source_refused() const noexcept -> bool;
    [[nodiscard]] auto bind_main_failure_sink_for_testing(std::optional<core::Error>**) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] static auto linux_io_publication_for_testing(platform_linux::detail::PreparedTopologySourceIO,
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&, void*,
        void (*)(void*, platform_linux::detail::PreparedTopologySourceIO&, std::optional<core::Error>&),
        bool (*)(void*) noexcept, void (*)(void*, bool) noexcept) noexcept
        -> std::expected<FrozenSourceLinuxIOProbe, FrozenSourceControlReason>;
    [[nodiscard]] static auto initial_call_for_testing(FrozenSourceInitialCallProbeMode, core::Error,
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&, void*,
        void (*)(void*, bool) noexcept) noexcept
        -> std::expected<FrozenSourceInitialCallProbe, FrozenSourceControlReason>;
    [[nodiscard]] static auto registration_for_testing(
        FrozenSourceRegistrationProbeMode, MonitoringHostInputTemplateAuthority,
        RunnerContextAcquisitionOperations, const void*, platform_linux::host_work::ResourceLedger&,
        runtime::RunnerCancellation&, void*, void (*)(void*, bool) noexcept) noexcept
        -> std::expected<FrozenSourceRegistrationProbe, FrozenSourceControlReason>;
    [[nodiscard]] static auto
    prepare_state_storage_for_testing(platform_linux::host_work::ResourceLedger&,
                                      runtime::RunnerCancellation&) noexcept
        -> std::expected<void, FrozenSourceControlReason>;
    [[nodiscard]] static auto retain_failure_facts_for_testing(
        platform_linux::host_work::ResourceLedger&, runtime::RunnerCancellation&,
        FrozenSourceControlFailureProbeFirst, void*, void (*)(void*, bool) noexcept) noexcept
        -> std::expected<FrozenSourceControlFailureProbe, FrozenSourceControlReason>;
    explicit FrozenSourceControlOwner(std::unique_ptr<FrozenSourceControlState>,
        platform_linux::host_work::ResourceLease) noexcept;
    std::unique_ptr<FrozenSourceControlState> state_;
    std::optional<platform_linux::host_work::ResourceLease> state_lease_;
    friend class FrozenSourceControlTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
