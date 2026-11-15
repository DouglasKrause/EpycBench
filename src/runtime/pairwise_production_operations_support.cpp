#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"
#include "runtime/kernel_module_session.hpp"
#include "runtime/pairwise_production_operations_support.hpp"
#include "memory/region_finalization_diagnostic_access.hpp"
#include "epycbench/kernel/region_initialization.hpp"
#include "epycbench/memory/region_binding.hpp"
#include "epycbench/memory/region_finalization.hpp"
#include "epycbench/memory/region_initialization.hpp"
#include "epycbench/memory/region_mapping.hpp"
#include "epycbench/memory/region_placement_evaluation.hpp"
#include "epycbench/memory/region_placement_query.hpp"
#include "epycbench/memory/region_protection.hpp"
#include "epycbench/memory/retained_region_cleanup.hpp"
#include "epycbench/runtime/persistent_worker_team.hpp"
#include "memory/region_binding_access.hpp"
#include "memory/region_finalization_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_query_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_protection_access.hpp"
#include "memory/retained_region_cleanup_access.hpp"
#include "runtime/worker_team_state.hpp"
#include "runtime/pairwise_production_operations_adapter_access.hpp"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace epycbench::runtime {
namespace {

std::atomic_flag adapter_fault_guard = ATOMIC_FLAG_INIT;
std::optional<detail::ProductionAdapterFaultScript> adapter_fault_script;
std::optional<std::uint32_t> calibration_stop_failure_worker;
std::optional<core::Error> adapter_fault_error;
std::atomic<std::uint64_t> adapter_prepare_invocation{};
std::atomic<void (*)() noexcept> emergency_cleanup_complete_hook{};
std::atomic<void (*)() noexcept> reportable_cleanup_begin_hook{};
std::atomic<void (*)() noexcept> reportable_cleanup_complete_hook{};
thread_local std::uint32_t lower_memory_attribution_depth{};
thread_local bool reportable_cleanup_scope_active{};

using LegacyProductionOperationFailure = detail::LegacyProductionOperationFailure;

template <typename T>
using LegacyProductionOperationResult = detail::LegacyProductionOperationResult<T>;

void begin_reportable_cleanup_scope() noexcept {
    if (reportable_cleanup_scope_active) return;
    reportable_cleanup_scope_active = true;
    if (const auto hook = reportable_cleanup_begin_hook.load()) hook();
}

void complete_reportable_cleanup_scope() noexcept {
    if (!reportable_cleanup_scope_active) return;
    if (const auto hook = reportable_cleanup_complete_hook.load()) hook();
    reportable_cleanup_scope_active = false;
}

class LowerMemoryAllocationAttribution final {
  public:
    LowerMemoryAllocationAttribution() noexcept { ++lower_memory_attribution_depth; }
    ~LowerMemoryAllocationAttribution() noexcept { --lower_memory_attribution_depth; }
    LowerMemoryAllocationAttribution(const LowerMemoryAllocationAttribution&) = delete;
};

class InjectedAdapterStandardException final : public std::exception {
  public:
    auto what() const noexcept -> const char* override { return "injected adapter fault"; }
};

auto failure(core::Error error, std::vector<core::Error> secondary = {})
    -> LegacyProductionOperationFailure {
    return {std::move(error), std::move(secondary)};
}

auto adapter_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.production_backend.invariant_failed"},
            .message = "Production operation adapter invariant failed",
            .operation = "runtime.production_operations",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

} // namespace

auto detail::production_adapter_boundary_error(const char* classification) -> core::Error {
    return adapter_error(classification);
}

namespace {

auto inject_adapter_fault(const void* context, std::uint64_t invocation,
                          detail::ProductionAdapterPreparationStage stage)
    -> std::expected<void, core::Error> {
    if (!adapter_fault_script || adapter_fault_script->context != context ||
        adapter_fault_script->invocation != invocation || adapter_fault_script->stage != stage)
        return {};
    const auto mode = adapter_fault_script->mode;
    adapter_fault_script.reset();
    begin_reportable_cleanup_scope();
    if (mode == detail::ProductionAdapterFaultMode::returned_error) {
        auto value = std::move(*adapter_fault_error);
        adapter_fault_error.reset();
        return std::unexpected{std::move(value)};
    }
    if (mode == detail::ProductionAdapterFaultMode::standard_exception)
        throw InjectedAdapterStandardException{};
    if (mode == detail::ProductionAdapterFaultMode::unknown_exception) throw 1;
    if (mode == detail::ProductionAdapterFaultMode::allocation_exception) throw std::bad_alloc{};
    return {};
}

auto forced_team_error(detail::ForcedTeamStopErrorKind kind) -> core::Error {
    std::string code{"runtime.worker_team.stop_failed"};
    std::string classification{"forced_join_failed"};
    if (kind == detail::ForcedTeamStopErrorKind::contained_exception) {
        code = "runtime.worker_team.contained_exception";
        classification = "contained_exception";
    } else if (kind == detail::ForcedTeamStopErrorKind::contained_unknown_failure) {
        code = "runtime.worker_team.contained_unknown_failure";
        classification = "contained_unknown_failure";
    } else if (kind == detail::ForcedTeamStopErrorKind::capacity_exhausted) {
        code = "runtime.worker_team.invariant_failed";
        classification = "force_error_capacity_exhausted";
    }
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Persistent worker-team operation failed",
            .operation = "worker_join",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

enum class CleanupBoundaryDisposition : std::uint8_t {
    none,
    contained_exception,
    contained_unknown
};

// All storage which can be written while a lower owner is active is acquired before
// PersistentWorkerTeam creation.  In particular, retaining a moved Error or a force
// disposition cannot grow a container.  Error projection is deliberately separate and
// is used only after the team has proved inactive.
class CleanupLedger final {
  public:
    explicit CleanupLedger(std::size_t worker_count)
        : capacity_{worker_count * 6U + 6U},
          error_capacity_{worker_count * 4U + 3U},
          errors_{std::make_unique<std::optional<core::Error>[]>(error_capacity_)},
          force_{std::make_unique<detail::ForcedTeamStopErrorKind[]>(worker_count * 2U + 3U)},
          force_capacity_{worker_count * 2U + 3U},
          overflow_error_{forced_team_error(
              detail::ForcedTeamStopErrorKind::capacity_exhausted)},
          prepare_contained_{adapter_error("contained_exception")},
          prepare_unknown_{adapter_error("contained_unknown_failure")} {
        projected_.reserve(capacity_);
        generic_stock_.reserve(capacity_);
        for (std::size_t index = 0; index < capacity_; ++index)
            generic_stock_.emplace_back();
        force_stock_.reserve(force_capacity_);
        for (std::size_t index = 0; index < force_capacity_; ++index)
            force_stock_.emplace_back();
    }

    CleanupLedger(const CleanupLedger&) = delete;
    CleanupLedger(CleanupLedger&&) = delete;

    void retain(core::Error error) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<core::Error>);
        if (error_count_ < error_capacity_ - 1U) {
            errors_[error_count_++].emplace(std::move(error));
            return;
        }
        overflowed_ = true;
    }

    void retain_force(detail::ForcedTeamStopErrorKind kind) noexcept {
        if (force_count_ < force_capacity_) force_[force_count_++] = kind;
        else overflowed_ = true;
    }

    void retain_boundary(CleanupBoundaryDisposition disposition) noexcept {
        if (boundary_ == CleanupBoundaryDisposition::none) boundary_ = disposition;
        else overflowed_ = true;
    }

    auto take_prepare_primary(CleanupBoundaryDisposition disposition) noexcept -> core::Error {
        if (prepare_primary_taken_ || disposition == CleanupBoundaryDisposition::none)
            std::terminate();
        prepare_primary_taken_ = true;
        return disposition == CleanupBoundaryDisposition::contained_exception
                   ? std::move(prepare_contained_)
                   : std::move(prepare_unknown_);
    }

    void append(core::Error error) noexcept {
        if (projected_.size() < capacity_ - 1U) projected_.push_back(std::move(error));
        else overflowed_ = true;
    }

    void append_contained(CleanupBoundaryDisposition disposition) noexcept {
        if (disposition == CleanupBoundaryDisposition::none) return;
        if (generic_used_ >= generic_stock_.size()) {
            overflowed_ = true;
            return;
        }
        auto& stock = generic_stock_[generic_used_++];
        append(disposition == CleanupBoundaryDisposition::contained_exception
                   ? std::move(stock.contained)
                   : std::move(stock.unknown));
    }

    template <class AppendMemory>
    auto project(CleanupBoundaryDisposition monitoring_boundary,
                 std::optional<core::Error>& monitoring_error,
                 AppendMemory&& append_memory,
                 std::optional<core::Error>& team_error,
                 bool monitoring_first) noexcept -> std::vector<core::Error> {
        const auto append_monitoring = [&] {
            if (monitoring_error) append(std::move(*monitoring_error));
            else append_contained(monitoring_boundary);
        };
        if (monitoring_first) append_monitoring();
        append_memory(*this);
        if (!monitoring_first) append_monitoring();
        if (team_error) append(std::move(*team_error));
        for (std::size_t index = 0; index < force_count_; ++index) {
            auto& stock = force_stock_[index];
            switch (force_[index]) {
            case detail::ForcedTeamStopErrorKind::join_failed:
                append(std::move(stock.join)); break;
            case detail::ForcedTeamStopErrorKind::contained_exception:
                append(std::move(stock.contained)); break;
            case detail::ForcedTeamStopErrorKind::contained_unknown_failure:
                append(std::move(stock.unknown)); break;
            case detail::ForcedTeamStopErrorKind::capacity_exhausted:
                append(std::move(stock.overflow)); break;
            }
        }
        for (std::size_t index = 0; index < error_count_; ++index)
            if (errors_[index]) append(std::move(*errors_[index]));
        if (overflowed_) {
            if (projected_.size() < capacity_)
                projected_.push_back(std::move(overflow_error_));
            else
                std::terminate();
        } else if (boundary_ != CleanupBoundaryDisposition::none &&
                   generic_used_ < generic_stock_.size() && projected_.size() < capacity_) {
            auto& stock = generic_stock_[generic_used_++];
            projected_.push_back(boundary_ == CleanupBoundaryDisposition::contained_exception
                                     ? std::move(stock.contained)
                                     : std::move(stock.unknown));
        } else if (boundary_ != CleanupBoundaryDisposition::none) {
            std::terminate();
        }
        return std::move(projected_);
    }

    void discard() noexcept {
        for (std::size_t index = 0; index < error_count_; ++index) errors_[index].reset();
        error_count_ = 0;
        force_count_ = 0;
        boundary_ = CleanupBoundaryDisposition::none;
        overflowed_ = false;
    }

  private:
    struct GenericStock {
        GenericStock()
            : contained{adapter_error("contained_exception")},
              unknown{adapter_error("contained_unknown_failure")} {}
        core::Error contained;
        core::Error unknown;
    };
    struct ForceStock {
        ForceStock()
            : join{forced_team_error(detail::ForcedTeamStopErrorKind::join_failed)},
              contained{forced_team_error(
                  detail::ForcedTeamStopErrorKind::contained_exception)},
              unknown{forced_team_error(
                  detail::ForcedTeamStopErrorKind::contained_unknown_failure)},
              overflow{forced_team_error(
                  detail::ForcedTeamStopErrorKind::capacity_exhausted)} {}
        core::Error join;
        core::Error contained;
        core::Error unknown;
        core::Error overflow;
    };
    std::size_t capacity_{};
    std::size_t error_capacity_{};
    std::unique_ptr<std::optional<core::Error>[]> errors_;
    std::unique_ptr<detail::ForcedTeamStopErrorKind[]> force_;
    std::size_t force_capacity_{};
    std::size_t error_count_{};
    std::size_t force_count_{};
    std::vector<core::Error> projected_;
    std::vector<GenericStock> generic_stock_;
    std::vector<ForceStock> force_stock_;
    std::size_t generic_used_{};
    core::Error overflow_error_;
    core::Error prepare_contained_;
    core::Error prepare_unknown_;
    bool prepare_primary_taken_{};
    CleanupBoundaryDisposition boundary_{CleanupBoundaryDisposition::none};
    bool overflowed_{};
};

struct TeamCleanupState {
    std::optional<core::Error> ordinary_error;
};

void stop_team_inactive(PersistentWorkerTeam& team, CleanupLedger& ledger,
                        TeamCleanupState& state) noexcept {
    try {
        auto stopped = team.stop_and_join();
        if (!stopped) state.ordinary_error.emplace(std::move(stopped.error()));
    } catch (const std::exception&) {
        ledger.retain_boundary(CleanupBoundaryDisposition::contained_exception);
    } catch (...) {
        ledger.retain_boundary(CleanupBoundaryDisposition::contained_unknown);
    }
    if (!team.active()) return;
    const auto forced = detail::PersistentWorkerTeamAccess::force_stop_and_join_after_failure(team);
    for (const auto& item : forced.errors) ledger.retain_force(item.kind);
    if (forced.disposition == detail::ForcedTeamStopDisposition::active) std::terminate();
}

template <typename T>
auto failed(std::expected<T, core::Error> value)
    -> LegacyProductionOperationResult<T> {
    if (!value) return std::unexpected{failure(std::move(value.error()))};
    return std::move(*value);
}

struct MemoryOperations {
    memory::detail::RegionMappingOperations mapping;
    memory::detail::RegionBindingOperations binding;
    memory::detail::RegionInitializationOperations initialization;
    memory::detail::RegionPlacementQueryOperations query;
    memory::detail::RegionProtectionOperations protection;
};

auto memory_operations(PairwiseProductionHostOperations& host) -> MemoryOperations {
    return {{host.context.get(), host.map},
            {host.context.get(), host.numa_mask, host.bind},
            {host.context.get(), host.write_prefault_byte},
            {host.context.get(), host.query_pages},
            {host.context.get(), host.protect_read_only}};
}

auto worker_dependencies(PairwiseProductionHostOperations& host)
    -> detail::WorkerTeamDependencies {
    return {{host.context.get(), host.current_thread_id, host.set_current_thread_affinity,
             host.get_current_thread_affinity, host.current_cpu},
            {host.context.get(), host.launch_worker}};
}

auto initializer_thunk(void* opaque, std::span<std::byte> bytes)
    -> std::expected<void, core::Error> {
    return static_cast<kernel::PreparedRegionInitializer*>(opaque)->initialize(bytes);
}

auto project_request(const AdmittedPlacementRequest& placement, std::size_t worker_index)
    -> std::expected<memory::RegionRequest, core::Error> {
    const auto& admitted = placement.workers()[worker_index];
    const auto& source = admitted.request().region();
    auto threshold = memory::ExactRatio::create(source.placement_threshold().numerator(),
                                                 source.placement_threshold().denominator());
    if (!threshold) return std::unexpected{threshold.error()};
    std::vector<memory::RegionNumaTarget> targets;
    targets.reserve(source.nodes().size());
    for (std::size_t index = 0; index < source.nodes().size(); ++index) {
        targets.push_back({source.nodes()[index].stable_numa_node_id(),
                           source.nodes()[index].linux_numa_node(), admitted.numa_nodes()[index]});
    }
    return memory::RegionRequest::create(
        {.worker_id = core::WorkerId{static_cast<std::uint32_t>(worker_index)},
         .requested_bytes = source.bytes(),
         .page_size = source.page_size_bytes(),
         .placement_policy = memory::PlacementPolicy::bind,
         .page_mode = memory::PageMode::base,
         .numa_targets = std::move(targets),
         .placement_threshold = *threshold,
         .prefault = source.prefault(),
         .verify_before = source.verify_before(),
         .verify_after = source.verify_after()});
}

struct PreparedVariantMemory {
    std::vector<std::optional<memory::ReadOnlyRegion>> regions;
};

struct PrepareMemoryContext {
    PairwiseProductionHostOperations* host;
    const AdmittedPlacementRequest* placement;
    kernel::PreparedRegionInitializer* initializer;
    PreparedVariantMemory* destination;
    struct CleanupSlots { std::optional<core::Error> primary; std::optional<core::Error> cleanup; };
    std::vector<CleanupSlots>* cleanup_slots;
    platform_linux::detail::NumaOperations* diagnostic{};
    platform_linux::detail::NumaDomainMaxNode* domain{};
    std::vector<platform_linux::detail::AllowedMemsWorkspace>* workspaces{};
    std::vector<platform_linux::detail::NumaBindInvocationFacts>* invocation_facts{};
    const detail::LinuxDiagnosticPreparationOverrides* overrides{};
    void* workspace_observer_context{};
    void (*workspace_observer)(void*, std::size_t, bool,
                               const platform_linux::detail::AllowedMemsWorkspace*) noexcept{};
    void (*facts_observer)(void*, std::size_t, bool,
                           const platform_linux::detail::NumaBindInvocationFacts*) noexcept{};
    bool second_variant{};
};
struct DiagnosedBindingContext {
    PairwiseProductionHostOperations* host;
    platform_linux::detail::NumaOperations* diagnostic;
    const detail::LinuxDiagnosticPreparationOverrides* overrides;
};

auto diagnosed_mask(void* opaque, const core::LinuxNumaNodeSet& nodes)
    -> std::expected<platform_linux::NativeNumaMask, core::Error> {
    auto& context = *static_cast<DiagnosedBindingContext*>(opaque);
    return context.host->numa_mask(context.host->context.get(), nodes);
}

void diagnosed_projection_fault(void* opaque) {
    const auto& context = *static_cast<DiagnosedBindingContext*>(opaque);
    if (context.overrides->projection_fault == 1) {
        throw std::runtime_error{"injected diagnosed projection failure"};
    }
    if (context.overrides->projection_fault == 2) throw 1;
}

auto diagnosed_bind(void* opaque, const platform_linux::AnonymousMapping& mapping,
                    const platform_linux::NativeNumaMask& nodes,
                    platform_linux::detail::NumaDomainMaxNode domain,
                    const platform_linux::detail::NumaBindInvocationFacts& facts,
                    platform_linux::detail::AllowedMemsWorkspace& workspace) noexcept
    -> std::expected<void, platform_linux::detail::NumaBindFailure> {
    try {
        auto& context = *static_cast<DiagnosedBindingContext*>(opaque);
        return platform_linux::detail::NumaAccess::bind_range_diagnosed(
            const_cast<std::byte*>(mapping.bytes().data()), mapping.size(), nodes, domain,
            facts, workspace, *context.diagnostic);
    } catch (const std::exception&) {
        return std::unexpected{
            platform_linux::detail::NumaAccess::take_contained(workspace, true)};
    } catch (...) {
        return std::unexpected{
            platform_linux::detail::NumaAccess::take_contained(workspace, false)};
    }
}

template <typename Owner>
void cleanup_retained(Owner&& owner, std::optional<core::Error>& slot) {
    auto cleaned = [&] {
        LowerMemoryAllocationAttribution attribution;
        return memory::RetainedRegionCleanup::cleanup(std::forward<Owner>(owner));
    }();
    if (!cleaned) {
        auto failure = std::move(cleaned.error());
        slot.emplace(memory::detail::RetainedRegionCleanupAccess::take_error(failure));
    }
}

auto prepare_memory_worker(void* opaque, const WorkerExecutionContext& worker,
                           const WorkerExecutionControl&) -> std::expected<void, core::Error> {
    auto& context = *static_cast<PrepareMemoryContext*>(opaque);
    if (worker.worker_index >= context.destination->regions.size() ||
        worker.worker_index >= context.cleanup_slots->size())
        return std::unexpected{adapter_error("memory_worker_index_out_of_range")};
    auto operations = memory_operations(*context.host);
    DiagnosedBindingContext diagnosed{context.host, context.diagnostic, context.overrides};
    if (context.diagnostic != nullptr) {
        if (context.domain == nullptr || context.workspaces == nullptr ||
            context.invocation_facts == nullptr ||
            worker.worker_index >= context.workspaces->size() ||
            worker.worker_index >= context.invocation_facts->size()) {
            return std::unexpected{adapter_error("diagnostic_binding_state_invalid")};
        }
        operations.binding = {.context = &diagnosed,
                              .create_mask = diagnosed_mask,
                              .bind = nullptr,
                              .bind_diagnosed = diagnosed_bind,
                              .domain = context.domain,
                              .workspace = &(*context.workspaces)[worker.worker_index],
                              .invocation_facts =
                                  &(*context.invocation_facts)[worker.worker_index],
                              .before_diagnosed_projection =
                                  context.overrides->projection_fault != 0
                                      ? diagnosed_projection_fault
                                      : nullptr};
        if (context.workspace_observer != nullptr) {
            context.workspace_observer(context.workspace_observer_context, worker.worker_index,
                                       context.second_variant,
                                       &(*context.workspaces)[worker.worker_index]);
        }
        if (context.facts_observer != nullptr) {
            context.facts_observer(context.workspace_observer_context, worker.worker_index,
                                   context.second_variant,
                                   &(*context.invocation_facts)[worker.worker_index]);
        }
    }
    auto request = project_request(*context.placement, worker.worker_index);
    if (!request) return std::unexpected{request.error()};
    auto mapping = memory::detail::RegionMappingAccess::create_with_operations(
        std::move(*request), operations.mapping);
    if (!mapping) return std::unexpected{mapping.error().primary()};
    auto bound = memory::detail::BoundRegionAccess::create_with_operations(
        std::move(*mapping), operations.binding);
    if (!bound) {
        auto error = bound.error().error();
        cleanup_retained(std::move(bound.error()).take_mapping(),
                         (*context.cleanup_slots)[worker.worker_index].primary);
        return std::unexpected{std::move(error)};
    }
    auto initializer = memory::RegionInitializer::create(context.initializer, initializer_thunk);
    if (!initializer) {
        cleanup_retained(std::move(*bound),
                         (*context.cleanup_slots)[worker.worker_index].primary);
        return std::unexpected{initializer.error()};
    }
    auto initialized = memory::detail::InitializedRegionAccess::create_with_operations(
        std::move(*bound), *initializer, operations.initialization);
    if (!initialized) {
        auto error = initialized.error().error();
        cleanup_retained(std::move(initialized.error()).take_region(),
                         (*context.cleanup_slots)[worker.worker_index].primary);
        return std::unexpected{std::move(error)};
    }
    auto observed = memory::detail::PlacementObservedRegionAccess::create_with_operations(
        std::move(*initialized), operations.query);
    if (!observed) {
        auto error = observed.error().error();
        cleanup_retained(std::move(observed.error()).take_region(),
                         (*context.cleanup_slots)[worker.worker_index].primary);
        return std::unexpected{std::move(error)};
    }
    auto evaluated = memory::PlacementEvaluatedRegion::create(std::move(*observed));
    if (!evaluated) {
        auto error = evaluated.error().error();
        cleanup_retained(std::move(evaluated.error()).take_region(),
                         (*context.cleanup_slots)[worker.worker_index].primary);
        return std::unexpected{std::move(error)};
    }
    if (!evaluated->execution_eligible()) {
        auto finalized = memory::FinalizedRegion::finalize_unprotected(
            std::move(*evaluated), memory::UnprotectedFinalizationReason::execution_ineligible);
        if (!finalized) {
            auto failed = std::move(finalized.error());
            (*context.cleanup_slots)[worker.worker_index].primary.emplace(
                memory::detail::RegionFinalizationAccess::take_primary_error(failed));
            (*context.cleanup_slots)[worker.worker_index].cleanup =
                memory::detail::RegionFinalizationAccess::take_cleanup_error(failed);
        }
        return std::unexpected{adapter_error("pre_run_not_qualified")};
    }
    auto protected_region = memory::detail::ReadOnlyRegionAccess::create_with_operations(
        std::move(*evaluated), operations.protection);
    if (!protected_region) {
        auto error = protected_region.error().error();
        auto finalized = memory::FinalizedRegion::finalize_unprotected(
            std::move(protected_region.error()).take_region(),
            memory::UnprotectedFinalizationReason::protection_failed);
        if (!finalized) {
            auto failed = std::move(finalized.error());
            (*context.cleanup_slots)[worker.worker_index].primary.emplace(
                memory::detail::RegionFinalizationAccess::take_primary_error(failed));
            (*context.cleanup_slots)[worker.worker_index].cleanup =
                memory::detail::RegionFinalizationAccess::take_cleanup_error(failed);
        }
        return std::unexpected{std::move(error)};
    }
    context.destination->regions[worker.worker_index].emplace(std::move(*protected_region));
    return {};
}

auto first_worker_error(const WorkerTeamCommandOutcome& outcome) -> std::optional<core::Error> {
    for (const auto& worker : outcome.workers)
        if (!worker.result) return worker.result.error();
    return std::nullopt;
}

using BandwidthPrepared = std::vector<std::optional<kernel::PreparedSequentialReadBandwidth>>;
struct LatencyPreparedWorker {
    kernel::PreparedDependentLoadLatency kernel;
    kernel::PreparedDependentLoadLatencyVerification verification;
};
using LatencyPrepared = std::vector<std::optional<LatencyPreparedWorker>>;

struct BandwidthResources {
    kernel::SequentialReadBandwidthConfig configuration;
    PreparedVariantMemory first_memory;
    PreparedVariantMemory second_memory;
    BandwidthPrepared first_kernel;
    BandwidthPrepared second_kernel;
};
struct LatencyResources {
    kernel::DependentLoadLatencyConfig configuration;
    std::uint64_t seed;
    PreparedVariantMemory first_memory;
    PreparedVariantMemory second_memory;
    LatencyPrepared first_kernel;
    LatencyPrepared second_kernel;
};
using FamilyResources = std::variant<BandwidthResources, LatencyResources>;

template <typename Work, typename WorkerEvidence> struct ExecuteCommand {
    PairwiseProductionHostOperations* host;
    Work work;
    kernel::DependentLoadLatencyBlockId block_id{0};
    core::ExperimentId experiment_id{0};
    std::uint64_t seed{};
    void* prepared{};
    std::vector<std::optional<WorkerEvidence>> results;
};

auto execute_bandwidth_worker(void* opaque, const WorkerExecutionContext& worker,
                              const WorkerExecutionControl& control)
    -> std::expected<void, core::Error> {
    using Evidence = measurement::SequentialReadBandwidthWorkerObservation;
    auto& command = *static_cast<ExecuteCommand<kernel::SequentialReadBandwidthPassCount, Evidence>*>(opaque);
    auto& prepared = (*static_cast<BandwidthPrepared*>(command.prepared))[worker.worker_index];
    auto start = command.host->clock.now(command.host->clock.context);
    if (!start) return std::unexpected{start.error()};
    const auto terminal = kernel::execute_sequential_read_bandwidth(*prepared, command.work);
    auto end = command.host->clock.now(command.host->clock.context);
    if (!end) return std::unexpected{end.error()};
    control.signal_timed_completion();
    auto measured = measurement::MeasurementInterval::create(*start, *end);
    if (!measured) return std::unexpected{measured.error()};
    auto work = kernel::account_sequential_read_bandwidth_work(*prepared, command.work);
    if (!work) return std::unexpected{work.error()};
    const auto checksum = kernel::PreparedSequentialReadBandwidthChecksum::prepare(*prepared);
    auto verification = kernel::verify_sequential_read_bandwidth_checksum(
        checksum.expected_terminal(command.work), terminal);
    if (!module_verification_matches(*prepared, verification))
        return std::unexpected{adapter_error("kernel_module_verification")};
    command.results[worker.worker_index].emplace(Evidence::create(
        worker.worker_id, *measured, *work, verification));
    return {};
}

auto execute_latency_worker(void* opaque, const WorkerExecutionContext& worker,
                            const WorkerExecutionControl& control)
    -> std::expected<void, core::Error> {
    using Evidence = measurement::DependentLoadLatencyWorkerObservation;
    auto& command = *static_cast<ExecuteCommand<kernel::DependentLoadLatencyStepCount, Evidence>*>(opaque);
    auto& prepared = (*static_cast<LatencyPrepared*>(command.prepared))[worker.worker_index];
    auto schedule = kernel::DependentLoadLatencySchedule::create(
        prepared->kernel, command.seed, command.experiment_id, command.block_id, command.work);
    if (!schedule) return std::unexpected{schedule.error()};
    auto start = command.host->clock.now(command.host->clock.context);
    if (!start) return std::unexpected{start.error()};
    const auto terminal = kernel::execute_dependent_load_latency(prepared->kernel, *schedule);
    auto end = command.host->clock.now(command.host->clock.context);
    if (!end) return std::unexpected{end.error()};
    control.signal_timed_completion();
    auto measured = measurement::MeasurementInterval::create(*start, *end);
    if (!measured) return std::unexpected{measured.error()};
    auto verification = kernel::verify_dependent_load_latency_terminal(
        prepared->verification.expected_terminal(*schedule), terminal);
    if (!module_verification_matches(prepared->kernel, verification))
        return std::unexpected{adapter_error("kernel_module_verification")};
    command.results[worker.worker_index].emplace(Evidence::create(
        worker.worker_id, *measured, *schedule, verification));
    return {};
}

template <typename Command, typename Evidence> struct ActiveGeneration {
    Command* command;
    PersistentWorkerGeneration generation;
    std::vector<measurement::WorkerMigrationValidityEvidence> migrations;
    monitoring::MonitoringAcquisitionDriver* monitoring;
    std::optional<monitoring::MonitoringObservationCapture> capture;
    std::optional<core::Error> capture_error;
};

using BandwidthCommand = ExecuteCommand<kernel::SequentialReadBandwidthPassCount,
                                        measurement::SequentialReadBandwidthWorkerObservation>;
using LatencyCommand = ExecuteCommand<kernel::DependentLoadLatencyStepCount,
                                      measurement::DependentLoadLatencyWorkerObservation>;
using BandwidthGeneration = ActiveGeneration<BandwidthCommand,
    measurement::SequentialReadBandwidthWorkerObservation>;
using LatencyGeneration = ActiveGeneration<LatencyCommand,
    measurement::DependentLoadLatencyWorkerObservation>;

template <typename Generation, typename Evidence>
auto await_ready(void* opaque, std::span<const core::WorkerId> expected) noexcept
    -> std::expected<void, core::Error> {
    auto& active = *static_cast<Generation*>(opaque);
    if (expected.size() != active.command->results.size())
        return std::unexpected{adapter_error("worker_count_mismatch")};
    return active.generation.await_ready();
}
template <typename Generation> void cancel_ready(void* opaque) noexcept {
    static_cast<Generation*>(opaque)->generation.cancel_before_release();
}
template <typename Generation> void release(void* opaque) noexcept {
    auto& active = *static_cast<Generation*>(opaque);
    auto capture = active.monitoring->begin_capture();
    if (capture) active.capture.emplace(std::move(*capture));
    else active.capture_error = capture.error();
    active.generation.release();
}
template <typename Generation>
auto await_timed(void* opaque) noexcept -> std::expected<void, core::Error> {
    auto& active = *static_cast<Generation*>(opaque);
    auto completed = active.generation.await_timed_completion();
    if (!completed) return completed;
    if (active.capture_error) return std::unexpected{*active.capture_error};
    return {};
}
template <typename Generation, typename Evidence>
auto collect(void* opaque) noexcept -> std::expected<std::vector<Evidence>, core::Error> {
    auto& active = *static_cast<Generation*>(opaque);
    auto quiescent = active.generation.await_quiescence();
    if (!quiescent) return std::unexpected{quiescent.error()};
    auto outcome = active.generation.collect_ordered_results();
    if (!outcome) return std::unexpected{outcome.error()};
    active.migrations.clear();
    std::vector<Evidence> evidence;
    evidence.reserve(outcome->workers.size());
    for (std::size_t index = 0; index < outcome->workers.size(); ++index) {
        const auto& result = outcome->workers[index];
        if (!result.result) return std::unexpected{result.result.error()};
        active.migrations.push_back(measurement::WorkerMigrationValidityEvidence::create(
            result.worker_id, result.migrated
                ? measurement::WorkerMigrationValidityState::migrated
                : measurement::WorkerMigrationValidityState::not_migrated));
        if (!active.command->results[index])
            return std::unexpected{adapter_error("missing_kernel_result")};
        evidence.push_back(std::move(*active.command->results[index]));
    }
    return evidence;
}

} // namespace

struct PairwiseProductionOperationsAdapter::State : std::enable_shared_from_this<State> {
    explicit State(PairwiseProductionHostOperations value) : host{std::move(value)} {}
    State(PairwiseProductionHostOperations value,
          platform_linux::detail::NumaOperations diagnostic,
          std::shared_ptr<void> owner,
          detail::LinuxDiagnosticPreparationOverrides overrides)
        : host{std::move(value)}, diagnostic_numa{diagnostic},
          diagnostic_owner{std::move(owner)}, diagnostic_overrides{overrides} {}

    PairwiseProductionHostOperations host;
    std::optional<platform_linux::detail::NumaOperations> diagnostic_numa;
    std::shared_ptr<void> diagnostic_owner;
    detail::LinuxDiagnosticPreparationOverrides diagnostic_overrides;
};

namespace {

auto cleanup_prepared_variant(PersistentWorkerTeam&, PairwiseProductionHostOperations&,
                              PreparedVariantMemory&,
                              std::span<detail::CleanupWorkerSlot>,
                              std::span<std::optional<memory::RegionFinalizationDiagnostic>>,
                              memory::PostRunVerificationApplicability) noexcept
    -> detail::CleanupCommandResult;
auto emergency_cleanup_prepared_variant(PersistentWorkerTeam&, PreparedVariantMemory&,
                                        std::span<detail::CleanupWorkerSlot>) noexcept
    -> detail::CleanupCommandResult;

class ProductionRunCore final : public detail::CalibrationProductionKernel {
  public:
    ProductionRunCore(std::shared_ptr<PairwiseProductionOperationsAdapter::State> state,
                      std::unique_ptr<RunnerCancellation> cancellation,
                      PersistentWorkerTeam team, FamilyResources resources,
                      monitoring::MonitoringAcquisitionDriver monitoring,
                      ipc::PlacementMeasurementInputs measurement,
                      std::unique_ptr<CleanupLedger> cleanup_ledger,
                      std::vector<detail::CleanupWorkerSlot> first_slots,
                      std::vector<detail::CleanupWorkerSlot> second_slots,
                      std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
                          first_diagnostics,
                      std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
                          second_diagnostics,
                      std::vector<core::WorkerId> finalization_worker_ids,
                      std::vector<FinalizedWorkerEvidence> first_finalized_workers,
                      std::vector<FinalizedWorkerEvidence> second_finalized_workers,
                      std::vector<detail::LocalFinalizationErrorAssociation>
                          finalization_associations,
                      std::optional<platform_linux::detail::NumaDomainMaxNode>
                          diagnostic_domain,
                      std::vector<platform_linux::detail::AllowedMemsWorkspace>
                          first_workspaces,
                      std::vector<platform_linux::detail::AllowedMemsWorkspace>
                          second_workspaces,
                      std::vector<platform_linux::detail::NumaBindInvocationFacts> first_facts,
                      std::vector<platform_linux::detail::NumaBindInvocationFacts> second_facts)
        noexcept
        : state_{std::move(state)}, cancellation_{std::move(cancellation)}, team_{std::move(team)},
          resources_{std::move(resources)}, monitoring_{std::move(monitoring)},
          measurement_{std::move(measurement)}, cleanup_ledger_{std::move(cleanup_ledger)},
          first_cleanup_slots_{std::move(first_slots)},
          second_cleanup_slots_{std::move(second_slots)},
          first_finalization_diagnostics_{std::move(first_diagnostics)},
          second_finalization_diagnostics_{std::move(second_diagnostics)},
          finalization_worker_ids_{std::move(finalization_worker_ids)},
          first_finalized_workers_{std::move(first_finalized_workers)},
          second_finalized_workers_{std::move(second_finalized_workers)},
          finalization_associations_{std::move(finalization_associations)},
          diagnostic_domain_{std::move(diagnostic_domain)},
          first_diagnostic_workspaces_{std::move(first_workspaces)},
          second_diagnostic_workspaces_{std::move(second_workspaces)},
          first_invocation_facts_{std::move(first_facts)},
          second_invocation_facts_{std::move(second_facts)} {}

    ~ProductionRunCore() noexcept {
        if (!finalized_) emergency_cleanup();
    }

    auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> LegacyProductionOperationResult<
            measurement::SequentialReadBandwidthCalibrationOutcome> override {
        try {
          auto* resources = std::get_if<BandwidthResources>(&resources_);
          if (resources == nullptr)
              return std::unexpected{failure(adapter_error("kernel_family_mismatch"))};
          struct Trial { ProductionRunCore* run; BandwidthPrepared* prepared; } trial{this, &resources->first_kernel};
          return failed(measurement::calibrate_sequential_read_bandwidth(
            policy, host().clock, {&trial, [](void* opaque, auto work) noexcept
                -> std::expected<measurement::MeasurementInterval, core::Error> {
                auto& value = *static_cast<Trial*>(opaque);
                return value.run->run_bandwidth(*value.prepared, work);
            }}));
        } catch (const std::exception&) {
            return std::unexpected{failure(adapter_error("contained_exception"))};
        } catch (...) {
            return std::unexpected{failure(adapter_error("contained_unknown_failure"))};
        }
    }

    auto calibrate_latency(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> LegacyProductionOperationResult<
            measurement::DependentLoadLatencyCalibrationOutcome> override {
        try {
          auto* resources = std::get_if<LatencyResources>(&resources_);
          if (resources == nullptr)
              return std::unexpected{failure(adapter_error("kernel_family_mismatch"))};
          struct Trial { ProductionRunCore* run; LatencyPrepared* prepared; } trial{this, &resources->first_kernel};
          return failed(measurement::calibrate_dependent_load_latency(
            policy, host().clock, {&trial, [](void* opaque, auto work) noexcept
                -> std::expected<measurement::MeasurementInterval, core::Error> {
                auto& value = *static_cast<Trial*>(opaque);
                return value.run->run_latency(*value.prepared, work,
                    kernel::DependentLoadLatencyBlockId{0}, core::ExperimentId{0});
            }}));
        } catch (const std::exception&) {
            return std::unexpected{failure(adapter_error("contained_exception"))};
        } catch (...) {
            return std::unexpected{failure(adapter_error("contained_unknown_failure"))};
        }
    }

    auto finalize_owners() -> detail::LegacyPairwiseFinalizationOutcome {
        if (finalized_) std::terminate();
        finalized_ = true;
        begin_reportable_cleanup_scope();
        const auto generations = finalize_memories();
        std::optional<core::Error> monitoring_error;
        CleanupBoundaryDisposition monitoring_boundary{};
        try {
            if (auto stopped = monitoring_.stop_and_join(); !stopped)
                monitoring_error.emplace(std::move(stopped.error()));
        } catch (const std::exception&) {
            monitoring_boundary = CleanupBoundaryDisposition::contained_exception;
        } catch (...) {
            monitoring_boundary = CleanupBoundaryDisposition::contained_unknown;
        }
        TeamCleanupState team_state;
        if (calibration_stop_failure_worker) {
            detail::PersistentWorkerTeamAccess::inject_stop_failure_for_test(
                team_, *calibration_stop_failure_worker);
            calibration_stop_failure_worker.reset();
        }
        stop_team_inactive(team_, *cleanup_ledger_, team_state);
        detail::PersistentWorkerTeamAccess::clear_stop_failures_for_test(team_);
        auto make_variant = [&](measurement::ComparisonVariantSlot slot,
                               auto& diagnostics, const auto& cleanup_slots, auto& workers,
                               std::uint64_t& ordinal) {
            for (std::size_t index = 0; index < diagnostics.size(); ++index) {
                if (!diagnostics[index] &&
                    cleanup_slots[index].disposition !=
                        detail::CleanupWorkerDisposition::contained_exception &&
                    cleanup_slots[index].disposition !=
                        detail::CleanupWorkerDisposition::contained_unknown)
                    std::terminate();
                std::optional<std::uint64_t> primary;
                std::optional<std::uint64_t> cleanup;
                if (cleanup_slots[index].primary ||
                    cleanup_slots[index].disposition != detail::CleanupWorkerDisposition::succeeded)
                    primary = ordinal++;
                if (cleanup_slots[index].cleanup) cleanup = ordinal++;
                auto diagnostic = diagnostics[index]
                    ? std::move(*diagnostics[index])
                    : memory::detail::RegionFinalizationDiagnosticAccess::unavailable(
                          cleanup_slots[index].disposition ==
                                  detail::CleanupWorkerDisposition::contained_exception
                              ? memory::RegionFinalizationUnavailableReason::contained_exception
                              : memory::RegionFinalizationUnavailableReason::contained_unknown_failure);
                workers.push_back({finalization_worker_ids_[index], std::move(diagnostic),
                                   std::nullopt, std::nullopt});
                if (primary || cleanup)
                    finalization_associations_.push_back(
                        {slot, index, primary, cleanup});
            }
            return FinalizedVariantEvidence{slot, std::move(workers)};
        };
        std::uint64_t ordinal{};
        auto first = make_variant(measurement::ComparisonVariantSlot::first,
                                  first_finalization_diagnostics_, first_cleanup_slots_,
                                  first_finalized_workers_, ordinal);
        auto second = make_variant(measurement::ComparisonVariantSlot::second,
                                   second_finalization_diagnostics_, second_cleanup_slots_,
                                   second_finalized_workers_, ordinal);
        const auto project_slots = [&](CleanupLedger& ledger,
                                       std::vector<detail::CleanupWorkerSlot>& slots,
                                       detail::CleanupGeneration generation) {
            for (auto& slot : slots) {
                if (slot.primary) { ledger.append(std::move(*slot.primary)); slot.primary.reset(); }
                else if (slot.disposition == detail::CleanupWorkerDisposition::contained_exception)
                    ledger.append_contained(CleanupBoundaryDisposition::contained_exception);
                else if (slot.disposition == detail::CleanupWorkerDisposition::contained_unknown)
                    ledger.append_contained(CleanupBoundaryDisposition::contained_unknown);
                if (slot.cleanup) { ledger.append(std::move(*slot.cleanup)); slot.cleanup.reset(); }
            }
            detail::PersistentWorkerTeamAccess::reset_cleanup_slots_after_consumption(
                team_, generation, slots);
        };
        // Post-transfer report order is first Memory, second Memory, Monitoring, team.
        auto errors = cleanup_ledger_->project(
            monitoring_boundary, monitoring_error,
            [&](CleanupLedger& ledger) {
                project_slots(ledger, first_cleanup_slots_, generations.first);
                project_slots(ledger, second_cleanup_slots_, generations.second);
            }, team_state.ordinary_error, false);
        complete_reportable_cleanup_scope();
        return {{std::move(first), std::move(second)},
                std::move(finalization_associations_), std::move(errors)};
    }

    auto finalize_calibration() noexcept -> LegacyProductionOperationResult<void> override {
        try {
            auto outcome = finalize_owners();
            if (outcome.errors.empty()) return {};
            auto primary = std::move(outcome.errors.front());
            outcome.errors.erase(outcome.errors.begin());
            return std::unexpected{failure(std::move(primary), std::move(outcome.errors))};
        } catch (const std::exception&) {
            return std::unexpected{failure(adapter_error("contained_exception"))};
        } catch (...) {
            return std::unexpected{failure(adapter_error("contained_unknown_failure"))};
        }
    }

  private:
    auto host() noexcept -> PairwiseProductionHostOperations& { return state_->host; }
    auto worker_ids() const -> std::vector<core::WorkerId> {
        std::vector<core::WorkerId> ids;
        ids.reserve(team_.readiness().size());
        for (const auto& worker : team_.readiness()) ids.push_back(worker.worker_id);
        return ids;
    }

    auto run_bandwidth(BandwidthPrepared& prepared,
                       kernel::SequentialReadBandwidthPassCount work)
        -> std::expected<measurement::MeasurementInterval, core::Error> {
        BandwidthCommand command{&host(), work, kernel::DependentLoadLatencyBlockId{0},
            core::ExperimentId{0}, 0, &prepared,
            std::vector<std::optional<measurement::SequentialReadBandwidthWorkerObservation>>(prepared.size())};
        auto start = host().clock.now(host().clock.context);
        if (!start) return std::unexpected{start.error()};
        auto outcome = team_.execute({&command, execute_bandwidth_worker}, WorkerObservationPolicy::none,
                                     WorkerCancellationPolicy::cleanup_insensitive);
        if (!outcome) return std::unexpected{outcome.error()};
        if (auto error = first_worker_error(*outcome)) return std::unexpected{*error};
        auto end = host().clock.now(host().clock.context);
        if (!end) return std::unexpected{end.error()};
        return measurement::MeasurementInterval::create(*start, *end);
    }

    auto run_latency(LatencyPrepared& prepared, kernel::DependentLoadLatencyStepCount work,
                     kernel::DependentLoadLatencyBlockId block, core::ExperimentId experiment)
        -> std::expected<measurement::MeasurementInterval, core::Error> {
        auto& resources = std::get<LatencyResources>(resources_);
        LatencyCommand command{&host(), work, block, experiment, resources.seed, &prepared,
            std::vector<std::optional<measurement::DependentLoadLatencyWorkerObservation>>(prepared.size())};
        auto start = host().clock.now(host().clock.context);
        if (!start) return std::unexpected{start.error()};
        auto outcome = team_.execute({&command, execute_latency_worker}, WorkerObservationPolicy::none,
                                     WorkerCancellationPolicy::cleanup_insensitive);
        if (!outcome) return std::unexpected{outcome.error()};
        if (auto error = first_worker_error(*outcome)) return std::unexpected{*error};
        auto end = host().clock.now(host().clock.context);
        if (!end) return std::unexpected{end.error()};
        return measurement::MeasurementInterval::create(*start, *end);
    }


    auto finalize_memories() noexcept
        -> std::pair<detail::CleanupGeneration, detail::CleanupGeneration> {
        return std::visit([&](auto& resources) noexcept {
            resources.first_kernel.clear();
            resources.second_kernel.clear();
            const auto first = cleanup_prepared_variant(
                team_, host(), resources.first_memory, first_cleanup_slots_,
                first_finalization_diagnostics_,
                memory::PostRunVerificationApplicability::applicable);
            const auto second = cleanup_prepared_variant(
                team_, host(), resources.second_memory, second_cleanup_slots_,
                second_finalization_diagnostics_,
                memory::PostRunVerificationApplicability::applicable);
            return std::pair{first.generation, second.generation};
        }, resources_);
    }

    void emergency_cleanup() noexcept {
        finalized_ = true;
        const auto generations = std::visit([&](auto& resources) noexcept {
            resources.first_kernel.clear();
            resources.second_kernel.clear();
            const auto first = emergency_cleanup_prepared_variant(
                team_, resources.first_memory, first_cleanup_slots_);
            const auto second = emergency_cleanup_prepared_variant(
                team_, resources.second_memory, second_cleanup_slots_);
            return std::pair{first.generation, second.generation};
        }, resources_);
        try { static_cast<void>(monitoring_.stop_and_join()); } catch (...) {}
        TeamCleanupState team_state;
        stop_team_inactive(team_, *cleanup_ledger_, team_state);
        team_state.ordinary_error.reset();
        const auto discard = [&](std::vector<detail::CleanupWorkerSlot>& slots,
                                 detail::CleanupGeneration generation) noexcept {
            for (auto& slot : slots) { slot.primary.reset(); slot.cleanup.reset(); }
            detail::PersistentWorkerTeamAccess::reset_cleanup_slots_after_consumption(
                team_, generation, slots);
        };
        discard(first_cleanup_slots_, generations.first);
        discard(second_cleanup_slots_, generations.second);
        cleanup_ledger_->discard();
        if (const auto hook = emergency_cleanup_complete_hook.load()) hook();
    }

    std::shared_ptr<PairwiseProductionOperationsAdapter::State> state_;
    std::unique_ptr<RunnerCancellation> cancellation_;
    PersistentWorkerTeam team_;
    FamilyResources resources_;
    monitoring::MonitoringAcquisitionDriver monitoring_;
    ipc::PlacementMeasurementInputs measurement_;
    std::unique_ptr<CleanupLedger> cleanup_ledger_;
    std::vector<detail::CleanupWorkerSlot> first_cleanup_slots_;
    std::vector<detail::CleanupWorkerSlot> second_cleanup_slots_;
    std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
        first_finalization_diagnostics_;
    std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
        second_finalization_diagnostics_;
    std::vector<core::WorkerId> finalization_worker_ids_;
    std::vector<FinalizedWorkerEvidence> first_finalized_workers_;
    std::vector<FinalizedWorkerEvidence> second_finalized_workers_;
    std::vector<detail::LocalFinalizationErrorAssociation> finalization_associations_;
    std::optional<platform_linux::detail::NumaDomainMaxNode> diagnostic_domain_;
    std::vector<platform_linux::detail::AllowedMemsWorkspace> first_diagnostic_workspaces_;
    std::vector<platform_linux::detail::AllowedMemsWorkspace> second_diagnostic_workspaces_;
    std::vector<platform_linux::detail::NumaBindInvocationFacts> first_invocation_facts_;
    std::vector<platform_linux::detail::NumaBindInvocationFacts> second_invocation_facts_;
    bool finalized_{};
};

static_assert(
    !std::derived_from<ProductionRunCore, CalibrationOnlyProductionRun::Implementation>);
static_assert(!std::same_as<detail::LegacyProductionOperationResult<void>,
                            CalibrationOnlyOperationResult<void>>);

auto prepare_initializer(const AdmittedPlacementRequest& placement)
    -> std::expected<kernel::PreparedRegionInitializer, core::Error> {
    const auto& kernel_inputs = placement.execution_inputs().kernel();
    if (const auto* bandwidth = std::get_if<ipc::PlacementBandwidthExecutionInputs>(&kernel_inputs))
        return kernel::PreparedRegionInitializer::prepare(
            kernel::RegionInitializationSpec::bandwidth(bandwidth->configuration()));
    const auto& latency = std::get<ipc::PlacementLatencyExecutionInputs>(kernel_inputs);
    auto specification = kernel::RegionInitializationSpec::latency(
        latency.configuration(), latency.resolved_permutation_seed());
    if (!specification) return std::unexpected{specification.error()};
    return kernel::PreparedRegionInitializer::prepare(std::move(*specification));
}

auto prepare_variant_memory(PersistentWorkerTeam& team, PairwiseProductionHostOperations& host,
                            const AdmittedPlacementRequest& placement,
                            PreparedVariantMemory& destination,
                            std::vector<PrepareMemoryContext::CleanupSlots>& cleanup_slots,
                            platform_linux::detail::NumaOperations* diagnostic = nullptr,
                            platform_linux::detail::NumaDomainMaxNode* domain = nullptr,
                            std::vector<platform_linux::detail::AllowedMemsWorkspace>* workspaces =
                                nullptr,
                            std::vector<platform_linux::detail::NumaBindInvocationFacts>* facts =
                                nullptr,
                            const detail::LinuxDiagnosticPreparationOverrides* overrides = nullptr,
                            bool second_variant = false)
    -> std::expected<void, core::Error> {
    auto initializer = prepare_initializer(placement);
    if (!initializer) return std::unexpected{initializer.error()};
    destination.regions.resize(placement.workers().size());
    PrepareMemoryContext context{
        &host,
        &placement,
        &*initializer,
        &destination,
        &cleanup_slots,
        diagnostic,
        domain,
        workspaces,
        facts,
        overrides,
        overrides != nullptr ? overrides->observer_context : nullptr,
        overrides != nullptr ? overrides->workspace_observer : nullptr,
        overrides != nullptr ? overrides->facts_observer : nullptr,
        second_variant};
    auto outcome = team.execute({&context, prepare_memory_worker}, WorkerObservationPolicy::none,
                                WorkerCancellationPolicy::cleanup_insensitive);
    if (!outcome) return std::unexpected{outcome.error()};
    if (auto error = first_worker_error(*outcome)) return std::unexpected{*error};
    return {};
}

auto prepare_family(const AdmittedPlacementRequest& first,
                    PreparedVariantMemory& first_memory, PreparedVariantMemory& second_memory)
    -> std::expected<FamilyResources, core::Error> {
    const auto& kernel_inputs = first.execution_inputs().kernel();
    if (const auto* bandwidth = std::get_if<ipc::PlacementBandwidthExecutionInputs>(&kernel_inputs)) {
        BandwidthPrepared first_kernel(first_memory.regions.size());
        BandwidthPrepared second_kernel(second_memory.regions.size());
        for (std::size_t index = 0; index < first_kernel.size(); ++index) {
            auto first_input = kernel::SequentialReadBandwidthInput::create(
                bandwidth->configuration(), first_memory.regions[index]->requested_region());
            auto second_input = kernel::SequentialReadBandwidthInput::create(
                bandwidth->configuration(), second_memory.regions[index]->requested_region());
            if (!first_input) return std::unexpected{first_input.error()};
            if (!second_input) return std::unexpected{second_input.error()};
            first_kernel[index].emplace(kernel::PreparedSequentialReadBandwidth::prepare(*first_input));
            second_kernel[index].emplace(kernel::PreparedSequentialReadBandwidth::prepare(*second_input));
            if (!bind_kernel_module(*first_kernel[index], first.request().experiment().kernel_id()) ||
                !bind_kernel_module(*second_kernel[index], first.request().experiment().kernel_id()))
                return std::unexpected{adapter_error("kernel_module_preparation")};
        }
        return FamilyResources{std::in_place_type<BandwidthResources>, bandwidth->configuration(),
            std::move(first_memory), std::move(second_memory), std::move(first_kernel),
            std::move(second_kernel)};
    }
    const auto& latency = std::get<ipc::PlacementLatencyExecutionInputs>(kernel_inputs);
    LatencyPrepared first_kernel(first_memory.regions.size());
    LatencyPrepared second_kernel(second_memory.regions.size());
    for (std::size_t index = 0; index < first_kernel.size(); ++index) {
        auto first_input = kernel::DependentLoadLatencyInput::create(
            latency.configuration(), first_memory.regions[index]->requested_region());
        auto second_input = kernel::DependentLoadLatencyInput::create(
            latency.configuration(), second_memory.regions[index]->requested_region());
        if (!first_input) return std::unexpected{first_input.error()};
        if (!second_input) return std::unexpected{second_input.error()};
        auto first_prepared = kernel::PreparedDependentLoadLatency::prepare(*first_input);
        auto second_prepared = kernel::PreparedDependentLoadLatency::prepare(*second_input);
        if (!bind_kernel_module(first_prepared, first.request().experiment().kernel_id()) ||
            !bind_kernel_module(second_prepared, first.request().experiment().kernel_id()))
            return std::unexpected{adapter_error("kernel_module_preparation")};
        auto first_verification = kernel::PreparedDependentLoadLatencyVerification::prepare(
            first_prepared, latency.resolved_permutation_seed());
        auto second_verification = kernel::PreparedDependentLoadLatencyVerification::prepare(
            second_prepared, latency.resolved_permutation_seed());
        if (!first_verification) return std::unexpected{first_verification.error()};
        if (!second_verification) return std::unexpected{second_verification.error()};
        first_kernel[index].emplace(LatencyPreparedWorker{std::move(first_prepared),
                                                          std::move(*first_verification)});
        second_kernel[index].emplace(LatencyPreparedWorker{std::move(second_prepared),
                                                            std::move(*second_verification)});
    }
    return FamilyResources{std::in_place_type<LatencyResources>, latency.configuration(),
        latency.resolved_permutation_seed(), std::move(first_memory), std::move(second_memory),
        std::move(first_kernel), std::move(second_kernel)};
}

auto cleanup_prepared_variant(PersistentWorkerTeam& team, PairwiseProductionHostOperations& host,
                              PreparedVariantMemory& memory,
                              std::span<detail::CleanupWorkerSlot> slots,
                              std::span<std::optional<memory::RegionFinalizationDiagnostic>> diagnostics,
                              memory::PostRunVerificationApplicability applicability) noexcept
    -> detail::CleanupCommandResult {
    struct Context { PreparedVariantMemory* memory; PairwiseProductionHostOperations* host;
                     std::span<std::optional<memory::RegionFinalizationDiagnostic>> diagnostics;
                     memory::PostRunVerificationApplicability applicability; };
    Context context{&memory, &host, diagnostics, applicability};
    const auto operation = [](void* opaque, const WorkerExecutionContext& worker,
                              const WorkerExecutionControl&)
        -> detail::CleanupWorkerCallbackResult {
        auto& value = *static_cast<Context*>(opaque);
        if (worker.worker_index >= value.memory->regions.size()) return {};
        auto& region = value.memory->regions[worker.worker_index];
        if (!region) return {};
        auto operations = memory_operations(*value.host);
        std::optional<memory::RegionFinalizationDiagnostic> diagnostic;
        auto result = [&] {
            LowerMemoryAllocationAttribution attribution;
            auto terminal = memory::detail::RegionFinalizationAccess::finalize_with_operations(
                std::move(*region), value.applicability, operations.query);
            if (worker.worker_index < value.diagnostics.size())
                diagnostic.emplace(terminal ? terminal->diagnostic()
                                            : terminal.error().diagnostic());
            return terminal;
        }();
        region.reset();
        if (!result) {
            auto failed = std::move(result.error());
            if (diagnostic)
                value.diagnostics[worker.worker_index].emplace(std::move(*diagnostic));
            return std::unexpected{detail::CleanupWorkerFailure{
                .primary = memory::detail::RegionFinalizationAccess::take_primary_error(failed),
                .cleanup = memory::detail::RegionFinalizationAccess::take_cleanup_error(failed)}};
        }
        if (diagnostic)
            value.diagnostics[worker.worker_index].emplace(std::move(*diagnostic));
        return {};
    };
    return detail::PersistentWorkerTeamAccess::execute_cleanup_preallocated(
        team, {.context = &context, .run = operation}, slots);
}

auto emergency_cleanup_prepared_variant(PersistentWorkerTeam& team,
                                        PreparedVariantMemory& memory,
                                        std::span<detail::CleanupWorkerSlot> slots) noexcept
    -> detail::CleanupCommandResult {
    struct Context { PreparedVariantMemory* memory; } context{&memory};
    const auto operation = [](void* opaque, const WorkerExecutionContext& worker,
                              const WorkerExecutionControl&) noexcept
        -> detail::CleanupWorkerCallbackResult {
        auto& value = *static_cast<Context*>(opaque);
        if (worker.worker_index >= value.memory->regions.size()) return {};
        auto& region = value.memory->regions[worker.worker_index];
        if (!region) return {};
        auto& evaluated = memory::detail::ReadOnlyRegionAccess::region(*region);
        auto& observed = memory::detail::PlacementEvaluatedRegionAccess::region(evaluated);
        auto& initialized = memory::detail::PlacementObservedRegionAccess::region(observed);
        auto& bound = memory::detail::InitializedRegionAccess::region(initialized);
        auto& mapping = memory::detail::BoundRegionAccess::mapping(bound);
        auto released = memory::detail::RegionMappingAccess::mapping(mapping).release();
        if (!released) std::terminate();
        region.reset();
        return {};
    };
    return detail::PersistentWorkerTeamAccess::execute_cleanup_preallocated(
        team, {.context = &context, .run = operation}, slots);
}

auto cleanup_slots(std::size_t count) -> std::vector<detail::CleanupWorkerSlot> {
    std::vector<detail::CleanupWorkerSlot> slots;
    slots.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        slots.emplace_back(core::WorkerId{static_cast<std::uint32_t>(index)});
    return slots;
}

class PreparationCleanupGuard final {
  public:
    PreparationCleanupGuard(PersistentWorkerTeam& team, PairwiseProductionHostOperations& host,
                            PreparedVariantMemory& first, PreparedVariantMemory& second,
                            std::optional<FamilyResources>& family,
                            std::optional<monitoring::MonitoringAcquisitionDriver>& monitoring,
                            CleanupLedger& ledger,
                            std::vector<detail::CleanupWorkerSlot>& first_slots,
                            std::vector<detail::CleanupWorkerSlot>& second_slots,
                            std::vector<PrepareMemoryContext::CleanupSlots>& first_prepare_slots,
                            std::vector<PrepareMemoryContext::CleanupSlots>& second_prepare_slots) noexcept
        : team_{team}, host_{host}, first_{first}, second_{second}, family_{family},
          monitoring_{monitoring}, ledger_{ledger}, first_slots_{first_slots},
          second_slots_{second_slots}, first_prepare_slots_{first_prepare_slots},
          second_prepare_slots_{second_prepare_slots} {}
    PreparationCleanupGuard(const PreparationCleanupGuard&) = delete;
    ~PreparationCleanupGuard() noexcept { cleanup(); }

    void disarm() noexcept { armed_ = false; }
    void cleanup() noexcept {
        if (!armed_) return;
        armed_ = false;
        begin_reportable_cleanup_scope();
        std::optional<core::Error> monitoring_error;
        CleanupBoundaryDisposition monitoring_boundary{};
        if (monitoring_) {
            try {
                if (auto stopped = monitoring_->stop_and_join(); !stopped)
                    monitoring_error.emplace(std::move(stopped.error()));
            } catch (const std::exception&) {
                monitoring_boundary = CleanupBoundaryDisposition::contained_exception;
            } catch (...) {
                monitoring_boundary = CleanupBoundaryDisposition::contained_unknown;
            }
        }
        std::optional<detail::CleanupGeneration> first_generation;
        std::optional<detail::CleanupGeneration> second_generation;
        if (family_) {
            std::visit([&](auto& resources) noexcept {
                    resources.first_kernel.clear();
                    resources.second_kernel.clear();
                    second_generation = cleanup_prepared_variant(
                        team_, host_, resources.second_memory, second_slots_,
                        {},
                        memory::PostRunVerificationApplicability::not_applicable).generation;
                    first_generation = cleanup_prepared_variant(
                        team_, host_, resources.first_memory, first_slots_,
                        {},
                        memory::PostRunVerificationApplicability::not_applicable).generation;
                }, *family_);
        } else {
            second_generation = cleanup_prepared_variant(
                team_, host_, second_, second_slots_,
                {},
                memory::PostRunVerificationApplicability::not_applicable).generation;
            first_generation = cleanup_prepared_variant(
                team_, host_, first_, first_slots_,
                {},
                memory::PostRunVerificationApplicability::not_applicable).generation;
        }
        TeamCleanupState team_state;
        stop_team_inactive(team_, ledger_, team_state);
        const auto project = [&](CleanupLedger& ledger,
                                 std::vector<PrepareMemoryContext::CleanupSlots>& prepare_slots,
                                 std::vector<detail::CleanupWorkerSlot>& slots,
                                 std::optional<detail::CleanupGeneration> generation) {
            for (auto& slot : prepare_slots) {
                if (slot.primary) { ledger.append(std::move(*slot.primary)); slot.primary.reset(); }
                if (slot.cleanup) { ledger.append(std::move(*slot.cleanup)); slot.cleanup.reset(); }
            }
            if (!generation) return;
            for (auto& slot : slots) {
                if (slot.primary) {
                    ledger.append(std::move(*slot.primary));
                    slot.primary.reset();
                }
                else if (slot.disposition == detail::CleanupWorkerDisposition::contained_exception)
                    ledger.append_contained(CleanupBoundaryDisposition::contained_exception);
                else if (slot.disposition == detail::CleanupWorkerDisposition::contained_unknown)
                    ledger.append_contained(CleanupBoundaryDisposition::contained_unknown);
                if (slot.cleanup) {
                    ledger.append(std::move(*slot.cleanup));
                    slot.cleanup.reset();
                }
            }
            detail::PersistentWorkerTeamAccess::reset_cleanup_slots_after_consumption(
                team_, *generation, slots);
        };
        // Pre-transfer report order is Monitoring, second Memory, first Memory, team.
        projected_ = ledger_.project(
            monitoring_boundary, monitoring_error,
            [&](CleanupLedger& ledger) {
                project(ledger, second_prepare_slots_, second_slots_, second_generation);
                project(ledger, first_prepare_slots_, first_slots_, first_generation);
            }, team_state.ordinary_error, true);
        complete_reportable_cleanup_scope();
        monitoring_.reset();
        family_.reset();
    }

    auto take_projected() noexcept -> std::vector<core::Error> { return std::move(projected_); }

  private:
    PersistentWorkerTeam& team_;
    PairwiseProductionHostOperations& host_;
    PreparedVariantMemory& first_;
    PreparedVariantMemory& second_;
    std::optional<FamilyResources>& family_;
    std::optional<monitoring::MonitoringAcquisitionDriver>& monitoring_;
    CleanupLedger& ledger_;
    std::vector<detail::CleanupWorkerSlot>& first_slots_;
    std::vector<detail::CleanupWorkerSlot>& second_slots_;
    std::vector<PrepareMemoryContext::CleanupSlots>& first_prepare_slots_;
    std::vector<PrepareMemoryContext::CleanupSlots>& second_prepare_slots_;
    std::vector<core::Error> projected_;
    bool armed_{true};
};

} // namespace

void detail::PairwiseProductionOperationsAdapterAccess::install_fault_script(
    ProductionAdapterFaultScript script) noexcept {
    if (adapter_fault_guard.test_and_set()) std::terminate();
    if (script.invocation == 0U) script.invocation = adapter_prepare_invocation.load() + 1U;
    adapter_fault_script.emplace(script);
    adapter_fault_error.emplace(adapter_error("injected_preparation_failure"));
    adapter_fault_guard.clear();
}

void detail::PairwiseProductionOperationsAdapterAccess::clear_fault_script() noexcept {
    if (adapter_fault_guard.test_and_set()) std::terminate();
    adapter_fault_script.reset();
    adapter_fault_error.reset();
    adapter_fault_guard.clear();
}

void detail::PairwiseProductionOperationsAdapterAccess::set_emergency_cleanup_complete_hook(
    void (*hook)() noexcept) noexcept {
    emergency_cleanup_complete_hook.store(hook);
}

void detail::PairwiseProductionOperationsAdapterAccess::set_reportable_cleanup_hooks(
    void (*begin)() noexcept, void (*complete)() noexcept) noexcept {
    reportable_cleanup_begin_hook.store(begin);
    reportable_cleanup_complete_hook.store(complete);
}

void detail::PairwiseProductionOperationsAdapterAccess::
    inject_calibration_stop_failure_for_test(std::uint32_t worker_index) noexcept {
    calibration_stop_failure_worker = worker_index;
}

auto detail::PairwiseProductionOperationsAdapterAccess::
    lower_memory_allocation_attribution_active() noexcept -> bool {
    return lower_memory_attribution_depth != 0U;
}

auto detail::PairwiseProductionOperationsAdapterAccess::probe_cleanup_ledger(
    std::size_t worker_count, std::size_t regular_records, bool boundary_record,
    std::size_t excess_records) -> std::vector<core::Error> {
    CleanupLedger ledger{worker_count};
    const auto ordinary_capacity = worker_count * 4U + 2U;
    const auto ordinary = std::min(regular_records, ordinary_capacity);
    for (std::size_t index = 0; index < ordinary; ++index)
        ledger.retain(adapter_error("probe_regular"));
    for (std::size_t index = ordinary; index < regular_records; ++index)
        ledger.retain_force(detail::ForcedTeamStopErrorKind::join_failed);
    if (boundary_record)
        ledger.retain_boundary(CleanupBoundaryDisposition::contained_exception);
    for (std::size_t index = 0; index < excess_records; ++index)
        ledger.retain_boundary(CleanupBoundaryDisposition::contained_unknown);
    std::optional<core::Error> monitoring_error;
    std::optional<core::Error> team_error;
    begin_reportable_cleanup_scope();
    auto result = ledger.project(CleanupBoundaryDisposition::none, monitoring_error,
                                 [](CleanupLedger&) {}, team_error, false);
    complete_reportable_cleanup_scope();
    return result;
}

auto detail::PairwiseProductionOperationsAdapterAccess::probe_cleanup_force_stock()
    -> std::vector<core::Error> {
    CleanupLedger ledger{1U};
    ledger.retain_force(ForcedTeamStopErrorKind::join_failed);
    ledger.retain_force(ForcedTeamStopErrorKind::contained_exception);
    ledger.retain_force(ForcedTeamStopErrorKind::contained_unknown_failure);
    ledger.retain_force(ForcedTeamStopErrorKind::capacity_exhausted);
    std::optional<core::Error> monitoring_error;
    std::optional<core::Error> team_error;
    begin_reportable_cleanup_scope();
    auto result = ledger.project(CleanupBoundaryDisposition::none, monitoring_error,
                                 [](CleanupLedger&) {}, team_error, false);
    complete_reportable_cleanup_scope();
    return result;
}

auto detail::PairwiseProductionOperationsAdapterAccess::probe_cleanup_order(
    bool monitoring_first) -> std::vector<core::Error> {
    CleanupLedger ledger{1U};
    std::optional<core::Error> monitoring_error{adapter_error("probe_monitoring")};
    std::optional<core::Error> team_error{adapter_error("probe_team")};
    auto memory_error = adapter_error("probe_memory");
    begin_reportable_cleanup_scope();
    auto result = ledger.project(
        CleanupBoundaryDisposition::none, monitoring_error,
        [&](CleanupLedger& target) { target.append(std::move(memory_error)); },
        team_error, monitoring_first);
    complete_reportable_cleanup_scope();
    return result;
}

auto detail::make_production_adapter_state(PairwiseProductionHostOperations host)
    -> std::shared_ptr<PairwiseProductionOperationsAdapter::State> {
    return std::make_shared<PairwiseProductionOperationsAdapter::State>(std::move(host));
}

auto detail::PairwiseProductionOperationsAdapterAccess::create_linux(
    PairwiseProductionHostOperations host, platform_linux::detail::NumaOperations diagnostic,
    std::shared_ptr<void> owner, LinuxDiagnosticPreparationOverrides overrides)
    -> PairwiseProductionOperationsAdapter {
    PairwiseProductionOperationsAdapter result{host};
    result.state_ = std::make_shared<PairwiseProductionOperationsAdapter::State>(
        std::move(host), diagnostic, std::move(owner), overrides);
    return result;
}

namespace {

struct CommonPreparationInputs {
    const AdmittedPlacementRequest& first;
    const AdmittedPlacementRequest& second;
    const monitoring::MonitoringExecutionPolicy& monitoring;
    const ServicePlacementPlan& service;
    const ipc::PlacementMeasurementInputs& measurement;
};

auto invocation_facts(const AdmittedPlacementRequest& placement)
    -> std::expected<std::vector<platform_linux::detail::NumaBindInvocationFacts>, core::Error> {
    const auto& topology = placement.context().topology();
    const auto& machine = topology.machine();
    if (machine.page_sizes.empty() || !machine.effective_nodes.value()) {
        return std::unexpected{adapter_error("diagnostic_invocation_authority_invalid")};
    }
    const auto base_page = std::ranges::min(machine.page_sizes).value();
    if (base_page == 0U) {
        return std::unexpected{adapter_error("diagnostic_invocation_authority_invalid")};
    }
    std::vector<platform_linux::detail::NumaBindInvocationFacts> result;
    result.reserve(placement.workers().size());
    for (const auto& worker : placement.workers()) {
        bool effective = true;
        bool online = true;
        bool memory_backed = true;
        for (const auto& selected : worker.request().region().nodes()) {
            const auto linux = selected.linux_numa_node().value();
            const auto legacy = core::NumaNodeId{linux};
            effective &= machine.effective_nodes.value()->contains(legacy);
            online &= machine.online_nodes.contains(legacy);
            const auto found = std::ranges::find_if(topology.numa_nodes(), [&](const auto& node) {
                return node.linux_node_id.value() && *node.linux_node_id.value() == linux;
            });
            memory_backed &=
                found != topology.numa_nodes().end() && found->installed_memory.value() &&
                found->installed_memory.value()->value() != 0U;
        }
        auto facts = platform_linux::detail::NumaBindInvocationFacts::create(
            base_page, effective, online, memory_backed);
        if (!facts)
            return std::unexpected{adapter_error("diagnostic_invocation_authority_invalid")};
        result.push_back(*facts);
    }
    return result;
}

auto prepare_common(PairwiseProductionOperationsAdapter::State& state,
                    const CommonPreparationInputs& inputs) noexcept
    -> LegacyProductionOperationResult<std::unique_ptr<ProductionRunCore>> {
    const auto invocation = adapter_prepare_invocation.fetch_add(1U) + 1U;
    try {
        auto cancellation = std::make_unique<RunnerCancellation>();
        const auto worker_count = inputs.first.workers().size();
        auto cleanup_ledger = std::make_unique<CleanupLedger>(worker_count);
        auto first_cleanup_slots = cleanup_slots(worker_count);
        auto second_cleanup_slots = cleanup_slots(worker_count);
        std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
            first_finalization_diagnostics(worker_count);
        std::vector<std::optional<memory::RegionFinalizationDiagnostic>>
            second_finalization_diagnostics(worker_count);
        std::vector<core::WorkerId> finalization_worker_ids;
        finalization_worker_ids.reserve(worker_count);
        std::vector<FinalizedWorkerEvidence> first_finalized_workers;
        std::vector<FinalizedWorkerEvidence> second_finalized_workers;
        std::vector<detail::LocalFinalizationErrorAssociation> finalization_associations;
        first_finalized_workers.reserve(worker_count);
        second_finalized_workers.reserve(worker_count);
        finalization_associations.reserve(worker_count * 2U);
        std::vector<PrepareMemoryContext::CleanupSlots> first_prepare_slots(worker_count);
        std::vector<PrepareMemoryContext::CleanupSlots> second_prepare_slots(worker_count);
        std::optional<platform_linux::detail::NumaDomainMaxNode> diagnostic_domain;
        std::vector<platform_linux::detail::AllowedMemsWorkspace> first_workspaces;
        std::vector<platform_linux::detail::AllowedMemsWorkspace> second_workspaces;
        std::vector<platform_linux::detail::NumaBindInvocationFacts> first_facts;
        std::vector<platform_linux::detail::NumaBindInvocationFacts> second_facts;
        if (state.diagnostic_numa) {
            if (state.diagnostic_numa->bind == nullptr ||
                state.diagnostic_numa->allowed_mems == nullptr) {
                return std::unexpected{
                    failure(adapter_error("diagnostic_binding_state_invalid"))};
            }
            if (state.diagnostic_overrides.force_context_mismatch ||
                &inputs.first.context() != &inputs.second.context() ||
                inputs.first.context().topology().machine().present_nodes !=
                    inputs.second.context().topology().machine().present_nodes) {
                return std::unexpected{failure(adapter_error("diagnostic_context_mismatch"))};
            }
            if (state.diagnostic_overrides.force_invocation_authority_invalid) {
                return std::unexpected{
                    failure(adapter_error("diagnostic_invocation_authority_invalid"))};
            }
            auto domain = platform_linux::detail::NumaAccess::checked_domain(
                inputs.first.context().topology().machine().present_nodes,
                state.diagnostic_overrides.native_max,
                state.diagnostic_overrides.size_max);
            if (!domain) return std::unexpected{failure(std::move(domain.error()))};
            const auto covered = [&](const AdmittedPlacementRequest& placement) {
                for (const auto& worker : placement.workers()) {
                    for (const auto& node : worker.request().region().nodes()) {
                        if (node.linux_numa_node().value() >= domain->value()) return false;
                    }
                }
                return true;
            };
            if (state.diagnostic_overrides.force_domain_too_small ||
                !covered(inputs.first) || !covered(inputs.second)) {
                return std::unexpected{failure(adapter_error("diagnostic_domain_too_small"))};
            }
            diagnostic_domain.emplace(*domain);
            auto prepared_first_facts = invocation_facts(inputs.first);
            if (!prepared_first_facts)
                return std::unexpected{failure(std::move(prepared_first_facts.error()))};
            auto prepared_second_facts = invocation_facts(inputs.second);
            if (!prepared_second_facts)
                return std::unexpected{failure(std::move(prepared_second_facts.error()))};
            first_facts = std::move(*prepared_first_facts);
            second_facts = std::move(*prepared_second_facts);
            first_workspaces.reserve(worker_count);
            second_workspaces.reserve(worker_count);
            for (std::size_t index = 0; index < worker_count; ++index) {
                auto first = platform_linux::detail::NumaAccess::create_allowed_workspace(
                    *domain, state.diagnostic_overrides.workspace_allocator);
                if (!first) return std::unexpected{failure(std::move(first.error()))};
                first_workspaces.push_back(std::move(*first));
                auto second = platform_linux::detail::NumaAccess::create_allowed_workspace(
                    *domain, state.diagnostic_overrides.workspace_allocator);
                if (!second) return std::unexpected{failure(std::move(second.error()))};
                second_workspaces.push_back(std::move(*second));
            }
        }
        auto team = detail::PersistentWorkerTeamAccess::create(
            inputs.first.workers(), *cancellation, worker_dependencies(state.host));
        if (!team) return std::unexpected{failure(std::move(team.error()))};
        if (inputs.second.workers().size() != team->readiness().size())
            return std::unexpected{failure(adapter_error("pairwise_worker_identity_mismatch"))};
        for (std::size_t index = 0; index < team->readiness().size(); ++index) {
            const auto& ready = team->readiness()[index];
            if (ready.worker_index != index ||
                ready.worker_id != core::WorkerId{static_cast<std::uint32_t>(index)})
                return std::unexpected{
                    failure(adapter_error("pairwise_worker_identity_mismatch"))};
            finalization_worker_ids.push_back(ready.worker_id);
        }
        PreparedVariantMemory first_memory, second_memory;
        std::optional<FamilyResources> family;
        std::optional<monitoring::MonitoringAcquisitionDriver> monitoring_owner;
        PreparationCleanupGuard guard{*team, state.host, first_memory, second_memory, family,
                                      monitoring_owner, *cleanup_ledger, first_cleanup_slots,
                                      second_cleanup_slots, first_prepare_slots,
                                      second_prepare_slots};
        try {
            const auto injected = [&](detail::ProductionAdapterPreparationStage stage)
                -> std::optional<LegacyProductionOperationFailure> {
                auto result = inject_adapter_fault(state.host.context.get(), invocation, stage);
                if (result) return std::nullopt;
                guard.cleanup();
                return failure(std::move(result.error()), guard.take_projected());
            };
            auto first = prepare_variant_memory(
                *team, state.host, inputs.first, first_memory, first_prepare_slots,
                state.diagnostic_numa ? &*state.diagnostic_numa : nullptr,
                diagnostic_domain ? &*diagnostic_domain : nullptr,
                state.diagnostic_numa ? &first_workspaces : nullptr,
                state.diagnostic_numa ? &first_facts : nullptr,
                state.diagnostic_numa ? &state.diagnostic_overrides : nullptr, false);
            if (auto fault = injected(detail::ProductionAdapterPreparationStage::first_memory))
                return std::unexpected{std::move(*fault)};
            auto second = first
                              ? prepare_variant_memory(
                                    *team, state.host, inputs.second, second_memory,
                                    second_prepare_slots,
                                    state.diagnostic_numa ? &*state.diagnostic_numa : nullptr,
                                    diagnostic_domain ? &*diagnostic_domain : nullptr,
                                    state.diagnostic_numa ? &second_workspaces : nullptr,
                                    state.diagnostic_numa ? &second_facts : nullptr,
                                    state.diagnostic_numa ? &state.diagnostic_overrides : nullptr,
                                    true)
                              : std::expected<void, core::Error>{
                                    std::unexpected{std::move(first.error())}};
            if (auto fault = injected(detail::ProductionAdapterPreparationStage::second_memory))
                return std::unexpected{std::move(*fault)};
            if (!second) {
                guard.cleanup();
                return std::unexpected{
                    failure(std::move(second.error()), guard.take_projected())};
            }
            auto prepared_family = prepare_family(inputs.first, first_memory, second_memory);
            if (!prepared_family) {
                guard.cleanup();
                return std::unexpected{
                    failure(std::move(prepared_family.error()), guard.take_projected())};
            }
            family.emplace(std::move(*prepared_family));
            if (auto fault = injected(detail::ProductionAdapterPreparationStage::family))
                return std::unexpected{std::move(*fault)};
            std::vector<monitoring::MonitoringWorkerScope> scopes;
            scopes.reserve(team->readiness().size());
            for (const auto& worker : team->readiness())
                scopes.push_back(monitoring::MonitoringWorkerScope::create(
                    worker.worker_id, worker.tid, worker.requested_cpu));
            if (auto fault = injected(detail::ProductionAdapterPreparationStage::worker_scopes))
                return std::unexpected{std::move(*fault)};
            std::optional<core::LinuxCpuId> service_cpu;
            for (const auto& assignment : inputs.service.assignments())
                if (assignment.role == ServiceRole::measurement_monitor)
                    service_cpu = assignment.linux_cpu;
            auto monitoring = monitoring::MonitoringAcquisitionDriver::create(
                inputs.monitoring, std::move(scopes), service_cpu, state.host.monitoring);
            if (!monitoring) {
                guard.cleanup();
                return std::unexpected{
                    failure(std::move(monitoring.error()), guard.take_projected())};
            }
            monitoring_owner.emplace(std::move(*monitoring));
            if (auto fault = injected(detail::ProductionAdapterPreparationStage::monitoring))
                return std::unexpected{std::move(*fault)};
            if (auto fault = injected(
                    detail::ProductionAdapterPreparationStage::destination_allocation))
                return std::unexpected{std::move(*fault)};
            if (auto fault = injected(
                    detail::ProductionAdapterPreparationStage::ownership_transfer))
                return std::unexpected{std::move(*fault)};
            auto implementation = std::make_unique<ProductionRunCore>(
                state.shared_from_this(), std::move(cancellation), std::move(*team),
                std::move(*family), std::move(*monitoring_owner), inputs.measurement,
                std::move(cleanup_ledger), std::move(first_cleanup_slots),
                std::move(second_cleanup_slots), std::move(first_finalization_diagnostics),
                std::move(second_finalization_diagnostics), std::move(finalization_worker_ids),
                std::move(first_finalized_workers), std::move(second_finalized_workers),
                std::move(finalization_associations),
                std::move(diagnostic_domain),
                std::move(first_workspaces), std::move(second_workspaces),
                std::move(first_facts), std::move(second_facts));
            guard.disarm();
            return implementation;
        } catch (const std::exception&) {
            auto primary = cleanup_ledger->take_prepare_primary(
                CleanupBoundaryDisposition::contained_exception);
            guard.cleanup();
            return std::unexpected{failure(std::move(primary), guard.take_projected())};
        } catch (...) {
            auto primary = cleanup_ledger->take_prepare_primary(
                CleanupBoundaryDisposition::contained_unknown);
            guard.cleanup();
            return std::unexpected{failure(std::move(primary), guard.take_projected())};
        }
    } catch (const std::exception&) {
        return std::unexpected{failure(adapter_error("contained_exception"))};
    } catch (...) {
        return std::unexpected{failure(adapter_error("contained_unknown_failure"))};
    }
}

} // namespace


auto detail::prepare_calibration_production_kernel(
    PairwiseProductionOperationsAdapter::State& state,
    const CalibrationOnlyProductionAdmission& inputs) noexcept
    -> detail::LegacyProductionOperationResult<std::unique_ptr<detail::CalibrationProductionKernel>> {
    try {
        auto prepared = prepare_common(
            state, {inputs.first(), inputs.second(), inputs.monitoring_policy(),
                    inputs.service_placement(),
                    inputs.first().execution_inputs().measurement()});
        if (!prepared) {
            return std::unexpected{std::move(prepared.error())};
        }
        return std::unique_ptr<detail::CalibrationProductionKernel>{std::move(*prepared)};
    } catch (const std::exception&) {
        return std::unexpected{detail::LegacyProductionOperationFailure{
            adapter_error("contained_exception"), {}}};
    } catch (...) {
        return std::unexpected{detail::LegacyProductionOperationFailure{
            adapter_error("contained_unknown_failure"), {}}};
    }
}

} // namespace epycbench::runtime
