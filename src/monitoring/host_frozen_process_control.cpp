#include "monitoring/host_frozen_process_control.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_frozen_process_control_test_access.hpp"
#include "monitoring/host_frozen_source_composition.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_environment_access.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "platform_linux/host_frozen_control_preparation.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
void put(std::span<std::byte> out,std::uint64_t value) noexcept {
    for(std::size_t i=out.size();i;--i) { out[i-1]=static_cast<std::byte>(value&255U);value>>=8U; }
}
auto get(std::span<const std::byte> in) noexcept -> std::uint64_t {
    std::uint64_t value{};for(auto b:in)value=(value<<8U)|std::to_integer<unsigned>(b);return value;
}
auto valid(const FrozenProcessRecord& r) noexcept -> bool {
    const auto type=static_cast<unsigned>(r.type);
    if(type<1U || type>10U || (r.phase!=5U && r.phase!=6U) || r.sequence==UINT32_MAX)return false;
    if(r.ordinal>(r.phase==5U ? 8U:384U) || r.comparison>=8U || r.block>=12U)return false;
    if((r.type==FrozenProcessType::ready || r.type==FrozenProcessType::terminal || r.type==FrozenProcessType::cancel || r.type==FrozenProcessType::finish) && (r.comparison || r.block))return false;
    if(r.type!=FrozenProcessType::permit && r.type!=FrozenProcessType::grant && r.deadline_ns)return false;
    if(r.type==FrozenProcessType::cell || r.type==FrozenProcessType::failure || r.type==FrozenProcessType::finish)
        return r.payload_bytes>0U && r.payload_bytes<=UINT64_C(67108864);
    if(r.type==FrozenProcessType::ack)return r.payload_bytes>0U && r.payload_bytes<=UINT64_C(67108864);
    if(r.payload_bytes)return false;
    if(r.type==FrozenProcessType::grant)return r.sequence==0U && !r.ordinal && !r.comparison && !r.block && r.deadline_ns;
    if(std::any_of(r.commitment.begin(),r.commitment.end(),[](auto b){return b!=std::byte{};}))return false;
    return (r.type==FrozenProcessType::permit) ? r.deadline_ns!=0U : r.deadline_ns==0U;
}
}
auto encode_frozen_process_record(const FrozenProcessRecord& r) noexcept
    -> std::expected<FrozenProcessBytes,FrozenProcessReason> {
    if(!valid(r))return std::unexpected{FrozenProcessReason::malformed};
    FrozenProcessBytes bytes{};bytes[0]=std::byte{1};bytes[1]=static_cast<std::byte>(r.type);bytes[2]=static_cast<std::byte>(r.phase);
    auto b=std::span{bytes};put(b.subspan(4,4),r.sequence);put(b.subspan(8,4),r.ordinal);
    put(b.subspan(12,2),r.comparison);put(b.subspan(14,2),r.block);put(b.subspan(16,8),r.deadline_ns);
    put(b.subspan(24,8),r.payload_bytes);std::copy(r.commitment.begin(),r.commitment.end(),bytes.begin()+32);return bytes;
}
auto decode_frozen_process_record(std::span<const std::byte> b) noexcept
    -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(b.size()!=64U || b[0]!=std::byte{1} || b[3]!=std::byte{})return std::unexpected{FrozenProcessReason::malformed};
    FrozenProcessRecord r{static_cast<FrozenProcessType>(b[1]),std::to_integer<std::uint8_t>(b[2]),
        static_cast<std::uint32_t>(get(b.subspan(4,4))),static_cast<std::uint32_t>(get(b.subspan(8,4))),
        static_cast<std::uint16_t>(get(b.subspan(12,2))),static_cast<std::uint16_t>(get(b.subspan(14,2))),
        get(b.subspan(16,8)),get(b.subspan(24,8)),{}};
    std::copy(b.begin()+32,b.end(),r.commitment.begin());if(!valid(r))return std::unexpected{FrozenProcessReason::malformed};return r;
}
auto frozen_process_grant(std::uint8_t phase,std::size_t workers,std::uint64_t deadline) noexcept
    -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(workers<1U || workers>8U || !deadline || (phase!=5U && phase!=6U))return std::unexpected{FrozenProcessReason::capacity};
    FrozenProcessRecord r{FrozenProcessType::grant,phase,0,0,0,0,deadline,0,{}};
    const std::array<std::uint32_t,5> quota{1U,static_cast<std::uint32_t>(workers+3U),static_cast<std::uint32_t>(workers),2U,0U};
    for(std::size_t i{};i<quota.size();++i) put(std::span{r.commitment}.subspan(i*4U,4U),quota[i]);
    return r;
}
auto check_frozen_process_grant(const FrozenProcessRecord& r,std::uint8_t phase,std::size_t workers) noexcept
    -> std::expected<platform_linux::host_work::Resources,FrozenProcessReason> {
    auto expected=frozen_process_grant(phase,workers,r.deadline_ns);
    if(!expected || r!=*expected)return std::unexpected{FrozenProcessReason::malformed};
    return platform_linux::host_work::Resources{1U,workers+3U,workers,2U,0U};
}
FrozenProcessCursor::FrozenProcessCursor(FrozenProcessCursor&& other) noexcept
    :bytes_{other.bytes_},offset_{other.offset_},sending_{other.sending_},terminal_{other.terminal_}{other.terminal_=true;}
auto FrozenProcessCursor::step(FrozenProcessIO io) noexcept -> std::expected<bool,FrozenProcessReason> {
    const auto fail=[&](FrozenProcessReason r)->std::expected<bool,FrozenProcessReason>{terminal_=true;return std::unexpected{r};};
    if(terminal_)return fail(FrozenProcessReason::state);
    if(!io.guard || (sending_ ? !io.write:!io.read))return fail(FrozenProcessReason::io);
    if(!io.guard(io.context))return fail(FrozenProcessReason::deadline);
    if(offset_==bytes_.size())return true;
    FrozenProcessTransfer result{};
    try { result=sending_ ? io.write(io.context,std::span<const std::byte>{bytes_}.subspan(offset_))
                         : io.read(io.context,std::span{bytes_}.subspan(offset_)); }
    catch(...) { return fail(FrozenProcessReason::exception); }
    if(result.kind==FrozenProcessTransferKind::progress) {
        if(!result.bytes || result.bytes>bytes_.size()-offset_)return fail(FrozenProcessReason::progress);
        offset_+=result.bytes;
    } else if(result.bytes)return fail(FrozenProcessReason::progress);
    else if(result.kind==FrozenProcessTransferKind::eof)return fail(FrozenProcessReason::eof);
    else if(result.kind==FrozenProcessTransferKind::failure)return fail(FrozenProcessReason::io);
    if(!io.guard(io.context))return fail(FrozenProcessReason::deadline);
    return offset_==bytes_.size();
}
auto FrozenProcessCursor::record() const noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(terminal_ || offset_!=bytes_.size())return std::unexpected{FrozenProcessReason::state};
    return decode_frozen_process_record(bytes_);
}
FrozenProcessChild::FrozenProcessChild(FrozenProcessChild&& other) noexcept
    :state_{other.state_},phase_{other.phase_},workers_{other.workers_},output_sequence_{other.output_sequence_},
     input_sequence_{other.input_sequence_},ordinal_{other.ordinal_},phase_deadline_{other.phase_deadline_},deadline_{other.deadline_},outstanding_{other.outstanding_}{other.state_=State::dead;}
auto FrozenProcessChild::refuse(FrozenProcessReason r) noexcept -> std::unexpected<FrozenProcessReason>{state_=State::dead;return std::unexpected{r};}
auto FrozenProcessChild::make(FrozenProcessType type) noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(output_sequence_==UINT32_MAX)return refuse(FrozenProcessReason::sequence);
    const bool position=type==FrozenProcessType::begin || type==FrozenProcessType::cell || type==FrozenProcessType::failure;
    return FrozenProcessRecord{type,phase_,output_sequence_++,ordinal_,
        static_cast<std::uint16_t>(!position ? 0U:phase_==5U ? std::min(ordinal_,7U):std::min(ordinal_/48U,7U)),
        static_cast<std::uint16_t>(!position || phase_==5U ? 0U:(ordinal_%48U)/4U),0,0,{}};
}
auto FrozenProcessChild::grant(const FrozenProcessRecord& r) noexcept
    -> std::expected<platform_linux::host_work::Resources,FrozenProcessReason> {
    if(state_!=State::initial)return refuse(FrozenProcessReason::state);
    auto quota=check_frozen_process_grant(r,phase_,workers_);if(!quota)return refuse(quota.error());
    phase_deadline_=r.deadline_ns;input_sequence_=1U;state_=State::granted;return quota;
}
auto FrozenProcessChild::ready() noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(state_!=State::granted)return refuse(FrozenProcessReason::state);
    state_=State::idle;return make(FrozenProcessType::ready);
}
auto FrozenProcessChild::begin() noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(state_!=State::idle || ordinal_>=(phase_==5U ? 8U:384U))return refuse(FrozenProcessReason::state);
    state_=State::permission;return make(FrozenProcessType::begin);
}
auto FrozenProcessChild::permit(const FrozenProcessRecord& r) noexcept -> std::expected<void,FrozenProcessReason> {
    if(state_!=State::permission || r.type!=FrozenProcessType::permit || r.phase!=phase_ ||
       r.sequence!=input_sequence_ || r.ordinal!=ordinal_ || !valid(r) ||
       r.comparison!=(phase_==5U ? ordinal_:ordinal_/48U) || r.block!=(phase_==5U ? 0U:(ordinal_%48U)/4U) ||
       r.deadline_ns>phase_deadline_)return refuse(FrozenProcessReason::state);
    ++input_sequence_;deadline_=r.deadline_ns;state_=State::observing;return {};
}
auto FrozenProcessChild::payload(FrozenProcessType type,std::uint64_t size,std::array<std::byte,32> hash) noexcept
    -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(state_!=State::observing || (type!=FrozenProcessType::cell && type!=FrozenProcessType::failure) || !size || size>UINT64_C(67108864))return refuse(FrozenProcessReason::state);
    auto record=make(type);if(!record)return record;record->payload_bytes=size;record->commitment=hash;
    outstanding_=*record;state_=State::acknowledgment;return record;
}
auto FrozenProcessChild::acknowledge(const FrozenProcessRecord& r) noexcept -> std::expected<void,FrozenProcessReason> {
    auto expected=outstanding_;expected.type=FrozenProcessType::ack;expected.sequence=input_sequence_;
    if((state_!=State::acknowledgment && state_!=State::finishing) || r!=expected)return refuse(FrozenProcessReason::state);
    ++input_sequence_;state_=state_==State::finishing ? State::finished:State::consumed;return {};
}
auto FrozenProcessChild::consume(std::size_t actual) noexcept -> std::expected<void,FrozenProcessReason> {
    if(state_!=State::consumed || outstanding_.type!=FrozenProcessType::cell || actual!=ordinal_+1U)return refuse(FrozenProcessReason::state);
    ++ordinal_;state_=State::idle;return {};
}
auto FrozenProcessChild::finish(std::uint64_t size,std::array<std::byte,32> hash) noexcept
    -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if((state_!=State::idle && !(state_==State::consumed && outstanding_.type==FrozenProcessType::failure)) || !size || size>UINT64_C(67108864))return refuse(FrozenProcessReason::state);
    auto r=make(FrozenProcessType::finish);if(!r)return r;r->payload_bytes=size;r->commitment=hash;outstanding_=*r;state_=State::finishing;return r;
}
auto FrozenProcessChild::terminal(std::size_t actual) noexcept -> std::expected<FrozenProcessRecord,FrozenProcessReason> {
    if(state_!=State::finished || actual!=ordinal_)return refuse(FrozenProcessReason::state);
    state_=State::dead;return make(FrozenProcessType::terminal);
}

namespace {
namespace work = platform_linux::host_work;
using Access = detail_RunnerContextAcquisitionAccess;
using ControlReason = FrozenSourceControlReason;
constexpr std::uint32_t idle = 0U, requested = 1U, executing = 2U, completed = 3U;
constexpr std::uint64_t wait_slice = UINT64_C(10000000);
auto control_stock(const char* classification) -> core::Error {
    core::Error result{.category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.host_source.control_failed"},
        .message = "Frozen source control failed", .operation = "frozen_source_control",
        .os_error = std::nullopt, .context = nullptr,
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command};
    core::detail::JsonPointerCleanup cleanup{&result.context};
    result.context = nlohmann::json::object();
    result.context["classification"] = classification;
    cleanup.value = nullptr;
    return result;
}
auto session_stock(bool revalidation, bool unknown) -> core::Error {
    core::Error result{.category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{"monitoring.host_environment.contained_failure"},
        .message = revalidation ? "Monitoring host revalidation failed"
                                : "Monitoring host prerequisite acquisition failed",
        .operation = revalidation ? "revalidate_host_environment" : "acquire_runner_context_session",
        .os_error = std::nullopt, .context = nullptr,
        .severity = core::ErrorSeverity::error, .continuation = core::ContinuationPolicy::abort_command};
    core::detail::JsonPointerCleanup cleanup{&result.context};
    result.context = nlohmann::json::object();
    result.context["classification"] = unknown ? "contained_unknown_failure" : "contained_exception";
    cleanup.value = nullptr;
    return result;
}
auto composition_stock(HostFrozenCompositionStage stage, bool unknown) -> core::Error {
    auto result = control_stock(unknown ? "contained_unknown_failure" : "contained_exception");
    core::detail::JsonCleanup cleanup{result.context};
    switch (stage) {
    case HostFrozenCompositionStage::environment:
        result.code = core::StableErrorCode{"monitoring.host_environment.contained_failure"};
        result.operation = "derive_host_environment";
        result.message = "Host environment derivation failed";
        break;
    case HostFrozenCompositionStage::plan:
        result.code = core::StableErrorCode{"monitoring.host_environment.contained_failure"};
        result.operation = "resolve_monitoring_host_qualification_plan";
        result.message = "Monitoring host qualification plan resolution failed";
        break;
    case HostFrozenCompositionStage::registration:
        result.code = core::StableErrorCode{"monitoring.host_environment.contained_failure"};
        result.operation = "register_selected_resources";
        result.message = "Monitoring selected-resource registration failed";
        break;
    case HostFrozenCompositionStage::calibration:
        result.code =
            core::StableErrorCode{"monitoring.host_qualification_composition.contained_failure"};
        result.operation = "compose_host_qualification_calibration_inputs";
        result.message = "Monitoring host qualification composition binding failed";
        break;
    }
    return core::Error(std::move(result));
}
struct RetiredControlStock final {
    core::Error value;
    ~RetiredControlStock() noexcept { core::detail::retire_json(value.context); }
};
void retire_error(std::optional<core::Error>& error) noexcept {
    if (error) core::detail::retire_json(error->context);
    error.reset();
}
void retire_observation(std::optional<std::expected<detail_HostRevalidationObservation,
                        detail_HostObservationFailure>>& result) noexcept {
    if (result && !*result && result->error().underlying)
        core::detail::retire_json(result->error().underlying->context);
    result.reset();
}
} // namespace

struct FrozenSourceControlState final {
    enum class Command : unsigned char { prepare, next, consume, probe };
    FrozenSourceControlState(std::uint8_t phase, std::size_t workers,
                             work::ResourceLedger& resources, runtime::RunnerCancellation& cancel,
                             bool source)
        : protocol{phase, workers}, ledger{resources}, cancellation{cancel}, capability{this},
          production{source}, main_stock{control_stock("original_main")},
          wait_stock{control_stock("main_mailbox")},
          acquisition_guard_stock{control_stock("source_acquisition_guard")},
          composition_guard_stock{control_stock("source_composition_guard")},
          preparation_stock{control_stock("control_preparation")},
          acquisition_stock{session_stock(false, false)},
          acquisition_unknown_stock{session_stock(false, true)},
          revalidation_stock{session_stock(true, false)},
          revalidation_unknown_stock{session_stock(true, true)},
          environment_stock{composition_stock(HostFrozenCompositionStage::environment, false)},
          environment_unknown_stock{
              composition_stock(HostFrozenCompositionStage::environment, true)},
          plan_stock{composition_stock(HostFrozenCompositionStage::plan, false)},
          plan_unknown_stock{composition_stock(HostFrozenCompositionStage::plan, true)},
          registration_stock{composition_stock(HostFrozenCompositionStage::registration, false)},
          registration_unknown_stock{
              composition_stock(HostFrozenCompositionStage::registration, true)},
          calibration_stock{composition_stock(HostFrozenCompositionStage::calibration, false)},
          calibration_unknown_stock{
              composition_stock(HostFrozenCompositionStage::calibration, true)} {}
    ~FrozenSourceControlState() noexcept {
        if (thread || !native.retired()) std::terminate();
        // All borrows ended and control joined before Session or its operations.
        cell.reset(); collection_failure.reset(); collection.reset();
        inputs.reset();
        source_composition.reset();
        session.reset();
        operations.reset();
        source_admission.reset();
        retire_observation(response);
        retire_error(raw_failure); retire_error(projected_failure); retire_error(main_raw_failure);
        if (create_failure && create_failure->authored_primary)
            core::detail::retire_json(create_failure->authored_primary->context);
        // End the prepared diagnostic contexts before the State storage
        // reservation and the independent Linux IO owner are released.
        const std::array stocks{
            &main_stock, &wait_stock, &acquisition_guard_stock, &composition_guard_stock,
            &preparation_stock, &acquisition_stock, &acquisition_unknown_stock,
            &revalidation_stock, &revalidation_unknown_stock, &environment_stock,
            &environment_unknown_stock, &plan_stock, &plan_unknown_stock,
            &registration_stock, &registration_unknown_stock, &calibration_stock,
            &calibration_unknown_stock};
        for (auto* stock : stocks) core::detail::retire_json(stock->value.context);
        linux_io.reset();
    }
    FrozenProcessChild protocol;
    work::ResourceLedger& ledger;
    runtime::RunnerCancellation& cancellation;
    work::FrozenControlPreparation native;
    FrozenSourceControlCapability capability;
    const bool production;
    std::optional<HostFrozenCollectionReason> probe_failure;
    bool synthetic_source_refused{};
    // Prepared before control launch. Its FD/scratch lease survives Session,
    // response borrows, late publication and join until actual State destruction.
    std::optional<platform_linux::detail::PreparedTopologySourceIO> linux_io;
    const HostFrozenSourceLaunchBinding* source_binding{};
    HostFrozenSourceLaunchClock source_clock;
    std::uint64_t main_invocation_end{}, acquired_request_end{};
    std::atomic<bool> linux_failure_published{};
    std::optional<RunnerContextAcquisitionOperations> operations;
    std::optional<RunnerContextAcquisitionSession> session;
    std::optional<HostFrozenSourceAdmission> source_admission;
    std::optional<HostFrozenSourceComposition> source_composition;
    runtime::ProductionRequestIdSource* source_ids{};
    HostFrozenCompositionStage composition_stage{HostFrozenCompositionStage::environment};
    std::optional<HostFrozenSourceInputs> inputs;
    std::span<const HostFrozenCollection::Endpoint> synthetic_endpoints;
    std::optional<core::LinuxCpuId> synthetic_control_cpu;
    std::optional<HostFrozenCollectionDependencies> dependencies;
    std::optional<MonitoringQ2NativeThreadOwner> thread;
    std::optional<MonitoringQ2NativeThreadCreateFailure> create_failure;
    MonitoringQ2CompactLedger<8> native_cleanup;
    std::optional<HostFrozenCollection> collection;
    std::optional<HostFrozenCell> cell;
    std::optional<HostFrozenCollectionFailure> collection_failure;
    std::optional<core::Error> raw_failure, projected_failure;
    // Original main is the sole writer. Once published, this slot is immutable
    // through response acquisition, abandonment, native join and State destruction.
    std::optional<core::Error> main_raw_failure;
    std::atomic<bool> main_raw_published{};
    bool lower_primary{}, main_response_accepted{}, main_post_checks_observed{};
    RetiredControlStock main_stock, wait_stock, acquisition_guard_stock, composition_guard_stock,
        preparation_stock;
    bool composing{};
    std::size_t main_call_depth{};
    RetiredControlStock acquisition_stock, acquisition_unknown_stock;
    RetiredControlStock revalidation_stock, revalidation_unknown_stock;
    RetiredControlStock environment_stock, environment_unknown_stock, plan_stock,
        plan_unknown_stock;
    RetiredControlStock registration_stock, registration_unknown_stock, calibration_stock,
        calibration_unknown_stock;
    auto take_composition_stock(bool unknown) noexcept -> core::Error {
        switch (composition_stage) {
        case HostFrozenCompositionStage::environment:
            return std::move(unknown ? environment_unknown_stock.value : environment_stock.value);
        case HostFrozenCompositionStage::plan:
            return std::move(unknown ? plan_unknown_stock.value : plan_stock.value);
        case HostFrozenCompositionStage::registration:
            return std::move(unknown ? registration_unknown_stock.value : registration_stock.value);
        case HostFrozenCompositionStage::calibration:
            return std::move(unknown ? calibration_unknown_stock.value : calibration_stock.value);
        }
        std::terminate();
    }
    // State storage lives through abandonment, response publication and join.
    std::atomic<std::uint32_t> mailbox{idle}, commands{idle};
    std::atomic<unsigned char> primary{};
    std::atomic<bool> abandoned{}, entry_done{};
    std::optional<std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure>> response;
    Command command{Command::prepare};
    bool consumption_settled{true}, retired{};
    std::size_t retirement_attempts{};
    std::uint64_t latest_main{}, latest_control{};
    FrozenSourceControlNativeFacts native_facts;
    std::atomic<bool> control_clock_published{}, reobservation_wait_published{}, command_wait_published{};

    // Main's prepared IO and native callbacks share the enclosing main clock
    // history. A semantic regression retains the successful actual syscall
    // pair before returning the checked failure; the next actor-local sample
    // may overwrite this current cell only after its owner copied the cause.
    static auto source_main_now(void* opaque, std::uint64_t* output) noexcept
        -> platform_linux::detail::RawStatusResult {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        auto result = work::source_launch_raw_now(state.source_clock.current, output,
                                                  &state.source_clock.actual);
        if (result.value == 0 && result.error_number == 0) {
            if (*output < state.latest_main) {
                state.source_clock.current.reason = work::SourceLaunchReason::clock;
                state.source_clock.current.operation = work::SourceLaunchChildOperation::clock;
                state.source_clock.current.raw_present = true;
                state.source_clock.current.native_result = state.source_clock.actual.value;
                state.source_clock.current.error_number = state.source_clock.actual.error_number;
                return {-1, ERANGE};
            }
            state.latest_main = *output;
        }
        return result;
    }

    void fail(ControlReason reason) noexcept {
        unsigned char empty{};
        static_cast<void>(primary.compare_exchange_strong(empty,
            static_cast<unsigned char>(static_cast<unsigned char>(reason) + 1U),
            std::memory_order_release, std::memory_order_relaxed));
    }
    auto failed() const noexcept -> std::optional<ControlReason> {
        const auto value = primary.load(std::memory_order_acquire);
        if (!value) return std::nullopt;
        return static_cast<ControlReason>(value - 1U);
    }
    auto sample_guard(std::uint64_t end, bool main,
        std::expected<std::uint64_t, work::ControlFailure> now) noexcept
        -> std::expected<std::uint64_t, ControlReason> {
        const auto retain = [&](work::ControlFailure failure) noexcept {
            auto& slot = main ? native_facts.main_clock : native_facts.control_clock;
            if (!slot) {
                slot.emplace(failure);
                if (!main) control_clock_published.store(true, std::memory_order_release);
            }
            fail(ControlReason::native);
        };
        if (!now) { retain(now.error()); return std::unexpected{ControlReason::native}; }
        auto& latest = main ? latest_main : latest_control;
        if (*now < latest) {
            retain({work::ControlOperation::clock, ERANGE});
            return std::unexpected{ControlReason::native};
        }
        latest = *now;
        if (!end || *now >= end) {
            static_cast<void>(cancellation.try_request(runtime::ipc::CancelReason::controller_timeout));
            fail(ControlReason::deadline); return std::unexpected{ControlReason::deadline};
        }
        if (cancellation.requested()) { fail(ControlReason::cancelled); return std::unexpected{ControlReason::cancelled}; }
        return *now;
    }
    auto guard(std::uint64_t end) noexcept -> std::expected<std::uint64_t, ControlReason> {
        if (cancellation.requested()) { fail(ControlReason::cancelled); return std::unexpected{ControlReason::cancelled}; }
        const bool main = native.on_original_main();
        if (!main && !native.on_control_thread()) { fail(ControlReason::placement); return std::unexpected{ControlReason::placement}; }
        return sample_guard(end, main, work::frozen_control_raw_now());
    }
    auto accept_wait(std::expected<void, MonitoringQ2CompactDiagnostic> result, bool reobservation) noexcept -> bool {
        if (result || result.error().operation == MonitoringQ2CompactOperation::timeout) return true;
        auto& slot = reobservation ? native_facts.reobservation_wait : native_facts.command_wait;
        auto& published = reobservation ? reobservation_wait_published : command_wait_published;
        if (!slot) { slot.emplace(result.error()); published.store(true, std::memory_order_release); }
        fail(reobservation ? ControlReason::mailbox : ControlReason::native);
        return false;
    }
    auto published_native_facts() const noexcept -> FrozenSourceControlNativeFacts {
        // Main's slot has one owning actor. Each control slot is written once;
        // an idle control can fail after command completion, so it has its own
        // publication flag instead of relying on command quiescence.
        FrozenSourceControlNativeFacts result;
        result.main_clock = native_facts.main_clock;
        if (control_clock_published.load(std::memory_order_acquire)) result.control_clock = native_facts.control_clock;
        if (reobservation_wait_published.load(std::memory_order_acquire)) result.reobservation_wait = native_facts.reobservation_wait;
        if (command_wait_published.load(std::memory_order_acquire)) result.command_wait = native_facts.command_wait;
        return result;
    }
    auto original_main_check() noexcept -> bool {
        if (!native.check_original_main()) { fail(ControlReason::placement); return false; }
        return true;
    }
    void publish_main_failure() noexcept {
        if (main_post_checks_observed && linux_io && !linux_io->active() && linux_io->failure()) linux_failure_published.store(true, std::memory_order_release);
        if (main_raw_failure) main_raw_published.store(true, std::memory_order_release);
    }
    void choose_main_failure(bool accepted_response) noexcept {
        if (accepted_response && !abandoned.load(std::memory_order_acquire) &&
            main_raw_published.load(std::memory_order_acquire) && !raw_failure)
            lower_primary = true;
    }
    auto raw_primary() const noexcept -> const core::Error* {
        const auto command_word = commands.load(std::memory_order_acquire);
        if (command_word == requested || command_word == executing) return nullptr;
        if (lower_primary && main_raw_published.load(std::memory_order_acquire)) return &*main_raw_failure;
        return raw_failure ? &*raw_failure : nullptr;
    }
    auto raw_late() const noexcept -> const core::Error* {
        const auto command_word = commands.load(std::memory_order_acquire);
        if (command_word == requested || command_word == executing || lower_primary ||
            !main_raw_published.load(std::memory_order_acquire)) return nullptr;
        return &*main_raw_failure;
    }
    void retain_projected_failure(core::Error&& error) noexcept {
        core::detail::JsonCleanup cleanup{error.context};
        if (!projected_failure) projected_failure.emplace(std::move(error));
    }
    auto revalidate_session(const RunnerContextAcquisitionSession& source) noexcept
        -> std::expected<HostRevalidationDisposition, core::Error> {
        try { return Access::revalidate_fallible(source); }
        catch (const std::exception&) { return std::unexpected{std::move(revalidation_stock.value)}; }
        catch (...) { return std::unexpected{std::move(revalidation_unknown_stock.value)}; }
    }
    struct MainCallChecks {
        void* context;
        bool (*mask)(void*) noexcept;
        std::expected<std::uint64_t, ControlReason> (*guard)(void*, std::uint64_t) noexcept;
    };
    template<class Call> auto on_main_checked(Call&& call, MainCallChecks checks) -> decltype(call()) {
        using Result = decltype(call());
        const bool outer_composition = composing && main_call_depth == 0U;
        struct DepthCleanup {
            std::size_t& depth;
            ~DepthCleanup() noexcept { --depth; }
        };
        ++main_call_depth;
        DepthCleanup depth_cleanup{main_call_depth};
        auto& refusal =
            outer_composition ? composition_guard_stock.value : acquisition_guard_stock.value;
        const auto check_guard = [&]() noexcept -> std::expected<std::uint64_t, ControlReason> {
            if (cancellation.requested()) {
                fail(ControlReason::cancelled);
                return std::unexpected{ControlReason::cancelled};
            }
            return checks.guard(checks.context, main_invocation_end);
        };
        const auto check_mask = [&]() noexcept {
            const bool valid_mask = checks.mask(checks.context);
            if (!valid_mask) fail(ControlReason::placement);
            return valid_mask;
        };
        if (!check_guard())
            return Result{std::unexpected{std::move(refusal)}};
        if (!check_mask()) return Result{std::unexpected{std::move(main_stock.value)}};
        try {
            auto result = call();
            core::detail::ExpectedErrorCleanup result_cleanup{result};
            const auto after = check_guard();
            const bool mask = check_mask();
            if (!after && !result && !main_raw_failure)
                main_raw_failure.emplace(std::move(result.error()));
            main_post_checks_observed = true;
            publish_main_failure();
            choose_main_failure(after.has_value()); // Acquisition occurs before native control exists.
            if (!after)
                return Result{std::unexpected{std::move(refusal)}};
            if (!result) { fail(ControlReason::session); return Result{std::move(result)}; }
            if (!mask) return Result{std::unexpected{std::move(main_stock.value)}};
            return Result{std::move(result)};
        } catch (const std::exception&) {
            const auto after = check_guard();
            static_cast<void>(check_mask());
            if (!after && !main_raw_failure)
                main_raw_failure.emplace(composing ? take_composition_stock(false)
                                                   : std::move(acquisition_stock.value));
            main_post_checks_observed = true;
            publish_main_failure();
            choose_main_failure(after.has_value());
            if (!after)
                return Result{std::unexpected{std::move(refusal)}};
            fail(ControlReason::session);
            throw; // The source owner contains after the post-call mask check.
        } catch (...) {
            const auto after = check_guard();
            static_cast<void>(check_mask());
            if (!after && !main_raw_failure)
                main_raw_failure.emplace(composing ? take_composition_stock(true)
                                                   : std::move(acquisition_unknown_stock.value));
            main_post_checks_observed = true;
            publish_main_failure();
            choose_main_failure(after.has_value());
            if (!after)
                return Result{std::unexpected{std::move(refusal)}};
            fail(ControlReason::session);
            throw;
        }
    }
    template<class Call> auto on_main(Call&& call) -> decltype(call()) {
        return on_main_checked(std::forward<Call>(call), MainCallChecks{this,
            +[](void* opaque) noexcept { return static_cast<FrozenSourceControlState*>(opaque)->original_main_check(); },
            +[](void* opaque, std::uint64_t end) noexcept { return static_cast<FrozenSourceControlState*>(opaque)->guard(end); }});
    }
    static auto proxy_acquire(void* opaque) -> std::expected<runtime::RunnerContext, core::Error> {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        return state.on_main([&] { return Access::acquire_context(*state.operations); });
    }
    static auto proxy_platform(void* opaque) -> std::expected<TypedSysnameDisposition, core::Error> {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        return state.on_main([&] { return Access::observe_platform(*state.operations); });
    }
    static auto proxy_descriptor(void* opaque, kernel::KernelId id) -> const kernel::KernelDescriptor* {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        return Access::find_descriptor(*state.operations, id);
    }
    static auto proxy_registration(void* opaque) -> std::expected<void, core::Error> {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        if (state.composing)
            state.composition_stage = HostFrozenCompositionStage::registration;
        auto result = state.on_main([&] { return Access::before_registration(*state.operations); });
        core::detail::ExpectedErrorCleanup cleanup{result};
        return decltype(result)(std::move(result));
    }
    static auto proxy_reobserve(void* opaque)
        -> std::expected<detail_HostRevalidationObservation, detail_HostObservationFailure> {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        if (!state.native.on_control_thread() ||
            state.mailbox.load(std::memory_order_acquire) != idle)
            return std::unexpected{detail_HostObservationFailure{
                detail_HostObservationFailureKind::contained_unknown_failure, std::nullopt}};
        const auto end = std::min(state.protocol.phase_deadline_, state.protocol.deadline_);
        state.acquired_request_end = end; // immutable until this response is acquired
        state.mailbox.store(requested, std::memory_order_release);
        monitoring_q2_atomic_wake(state.mailbox);
        for (;;) {
            const auto word = state.mailbox.load(std::memory_order_acquire);
            if (word == completed) {
                const auto accepted = state.guard(end);
                if (!accepted || state.abandoned.load(std::memory_order_acquire)) {
                    state.abandoned.store(true, std::memory_order_release);
                    return std::unexpected{detail_HostObservationFailure{
                        detail_HostObservationFailureKind::expected_error, std::move(state.wait_stock.value)}};
                }
                state.choose_main_failure(state.main_response_accepted);
                auto result = std::move(*state.response);
                retire_observation(state.response); // moved-from context, no clone
                state.mailbox.store(idle, std::memory_order_release);
                return result;
            }
            auto now = state.guard(end);
            if (!now) {
                // Main can still be in a synchronous provider call. Do not
                // clear/reuse the slot or release its storage on abandonment.
                state.abandoned.store(true, std::memory_order_release);
                return std::unexpected{detail_HostObservationFailure{
                    detail_HostObservationFailureKind::expected_error, std::move(state.wait_stock.value)}};
            }
            const auto slice = std::min(end, *now > UINT64_MAX - wait_slice ? end : *now + wait_slice);
            auto waited = monitoring_q2_bounded_atomic_wait_compact(state.mailbox, word, {{slice}, {}});
            if (!state.accept_wait(waited, true)) {
                state.abandoned.store(true, std::memory_order_release);
                return std::unexpected{detail_HostObservationFailure{
                    detail_HostObservationFailureKind::expected_error, std::move(state.wait_stock.value)}};
            }
        }
    }
    auto publish_reobservation() noexcept -> std::expected<void, ControlReason> {
        std::uint32_t pending = requested;
        if (!mailbox.compare_exchange_strong(pending, executing, std::memory_order_acq_rel)) return {};
        const auto end = acquired_request_end;
        main_invocation_end = end; // original main alone writes the borrowed source value
        const auto publish_error = [&](core::Error&& error) {
            response.emplace(std::unexpected{detail_HostObservationFailure{
                detail_HostObservationFailureKind::expected_error, std::move(error)}});
        };
        try {
            auto before = guard(end);
            if (!before || !original_main_check()) publish_error(std::move(main_stock.value));
            else {
                auto result = Access::reobserve(*operations);
                    if (!result) fail(ControlReason::session);
                const bool mask = original_main_check();
                auto after = guard(end);
                main_response_accepted = after.has_value() && (!result || mask);
                if (!result) response.emplace(std::move(result));
                else if (!mask || !after) publish_error(std::move(main_stock.value));
                else response.emplace(std::move(result));
            }
        } catch (const std::exception&) {
            fail(ControlReason::session);
            const bool mask = original_main_check();
            const auto after = guard(end);
            main_response_accepted = after.has_value() && (main_raw_failure.has_value() || mask);
            response.emplace(std::unexpected{detail_HostObservationFailure{
                detail_HostObservationFailureKind::contained_exception, std::nullopt}});
        } catch (...) {
            fail(ControlReason::session);
            const bool mask = original_main_check();
            const auto after = guard(end);
            main_response_accepted = after.has_value() && (main_raw_failure.has_value() || mask);
            response.emplace(std::unexpected{detail_HostObservationFailure{
                detail_HostObservationFailureKind::contained_unknown_failure, std::nullopt}});
        }
        main_post_checks_observed = true;
        publish_main_failure();
        mailbox.store(completed, std::memory_order_release);
        monitoring_q2_atomic_wake(mailbox);
        return {};
    }
    static auto prepare_native(void* opaque) noexcept -> std::expected<void, core::Error> {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        if (state.cancellation.requested()) {
            state.fail(ControlReason::cancelled);
            return std::unexpected{std::move(state.preparation_stock.value)};
        }
        if (!state.native.pin_and_observe_control()) {
            state.fail(ControlReason::placement);
            return std::unexpected{std::move(state.preparation_stock.value)};
        }
        if (state.cancellation.requested()) {
            state.fail(ControlReason::cancelled);
            return std::unexpected{std::move(state.preparation_stock.value)};
        }
        return {};
    }
    static void control_entry(void* opaque, MonitoringQ2NativeThreadOwner::StopToken stop) noexcept {
        auto& state = *static_cast<FrozenSourceControlState*>(opaque);
        struct Completion final {
            FrozenSourceControlState& owner;
            ~Completion() noexcept {
                owner.entry_done.store(true, std::memory_order_release);
                const auto pending = owner.commands.load(std::memory_order_acquire);
                if (pending == requested || pending == executing) {
                    owner.fail(ControlReason::cancelled);
                    owner.commands.store(completed, std::memory_order_release);
                    monitoring_q2_atomic_wake(owner.commands);
                }
            }
        } completion{state};
        for (;;) {
            auto word = state.commands.load(std::memory_order_acquire);
            if (stop.stop_requested() || state.cancellation.requested() || state.failed()) {
                if (state.cancellation.requested()) state.fail(ControlReason::cancelled);
                if (word == requested || word == executing) {
                    state.fail(ControlReason::cancelled);
                    state.commands.store(completed, std::memory_order_release);
                    monitoring_q2_atomic_wake(state.commands);
                }
                return;
            }
            if (word == requested && state.commands.compare_exchange_strong(word, executing, std::memory_order_acq_rel)) {
                if (state.command == Command::probe) {
                    // Explicit synthetic seam exercises the common owner gate,
                    // but public source preparation must refuse this domain.
                    auto refused = HostFrozenCollection::prepare_q5_source(*state.inputs,
                        state.ledger, state.cancellation, *state.dependencies);
                    state.synthetic_source_refused = !refused &&
                        refused.error().reason() == HostFrozenCollectionReason::association;
                    const HostFrozenCellIdentity identity{HostFrozenPhase::q5, HostQ0bFamily::bandwidth,
                        HostFrozenConfiguration::c1, 0U, 0U, 0U, 0U, measurement::ComparisonVariantSlot::second};
                    auto checked = state.capability.before_prepare_owned(*state.inputs, identity, state.cancellation);
                    if (!checked) state.probe_failure = checked.error();
                } else if (state.command == Command::prepare) {
                    auto result = !state.production && !state.synthetic_endpoints.empty()
                        ? HostFrozenCollection::prepare_owned_synthetic(
                            state.protocol.phase_ == 5U ? HostFrozenPhase::q5 : HostFrozenPhase::q6,
                            state.synthetic_endpoints, *state.inputs, state.ledger,
                            state.cancellation, *state.dependencies)
                        : state.protocol.phase_ == 5U
                        ? HostFrozenCollection::prepare_q5_source(*state.inputs, state.ledger, state.cancellation, *state.dependencies)
                        : HostFrozenCollection::prepare_q6_source(*state.inputs, state.ledger, state.cancellation, *state.dependencies);
                    if (!result) { state.collection_failure.emplace(std::move(result.error())); state.fail(ControlReason::collection); }
                    else state.collection.emplace(std::move(*result));
                } else if (state.command == Command::next) {
                    auto result = state.collection->next();
                    if (!result) { state.collection_failure.emplace(std::move(result.error())); state.fail(ControlReason::collection); }
                    else if (!*result) state.fail(ControlReason::collection);
                    else state.cell.emplace(std::move(**result));
                } else {
                    auto result = state.collection->consume_cell(std::move(*state.cell));
                    state.cell.reset();
                    if (!result) state.fail(ControlReason::collection);
                }
                state.commands.store(completed, std::memory_order_release);
                monitoring_q2_atomic_wake(state.commands);
                continue;
            }
            auto now = state.guard(state.protocol.phase_deadline_);
            if (!now) return;
            const auto slice = std::min(state.protocol.phase_deadline_,
                *now > UINT64_MAX - wait_slice ? state.protocol.phase_deadline_ : *now + wait_slice);
            auto waited = monitoring_q2_bounded_atomic_wait_compact(state.commands, word, {{slice}, {}});
            if (!state.accept_wait(waited, false)) return;
        }
    }
    auto enqueue(Command value) noexcept -> std::expected<void, ControlReason> {
        if (!native.on_original_main() || !thread || retired) return std::unexpected{ControlReason::invalid_input};
        if (auto failure = failed()) return std::unexpected{*failure};
        if (!guard(protocol.phase_deadline_)) return std::unexpected{*failed()};
        if (entry_done.load(std::memory_order_acquire)) return std::unexpected{ControlReason::cancelled};
        const auto word = commands.load(std::memory_order_acquire);
        if ((word != idle && word != completed) || !consumption_settled)
            return std::unexpected{ControlReason::busy};
        command = value;
        if (value == Command::consume) consumption_settled = false;
        commands.store(requested, std::memory_order_release);
        monitoring_q2_atomic_wake(commands);
        if (entry_done.load(std::memory_order_acquire)) {
            fail(ControlReason::cancelled);
            commands.store(completed, std::memory_order_release);
            return std::unexpected{ControlReason::cancelled};
        }
        return {};
    }
};

auto FrozenSourceControlCapability::phase_deadline_owned(const HostFrozenSourceInputs& inputs,
    HostFrozenPhase phase, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<std::uint64_t, HostFrozenCollectionReason> {
    if (!state_ || !state_->session || &inputs.session != &*state_->session ||
        &cancellation != &state_->cancellation || inputs.control != this ||
        !state_->inputs || &inputs.plan != &state_->inputs->plan ||
        &inputs.calibration != &state_->inputs->calibration || &inputs.public_values != &state_->inputs->public_values ||
        &inputs.request_ids != &state_->inputs->request_ids ||
        (phase == HostFrozenPhase::q5 ? 5U : 6U) != state_->protocol.phase_ ||
        !state_->native.on_control_thread() ||
        !state_->native.check_control()) return std::unexpected{HostFrozenCollectionReason::association};
    auto checked = state_->guard(state_->protocol.phase_deadline_);
    if (!checked) return std::unexpected{checked.error() == ControlReason::cancelled
        ? HostFrozenCollectionReason::cancelled : HostFrozenCollectionReason::deadline};
    return state_->protocol.phase_deadline_;
}
auto FrozenSourceControlCapability::permit_deadline() const noexcept -> std::uint64_t {
    return state_ ? state_->protocol.deadline_ : 0U;
}
auto FrozenSourceControlCapability::before_activate_owned(const HostFrozenSourceInputs& inputs,
    const HostFrozenCellIdentity& identity, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<void, HostFrozenCollectionReason> {
    auto phase = phase_deadline_owned(inputs, identity.phase, cancellation);
    if (!phase) return std::unexpected{phase.error()};
    if (state_->protocol.state_ != FrozenProcessChild::State::observing ||
        state_->protocol.ordinal_ != identity.ordinal) return std::unexpected{HostFrozenCollectionReason::association};
    auto checked = state_->guard(std::min(*phase, permit_deadline()));
    if (!checked) return std::unexpected{checked.error() == ControlReason::cancelled
        ? HostFrozenCollectionReason::cancelled : HostFrozenCollectionReason::deadline};
    return {};
}
auto FrozenSourceControlCapability::before_prepare_owned(const HostFrozenSourceInputs& inputs,
    const HostFrozenCellIdentity& identity, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<void, HostFrozenCollectionReason> {
    auto before = before_activate_owned(inputs, identity, cancellation);
    if (!before) return before;
    auto result = state_->revalidate_session(inputs.session);
    core::detail::ExpectedErrorCleanup cleanup{result};
    if (!result) {
        state_->retain_projected_failure(std::move(result.error()));
        state_->fail(ControlReason::session);
        return std::unexpected{HostFrozenCollectionReason::admission};
    }
    if (*result != HostRevalidationDisposition::matched) {
        state_->fail(ControlReason::session); return std::unexpected{HostFrozenCollectionReason::association};
    }
    return before_activate_owned(inputs, identity, cancellation);
}

auto FrozenSourceControlCapability::phase_deadline(const HostFrozenSourceInputs& inputs,
    HostFrozenPhase phase, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<std::uint64_t, HostFrozenCollectionReason> {
    if (!state_ || !state_->production || !state_->source_composition ||
        !state_->source_admission || !state_->source_admission->values() ||
        &inputs.public_values != *state_->source_admission->values() ||
        inputs.environment != &state_->source_composition->environment() ||
        &inputs.plan != &state_->source_composition->plan() ||
        &inputs.calibration != &state_->source_composition->calibration() ||
        &inputs.request_ids != state_->source_ids || inputs.raw_failure != &state_->raw_failure)
        return std::unexpected{HostFrozenCollectionReason::association};
    return phase_deadline_owned(inputs, phase, cancellation);
}
auto FrozenSourceControlCapability::before_prepare(const HostFrozenSourceInputs& inputs,
    const HostFrozenCellIdentity& identity, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<void, HostFrozenCollectionReason> {
    if (!state_ || !state_->production || !state_->source_composition ||
        !state_->source_admission || !state_->source_admission->values() ||
        &inputs.public_values != *state_->source_admission->values() ||
        inputs.environment != &state_->source_composition->environment() ||
        &inputs.plan != &state_->source_composition->plan() ||
        &inputs.calibration != &state_->source_composition->calibration() ||
        &inputs.request_ids != state_->source_ids || inputs.raw_failure != &state_->raw_failure)
        return std::unexpected{HostFrozenCollectionReason::association};
    return before_prepare_owned(inputs, identity, cancellation);
}
auto FrozenSourceControlCapability::before_activate(const HostFrozenSourceInputs& inputs,
    const HostFrozenCellIdentity& identity, runtime::RunnerCancellation& cancellation) const noexcept
    -> std::expected<void, HostFrozenCollectionReason> {
    if (!state_ || !state_->production || !state_->source_composition ||
        !state_->source_admission || !state_->source_admission->values() ||
        &inputs.public_values != *state_->source_admission->values() ||
        inputs.environment != &state_->source_composition->environment() ||
        &inputs.plan != &state_->source_composition->plan() ||
        &inputs.calibration != &state_->source_composition->calibration() ||
        &inputs.request_ids != state_->source_ids || inputs.raw_failure != &state_->raw_failure)
        return std::unexpected{HostFrozenCollectionReason::association};
    return before_activate_owned(inputs, identity, cancellation);
}

FrozenSourceControlOwner::FrozenSourceControlOwner(std::unique_ptr<FrozenSourceControlState> state,
    work::ResourceLease lease) noexcept : state_{std::move(state)}, state_lease_{std::move(lease)} {}
FrozenSourceControlOwner::FrozenSourceControlOwner(FrozenSourceControlOwner&&) noexcept = default;
FrozenSourceControlOwner::~FrozenSourceControlOwner() noexcept {
    state_.reset(); // Actual State storage and last evidence borrows end first.
    if (state_lease_ && state_lease_->active() && !state_lease_->retire()) std::terminate();
}
auto FrozenSourceControlOwner::acquire_on_original_main(MonitoringHostInputTemplateAuthority input,
    std::uint8_t phase, std::size_t workers, const FrozenProcessRecord& grant,
    work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation, std::size_t capacity,
    const platform_linux::detail::TopologySourceLimits* source_limits) noexcept
    -> std::expected<FrozenSourceControlOwner, ControlReason> {
    return acquire_impl(std::move(input), std::nullopt, phase, workers, grant, ledger, cancellation, capacity, source_limits);
}
auto FrozenSourceControlOwner::acquire_source_on_original_main(
    HostFrozenSourceAdmission admission, MonitoringHostInputTemplateAuthority input,
    runtime::ProductionRequestIdSource& ids, const HostFrozenSourceLaunchBinding& binding,
    std::uint8_t phase, std::size_t workers,
    const FrozenProcessRecord& grant, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancellation, std::size_t capacity,
    const platform_linux::detail::TopologySourceLimits& limits) noexcept
    -> std::expected<FrozenSourceControlOwner, ControlReason> {
    if (!admission.values() || !binding.ready() || !binding.guard() ||
        &binding.owning_cancellation() != &cancellation ||
        static_cast<unsigned>(binding.grant_record().phase) != phase ||
        binding.grant_record().phase_end_ns != grant.deadline_ns)
        return std::unexpected{ControlReason::invalid_input};
    return acquire_impl(std::move(input), std::nullopt, phase, workers, grant, ledger, cancellation,
                        capacity, &limits,
                        std::optional<HostFrozenSourceAdmission>{std::move(admission)}, &ids, &binding);
}
auto FrozenSourceControlOwner::acquire_impl(
    MonitoringHostInputTemplateAuthority input,
    std::optional<RunnerContextAcquisitionOperations> injected, std::uint8_t phase,
    std::size_t workers, const FrozenProcessRecord& grant, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancellation, std::size_t capacity,
    const platform_linux::detail::TopologySourceLimits* source_limits,
    std::optional<HostFrozenSourceAdmission> admission,
    runtime::ProductionRequestIdSource* ids,
    const HostFrozenSourceLaunchBinding* binding) noexcept
    -> std::expected<FrozenSourceControlOwner, ControlReason> {
    if ((!injected && (!source_limits || !platform_linux::detail::topology_source_limits_valid(*source_limits))) ||
        !check_frozen_process_grant(grant, phase, workers) || ledger.poisoned() || !capacity || capacity > UINT32_MAX)
        return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(phase, workers, ledger, cancellation, !injected); }
    catch (...) {
        if (!lease->retire()) std::terminate();
        return std::unexpected{ControlReason::construction};
    }
    if (admission)
        state->source_admission.emplace(std::move(*admission));
    state->source_ids = ids;
    state->source_binding = binding;
    static_cast<void>(state->protocol.grant(grant));
    state->main_invocation_end = grant.deadline_ns;
    if (!state->native.capture_original_main(ledger, capacity)) state->fail(ControlReason::native);
    else if (state->guard(grant.deadline_ns)) {
        try {
            if (!injected) {
                const work::GuardOperations checks{state.get(), FrozenSourceControlState::source_main_now,
                    +[](void* opaque) noexcept { return static_cast<FrozenSourceControlState*>(opaque)->cancellation.requested(); },
                    &state->source_clock.actual};
                auto prepared = platform_linux::detail::PreparedTopologySourceIO::prepare(
                    ledger, *source_limits, platform_linux::detail::production_topology_source_operations(),
                    checks, {"/sys", "/proc", "/sys/fs/cgroup"}, state->native.original_main_id());
                if (!prepared) {
                    state->fail(prepared.error().reason == platform_linux::detail::TopologySourceReason::capacity
                        ? ControlReason::capacity : ControlReason::construction);
                } else {
                    state->linux_io.emplace(std::move(*prepared));
                }
            }
            if (!state->failed()) {
            auto operations = injected
                ? std::expected<RunnerContextAcquisitionOperations, core::Error>{std::move(*injected)}
                : Access::production_operations(&state->main_raw_failure, state->linux_io ? &*state->linux_io : nullptr,
                                                &state->main_invocation_end);
            core::detail::ExpectedErrorCleanup operations_cleanup{operations};
            if (!operations) { state->raw_failure.emplace(std::move(operations.error())); state->fail(ControlReason::session); }
            else {
                state->operations.emplace(std::move(*operations));
                auto proxy = RunnerContextAcquisitionOperations::create(state.get(), [](void*) noexcept {},
                    FrozenSourceControlState::proxy_acquire, FrozenSourceControlState::proxy_platform,
                    FrozenSourceControlState::proxy_descriptor, FrozenSourceControlState::proxy_reobserve,
                    FrozenSourceControlState::proxy_registration);
                core::detail::ExpectedErrorCleanup proxy_cleanup{proxy};
                if (!proxy) { state->raw_failure.emplace(std::move(proxy.error())); state->fail(ControlReason::session); }
                else {
                    Access::attach_raw_failure_sink(*proxy, &state->raw_failure);
                    auto session = Access::acquire_with_fallible(std::move(input), std::move(*proxy));
                    core::detail::ExpectedErrorCleanup session_cleanup{session};
                    if (!session) { state->retain_projected_failure(std::move(session.error())); state->fail(ControlReason::session); }
                    else state->session.emplace(std::move(*session));
                }
            }
            }
        } catch (const std::exception&) {
            state->retain_projected_failure(std::move(state->acquisition_stock.value));
            state->fail(ControlReason::construction);
        } catch (...) {
            state->retain_projected_failure(std::move(state->acquisition_unknown_stock.value));
            state->fail(ControlReason::construction);
        }
        if (state->source_admission && state->source_ids && state->session && !state->failed()) {
            state->composing = true;
            struct CompositionCleanup {
                bool& active;
                ~CompositionCleanup() noexcept { active = false; }
            };
            CompositionCleanup composition_cleanup{state->composing};
            try {
                auto bundle = state->on_main([&] {
                    return HostFrozenSourceComposition::prepare(*state->session, *state->source_ids,
                                                                state->composition_stage,
                                                                &state->main_raw_failure);
                });
                core::detail::ExpectedErrorCleanup bundle_cleanup{bundle};
                if (!bundle) {
                    state->retain_projected_failure(std::move(bundle.error()));
                    state->fail(ControlReason::session);
                } else
                    state->source_composition.emplace(std::move(*bundle));
            } catch (const std::exception&) {
                state->retain_projected_failure(state->take_composition_stock(false));
                state->fail(ControlReason::construction);
            } catch (...) {
                state->retain_projected_failure(state->take_composition_stock(true));
                state->fail(ControlReason::construction);
            }
        }
        static_cast<void>(state->guard(grant.deadline_ns));
        state->main_post_checks_observed = true;
        state->publish_main_failure();
    }
    // Failed acquisition still returns the concrete recoverable owner. The only
    // error-only returns above precede native acquisitions.
    return FrozenSourceControlOwner{std::move(state), std::move(*lease)};
}
auto FrozenSourceControlOwner::session() const noexcept -> const RunnerContextAcquisitionSession* {
    return state_ && state_->session ? &*state_->session : nullptr;
}
auto FrozenSourceControlOwner::start_source_control(
    HostFrozenCollectionDependencies dependencies) noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->source_admission || !state_->source_composition ||
        !state_->source_ids || !state_->session)
        return std::unexpected{ControlReason::invalid_input};
    auto values = state_->source_admission->values();
    if (!values)
        return std::unexpected{ControlReason::invalid_input};
    const auto& bundle = *state_->source_composition;
    return start_control({*state_->session, bundle.plan(), bundle.calibration(), **values,
                          *state_->source_ids, &state_->capability, &bundle.environment(),
                          &state_->raw_failure},
                         std::move(dependencies));
}
auto FrozenSourceControlOwner::start_owned_synthetic_for_testing(
    std::span<const HostFrozenCollection::Endpoint> endpoints,
    HostFrozenCollectionDependencies dependencies, core::LinuxCpuId control_cpu) noexcept
    -> std::expected<void, ControlReason> {
    if (!state_ || state_->production || !state_->source_admission || !state_->source_composition ||
        !state_->source_ids || !state_->session || endpoints.size() != 10U ||
        !state_->synthetic_endpoints.empty() || state_->synthetic_control_cpu)
        return std::unexpected{ControlReason::invalid_input};
    auto values = state_->source_admission->values();
    if (!values) return std::unexpected{ControlReason::invalid_input};
    state_->synthetic_endpoints = endpoints;
    state_->synthetic_control_cpu = control_cpu;
    const auto& bundle = *state_->source_composition;
    return start_control({*state_->session, bundle.plan(), bundle.calibration(), **values,
        *state_->source_ids, &state_->capability, &bundle.environment(), &state_->raw_failure},
        std::move(dependencies));
}
auto FrozenSourceControlOwner::collection_inputs_for_testing() const noexcept
    -> std::optional<HostFrozenSourceInputs> {
    if (!state_ || state_->production || !state_->source_admission || !state_->source_composition ||
        !state_->session || !state_->source_ids || state_->thread || state_->retired || state_->failed())
        return std::nullopt;
    const auto values = state_->source_admission->values();
    if (!values) return std::nullopt;
    const auto& bundle = *state_->source_composition;
    return HostFrozenSourceInputs{*state_->session, bundle.plan(), bundle.calibration(), **values,
        *state_->source_ids, &state_->capability, &bundle.environment(), &state_->raw_failure};
}
auto FrozenSourceControlOwner::start_control(HostFrozenSourceInputs inputs, HostFrozenCollectionDependencies dependencies) noexcept
    -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || !state_->session ||
        &inputs.session != &*state_->session || state_->thread || state_->retired)
        return std::unexpected{ControlReason::invalid_input};
    auto& state = *state_;
    if (state.production &&
        (!state.source_admission || !state.source_composition ||
         !state.source_admission->values() ||
         &inputs.public_values != *state.source_admission->values() ||
         inputs.environment != &state.source_composition->environment() ||
         &inputs.plan != &state.source_composition->plan() ||
         &inputs.calibration != &state.source_composition->calibration() ||
         &inputs.request_ids != state.source_ids || inputs.raw_failure != &state.raw_failure))
        return std::unexpected{ControlReason::invalid_input};
    if (auto failed = state.failed()) return std::unexpected{*failed};
    const auto service = inputs.plan.service_cores();
    if (service.size() != 2U || service[0].role != "control" || service[1].role != "measurement" ||
        inputs.plan.bandwidth_workers().size() != state.protocol.workers_)
        return std::unexpected{ControlReason::invalid_input};
    if (!state.guard(state.protocol.phase_deadline_) || !state.original_main_check()) return std::unexpected{*state.failed()};
    inputs.control = &state.capability;
    state.inputs.emplace(inputs);
    dependencies.guard = {&state,
        [](void*, std::uint64_t* output) noexcept -> platform_linux::detail::RawStatusResult {
            auto now = work::frozen_control_raw_now();
            if (!now) return {-1, now.error().native_error};
            *output = *now; return {0,0};
        }, [](void* opaque) noexcept { return static_cast<FrozenSourceControlState*>(opaque)->cancellation.requested(); }};
    state.dependencies.emplace(std::move(dependencies));
    if (!state.native.prepare_control(state.ledger,
        state.synthetic_control_cpu.value_or(service[0].execution_linux_cpu))) {
        state.fail(ControlReason::native); return std::unexpected{ControlReason::native};
    }
    auto created = MonitoringQ2NativeThreadOwner::create_parked(
        {MonitoringQ2NativeThreadRole::target, MonitoringQ2NativeThreadOwner::maximum_targets - 1U}, &state,
        FrozenSourceControlState::control_entry, {{state.protocol.phase_deadline_},
            {&state.cancellation, [](void* token) noexcept {
                return static_cast<runtime::RunnerCancellation*>(token)->requested(); }, nullptr, nullptr}}, nullptr,
        FrozenSourceControlState::prepare_native, state.native.attributes(), wait_slice);
    if (!created) {
        state.create_failure.emplace(std::move(created).take_failure());
        state.thread = std::move(created).recovery_owner();
        state.fail(ControlReason::native); return std::unexpected{ControlReason::native};
    }
    state.thread.emplace(std::move(created).owner());
    if (!state.guard(state.protocol.phase_deadline_) || !state.original_main_check()) {
        state.thread->request_stop();
        return std::unexpected{*state.failed()};
    }
    auto released = state.thread->release_start();
    if (!released) { state.native_cleanup.append(released.error()); state.fail(ControlReason::native); return std::unexpected{ControlReason::native}; }
    return {};
}
auto FrozenSourceControlOwner::enqueue_prepare() noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main()) return std::unexpected{ControlReason::invalid_input};
    const auto word = state_->commands.load(std::memory_order_acquire);
    if (word != idle && word != completed) return std::unexpected{ControlReason::busy};
    if (state_->collection) return std::unexpected{ControlReason::invalid_input};
    return state_->enqueue(FrozenSourceControlState::Command::prepare);
}
auto FrozenSourceControlOwner::ready() noexcept -> std::expected<FrozenProcessRecord, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || status() != FrozenSourceControlStatus::complete || !state_->collection)
        return std::unexpected{ControlReason::busy};
    auto result = state_->protocol.ready();
    if (!result) return std::unexpected{ControlReason::protocol};
    return *result;
}
auto FrozenSourceControlOwner::begin() noexcept -> std::expected<FrozenProcessRecord, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || !state_->consumption_settled ||
        status() != FrozenSourceControlStatus::complete) return std::unexpected{ControlReason::busy};
    auto result = state_->protocol.begin();
    if (!result) return std::unexpected{ControlReason::protocol};
    return *result;
}
auto FrozenSourceControlOwner::enqueue_next(const FrozenProcessRecord& permit) noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || status() != FrozenSourceControlStatus::complete || state_->cell)
        return std::unexpected{ControlReason::busy};
    auto admitted = state_->protocol.permit(permit);
    if (!admitted) return std::unexpected{ControlReason::protocol};
    return state_->enqueue(FrozenSourceControlState::Command::next);
}
auto FrozenSourceControlOwner::payload(FrozenProcessType type, std::uint64_t bytes, std::array<std::byte,32> hash) noexcept
    -> std::expected<FrozenProcessRecord, ControlReason> {
    if (!state_ || !state_->native.on_original_main() ||
        state_->commands.load(std::memory_order_acquire) != completed ||
        (type == FrozenProcessType::cell ? !state_->cell : !state_->collection_failure))
        return std::unexpected{ControlReason::busy};
    auto result = state_->protocol.payload(type, bytes, hash);
    if (!result) return std::unexpected{ControlReason::protocol};
    return *result;
}
auto FrozenSourceControlOwner::enqueue_consume(const FrozenProcessRecord& ack) noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || status() != FrozenSourceControlStatus::complete || !state_->cell)
        return std::unexpected{ControlReason::busy};
    auto admitted = state_->protocol.acknowledge(ack);
    if (!admitted) return std::unexpected{ControlReason::protocol};
    return state_->enqueue(FrozenSourceControlState::Command::consume);
}
auto FrozenSourceControlOwner::service_once() noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main() || state_->retired) return std::unexpected{ControlReason::invalid_input};
    auto& state = *state_;
    auto result = state.publish_reobservation();
    if (!result) return result;
    if (state.command == FrozenSourceControlState::Command::consume && !state.consumption_settled &&
        state.commands.load(std::memory_order_acquire) == completed) {
        if (!state.failed()) {
            const auto snapshot = state.collection->snapshot();
            if (!snapshot || !state.protocol.consume(snapshot->consumed)) state.fail(ControlReason::protocol);
        }
        state.consumption_settled = true;
    }
    return {};
}
auto FrozenSourceControlOwner::snapshot() const noexcept -> std::optional<HostFrozenCollectionSnapshot> {
    if (!state_ || !state_->native.on_original_main() || status() == FrozenSourceControlStatus::pending || !state_->collection)
        return std::nullopt;
    return state_->collection->snapshot();
}
auto FrozenSourceControlOwner::finish(std::uint64_t bytes, std::array<std::byte,32> hash) noexcept
    -> std::expected<FrozenProcessRecord, ControlReason> {
    const auto actual = snapshot();
    if (!actual || !actual->completed || actual->consumed != actual->expected ||
        !state_->consumption_settled || state_->cell || status() != FrozenSourceControlStatus::complete)
        return std::unexpected{ControlReason::busy};
    auto result = state_->protocol.finish(bytes, hash);
    if (!result) return std::unexpected{ControlReason::protocol};
    return *result;
}
auto FrozenSourceControlOwner::finish_acknowledgment(const FrozenProcessRecord& ack) noexcept
    -> std::expected<FrozenProcessRecord, ControlReason> {
    const auto actual = snapshot();
    if (!actual || !actual->completed || !state_->protocol.acknowledge(ack))
        return std::unexpected{ControlReason::protocol};
    auto result = state_->protocol.terminal(actual->consumed);
    if (!result) return std::unexpected{ControlReason::protocol};
    return *result;
}
auto FrozenSourceControlOwner::status() const noexcept -> FrozenSourceControlStatus {
    if (!state_ || state_->retired) return FrozenSourceControlStatus::retired;
    const auto word = state_->commands.load(std::memory_order_acquire);
    if (word == requested || word == executing) return FrozenSourceControlStatus::pending;
    if (state_->failed()) return FrozenSourceControlStatus::failed;
    return word == completed ? FrozenSourceControlStatus::complete : FrozenSourceControlStatus::idle;
}
auto FrozenSourceControlOwner::cell() const noexcept -> const HostFrozenCell* {
    return state_ && state_->native.on_original_main() && status() == FrozenSourceControlStatus::complete && state_->cell ? &*state_->cell : nullptr;
}
auto FrozenSourceControlOwner::quiescent_collection() const noexcept -> const HostFrozenCollection* {
    if (!state_ || !state_->native.on_original_main() || state_->retired || state_->cell ||
        state_->commands.load(std::memory_order_acquire) != completed ||
        state_->mailbox.load(std::memory_order_acquire) == requested ||
        state_->mailbox.load(std::memory_order_acquire) == executing || !state_->collection)
        return nullptr;
    const auto snapshot = state_->collection->snapshot();
    return snapshot && snapshot->terminal && !snapshot->outstanding ? &*state_->collection : nullptr;
}
auto FrozenSourceControlOwner::source_admission() const noexcept -> const HostFrozenSourceAdmission* {
    if (!state_ || !state_->production || !state_->native.on_original_main() ||
        status() != FrozenSourceControlStatus::complete || state_->cell || state_->collection_failure ||
        !state_->consumption_settled || !state_->source_admission || !state_->source_composition ||
        !state_->session || state_->protocol.state_ != FrozenProcessChild::State::finished ||
        state_->mailbox.load(std::memory_order_acquire) == requested ||
        state_->mailbox.load(std::memory_order_acquire) == executing || !state_->collection)
        return nullptr;
    const auto snapshot = state_->collection->snapshot();
    if (!snapshot || !snapshot->completed || snapshot->outstanding ||
        snapshot->consumed != snapshot->expected || !state_->source_admission->retained_history() ||
        !state_->source_admission->retained_source_provenance())
        return nullptr;
    return &*state_->source_admission;
}
auto FrozenSourceControlOwner::collection_failure() const noexcept -> const HostFrozenCollectionFailure* {
    return state_ && state_->native.on_original_main() && state_->commands.load(std::memory_order_acquire) == completed && state_->collection_failure ? &*state_->collection_failure : nullptr;
}
auto FrozenSourceControlOwner::failure_reason() const noexcept -> std::optional<ControlReason> {
    return state_ ? state_->failed() : std::nullopt;
}
auto FrozenSourceControlOwner::raw_session_failure() const noexcept -> const core::Error* {
    if (!state_ || !state_->native.on_original_main() || status() == FrozenSourceControlStatus::pending ||
        state_->mailbox.load(std::memory_order_acquire) == executing) return nullptr;
    return state_->raw_primary();
}
auto FrozenSourceControlOwner::raw_late_session_failure() const noexcept -> const core::Error* {
    if (!state_ || !state_->native.on_original_main()) return nullptr;
    return state_->raw_late();
}
auto FrozenSourceControlOwner::bind_main_failure_sink_for_testing(
    std::optional<core::Error>** destination) noexcept -> std::expected<void, ControlReason> {
    if (!destination || *destination || !state_ || state_->production ||
        !state_->native.on_original_main() || !state_->session || state_->inputs || state_->thread ||
        state_->retired || state_->failed() || state_->main_raw_failure ||
        state_->main_raw_published.load(std::memory_order_acquire) ||
        state_->commands.load(std::memory_order_acquire) != idle ||
        state_->mailbox.load(std::memory_order_acquire) != idle)
        return std::unexpected{ControlReason::invalid_input};
    *destination = &state_->main_raw_failure;
    return {};
}
auto FrozenSourceControlOwner::diagnostics() const noexcept -> std::optional<FrozenSourceControlDiagnostics> {
    if (!state_ || !state_->native.on_original_main() || status() == FrozenSourceControlStatus::pending)
        return std::nullopt;
    return FrozenSourceControlDiagnostics{state_->native.failure(), state_->native.control_failure(),
        state_->native.cleanup_failures(), state_->create_failure ? &*state_->create_failure : nullptr,
        &state_->native_cleanup, !state_->retired && state_->retirement_attempts >= 2U,
        state_->published_native_facts()};
}
auto FrozenSourceControlOwner::enqueue_probe(const FrozenProcessRecord& permit) noexcept
    -> std::expected<void, ControlReason> {
    if (!state_ || state_->production || !state_->native.on_original_main() || !state_->inputs ||
        state_->commands.load(std::memory_order_acquire) != idle) return std::unexpected{ControlReason::invalid_input};
    if (!state_->protocol.ready() || !state_->protocol.begin() || !state_->protocol.permit(permit))
        return std::unexpected{ControlReason::protocol};
    return state_->enqueue(FrozenSourceControlState::Command::probe);
}
auto FrozenSourceControlOwner::probe_result() const noexcept -> std::optional<HostFrozenCollectionReason> {
    if (!state_ || state_->production || status() == FrozenSourceControlStatus::pending)
        return HostFrozenCollectionReason::unavailable;
    return state_->probe_failure;
}
auto FrozenSourceControlOwner::source_refused() const noexcept -> bool {
    return state_ && !state_->production && status() != FrozenSourceControlStatus::pending && state_->synthetic_source_refused;
}
auto FrozenSourceControlOwner::prepare_state_storage_for_testing(work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancellation) noexcept -> std::expected<void, ControlReason> {
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    try {
        auto state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false);
        state.reset();
    } catch (...) {
        if (!lease->retire()) std::terminate();
        return std::unexpected{ControlReason::construction};
    }
    if (!lease->retire()) std::terminate();
    return {};
}
auto FrozenSourceControlOwner::retain_failure_facts_for_testing(work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancellation, FrozenSourceControlFailureProbeFirst first,
    void* context, void (*allocation_denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceControlFailureProbe, ControlReason> {
    using First = FrozenSourceControlFailureProbeFirst;
    if (!allocation_denial || first > First::regression) return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false); }
    catch (...) {
        if (!lease->retire()) std::terminate();
        return std::unexpected{ControlReason::construction};
    }
    allocation_denial(context, true);
    const auto clock = [&](bool main, int code) noexcept {
        static_cast<void>(state->sample_guard(100U, main,
            std::unexpected{work::ControlFailure{work::ControlOperation::clock, code}}));
    };
    const auto wait = [&](bool reobservation, int code) noexcept {
        static_cast<void>(state->accept_wait(
            std::unexpected{MonitoringQ2CompactDiagnostic{MonitoringQ2CompactOperation::wait, code}}, reobservation));
    };
    switch (first) {
    case First::main_clock: clock(true, EIO); break;
    case First::control_clock: clock(false, ENOSYS); break;
    case First::reobservation_wait: wait(true, EFAULT); break;
    case First::command_wait: wait(false, EINVAL); break;
    case First::session: state->fail(ControlReason::session); break;
    case First::regression:
        static_cast<void>(state->sample_guard(100U, true, 50U));
        static_cast<void>(state->sample_guard(100U, true, 40U));
        break;
    }
    clock(true, EIO); clock(false, ENOSYS); wait(true, EFAULT); wait(false, EINVAL);
    // Later failures and timeout slices cannot replace first native facts.
    clock(true, ESTALE); clock(false, ESTALE); wait(true, ENXIO); wait(false, ENXIO);
    static_cast<void>(state->accept_wait({}, true));
    static_cast<void>(state->accept_wait(
        std::unexpected{MonitoringQ2CompactDiagnostic{MonitoringQ2CompactOperation::timeout, ETIMEDOUT}}, false));
    state->native_cleanup.append({MonitoringQ2CompactOperation::cond_destroy, EBUSY});
    state->native_cleanup.append({MonitoringQ2CompactOperation::mutex_destroy, EPERM});
    FrozenSourceControlFailureProbe evidence{
        state->published_native_facts(), state->native_cleanup, state->failed()};
    state.reset(); // Actual stocks and State storage retire while denial holds.
    if (!lease->retire()) std::terminate();
    allocation_denial(context, false);
    return evidence;
}
auto probe_frozen_source_session_stocks(const RunnerContextAcquisitionSession& session,
    bool unknown, work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
    void* context, void (*denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceSessionStockProbe, ControlReason> {
    if (!denial) return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false); }
    catch (...) { if (!lease->retire()) std::terminate(); return std::unexpected{ControlReason::construction}; }
    const auto* original = (unknown ? state->revalidation_unknown_stock.value : state->revalidation_stock.value)
        .context.get_ptr<const nlohmann::json::object_t*>();
    FrozenSourceSessionStockProbe evidence{};
    denial(context, true);
    {
        auto result = state->revalidate_session(session);
        core::detail::ExpectedErrorCleanup cleanup{result};
        if (!result) {
            state->retain_projected_failure(std::move(result.error()));
            auto& first = *state->projected_failure;
            evidence.identity = first.context.get_ptr<const nlohmann::json::object_t*>() == original;
            evidence.projection = first.code.value() == "monitoring.host_environment.contained_failure" &&
                first.operation == "revalidate_host_environment" &&
                core::detail::json_string_equal(first.context.at("classification"),
                    unknown ? "contained_unknown_failure" : "contained_exception");
            // A later actual temporary is retired instead of replacing the first.
            state->retain_projected_failure(std::move(state->acquisition_stock.value));
            state->retain_projected_failure(std::move(state->acquisition_unknown_stock.value));
            evidence.immutable_first = state->projected_failure->context.get_ptr<const nlohmann::json::object_t*>() == original;
        }
        evidence.composition_stocks = true;
        for (const auto stage :
             {HostFrozenCompositionStage::environment, HostFrozenCompositionStage::plan,
              HostFrozenCompositionStage::registration, HostFrozenCompositionStage::calibration}) {
            state->composition_stage = stage;
            for (bool unexpected : {false, true}) {
                auto value = state->take_composition_stock(unexpected);
                core::detail::JsonCleanup composition_value_cleanup{value.context};
                const auto operation = stage == HostFrozenCompositionStage::environment
                                           ? "derive_host_environment"
                                       : stage == HostFrozenCompositionStage::plan
                                           ? "resolve_monitoring_host_qualification_plan"
                                       : stage == HostFrozenCompositionStage::registration
                                           ? "register_selected_resources"
                                           : "compose_host_qualification_calibration_inputs";
                evidence.composition_stocks =
                    evidence.composition_stocks && value.operation == operation &&
                    core::detail::json_string_equal(value.context.at("classification"),
                                                    unexpected ? "contained_unknown_failure"
                                                               : "contained_exception");
                auto moved = std::move(value);
                core::detail::JsonCleanup moved_cleanup{moved.context};
                evidence.composition_stocks =
                    evidence.composition_stocks && moved.operation == operation;
            }
        }
        evidence.control_stocks = core::detail::json_string_equal(state->main_stock.value.context.at("classification"), "original_main") &&
            core::detail::json_string_equal(state->wait_stock.value.context.at("classification"), "main_mailbox") &&
            core::detail::json_string_equal(state->acquisition_guard_stock.value.context.at("classification"), "source_acquisition_guard");
        std::optional<core::Error> moved;
        if (state->projected_failure) moved.emplace(std::move(*state->projected_failure));
        state.reset(); // The actual stocks and their moved-from cells retire under denial.
        evidence.charged = ledger.outstanding().ordinary_bytes == storage.ordinary_bytes &&
            ledger.outstanding().address_space_bytes == storage.address_space_bytes;
        if (moved) core::detail::retire_json(moved->context);
        moved.reset(); // Last moved diagnostic owner ends before charge return.
    }
    if (!lease->retire()) std::terminate();
    denial(context, false);
    return evidence;
}

auto probe_frozen_source_raw_failure_order(bool timeout_first, core::Error actual,
    work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
    void* context, void (*denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceRawOrderProbe, ControlReason> {
    core::detail::JsonCleanup actual_cleanup{actual.context};
    if (!denial) return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false); }
    catch (...) { if (!lease->retire()) std::terminate(); return std::unexpected{ControlReason::construction}; }
    const auto* original = actual.context.get_ptr<const nlohmann::json::object_t*>();
    FrozenSourceRawOrderProbe evidence{};
    denial(context, true);
    state->commands.store(executing, std::memory_order_release);
    if (timeout_first) {
        state->raw_failure.emplace(std::move(state->wait_stock.value));
        state->fail(ControlReason::deadline);
        state->abandoned.store(true, std::memory_order_release);
        state->commands.store(completed, std::memory_order_release);
    }
    // Original-main actor initializes, then publishes only its first completed
    // cause. The real call sites perform their post-provider mask check first.
    state->main_raw_failure.emplace(std::move(actual));
    state->choose_main_failure(true);
    evidence.unpublished_hidden = !state->lower_primary && state->raw_late() == nullptr;
    state->publish_main_failure();
    state->mailbox.store(completed, std::memory_order_release);
    state->choose_main_failure(false); // Refused response cannot select a lower primary.
    evidence.immutable = !state->lower_primary;
    state->choose_main_failure(true); // Timeout abandonment still refuses this choice.
    evidence.pending_hidden = timeout_first ||
        (state->raw_primary() == nullptr && state->raw_late() == nullptr);
    state->commands.store(completed, std::memory_order_release);
    const auto* primary_failure = state->raw_primary();
    const auto* secondary_failure = state->raw_late();
    evidence.primary = timeout_first
        ? primary_failure == &*state->raw_failure &&
          core::detail::json_string_equal(primary_failure->context.at("classification"), "main_mailbox")
        : primary_failure == &*state->main_raw_failure &&
          primary_failure->context.get_ptr<const nlohmann::json::object_t*>() == original;
    evidence.secondary = timeout_first
        ? secondary_failure == &*state->main_raw_failure &&
          secondary_failure->context.get_ptr<const nlohmann::json::object_t*>() == original
        : secondary_failure == nullptr;
    evidence.immutable = evidence.immutable &&
        state->main_raw_failure->context.get_ptr<const nlohmann::json::object_t*>() == original;
    state.reset();
    evidence.charged = ledger.outstanding().ordinary_bytes == storage.ordinary_bytes &&
        ledger.outstanding().address_space_bytes == storage.address_space_bytes;
    if (!lease->retire()) std::terminate();
    denial(context, false);
    return evidence;
}

auto FrozenSourceControlOwner::initial_call_for_testing(FrozenSourceInitialCallProbeMode mode,
    core::Error actual, work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation,
    void* context, void (*denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceInitialCallProbe, ControlReason> {
    core::detail::JsonCleanup actual_cleanup{actual.context};
    if (!denial) return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false); }
    catch (...) { if (!lease->retire()) std::terminate(); return std::unexpected{ControlReason::construction}; }
    using Mode = FrozenSourceInitialCallProbeMode;
    const bool late = mode == Mode::late_core || mode == Mode::late_standard || mode == Mode::late_unknown ||
        mode == Mode::late_core_standard || mode == Mode::late_core_unknown || mode == Mode::late_success;
    const bool standard = mode == Mode::late_standard || mode == Mode::late_core_standard || mode == Mode::timely_standard;
    const bool unknown = mode == Mode::late_unknown || mode == Mode::late_core_unknown || mode == Mode::timely_unknown;
    const bool actual_before_throw = mode == Mode::late_core_standard || mode == Mode::late_core_unknown;
    const auto* original = actual.context.get_ptr<const nlohmann::json::object_t*>();
    state->main_invocation_end = 100U;
    FrozenSourceInitialCallProbe evidence{};
    struct Checks {
        FrozenSourceControlState& state;
        FrozenSourceInitialCallProbe& evidence;
        std::uint64_t now;
        bool mask;
        unsigned guards{};
    } checks{*state, evidence, mode == Mode::pre_expired ? 100U : 90U, mode != Mode::timely_core_mask};
    if (mode == Mode::pre_cancelled)
        static_cast<void>(cancellation.try_request(runtime::ipc::CancelReason::controller_shutdown));
    denial(context, true);
    try {
        auto result = state->on_main_checked([&]() -> std::expected<TypedSysnameDisposition, core::Error> {
            evidence.provider_called = true;
            evidence.guard_before_provider = checks.guards == 1U;
            if (late) checks.now = 100U;
            if (actual_before_throw) state->main_raw_failure.emplace(std::move(actual));
            if (standard) throw std::bad_alloc{};
            if (unknown) throw 7;
            if (mode == Mode::late_success) return TypedSysnameDisposition::linux;
            if (mode == Mode::late_core) return std::unexpected{std::move(actual)};
            state->main_raw_failure.emplace(std::move(actual));
            return std::unexpected{std::move(state->preparation_stock.value)};
        }, FrozenSourceControlState::MainCallChecks{&checks,
            +[](void* opaque) noexcept {
                auto& value = *static_cast<Checks*>(opaque); ++value.evidence.mask_checks;
                return value.mask || value.evidence.mask_checks == 1U;
            },
            +[](void* opaque, std::uint64_t end) noexcept {
                auto& value = *static_cast<Checks*>(opaque); ++value.guards;
                return value.state.sample_guard(end, true, value.now);
            }});
        core::detail::ExpectedErrorCleanup result_cleanup{result};
        if (!result) state->raw_failure.emplace(std::move(result.error()));
    } catch (const std::exception&) {
        state->retain_projected_failure(std::move(state->acquisition_stock.value));
    } catch (...) {
        state->retain_projected_failure(std::move(state->acquisition_unknown_stock.value));
    }
    const auto* primary_failure = state->raw_primary();
    const auto* late_failure = state->raw_late();
    evidence.primary_guard = primary_failure && !state->lower_primary && core::detail::json_string_equal(
        primary_failure->context.at("classification"), "source_acquisition_guard");
    evidence.lower_primary = state->lower_primary;
    evidence.lower_late = late_failure != nullptr;
    const auto* lower = state->main_raw_failure ? &*state->main_raw_failure : nullptr;
    evidence.actual_identity = lower && lower->context.get_ptr<const nlohmann::json::object_t*>() == original;
    evidence.actual_errno = lower && lower->os_error && lower->os_error->value == EIO;
    const auto* classified = lower ? lower : state->projected_failure ? &*state->projected_failure : nullptr;
    if (classified && !evidence.actual_identity) {
        evidence.standard = core::detail::json_string_equal(classified->context.at("classification"), "contained_exception");
        evidence.unknown = core::detail::json_string_equal(classified->context.at("classification"), "contained_unknown_failure");
    }
    const auto reason = state->failed();
    state->choose_main_failure(false);
    evidence.immutable = reason == state->failed() && state->raw_primary() == primary_failure && state->raw_late() == late_failure;
    evidence.reason = reason;
    core::detail::retire_json(actual.context);
    {
        FrozenSourceControlOwner first{std::move(state), std::move(*lease)};
        FrozenSourceControlOwner moved{std::move(first)};
        evidence.charged = ledger.outstanding().ordinary_bytes == storage.ordinary_bytes;
    }
    denial(context, false);
    return evidence;
}

auto FrozenSourceControlOwner::registration_for_testing(
    FrozenSourceRegistrationProbeMode mode, MonitoringHostInputTemplateAuthority input,
    RunnerContextAcquisitionOperations operations, const void* expected_context,
    work::ResourceLedger& ledger, runtime::RunnerCancellation& cancellation, void* context,
    void (*denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceRegistrationProbe, ControlReason> {
    if (!denial)
        return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease)
        return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try {
        state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false);
    } catch (...) {
        if (!lease->retire())
            std::terminate();
        return std::unexpected{ControlReason::construction};
    }
    state->operations.emplace(std::move(operations));
    state->main_invocation_end = 100U;
    FrozenSourceRegistrationProbe evidence{};
    using Mode = FrozenSourceRegistrationProbeMode;
    struct Bridge {
        FrozenSourceControlState& state;
        FrozenSourceRegistrationProbe& evidence;
        Mode mode;
        void* context;
        void (*denial)(void*, bool) noexcept;
        std::uint64_t now{90U};
        auto checks() noexcept -> FrozenSourceControlState::MainCallChecks {
            return {this,
                    +[](void* opaque) noexcept {
                        auto& value = *static_cast<Bridge*>(opaque);
                        ++value.evidence.mask_checks;
                        return value.mode != Mode::post_mask_refusal ||
                               value.evidence.mask_checks != 3U;
                    },
                    +[](void* opaque, std::uint64_t end) noexcept {
                        auto& value = *static_cast<Bridge*>(opaque);
                        ++value.evidence.guard_checks;
                        value.evidence.original_end = value.evidence.original_end && end == 100U;
                        return value.state.sample_guard(end, true, value.now);
                    }};
        }
        static auto registration(void* opaque) -> std::expected<void, core::Error> {
            auto& value = *static_cast<Bridge*>(opaque);
            value.state.composition_stage = HostFrozenCompositionStage::registration;
            value.evidence.nested = value.state.composing && value.state.main_call_depth == 1U;
            if (value.mode == Mode::cancel_before_hook)
                static_cast<void>(value.state.cancellation.try_request(
                    runtime::ipc::CancelReason::controller_shutdown));
            if (value.mode == Mode::expire_before_hook)
                value.now = 100U;
            auto result = value.state.on_main_checked(
                [&]() -> std::expected<void, core::Error> {
                    ++value.evidence.hook_calls;
                    value.evidence.nested =
                        value.evidence.nested && value.state.main_call_depth == 2U;
                    auto hook = Access::before_registration(*value.state.operations);
                    core::detail::ExpectedErrorCleanup hook_cleanup{hook};
                    if (value.mode == Mode::cancel_after_hook)
                        static_cast<void>(value.state.cancellation.try_request(
                            runtime::ipc::CancelReason::controller_shutdown));
                    if (value.mode == Mode::expire_after_hook)
                        value.now = 100U;
                    if (value.mode == Mode::core_projection_denial && !hook)
                        value.denial(value.context, true);
                    return decltype(hook)(std::move(hook));
                },
                value.checks());
            core::detail::ExpectedErrorCleanup result_cleanup{result};
            return decltype(result)(std::move(result));
        }
    } bridge{*state, evidence, mode, context, denial};
    evidence.original_token = &state->cancellation == &cancellation;
    evidence.original_end = true;
    const auto* inner_guard =
        state->acquisition_guard_stock.value.context.get_ptr<const nlohmann::json::object_t*>();
    const auto* outer_guard =
        state->composition_guard_stock.value.context.get_ptr<const nlohmann::json::object_t*>();
    evidence.distinct_stocks =
        inner_guard && outer_guard && inner_guard != outer_guard &&
        !state->acquisition_guard_stock.value.code.value().empty() &&
        !state->composition_guard_stock.value.code.value().empty() &&
        core::detail::json_string_equal(
            state->acquisition_guard_stock.value.context.at("classification"),
            "source_acquisition_guard") &&
        core::detail::json_string_equal(
            state->composition_guard_stock.value.context.at("classification"),
            "source_composition_guard");
    bool prepared{};
    std::optional<HostEnvironmentAuthority> environment;
    try {
        auto proxy = RunnerContextAcquisitionOperations::create(
            &bridge, +[](void*) noexcept {},
            +[](void* opaque) {
                return Access::acquire_context(*static_cast<Bridge*>(opaque)->state.operations);
            },
            +[](void* opaque) {
                return Access::observe_platform(*static_cast<Bridge*>(opaque)->state.operations);
            },
            +[](void* opaque, kernel::KernelId id) {
                return Access::find_descriptor(*static_cast<Bridge*>(opaque)->state.operations, id);
            },
            +[](void* opaque) {
                return Access::reobserve(*static_cast<Bridge*>(opaque)->state.operations);
            },
            Bridge::registration);
        core::detail::ExpectedErrorCleanup proxy_cleanup{proxy};
        if (proxy) {
            auto session = Access::acquire_with_fallible(std::move(input), std::move(*proxy));
            core::detail::ExpectedErrorCleanup session_cleanup{session};
            if (session) {
                state->session.emplace(std::move(*session));
                auto derived = derive_host_environment_fallible(*state->session);
                core::detail::ExpectedErrorCleanup derived_cleanup{derived};
                if (derived) {
                    environment.emplace(std::move(*derived));
                    prepared = true;
                }
            }
        }
        if (prepared) {
            state->composing = true;
            try {
                auto plan = state->on_main_checked(
                    [&] {
                        return detail_HostQualificationPlanAccess::resolve_fallible(
                            *state->session, *environment, &state->main_raw_failure,
                            &state->composition_stage);
                    },
                    bridge.checks());
                core::detail::ExpectedErrorCleanup plan_cleanup{plan};
                if (!plan)
                    state->retain_projected_failure(std::move(plan.error()));
            } catch (const std::bad_alloc&) {
                evidence.projection_denied = true;
                state->retain_projected_failure(state->take_composition_stock(false));
            } catch (const std::exception&) {
                state->retain_projected_failure(state->take_composition_stock(false));
            } catch (...) {
                state->retain_projected_failure(state->take_composition_stock(true));
            }
            state->composing = false;
            try {
                auto requirements =
                    detail_HostQualificationPlanAccess::requirements_fallible(*state->session);
                core::detail::ExpectedErrorCleanup requirements_cleanup{requirements};
                evidence.not_committed = !requirements;
            } catch (const std::bad_alloc&) {
                // This getter only allocates its invalid-session error when no
                // requirements were committed. Denial stays active through teardown.
                evidence.not_committed = true;
            }
            const auto* primary_failure = state->raw_primary();
            const auto* late_failure = state->raw_late();
            const auto* lower = state->main_raw_failure ? &*state->main_raw_failure : nullptr;
            evidence.lower_primary = state->lower_primary;
            evidence.lower_late = late_failure != nullptr;
            evidence.outer_guard =
                state->projected_failure &&
                state->projected_failure->context.get_ptr<const nlohmann::json::object_t*>() ==
                    outer_guard;
            evidence.lower_guard =
                lower && lower->context.get_ptr<const nlohmann::json::object_t*>() == inner_guard;
            evidence.actual_identity =
                lower && expected_context &&
                lower->context.get_ptr<const nlohmann::json::object_t*>() == expected_context;
            evidence.actual_errno = lower && lower->os_error && lower->os_error->value == EIO;
            evidence.reason = state->failed();
            const auto* projected =
                state->projected_failure
                    ? state->projected_failure->context.get_ptr<const nlohmann::json::object_t*>()
                    : nullptr;
            state->choose_main_failure(false);
            state->retain_projected_failure(std::move(state->acquisition_unknown_stock.value));
            evidence.immutable =
                evidence.reason == state->failed() && state->raw_primary() == primary_failure &&
                state->raw_late() == late_failure && state->projected_failure &&
                state->projected_failure->context.get_ptr<const nlohmann::json::object_t*>() ==
                    projected;
            evidence.original_end = evidence.original_end && state->main_invocation_end == 100U;
        }
    } catch (...) {
        prepared = false;
    }
    denial(context, true);
    environment.reset();
    {
        FrozenSourceControlOwner first{std::move(state), std::move(*lease)};
        FrozenSourceControlOwner moved{std::move(first)};
        evidence.charged = ledger.outstanding().ordinary_bytes == storage.ordinary_bytes &&
                           ledger.outstanding().address_space_bytes == storage.address_space_bytes;
    }
    denial(context, false);
    if (!prepared)
        return std::unexpected{ControlReason::construction};
    return evidence;
}

auto FrozenSourceControlOwner::linux_io_publication_for_testing(
    platform_linux::detail::PreparedTopologySourceIO prepared, work::ResourceLedger& ledger,
    runtime::RunnerCancellation& cancellation, void* context,
    void (*invoke)(void*, platform_linux::detail::PreparedTopologySourceIO&,
                   std::optional<core::Error>&),
    bool (*post_main)(void*) noexcept, void (*denial)(void*, bool) noexcept) noexcept
    -> std::expected<FrozenSourceLinuxIOProbe, ControlReason> {
    if (!invoke || !post_main || !denial) return std::unexpected{ControlReason::invalid_input};
    work::Resources storage{};
    storage.ordinary_bytes = core::ByteCount::from_base_units(sizeof(FrozenSourceControlState));
    storage.address_space_bytes = storage.ordinary_bytes;
    auto lease = ledger.reserve(storage);
    if (!lease) return std::unexpected{ControlReason::capacity};
    std::unique_ptr<FrozenSourceControlState> state;
    try { state = std::make_unique<FrozenSourceControlState>(5U, 1U, ledger, cancellation, false); }
    catch (...) { if (!lease->retire()) std::terminate(); return std::unexpected{ControlReason::construction}; }
    state->linux_io.emplace(std::move(prepared));
    auto* actual = state.get();
    FrozenSourceLinuxIOProbe evidence{};
    {
        FrozenSourceControlOwner first{std::move(state), std::move(*lease)};
        actual->commands.store(requested, std::memory_order_release);
        actual->mailbox.store(executing, std::memory_order_release);
        try { invoke(context, *actual->linux_io, actual->main_raw_failure); }
        catch (const std::exception&) { actual->retain_projected_failure(std::move(actual->acquisition_stock.value)); }
        catch (...) { actual->retain_projected_failure(std::move(actual->acquisition_unknown_stock.value)); }
        static_cast<void>(actual->linux_io->finish());
        actual->publish_main_failure(); // publication is refused before the synthetic post-main result
        evidence.unpublished_hidden = !actual->linux_failure_published.load(std::memory_order_acquire) && !first.raw_linux_source_failure();
        actual->main_post_checks_observed = post_main(context);
        actual->publish_main_failure();
        evidence.pending_hidden = !first.raw_linux_source_failure() && first.raw_linux_source_cleanup().empty();
        actual->commands.store(completed, std::memory_order_release);
        evidence.executing_hidden = !first.raw_linux_source_failure();
        actual->mailbox.store(completed, std::memory_order_release);
        const auto* failure = first.raw_linux_source_failure();
        evidence.published = failure != nullptr;
        if (failure) evidence.primary = *failure;
        const auto facts = first.raw_linux_source_cleanup();
        evidence.cleanup_count = facts.size();
        std::copy(facts.begin(), facts.end(), evidence.cleanup.begin());
        const auto before = failure;
        actual->publish_main_failure();
        evidence.immutable = before && first.raw_linux_source_failure() == before;
        evidence.charged = ledger.outstanding().descriptors == 6 && ledger.outstanding().ordinary_bytes.value() >= storage.ordinary_bytes.value();
        denial(context, true);
        FrozenSourceControlOwner moved{std::move(first)};
    } // Same actual State, Core diagnostics and prepared IO owner die under denial.
    denial(context, false);
    return evidence;
}
auto FrozenSourceControlOwner::raw_linux_source_failure() const noexcept
    -> const platform_linux::detail::TopologySourceFailure* {
    if (!state_ || !state_->linux_io || !state_->linux_failure_published.load(std::memory_order_acquire)) return nullptr;
    const auto command = state_->commands.load(std::memory_order_acquire);
    const auto mailbox = state_->mailbox.load(std::memory_order_acquire);
    if (command == requested || command == executing || mailbox == executing) return nullptr;
    return state_->linux_io->failure();
}
auto FrozenSourceControlOwner::raw_linux_source_cleanup() const noexcept
    -> std::span<const platform_linux::detail::TopologySourceCloseFact> {
    return raw_linux_source_failure() ? state_->linux_io->cleanup()
        : std::span<const platform_linux::detail::TopologySourceCloseFact>{};
}
auto FrozenSourceControlOwner::retire(MonitoringQ2RawDeadline deadline) noexcept -> std::expected<void, ControlReason> {
    if (!state_ || !state_->native.on_original_main()) return std::unexpected{ControlReason::invalid_input};
    auto& state = *state_;
    if (state.retired) return {};
    if (state.retirement_attempts >= 2U) return std::unexpected{ControlReason::retirement};
    ++state.retirement_attempts;
    deadline.nanoseconds = std::min(deadline.nanoseconds, state.protocol.phase_deadline_);
    // Cancellation is the actual Run token, even while a mailbox response is pending.
    const auto terminal_snapshot = state.collection ? state.collection->snapshot() : std::nullopt;
    // The genuine FINISH ACK precedes terminal creation. Creating that one-use
    // record advances finished to dead before caller-owned cleanup and send.
    // Both completed states stop the control thread without cancelling the
    // enclosing token needed to transmit its terminal retirement record.
    const bool terminal_protocol = state.protocol.state_ == FrozenProcessChild::State::finished ||
        state.protocol.state_ == FrozenProcessChild::State::dead;
    const bool completed_phase = terminal_protocol &&
        terminal_snapshot && terminal_snapshot->completed && !terminal_snapshot->outstanding &&
        !state.cell && state.commands.load(std::memory_order_acquire) == completed &&
        state.mailbox.load(std::memory_order_acquire) != executing && !state.failed();
    if (!completed_phase)
        static_cast<void>(state.cancellation.try_request(runtime::ipc::CancelReason::controller_shutdown));
    if (state.thread) state.thread->request_stop();
    monitoring_q2_atomic_wake(state.commands); monitoring_q2_atomic_wake(state.mailbox);
    static_cast<void>(state.publish_reobservation());
    if (state.thread) {
        auto result = state.thread->timed_join({deadline, {}});
        auto cleanup = state.thread->take_cleanup_diagnostics();
        if (!result) state.native_cleanup.append(result.error());
        for (std::size_t i = 0; i < cleanup.size(); ++i) state.native_cleanup.append(cleanup[i]);
        if (!result) {
            // Joined does not imply all sync cleanup succeeded; timed_join's
            // success is the complete retirement certification of this owner.
            state.fail(ControlReason::retirement); return std::unexpected{ControlReason::retirement};
        }
        state.thread.reset(); // Destroy actual native State before releasing backing/charge.
    }
    if (state.collection_failure && state.collection_failure->charge_retained()) {
        state.fail(ControlReason::retirement);
        return std::unexpected{ControlReason::retirement};
    }
    state.cell.reset(); state.collection_failure.reset(); state.collection.reset();
    retire_observation(state.response);
    state.inputs.reset();
    state.source_composition.reset();
    state.session.reset();
    state.operations.reset();
    // Raw diagnostic borrows remain valid until this same State dies.
    if (!state.native.retire()) { state.fail(ControlReason::retirement); return std::unexpected{ControlReason::retirement}; }
    if (state.linux_io && state.linux_io->uncertain()) {
        // The consuming close tokens are gone, but that is not confirmation
        // that their native resources retired. Keep the prepared lease and
        // recovery owner until actual destruction poisons its enclosing charge.
        state.fail(ControlReason::retirement);
        return std::unexpected{ControlReason::retirement};
    }
    state.retired = true;
    return {};
}
} // namespace epycbench::monitoring::qualification::detail
