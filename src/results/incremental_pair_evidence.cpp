#include "results/incremental_pair_evidence.hpp"
#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <numeric>
#include "core/json_ownership.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "runtime/finalized_evidence_wire_memory_projection.hpp"
namespace epycbench::results::block_evidence {
namespace wire = runtime::wire_v1;
namespace d = wire::detail;
auto PreparationFactsLease::retain() const noexcept -> PreparationFactsLease {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return PreparationFactsLease{*actual};
    return PreparationFactsLease{std::get<1>(value_)};
}
auto PreparationFactsLease::published() const noexcept -> bool {
    const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_);
    return actual && actual->published();
}
auto PreparationFactsLease::domain() const noexcept -> runtime::detail::MonitoringPreparationFactDomain {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return actual->domain();
    const auto& decoded=std::get<1>(value_);
    return decoded?decoded->domain:runtime::detail::MonitoringPreparationFactDomain::synthetic;
}
auto PreparationFactsLease::facts() const noexcept -> std::optional<runtime::detail::PreparationGraphFactsView> {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return actual->facts();
    const auto& decoded_value=std::get<1>(value_);
    if (!decoded_value) return std::nullopt;
    const auto& v=*decoded_value;
    return runtime::detail::PreparationGraphFactsView{v.policies,v.monitoring,v.provenance,v.measurement,
        v.experiment,v.first_request,v.second_request,v.workers,v.memory,v.kernels,v.targets,v.pages,v.frequency,
        v.cpus,v.strings,v.frequency_count};
}
auto PreparationFactsLease::policies() const noexcept -> const runtime::detail::PreparationGraphPolicies* {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return actual->policies();
    const auto& value=std::get<1>(value_); return value && value->policies ? &*value->policies : nullptr;
}
auto PreparationFactsLease::monitoring() const noexcept -> const runtime::detail::PreparationMonitoringIdentity* {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return actual->monitoring();
    const auto& value=std::get<1>(value_); return value && value->monitoring ? &*value->monitoring : nullptr;
}
auto PreparationFactsLease::frequency_count() const noexcept -> std::size_t {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) return actual->frequency_count();
    const auto& value=std::get<1>(value_); return value ? value->frequency_count : 0U;
}
auto PreparationFactsLease::frequency(std::size_t index) const noexcept -> std::optional<runtime::detail::MonitoringPreparationFrequencyView> {
    const auto view=facts();
    if (!view || index>=view->frequency_count || index>=view->frequency.size()) return std::nullopt;
    const auto& row=view->frequency[index];
    if (row.cpus.offset>view->cpus.size() || row.cpus.size>view->cpus.size()-row.cpus.offset) return std::nullopt;
    return runtime::detail::MonitoringPreparationFrequencyView{view->cpus.subspan(row.cpus.offset,row.cpus.size),row.kind,
        view->text(row.driver),view->text(row.governor),row.minimum,row.maximum};
}
auto PreparationFactsLease::retained_bytes() const noexcept -> std::size_t {
    if (const auto* actual=std::get_if<runtime::detail::PairwisePreparationGraph>(&value_)) {
        constexpr auto extra=sizeof(PreparationFactsLease)-sizeof(runtime::detail::PairwisePreparationGraph);
        return actual->usage().retained>SIZE_MAX-extra ? SIZE_MAX : actual->usage().retained+extra;
    }
    const auto& value=std::get<1>(value_); return value ? value->retained : 0U;
}
namespace {
using Sum = d::TypedStockSum;
auto error(ErrorCode code, Phase phase = Phase::event) noexcept {
    return std::unexpected{Error{code, phase}};
}
auto exact_policy(const runtime::StatisticsProductionPolicy& a,
                  const runtime::StatisticsProductionPolicy& b) noexcept -> bool {
    const auto same = [](double x, double y) { return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y); };
    return a == b &&
        same(a.comparison_bootstrap_policy().confidence().value(), b.comparison_bootstrap_policy().confidence().value()) &&
        same(a.stopping_bootstrap_policy().confidence().value(), b.stopping_bootstrap_policy().confidence().value()) &&
        same(a.effective_practical_effect_fraction(), b.effective_practical_effect_fraction()) &&
        same(a.practical_effect_threshold().delta(), b.practical_effect_threshold().delta()) &&
        same(a.relative_half_width_target().value(), b.relative_half_width_target().value());
}
auto same_terminal(const runtime::PairwiseRunTerminalEvidence& a,
                   const runtime::PairwiseRunTerminalEvidence& b) noexcept -> bool {
    if (a.index() != b.index()) return false;
    if (const auto* normal = std::get_if<runtime::NormalRunTerminalEvidence>(&a)) {
        const auto& x = normal->outcome;
        const auto& y = std::get<runtime::NormalRunTerminalEvidence>(b).outcome;
        return x.experiment_id() == y.experiment_id() && x.policy() == y.policy() &&
            x.disposition() == y.disposition() && x.completed_blocks() == y.completed_blocks() &&
            x.complete_valid_blocks() == y.complete_valid_blocks() &&
            x.valid_first_variant_observations() == y.valid_first_variant_observations() &&
            x.valid_second_variant_observations() == y.valid_second_variant_observations();
    }
    const auto& x = std::get<runtime::OperationalRunTerminalEvidence>(a);
    const auto& y = std::get<runtime::OperationalRunTerminalEvidence>(b);
    return x.primary_error == y.primary_error && x.secondary_errors == y.secondary_errors &&
        x.active_block_id == y.active_block_id;
}
auto valid_error(const runtime::FinalizedEvidenceError& value) noexcept -> bool {
    using namespace runtime;
    if (static_cast<unsigned>(value.code()) >= static_cast<unsigned>(PairwiseFailureCode::count) ||
        static_cast<unsigned>(value.operation()) >= static_cast<unsigned>(PairwiseFailureOperation::count) ||
        static_cast<unsigned>(value.reason()) >= static_cast<unsigned>(PairwiseFailureReason::count))
        return false;
    std::optional<int> native;
    if (value.os_error()) {
        if (value.os_error()->domain() != PairwiseOsDomain::posix_errno) return false;
        native = value.os_error()->value();
    }
    return pairwise_validate_failure(value.subsystem(), value.code(), value.operation(),
                                    value.reason(), value.context(), native);
}
struct ControlReadinessStaging final {
    Sum strings, payload, retained, peak;
    std::size_t side{}, index{};
};
static_assert(std::is_nothrow_move_constructible_v<ControlReadinessEvent>);
struct MemoryPreparationStaging final {
    Sum workers, targets, pages, strings, payload, retained, peak;
    std::size_t row{}, target{}, page{};
    std::array<std::uint64_t, 4> ratio{};
    MemoryPreparationUsage current;
};
// Conservative named valid-path evaluator working set, including non-elided returns.
struct PlacementEvaluatorScalars final {
    std::array<std::uint64_t, 19> counters;
    std::optional<core::LinuxNumaNodeId> prior;
    core::LinuxNumaNodeId observed;
    std::array<bool, 3> flags;
    memory::detail::ExactRatioOrdering ordering;
    memory::PlacementQualificationState qualification;
    std::strong_ordering comparison;
    std::array<memory::ExactRatio, 2> ratios;
    std::array<const void*, 6> references;
};
auto same_evaluation(const memory::RegionPlacementEvaluation& a,
                     const memory::RegionPlacementEvaluation& b) noexcept -> bool {
    return a.requested_node_pages() == b.requested_node_pages() &&
        a.unexpected_node_pages() == b.unexpected_node_pages() &&
        a.unresolved_pages() == b.unresolved_pages() && a.observed_ratio() == b.observed_ratio();
}
auto same_observation(const memory::RegionPlacementObservation& a,
                      const memory::RegionPlacementObservation& b) noexcept -> bool {
    return a.total_pages() == b.total_pages() && a.resident_pages() == b.resident_pages() &&
        a.unresolved_pages() == b.unresolved_pages() &&
        std::ranges::equal(a.resident_page_counts(), b.resident_page_counts());
}
auto same_request(const memory::RegionRequest& a, const memory::RegionRequest& b) noexcept -> bool {
    return a.worker_id() == b.worker_id() && a.requested_bytes() == b.requested_bytes() &&
        a.page_size() == b.page_size() && a.placement_policy() == b.placement_policy() &&
        a.page_mode() == b.page_mode() && std::ranges::equal(a.numa_targets(), b.numa_targets()) &&
        a.placement_threshold() == b.placement_threshold() && a.prefault() == b.prefault() &&
        a.verify_before() == b.verify_before() && a.verify_after() == b.verify_after();
}
auto valid_preparation(const MemoryPreparationRow& row, const ExpectedMemoryCell& expected,
                       core::WorkerId id) noexcept -> bool {
    const auto& request = row.request;
    const auto& source = expected.request;
    if (request.worker_id() != id || request.requested_bytes() != source.bytes() ||
        request.page_size() != source.page_size_bytes() ||
        request.placement_policy() != memory::PlacementPolicy::bind || request.page_mode() != memory::PageMode::base ||
        request.requested_bytes().value() == 0U || request.page_size().value() == 0U ||
        request.requested_bytes().value() % request.page_size().value() != 0U ||
        request.prefault() != source.prefault() || request.verify_before() != source.verify_before() ||
        request.verify_after() != source.verify_after() || source.nodes().empty() ||
        request.numa_targets().size() != source.nodes().size() || expected.resolved_nodes.size() != source.nodes().size()) return false;
    const auto numerator = source.placement_threshold().numerator();
    const auto denominator = source.placement_threshold().denominator();
    if (!denominator || numerator > denominator) return false;
    const auto divisor = std::gcd(numerator, denominator);
    if (request.placement_threshold().numerator() != numerator / divisor ||
        request.placement_threshold().denominator() != denominator / divisor) return false;
    for (std::size_t i{}; i < source.nodes().size(); ++i) {
        const auto& target = request.numa_targets()[i];
        if (target.stable_node != source.nodes()[i].stable_numa_node_id() ||
            target.linux_node != source.nodes()[i].linux_numa_node() || target.topology_node != expected.resolved_nodes[i]) return false;
    }
    if (row.binding.policy() != memory::PlacementPolicy::bind || row.binding.range() != memory::BindingRange::full_mapping ||
        !row.binding.policy_call_accepted() || row.binding.migration_requested() || row.binding.strict_verification_requested() ||
        row.initialization.range() != memory::InitializationRange::logical_requested_region ||
        row.initialization.prefault_requested() != request.prefault() ||
        row.initialization.prefault_pass_performed() != request.prefault() ||
        !row.initialization.initialization_completed() || row.initialization.padding_touched() ||
        row.protection.protection_kind() != memory::RegionProtectionKind::read_only ||
        row.protection.range() != memory::RegionProtectionRange::full_mapping ||
        !row.protection.protection_accepted() || row.protection.writable() || row.protection.executable()) return false;
    if (row.state == memory::PreRunDiagnosticState::skipped)
        return !request.verify_before() && !row.observation && !row.evaluation && !row.qualification;
    if (row.state != memory::PreRunDiagnosticState::observed || !request.verify_before() ||
        !row.observation || !row.evaluation || row.qualification != memory::PlacementQualificationState::qualified ||
        row.observation->total_pages() != request.requested_bytes().value() / request.page_size().value() ||
        !d::placement_evaluator_domain(*row.observation)) return false;
    auto evaluated = memory::detail::PlacementEvaluatedRegionAccess::evaluate_region_placement(request, *row.observation);
    core::detail::ExpectedErrorCleanup cleanup{evaluated};
    return evaluated && evaluated->qualification_state == *row.qualification &&
        same_evaluation(evaluated->evaluation, *row.evaluation);
}
auto matches_preparation(const MemoryPreparationEvent& event,
                         const runtime::FinalizedVariantEvidence& variant) noexcept -> bool {
    if (event.workers.size() != variant.workers.size()) return false;
    for (std::size_t i{}; i < event.workers.size(); ++i) {
        const auto& row = event.workers[i];
        const auto& worker = variant.workers[i];
        if (worker.worker_id != row.request.worker_id()) return false;
        const auto* available = worker.region.available();
        if (!available) continue; // The retained earlier facts survive unavailable finalization.
        const auto& pre = available->pre_run();
        if (!same_request(row.request, available->request()) ||
            row.binding.policy() != available->binding_evidence().policy() ||
            row.binding.range() != available->binding_evidence().range() ||
            row.binding.policy_call_accepted() != available->binding_evidence().policy_call_accepted() ||
            row.binding.migration_requested() != available->binding_evidence().migration_requested() ||
            row.binding.strict_verification_requested() != available->binding_evidence().strict_verification_requested() ||
            row.initialization.range() != available->initialization_evidence().range() ||
            row.initialization.prefault_requested() != available->initialization_evidence().prefault_requested() ||
            row.initialization.prefault_pass_performed() != available->initialization_evidence().prefault_pass_performed() ||
            row.initialization.initialization_completed() != available->initialization_evidence().initialization_completed() ||
            row.initialization.padding_touched() != available->initialization_evidence().padding_touched() ||
            row.state != pre.state() || row.qualification != pre.qualification_state() ||
            row.observation.has_value() != pre.observation().has_value() ||
            row.evaluation.has_value() != pre.evaluation().has_value() || !available->protection_evidence()) return false;
        const auto& protection = *available->protection_evidence();
        if (row.protection.protection_kind() != protection.protection_kind() || row.protection.range() != protection.range() ||
            row.protection.protection_accepted() != protection.protection_accepted() || row.protection.writable() != protection.writable() ||
            row.protection.executable() != protection.executable()) return false;
        if (row.observation && !same_observation(*row.observation, *pre.observation())) return false;
        if (row.evaluation && !same_evaluation(*row.evaluation, *pre.evaluation())) return false;
    }
    return true;
}
static_assert(std::is_nothrow_move_constructible_v<MemoryPreparationEvent>);

struct ReadinessStaging final {
    std::vector<std::uint64_t> tids;
    Sum strings, payload, retained, peak;
    std::size_t index{};
};
static_assert(std::is_nothrow_move_constructible_v<ReadinessEvent>);
using Slot = std::optional<Accumulator>;
constexpr auto fixed_stock =
    sizeof(d::RootReconstructionUsage) + sizeof(d::ComponentProjectionCount) + sizeof(Error) +
    sizeof(wire::LogicalBody) + 2U * sizeof(runtime::FinalizedPairwiseRunEvidence) +
    sizeof(runtime::PairwiseRunEvidenceLedger);
} // namespace
struct PairPrefix::State final {
    std::optional<PairFactsLease> context;
    Domain domain{};
    PairLimits limits;
    std::vector<Slot> blocks;
    std::vector<std::uint64_t> ids;
    std::vector<StoppingEvent> stopping;
    std::optional<runtime::StatisticsProductionPolicy> statistics;
    std::optional<measurement::RepetitionPolicy> repetition;
    std::size_t count{}, event_count{};
    std::uint64_t sequence{};
    bool consumed{}, active{}, terminal_mode{}, finalization_mode{}, readiness_mode{}, memory_mode{}, control_mode{}, monitoring_mode{}, preparation_mode{}, outcomes_mode{};
    std::optional<ControlReadinessEvent> control;
    std::optional<MonitoringPreparationEvent> monitoring;
    std::optional<PreparationAssociationEvent> preparation;
    std::optional<CalibrationEvent> calibration;
    std::optional<WarmupEvent> warmup;
    std::array<std::optional<MemoryPreparationEvent>, 2> memory;
    std::optional<ReadinessEvent> readiness;
    std::optional<FinalizationEvent> finalization;
    std::optional<TerminalEvent> terminal;
    std::string root;
    PairUsage usage;
    bool timing_mode{};
    observation_timing::Limits timing_limits;
    observation_timing::Usage timing_usage;
};
PairPrefix::PairPrefix(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PairPrefix::PairPrefix(PairPrefix&&) noexcept = default;
auto PairPrefix::operator=(PairPrefix&&) noexcept -> PairPrefix& = default;
PairPrefix::~PairPrefix() = default;
auto PairPrefix::events(std::size_t index) const noexcept -> std::span<const Event> {
    return state_ && index < state_->count ? state_->blocks[index]->events()
                                           : std::span<const Event>{};
}
auto PairPrefix::block_count() const noexcept -> std::size_t { return state_ ? state_->count : 0U; }
auto PairPrefix::consumed() const noexcept -> bool { return !state_ || state_->consumed; }
auto PairPrefix::lifecycle_context() const noexcept -> const ExpectedPairContext* {
    return state_ && state_->context && state_->context->available() ? state_->context->actual_context() : nullptr;
}
auto PairPrefix::lifecycle_outcomes_mode() const noexcept -> bool {
    return state_ && state_->outcomes_mode;
}
auto PairPrefix::canonical_root() const noexcept -> std::string_view {
    return state_ ? state_->root : std::string_view{};
}
auto PairPrefix::usage() const noexcept -> PairUsage {
    return state_ ? state_->usage : PairUsage{};
}
auto PairPrefix::domain() const noexcept -> std::optional<Domain> {
    return state_ ? std::optional{state_->domain} : std::nullopt;
}
auto PairPrefix::stopping_events() const noexcept -> std::span<const StoppingEvent> {
    return state_ ? std::span<const StoppingEvent>{state_->stopping} : std::span<const StoppingEvent>{};
}
auto PairPrefix::prepare(ExpectedPairContext&& context, Domain domain, PairLimits limits)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, std::nullopt, std::nullopt);
}
auto PairPrefix::prepare_with_stopping(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition);
}
auto PairPrefix::prepare_with_terminal(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true);
}
auto PairPrefix::prepare_with_finalization(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true, true);
}
auto PairPrefix::prepare_with_worker_readiness(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true, true, true);
}
auto PairPrefix::prepare_with_memory_preparation(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true, true, true, true);
}
auto PairPrefix::prepare_with_monitoring_preparation(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true, true, true, true, true, true);
}
auto PairPrefix::prepare_with_complete_preparation(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context),domain,limits,statistics,repetition,true,true,true,true,true,true,true);
}
auto PairPrefix::prepare_with_preparation_outcomes(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context),domain,limits,statistics,repetition,true,true,true,true,true,true,true,true);
}
namespace {
struct TimingFactoryCells final {
    ExpectedPairContext* context;
    Domain domain;
    PairLimits limits;
    runtime::StatisticsProductionPolicy statistics;
    measurement::RepetitionPolicy repetition;
    observation_timing::Limits timing;
    std::size_t ceiling, fixed;
    std::expected<PairPrefix, Error> result, returned;
    ErrorCode reject_code;
    Phase reject_phase;
    Error rejection;
    std::unexpected<Error> returned_rejection;
};
}
auto PairPrefix::timing_factory_staging_bytes() noexcept -> std::size_t { return sizeof(TimingFactoryCells); }
auto PairPrefix::prepare_with_observation_timing(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition,
    observation_timing::Limits timing) -> std::expected<PairPrefix, Error> {
    const auto ceiling = limits.simultaneous_bytes;
    const auto fixed = timing_factory_staging_bytes();
    if (fixed > ceiling) return error(ErrorCode::capacity, Phase::context);
    limits.simultaneous_bytes -= fixed;
    auto result = prepare_with_preparation_outcomes(std::move(context), domain, limits, statistics, repetition);
    if (result) {
        result->state_->timing_mode = true; result->state_->timing_limits = timing;
        result->state_->limits.simultaneous_bytes = ceiling;
    }
    return result;
}
auto PairPrefix::prepare_decoded(PairFactsLease&& context, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition,
    observation_timing::Limits timing) -> std::expected<PairPrefix,Error> {
    if(!context.available()||context.origin()!=PairFactsLease::Origin::decoded_evidence)
        return error(ErrorCode::identity,Phase::context);
    const auto ceiling=limits.simultaneous_bytes;const auto fixed=timing_factory_staging_bytes();
    if(fixed>ceiling)return error(ErrorCode::capacity,Phase::context);
    limits.simultaneous_bytes-=fixed;
    const auto retained_domain=context.decoded_domain();
    if(!retained_domain)return error(ErrorCode::identity,Phase::context);
    const auto domain=*retained_domain==process_subject::Domain::supplied_observation?Domain::supplied_observation:Domain::synthetic_execution;
    auto result=prepare_impl(std::move(context),domain,limits,statistics,repetition,true,true,true,true,true,true,true,true);
    if(result){result->state_->timing_mode=true;result->state_->timing_limits=timing;result->state_->limits.simultaneous_bytes=ceiling;}
    return result;
}
auto PairPrefix::timing_usage() const noexcept -> observation_timing::Usage {
    return state_ ? state_->timing_usage : observation_timing::Usage{};
}
auto PairPrefix::timing_coverage() const noexcept -> observation_timing::Coverage {
    observation_timing::Coverage result{};
    if (!state_ || !state_->timing_mode) return result;
    result.requested = true; result.completed_starts_available = true;
    for (std::size_t block{}; block < state_->count; ++block) for (const auto& event : events(block)) {
        if (!event.timing) continue;
        const auto disposition = *event.timing->disposition();
        const auto index = static_cast<std::size_t>(disposition);
        if (event.kind == EventKind::factual) {
            ++result.completed; ++result.completed_dispositions[index];
            if (disposition != observation_timing::Disposition::available) result.completed_starts_available = false;
        } else { ++result.failed; ++result.failed_dispositions[index]; }
    }
    return result;
}
auto PairPrefix::calibration_event() const noexcept -> const CalibrationEvent* {
    return state_ && state_->calibration ? &*state_->calibration : nullptr;
}
auto PairPrefix::warmup_event() const noexcept -> const WarmupEvent* {
    return state_ && state_->warmup ? &*state_->warmup : nullptr;
}
auto PairPrefix::calibration(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    return preparation_outcome(seq,time,bytes,wire::EvidenceComponentKind::calibration);
}
auto PairPrefix::warmup(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    return preparation_outcome(seq,time,bytes,wire::EvidenceComponentKind::warmup);
}
namespace {
struct OutcomeAdmissionStaging final {
    std::string_view bytes;
    const journal::Timestamp* time;
    PairPrefix::State* state;
    const runtime::detail::PreparationGraphPolicies* policy;
    const CalibrationEvent* prior_calibration;
    const PreparationAssociationEvent* preparation;
    const Limits* limits;
    const PreparationOutcomeLimits* cap;
    wire::EvidenceComponentKind kind;
    bool calibration;
    std::uint64_t sequence, prerequisite;
    std::size_t ceiling, payload_limit, scratch_limit;
    std::expected<d::ComponentWireShape,d::WireJsonError> shape;
    std::expected<std::size_t,d::WireJsonError> j,p,q,i,jp,pp,qp,ip;
    std::expected<wire::LogicalBody,wire::LogicalCodecError> logical;
    std::expected<runtime::PairwiseCalibrationEvidence,d::FixedTypedReconstructionError> calibration_value;
    std::expected<runtime::PairwiseWarmupEvidence,d::FixedTypedReconstructionError> warmup_value;
    std::optional<runtime::PairwiseCalibrationEvidence> calibrated;
    std::optional<runtime::PairwiseWarmupEvidence> warmed;
    d::PreparationOutcomeStocks stock;
    // typed_preparation_outcome_stocks has its own local owner and a
    // non-elided return, in addition to the caller's stock above.
    d::PreparationOutcomeStocks stock_helper_result, stock_helper_return;
    wire::EvidenceComponentKind stock_helper_kind;
    const nlohmann::json* stock_helper_value;
    const std::string* stock_helper_text;
    std::uint64_t stock_helper_window, stock_helper_count, stock_trial_count_argument;
    std::from_chars_result stock_helper_conversion;
    // TypedStockSum::add keeps product while computing sum; both called
    // checked helpers also have distinct return cells and scalar arguments.
    Sum* stock_sum_receiver;
    std::size_t stock_n, stock_size, stock_multiply_n, stock_multiply_size, stock_add_left, stock_add_right;
    std::expected<std::size_t,d::WireJsonError> stock_product, stock_sum, stock_product_return, stock_sum_return;
    d::ComponentProjectionCount count;
    d::WireJson projected;
    std::expected<std::string,d::WireJsonError> canonical;
    CalibrationEvent calibration_candidate;
    WarmupEvent warmup_candidate;
    journal::Timestamp timestamp_copy;
    std::string canonical_move;
    Sum base,parse,retained,reconstruct,projection;
    PreparationOutcomeUsage usage;
    const void *reject_capture,*reconstruction_capture;
    Error failure, reject_value;
    std::unexpected<Error> unexpected_return;
    std::expected<void,Error> error_return, admission_return;
    const d::FixedTypedReconstructionError* detail;
    std::string_view path;
    std::optional<std::uint64_t> selected;
    std::size_t identity_index;
    // Graph/selection comparison visitors have only borrowed captures and fixed
    // policy/count/estimate accessor returns, all summed explicitly here.
    const void *policy_capture,*outcome_capture,*selected_capture;
    const measurement::CalibrationDurationPolicy* calibration_policy;
    const measurement::WarmupPolicy* warmup_policy;
    std::optional<kernel::SequentialReadBandwidthPassCount> bandwidth_selected;
    std::optional<kernel::DependentLoadLatencyStepCount> latency_selected;
    measurement::WarmupStabilityThreshold actual_threshold, prepared_threshold;
};
static_assert(sizeof(decltype(OutcomeAdmissionStaging::stock_helper_return))==
    sizeof(decltype(d::typed_preparation_outcome_stocks(wire::EvidenceComponentKind::calibration,std::declval<const nlohmann::json&>()))));
static_assert(sizeof(decltype(OutcomeAdmissionStaging::calibration_candidate))==sizeof(CalibrationEvent));
static_assert(sizeof(decltype(OutcomeAdmissionStaging::warmup_candidate))==sizeof(WarmupEvent));
}
auto PairPrefix::preparation_outcome_staging_bytes() noexcept -> std::size_t {
    auto total=sizeof(OutcomeAdmissionStaging);
    for (const auto bytes:d::preparation_outcome_reconstruction_staging()) total+=bytes;
    return total;
}
namespace {
auto selected_work(const CalibrationEvent& event) noexcept -> std::optional<std::uint64_t> {
    return std::visit([](const auto& value) -> std::optional<std::uint64_t> {
        if (value.selected_candidate()) return value.selected_candidate()->value();
        return std::nullopt;
    },event.evidence);
}
auto matches_calibration(const runtime::detail::PreparationGraphPolicies& policy,
                         const runtime::PairwiseCalibrationEvidence& value) noexcept -> bool {
    return policy.calibration_enabled && value.index()==policy.kernel.index() &&
        std::visit([&](const auto& outcome) {
            const auto& actual=outcome.duration_policy();
            return actual.minimum_duration()==policy.calibration.minimum_duration() &&
                actual.target_duration()==policy.calibration.target_duration() &&
                actual.maximum_duration()==policy.calibration.maximum_duration();
        },value);
}
auto matches_warmup(const runtime::detail::PreparationGraphPolicies& policy,
                    std::uint64_t selected, const runtime::PairwiseWarmupEvidence& value) noexcept -> bool {
    return value.index()==policy.kernel.index() && std::visit([&](const auto& outcome) {
        const auto& actual=outcome.policy();
        return outcome.selected_work().value()==selected &&
            actual.minimum_trials()==policy.warmup.minimum_trials() &&
            actual.maximum_trials()==policy.warmup.maximum_trials() &&
            actual.stability_window()==policy.warmup.stability_window() &&
            actual.stability_threshold().numerator()==policy.warmup.stability_threshold().numerator() &&
            actual.stability_threshold().denominator()==policy.warmup.stability_threshold().denominator();
    },value);
}
auto matches_selected_fact(const CalibrationEvent& calibration,
                           const runtime::FactualObservationEvidence& fact) noexcept -> bool {
    const auto selected=selected_work(calibration);
    if (!selected || fact.observation.index()!=calibration.evidence.index()) return false;
    const auto overhead=std::visit([](const auto& outcome) { return outcome.timer_overhead_estimate().duration(); },calibration.evidence);
    if (fact.validity.timer_overhead().estimate().duration()!=overhead) return false;
    return std::visit([&](const auto& observation) {
        if constexpr (requires { observation.pass_count(); }) return observation.pass_count().value()==*selected;
        else return std::ranges::all_of(observation.workers(),[&](const auto& worker) {
            return worker.schedule().step_count().value()==*selected;
        });
    },fact.observation);
}
}
auto PairPrefix::preparation_outcome(std::uint64_t seq, const journal::Timestamp& time,
    std::string_view bytes, wire::EvidenceComponentKind kind) -> std::expected<void,Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s=*state_;
    auto reject=[&](ErrorCode code, Phase phase=Phase::event) {
        s.consumed=true; return error(code,phase);
    };
    const bool calibration=kind==wire::EvidenceComponentKind::calibration;
    if (!s.outcomes_mode || !s.preparation || s.count || !s.stopping.empty() || s.terminal || s.finalization ||
        (calibration ? bool(s.calibration) || bool(s.warmup) : bool(s.warmup) || !s.calibration)) return reject(ErrorCode::state);
    if (s.sequence==UINT64_MAX || !seq || seq!=s.sequence+1U || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count==s.limits.events) return reject(ErrorCode::capacity);
    const auto& policy=*s.preparation->graph.policies();
    if (!policy.repetition.maximum_blocks() || !policy.calibration_enabled ||
        (!calibration && !selected_work(*s.calibration))) return reject(ErrorCode::association);
    const auto& limits=s.limits.component;
    const auto ceiling=std::min(limits.simultaneous_bytes,s.limits.simultaneous_bytes);
    if (bytes.size()>limits.canonical_bytes || bytes.size()>std::string{}.max_size()) return reject(ErrorCode::capacity,Phase::parse);
    const auto shape=d::count_component_wire_shape(bytes,limits.depth);
    if (!shape) return reject(shape.error().code==d::WireJsonFailure::malformed?ErrorCode::malformed:ErrorCode::capacity,Phase::parse);
    if (shape->nodes>limits.nodes || shape->strings>limits.strings || shape->elements>limits.array_elements)
        return reject(ErrorCode::capacity,Phase::parse);
    const auto j=d::component_dom_stock(shape->nodes,shape->strings);
    const auto p=d::component_parser_stock(shape->bytes,shape->depth);
    const auto q=d::component_renderer_stock(shape->depth,shape->members,shape->bytes);
    const auto i=d::multiply_component_stock(shape->depth,sizeof(d::JsonRetirementFrame));
    if (!j || !p || !q || !i) return reject(ErrorCode::capacity,Phase::parse);
    Sum base{s.usage.retained}; base.add(time.text().size()); base.add(preparation_outcome_staging_bytes());
    Sum parse{base.value}; parse.add(2U,bytes.size()); parse.add(2U,*j); parse.add(*p); parse.add(2U,*q);
    if (!base.valid || !parse.valid || parse.value>ceiling) return reject(ErrorCode::capacity,Phase::parse);
    try {
        auto logical=wire::decode_evidence_component(kind,wire::EvidenceComponentSide::first,bytes,s.context->capacity());
        if (!logical) return reject(logical.error().code==wire::LogicalCodecErrorCode::allocation_failure?
            ErrorCode::allocation_failure:ErrorCode::malformed,Phase::parse);
        const auto stock=d::typed_preparation_outcome_stocks(kind,logical->value());
        const auto& cap=s.limits.preparation_outcomes;
        const auto payload_limit=calibration?cap.calibration_payload:cap.warmup_payload;
        const auto scratch_limit=calibration?cap.calibration_scratch:cap.warmup_scratch;
        if (!stock.owned.valid || !stock.scratch.valid || stock.candidates>cap.candidates || stock.trials>cap.trials ||
            stock.owned.value>payload_limit || stock.scratch.value>scratch_limit ||
            stock.candidates>std::vector<measurement::SequentialReadBandwidthCalibrationTrial>{}.max_size() ||
            stock.candidates>std::vector<measurement::DependentLoadLatencyCalibrationTrial>{}.max_size() ||
            stock.candidates>std::vector<std::uint64_t>{}.max_size() ||
            stock.trials>std::vector<measurement::WarmupTrial>{}.max_size() ||
            stock.scratch.value/sizeof(std::uint64_t)>std::vector<std::uint64_t>{}.max_size() ||
            stock.trials>policy.warmup.maximum_trials()) return reject(ErrorCode::capacity,Phase::reconstruct);
        Sum retained{s.usage.retained}; retained.add(time.text().size()); retained.add(bytes.size()); retained.add(stock.owned.value);
        Sum reconstruct{base.value}; reconstruct.add(2U,bytes.size()); reconstruct.add(*j); reconstruct.add(*i);
        reconstruct.add(2U,stock.owned.value); reconstruct.add(stock.scratch.value);
        if (!retained.valid || !reconstruct.valid || retained.value>s.limits.retained_bytes || reconstruct.value>ceiling)
            return reject(ErrorCode::capacity,Phase::reconstruct);
        std::optional<runtime::PairwiseCalibrationEvidence> calibrated;
        std::optional<runtime::PairwiseWarmupEvidence> warmed;
        auto reconstruction_error=[&](const d::FixedTypedReconstructionError& detail) -> std::expected<void,Error> {
            s.consumed=true;
            Error failure{ErrorCode::reconstruction,Phase::reconstruct};
            failure.detail_code=static_cast<std::uint8_t>(detail.code);
            const auto path=detail.path.view();
            if (!detail.path.fixed() || detail.path.overflowed() || path.size()>failure.diagnostic.identity.size())
                failure.code=ErrorCode::diagnostic_overflow;
            else {
                std::copy(path.begin(),path.end(),failure.diagnostic.identity.begin());
                failure.diagnostic.identity_size=path.size();
            }
            return std::unexpected{failure};
        };
        if (calibration) {
            auto value=d::fixed_reconstruct_calibration_component(logical->value());
            if (!value) return reconstruction_error(value.error());
            if (!matches_calibration(policy,*value)) return reject(ErrorCode::association);
            calibrated.emplace(std::move(*value));
        } else {
            auto value=d::fixed_reconstruct_warmup_component(logical->value());
            if (!value) return reconstruction_error(value.error());
            if (!matches_warmup(policy,*selected_work(*s.calibration),*value)) return reject(ErrorCode::association);
            warmed.emplace(std::move(*value));
        }
        const auto count=calibration?d::count_calibration_component(*calibrated):d::count_warmup_component(*warmed);
        if (count.overflow || count.invalid || count.nodes>limits.nodes || count.strings>limits.strings ||
            count.elements>limits.array_elements || count.depth>limits.depth || count.bytes!=bytes.size())
            return reject(ErrorCode::projection,Phase::project);
        const auto jp=d::component_dom_stock(count.nodes,count.strings);
        const auto pp=d::component_parser_stock(count.bytes,count.depth);
        const auto qp=d::component_renderer_stock(count.depth,count.members,count.bytes);
        const auto ip=d::multiply_component_stock(count.depth,sizeof(d::JsonRetirementFrame));
        if (!jp || !pp || !qp || !ip) return reject(ErrorCode::capacity,Phase::project);
        Sum projection{base.value}; projection.add(2U,bytes.size()); projection.add(*j); projection.add(*i);
        projection.add(stock.owned.value); projection.add(3U,*jp); projection.add(*pp); projection.add(2U,*qp);
        projection.add(3U,count.bytes); projection.add(*ip);
        if (!projection.valid || projection.value>ceiling) return reject(ErrorCode::capacity,Phase::project);
        d::WireJson projected{count.depth};
        projected.value=calibration?d::project_calibration_component(*calibrated):d::project_warmup_component(*warmed);
        auto canonical=d::render_wire_json(projected.value,count.depth,count.nodes,count.bytes);
        if (!canonical || *canonical!=bytes) return reject(ErrorCode::projection,Phase::project);
        const PreparationOutcomeUsage usage{parse.value,reconstruct.value,projection.value,stock.owned.value,
            stock.scratch.value,stock.candidates,stock.trials};
        if (calibration) {
            CalibrationEvent candidate{seq,time,s.preparation->sequence,std::move(*calibrated),std::move(*canonical),usage};
            static_assert(std::is_nothrow_move_constructible_v<CalibrationEvent>);
            s.calibration.emplace(std::move(candidate));
        } else {
            WarmupEvent candidate{seq,time,s.calibration->sequence,std::move(*warmed),std::move(*canonical),usage};
            static_assert(std::is_nothrow_move_constructible_v<WarmupEvent>);
            s.warmup.emplace(std::move(candidate));
        }
        s.sequence=seq; ++s.event_count; s.usage.retained=retained.value;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
namespace {
struct PreparationAssociationStaging final {
    const ControlReadinessEvent* control{};
    const ReadinessEvent* workers{};
    const MemoryPreparationEvent* first{};
    const MemoryPreparationEvent* second{};
    const MonitoringPreparationEvent* monitoring{};
    std::optional<PreparationFactsLease> lease;
    std::optional<PreparationAssociationEvent> event;
    std::size_t side{}, row{}, domain{}, offset{};
    Sum retained, peak;
    std::expected<void,Error> result;
    Error error{};
};
}
auto PairPrefix::preparation_staging_bytes() noexcept -> std::size_t {
    return sizeof(PreparationAssociationStaging) + sizeof(PreparationFactsLease) +
        sizeof(std::optional<runtime::detail::PreparationGraphFactsView>) +
        runtime::detail::PairwisePreparationGraph::comparison_staging_bytes() +
        2U*sizeof(std::optional<runtime::detail::MonitoringPreparationFrequencyView>) +
        sizeof(std::unexpected<Error>) + sizeof(std::expected<void,Error>);
}
auto PairPrefix::preparation_event() const noexcept -> const PreparationAssociationEvent* {
    return state_ && state_->preparation ? &*state_->preparation : nullptr;
}
auto PairPrefix::preparation_association(std::uint64_t seq, const journal::Timestamp& time,
    const runtime::detail::PairwisePreparationGraph& graph) -> std::expected<void,Error> {
    const PreparationFactsLease lease{graph};
    return preparation_association_facts(seq,time,lease);
}
auto PairPrefix::preparation_association_facts(std::uint64_t seq, const journal::Timestamp& time,
    const PreparationFactsLease& graph) -> std::expected<void,Error> {
    using namespace runtime::detail;
    if (consumed()) return error(ErrorCode::state);
    auto& s=*state_;
    PreparationAssociationStaging stage;
    auto reject=[&](ErrorCode code) -> std::expected<void,Error> {
        s.consumed=true; stage.error.code=code; stage.error.phase=Phase::event;
        stage.result=std::unexpected{stage.error};
        return std::move(stage.result);
    };
    if (!s.preparation_mode || s.preparation || !s.control || !s.readiness || !s.memory[0] || !s.memory[1] || !s.monitoring ||
        s.count || !s.stopping.empty() || s.terminal || s.finalization) return reject(ErrorCode::state);
    if (!seq || s.sequence==UINT64_MAX || seq!=s.sequence+1U || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count==s.limits.events) return reject(ErrorCode::capacity);
    const auto facts=graph.facts();
    if (!facts || graph.decoded()!=(s.context->origin()==PairFactsLease::Origin::decoded_evidence) ||
        (!graph.decoded() && !graph.published()) ||
        graph.domain()!=s.monitoring->domain ||
        (!graph.decoded()&&(s.domain==Domain::synthetic_execution)!=(graph.domain()==MonitoringPreparationFactDomain::synthetic)) ||
        !facts->matches_identity(s.context->provenance()) || facts->workers.size()!=s.readiness->workers.size())
        return reject(ErrorCode::identity);
    const auto& expected=*s.context->complete_preparation();
    if (!facts->matches_inputs(expected.measurement,expected.statistics,s.monitoring->policy,
        s.context->kernel(false),s.context->kernel(true),expected.first_request,expected.second_request,expected.comparison_seed) ||
        !exact_policy(graph.policies()->statistics,*s.statistics) || graph.policies()->repetition!=*s.repetition) return reject(ErrorCode::identity);
    stage.control=&*s.control; stage.workers=&*s.readiness; stage.first=&*s.memory[0]; stage.second=&*s.memory[1]; stage.monitoring=&*s.monitoring;
    for (;stage.row<stage.workers->workers.size();++stage.row) {
        if (!facts->matches_worker(stage.row,stage.workers->workers[stage.row])) return reject(ErrorCode::association);
        for (stage.side=0;stage.side<2U;++stage.side) {
            const auto& row=(stage.side?stage.second:stage.first)->workers[stage.row];
            if (!facts->matches_memory(stage.side!=0U,stage.row,{row.request,row.binding,row.initialization,
                row.state==memory::PreRunDiagnosticState::skipped ? memory::PreRunPlacementQueryState::skipped : memory::PreRunPlacementQueryState::observed,
                row.observation,row.evaluation,row.qualification.value_or(memory::PlacementQualificationState::skipped),row.protection})) return reject(ErrorCode::association);
        }
    }
    const auto* identity=graph.monitoring();
    if (!identity || identity->channels!=stage.monitoring->channels || identity->service!=stage.monitoring->service ||
        identity->physical!=stage.monitoring->physical || identity->effective!=stage.monitoring->effective ||
        identity->provenance!=stage.monitoring->provenance || identity->capacity!=stage.monitoring->capacity || identity->profile!=stage.monitoring->profile ||
        graph.frequency_count()!=stage.monitoring->frequency.size()) return reject(ErrorCode::association);
    for (;stage.domain<graph.frequency_count();++stage.domain) {
        const auto row=graph.frequency(stage.domain);
        const auto& other=stage.monitoring->frequency[stage.domain];
        if (!row || !std::ranges::equal(row->cpus,other.cpus) || row->kind!=other.kind || row->driver!=other.driver ||
            row->governor!=other.governor || row->minimum_hz!=other.minimum_hz || row->maximum_hz!=other.maximum_hz) return reject(ErrorCode::association);
    }
    stage.retained.add(s.usage.retained); stage.retained.add(graph.retained_bytes()); stage.retained.add(time.text().size());
    stage.peak.add(stage.retained.value); stage.peak.add(preparation_staging_bytes());
    if (!stage.retained.valid || !stage.peak.valid || stage.retained.value>s.limits.retained_bytes || stage.peak.value>s.limits.simultaneous_bytes)
        return reject(ErrorCode::capacity);
    try {
        stage.lease.emplace(graph.retain());
        stage.event.emplace(seq,time,std::array{stage.control->sequence,stage.workers->sequence,stage.first->sequence,
            stage.second->sequence,stage.monitoring->sequence},std::move(*stage.lease));
        s.preparation.emplace(std::move(*stage.event)); s.usage.retained=stage.retained.value; s.sequence=seq; ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::prepare_with_control_readiness(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    runtime::StatisticsProductionPolicy statistics, measurement::RepetitionPolicy repetition)
    -> std::expected<PairPrefix, Error> {
    return prepare_impl(std::move(context), domain, limits, statistics, repetition, true, true, true, true, true);
}
auto PairPrefix::control_readiness_event() const noexcept -> const ControlReadinessEvent* {
    return state_ && state_->control ? &*state_->control : nullptr;
}
namespace {
struct MonitoringMembership final { core::LinuxCpuId cpu; std::size_t domain; };
struct MonitoringPreparationStaging final {
    std::vector<MonitoringMembership> memberships;
    std::optional<MonitoringPreparationEvent> event;
    Sum workers, domains, members, strings, timestamp, retained, scratch, peak, payload;
    std::size_t worker{}, domain{}, member{}, label{}, channel{}, lower{}, upper{};
    bool comparison{};
};
static_assert(std::is_nothrow_move_constructible_v<MonitoringPreparationEvent>);
auto valid_monitoring_token(std::string_view text) noexcept -> bool {
    return !text.empty() && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20U || c == 0x7fU; });
}
auto matches_monitoring(const MonitoringPreparationEvent& event,
                       const runtime::FactualObservationEvidence& factual) noexcept -> bool {
    using namespace monitoring;
    if (!factual.monitoring) return false;
    const auto& capture = *factual.monitoring;
    const auto& e = capture.evidence();
    const auto& raw = capture.raw();
    const auto& a = capture.activity();
    const std::array states{e.page_faults_state(), e.context_switches_state(), e.temperature_state(), e.frequency_state()};
    const std::array populated{bool(e.page_faults()), bool(e.context_switches()), bool(e.temperature()), bool(e.frequency())};
    const std::array acquisitions{!raw.page_fault_before().empty() || !raw.page_fault_after().empty(),
        !raw.context_switches().empty() || a.context_drains() != 0U,
        !raw.temperature().empty() || a.temperature_attempts() != 0U,
        !raw.frequency().empty() || !raw.frequency_metadata().empty() || a.frequency_attempts() != 0U};
    for (std::size_t i{}; i < states.size(); ++i) {
        if (event.channels[i].effective == MonitoringEffectiveChannelDisposition::active) {
            if (!populated[i]) return false;
            continue;
        }
        const auto expected = event.channels[i].effective == MonitoringEffectiveChannelDisposition::unsupported_warning
            ? MonitoringSignalState::unavailable : MonitoringSignalState::not_applicable;
        if (states[i] != expected || populated[i] || acquisitions[i]) return false;
    }
    if (event.channels[3].effective != MonitoringEffectiveChannelDisposition::active) return true;
    if (raw.frequency_metadata().size() != event.frequency.size() || raw.frequency().size() != event.frequency.size()) return false;
    for (std::size_t i{}; i < event.frequency.size(); ++i) {
        const auto& prepared = event.frequency[i];
        const auto& metadata = raw.frequency_metadata()[i];
        if (!std::ranges::equal(metadata.domain_id().cpus(), prepared.cpus) ||
            metadata.source_kind() != prepared.kind || metadata.scaling_driver().value() != prepared.driver ||
            metadata.scaling_governor().value() != prepared.governor || metadata.scaling_minimum().value() != prepared.minimum_hz ||
            metadata.scaling_maximum().value() != prepared.maximum_hz ||
            !std::ranges::equal(raw.frequency()[i].domain_id.cpus(), prepared.cpus)) return false;
        for (const auto& acquisition : raw.frequency()[i].acquisitions)
            if (acquisition.payload() && (!std::ranges::equal(acquisition.payload()->domain_id().cpus(), prepared.cpus) ||
                acquisition.payload()->source_kind() != prepared.kind)) return false;
    }
    if (e.frequency()) {
        if (e.frequency()->domains().size() != event.frequency.size()) return false;
        for (std::size_t i{}; i < event.frequency.size(); ++i)
            if (!std::ranges::equal(e.frequency()->domains()[i].domain_id().cpus(), event.frequency[i].cpus) ||
                e.frequency()->domains()[i].source_kind() != event.frequency[i].kind) return false;
    }
    return true;
}
}
auto PairPrefix::monitoring_preparation_staging_bytes() noexcept -> std::size_t {
    using namespace runtime::detail;
    return sizeof(MonitoringPreparationStaging) + sizeof(MonitoringFactsView) + 2U*sizeof(std::optional<MonitoringPreparationFactView>) +
        2U*sizeof(std::optional<MonitoringPreparationFrequencyView>) + 2U*sizeof(MonitoringPreparationFrequencyView) + 2U*sizeof(std::expected<void,Error>) +
        2U*sizeof(std::unexpected<Error>) + 2U*sizeof(Error);
}
auto PairPrefix::monitoring_preparation_event() const noexcept -> const MonitoringPreparationEvent* {
    return state_ && state_->monitoring ? &*state_->monitoring : nullptr;
}
auto PairPrefix::monitoring_preparation(std::uint64_t seq, const journal::Timestamp& time,
    const runtime::detail::MonitoringPreparationFactView& view) -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    if (state_->context->origin()!=PairFactsLease::Origin::actual_context) {
        state_->consumed=true; return error(ErrorCode::identity);
    }
    const auto& admission=view.admission();
    const auto& authority=admission.authority();
    const MonitoringFactsView facts{view.domain(),admission.monitoring_policy(),view.channels(),view.service(),
        authority.physical_fingerprint(),authority.effective_fingerprint(),authority.provenance(),
        admission.capacity_contract_identity(),admission.finalized_error_profile_identity(),view.workers(),
        view.frequency_domains(),&view,[](const void* backing,std::size_t index) noexcept {
            return static_cast<const runtime::detail::MonitoringPreparationFactView*>(backing)->frequency(index);
        }};
    return monitoring_preparation_facts(seq,time,facts);
}
auto PairPrefix::monitoring_preparation_facts(std::uint64_t seq, const journal::Timestamp& time,
    const MonitoringFactsView& view) -> std::expected<void, Error> {
    using namespace runtime::detail;
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) { s.consumed = true; return error(code); };
    if (!s.monitoring_mode || s.monitoring || !s.control || !s.readiness || !s.memory[0] || !s.memory[1] ||
        s.count || !s.stopping.empty() || s.terminal || s.finalization) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events) return reject(ErrorCode::capacity);
    if ((s.context->origin()!=PairFactsLease::Origin::decoded_evidence || s.domain==Domain::synthetic_execution) &&
        (s.domain == Domain::synthetic_execution) != (view.domain() == MonitoringPreparationFactDomain::synthetic))
        return reject(ErrorCode::identity);
    const auto& expected = *s.context->monitoring_preparation();
    if (view.policy != expected.policy || view.physical != expected.physical ||
        view.effective != expected.effective ||
        view.provenance.kind() != platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::scenario ||
        view.provenance.authority_digest() != expected.scenario ||
        view.workers().size() != s.readiness->workers.size()) return reject(ErrorCode::identity);
    if (view.service() && (!expected.assignment || view.service()->assigned != expected.assignment->linux_cpu ||
        view.service()->verified_singleton != expected.assignment->linux_cpu || view.service()->observed != expected.assignment->linux_cpu))
        return reject(ErrorCode::identity);
    MonitoringPreparationStaging stage;
    stage.workers.add(view.workers().size()); stage.domains.add(view.frequency_domains());
    if (stage.workers.value > s.limits.monitoring_preparation.workers || stage.domains.value > s.limits.monitoring_preparation.domains)
        return reject(ErrorCode::capacity);
    for (; stage.worker < view.workers().size(); ++stage.worker) {
        const auto& worker = view.workers()[stage.worker];
        const auto& ready = s.readiness->workers[stage.worker];
        if (worker.worker_id() != ready.worker_id || worker.linux_cpu() != ready.requested_cpu ||
            worker.operational_thread_id() != ready.tid) return reject(ErrorCode::identity);
    }
    for (; stage.domain < view.frequency_domains(); ++stage.domain) {
        const auto row = view.frequency(stage.domain);
        if (!row || row->cpus.empty() || !std::ranges::is_sorted(row->cpus) ||
            std::adjacent_find(row->cpus.begin(), row->cpus.end()) != row->cpus.end() ||
            (row->kind != monitoring::FrequencySourceKind::hardware_current && row->kind != monitoring::FrequencySourceKind::scaling_current) ||
            !valid_monitoring_token(row->driver) || !valid_monitoring_token(row->governor) ||
            !row->minimum_hz || row->minimum_hz > row->maximum_hz) return reject(ErrorCode::malformed);
        if (stage.domain) {
            const auto previous = view.frequency(stage.domain-1U);
            if (!previous || !std::lexicographical_compare(previous->cpus.begin(), previous->cpus.end(),
                    row->cpus.begin(), row->cpus.end())) return reject(ErrorCode::identity);
        }
        stage.members.add(row->cpus.size()); stage.strings.add(row->driver.size()); stage.strings.add(row->governor.size());
    }
    stage.payload.add(stage.workers.value, sizeof(MonitoringPreparationWorker));
    stage.payload.add(stage.domains.value, sizeof(MonitoringPreparationFrequency));
    stage.payload.add(stage.members.value, sizeof(core::LinuxCpuId));
    stage.payload.add(stage.strings.value); stage.payload.add(time.text().size());
    stage.scratch.add(stage.members.value, sizeof(MonitoringMembership));
    stage.retained.add(s.usage.retained); stage.retained.add(stage.payload.value);
    stage.peak.add(stage.retained.value); stage.peak.add(stage.scratch.value); stage.peak.add(monitoring_preparation_staging_bytes());
    if (!stage.members.valid || !stage.strings.valid || !stage.payload.valid || !stage.scratch.valid || !stage.retained.valid || !stage.peak.valid ||
        stage.members.value > s.limits.monitoring_preparation.memberships || stage.strings.value > s.limits.monitoring_preparation.strings ||
        stage.retained.value > s.limits.retained_bytes || stage.peak.value > s.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        stage.memberships.reserve(stage.members.value);
        for (stage.domain=0; stage.domain<view.frequency_domains(); ++stage.domain)
            for (const auto cpu : view.frequency(stage.domain)->cpus) stage.memberships.push_back({cpu,stage.domain});
        std::ranges::sort(stage.memberships, {}, &MonitoringMembership::cpu);
        for (stage.member=1; stage.member<stage.memberships.size(); ++stage.member)
            if (stage.memberships[stage.member-1U].cpu == stage.memberships[stage.member].cpu) return reject(ErrorCode::identity);
        if (view.channels()[3].support == MonitoringPreparationSupport::supported)
            for (const auto& worker : view.workers())
                if (!std::ranges::binary_search(stage.memberships, worker.linux_cpu(), {}, &MonitoringMembership::cpu)) return reject(ErrorCode::identity);
        stage.event.emplace(seq,time,view.domain(),expected.policy,view.channels(),view.service(),
            view.physical,view.effective,view.provenance,
            view.capacity,view.profile);
        stage.event->workers.reserve(stage.workers.value); stage.event->frequency.reserve(stage.domains.value);
        for (const auto& worker : view.workers()) stage.event->workers.push_back({worker.worker_id(),worker.linux_cpu()});
        for (stage.domain=0; stage.domain<view.frequency_domains(); ++stage.domain) {
            const auto row=view.frequency(stage.domain);
            auto& owned=stage.event->frequency.emplace_back();
            owned.kind=row->kind; owned.minimum_hz=row->minimum_hz; owned.maximum_hz=row->maximum_hz;
            owned.cpus.assign(row->cpus.begin(),row->cpus.end()); owned.driver.assign(row->driver); owned.governor.assign(row->governor);
        }
        s.monitoring.emplace(std::move(*stage.event)); s.usage.retained=stage.retained.value;
        s.sequence=seq; ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::control_readiness_staging_bytes() noexcept -> std::size_t {
    return sizeof(ControlReadinessStaging) + sizeof(Error) + sizeof(std::unexpected<Error>) +
        sizeof(std::expected<void, Error>);
}
auto PairPrefix::control_readiness(std::uint64_t seq, const journal::Timestamp& time,
    const runtime::ServiceThreadReadyEvidence& row) -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) { s.consumed = true; return error(code); };
    if (!s.control_mode || s.control || s.readiness || s.memory[0] || s.memory[1] || s.count ||
        !s.stopping.empty() || s.finalization || s.terminal) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events) return reject(ErrorCode::capacity);
    const auto& expected = *s.context->control_assignment();
    if (row.role != runtime::ServiceRole::control_monitor || row.role != expected.role ||
        row.stable_pu != expected.stable_pu || row.stable_core != expected.stable_core ||
        row.assigned_cpu != expected.linux_cpu || row.observed_cpu != row.assigned_cpu ||
        row.observed_affinity.cardinality() != 1U || *row.observed_affinity.begin() != row.assigned_cpu)
        return reject(ErrorCode::identity);
    ControlReadinessStaging stage;
    for (; stage.side < 2U; ++stage.side) {
        const auto workers = s.context->assignments(stage.side != 0U);
        for (stage.index = 0; stage.index < workers.size(); ++stage.index)
            if (workers[stage.index].stable_core_id() == row.stable_core ||
                workers[stage.index].linux_cpu() == row.assigned_cpu) return reject(ErrorCode::identity);
    }
    stage.strings.add(row.stable_pu.value().size()); stage.strings.add(row.stable_core.value().size());
    stage.payload.add(stage.strings.value); stage.payload.add(time.text().size());
    stage.payload.add(2U, sizeof(std::uint32_t));
    stage.retained.add(s.usage.retained); stage.retained.add(stage.payload.value);
    stage.peak.add(stage.retained.value); stage.peak.add(sizeof(ControlReadinessEvent));
    stage.peak.add(control_readiness_staging_bytes());
    if (!stage.strings.valid || !stage.payload.valid || !stage.retained.valid || !stage.peak.valid ||
        stage.strings.value > s.limits.control_strings || stage.retained.value > s.limits.retained_bytes ||
        stage.peak.value > s.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        ControlReadinessEvent candidate{seq, time, row};
        s.control.emplace(std::move(candidate)); s.usage.retained = stage.retained.value;
        s.sequence = seq; ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::memory_preparation_event(bool second) const noexcept -> const MemoryPreparationEvent* {
    if (!state_) return nullptr;
    const auto& value = state_->memory[second ? 1U : 0U];
    return value ? &*value : nullptr;
}
auto PairPrefix::memory_preparation_staging_bytes() noexcept -> std::size_t {
    using Evaluation = memory::detail::PlacementEvaluationResult;
    using Evaluated = std::expected<Evaluation, core::Error>;
    return sizeof(MemoryPreparationStaging) + sizeof(MemoryPreparationEvent) + 2U * sizeof(Evaluated) +
        sizeof(std::expected<memory::ExactRatio, core::Error>) + 2U * sizeof(Evaluation) +
        sizeof(core::detail::ExpectedErrorCleanup<Evaluated>) + sizeof(Error) + sizeof(std::unexpected<Error>) +
        sizeof(std::expected<void, Error>) + sizeof(PlacementEvaluatorScalars);
}
auto PairPrefix::memory_preparation(std::uint64_t seq, const journal::Timestamp& time, bool second,
    std::span<const MemoryPreparationRow> rows) -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) { s.consumed = true; return error(code); };
    const auto index = second ? 1U : 0U;
    if (!s.memory_mode || !s.readiness || s.memory[index] || (second && !s.memory[0]) ||
        s.count || !s.stopping.empty() || s.finalization || s.terminal) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events || rows.size() > std::vector<MemoryPreparationRow>{}.max_size())
        return reject(ErrorCode::capacity);
    const auto expected = s.context->memory_requests(second);
    if (rows.size() != expected.size()) return reject(ErrorCode::identity);
    MemoryPreparationStaging stage;
    stage.workers.add(rows.size());
    for (const auto& row : rows) {
        if (row.request.numa_targets().size() > std::vector<memory::RegionNumaTarget>{}.max_size())
            return reject(ErrorCode::capacity);
        stage.targets.add(row.request.numa_targets().size());
        for (const auto& target : row.request.numa_targets()) stage.strings.add(target.stable_node.value().size());
        if (row.observation) {
            if (row.observation->resident_page_counts().size() > std::vector<memory::LinuxNodePageCount>{}.max_size())
                return reject(ErrorCode::capacity);
            stage.pages.add(row.observation->resident_page_counts().size());
        }
    }
    stage.payload.add(rows.size(), sizeof(MemoryPreparationRow));
    stage.payload.add(stage.targets.value, sizeof(memory::RegionNumaTarget));
    stage.payload.add(stage.pages.value, sizeof(memory::LinuxNodePageCount));
    stage.payload.add(stage.strings.value);
    stage.current = {stage.workers.value, stage.targets.value, stage.pages.value,
        stage.strings.value, stage.payload.value};
    stage.retained.add(s.usage.retained); stage.retained.add(stage.payload.value); stage.retained.add(time.text().size());
    stage.peak.add(stage.retained.value); stage.peak.add(memory_preparation_staging_bytes());
    if (s.memory[0]) {
        const auto& prior = s.memory[0]->usage;
        stage.workers.add(prior.workers); stage.targets.add(prior.targets); stage.pages.add(prior.pages);
        stage.strings.add(prior.strings); stage.payload.add(prior.payload);
    }
    const auto& cap = s.limits.memory_preparation;
    if (!stage.workers.valid || !stage.targets.valid || !stage.pages.valid || !stage.strings.valid ||
        !stage.payload.valid || !stage.retained.valid || !stage.peak.valid ||
        stage.workers.value > cap.workers || stage.targets.value > cap.targets || stage.pages.value > cap.pages ||
        stage.strings.value > cap.strings || stage.payload.value > cap.typed_payload ||
        stage.retained.value > s.limits.retained_bytes || stage.peak.value > s.limits.simultaneous_bytes)
        return reject(ErrorCode::capacity);
    for (; stage.row < rows.size(); ++stage.row)
        if (!valid_preparation(rows[stage.row], expected[stage.row], s.context->workers()[stage.row]))
            return reject(ErrorCode::association);
    try {
        MemoryPreparationEvent candidate{seq, time, {rows.begin(), rows.end()}, stage.current};
        s.memory[index].emplace(std::move(candidate));
        s.usage.retained = stage.retained.value; s.sequence = seq; ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}

auto PairPrefix::readiness_event() const noexcept -> const ReadinessEvent* {
    return state_ && state_->readiness ? &*state_->readiness : nullptr;
}
auto PairPrefix::readiness_staging_bytes() noexcept -> std::size_t {
    // Include fixed failure construction/transfer even when return objects are not elided.
    return sizeof(ReadinessStaging) + sizeof(Error) + sizeof(std::unexpected<Error>) +
        sizeof(std::expected<void, Error>);
}
auto PairPrefix::worker_readiness(std::uint64_t seq, const journal::Timestamp& time,
    std::span<const runtime::WorkerReadyEvidence> workers) -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) { s.consumed = true; return error(code); };
    if ((s.control_mode && !s.control) || !s.readiness_mode || s.readiness || s.count || !s.stopping.empty() || s.finalization || s.terminal)
        return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events || workers.size() > s.limits.readiness.workers ||
        workers.size() > std::vector<runtime::WorkerReadyEvidence>{}.max_size() ||
        workers.size() > std::vector<std::uint64_t>{}.max_size()) return reject(ErrorCode::capacity);
    const auto assignments = s.context->assignments(false);
    if (workers.size() != assignments.size()) return reject(ErrorCode::identity);
    ReadinessStaging stage;
    for (; stage.index < workers.size(); ++stage.index) {
        const auto& row = workers[stage.index];
        const auto& expected = assignments[stage.index];
        if ((s.control && row.tid == s.control->evidence.tid) || row.worker_index != stage.index || row.worker_id != s.context->workers()[stage.index] ||
            row.stable_pu != expected.stable_pu_id() || row.stable_core != expected.stable_core_id() ||
            row.requested_cpu != expected.linux_cpu() || row.observed_cpu != row.requested_cpu ||
            row.observed_affinity.cardinality() != 1U || *row.observed_affinity.begin() != row.requested_cpu)
            return reject(ErrorCode::identity);
        stage.strings.add(row.stable_pu.value().size());
        stage.strings.add(row.stable_core.value().size());
    }
    stage.payload.add(workers.size(), sizeof(runtime::WorkerReadyEvidence) + 2U * sizeof(std::uint32_t));
    stage.payload.add(stage.strings.value); stage.payload.add(time.text().size());
    stage.retained.add(s.usage.retained); stage.retained.add(stage.payload.value);
    stage.peak.add(stage.retained.value); stage.peak.add(readiness_staging_bytes());
    stage.peak.add(sizeof(ReadinessEvent)); stage.peak.add(workers.size(), sizeof(std::uint64_t));
    if (!stage.strings.valid || !stage.payload.valid || !stage.retained.valid || !stage.peak.valid ||
        stage.strings.value > s.limits.readiness.strings || stage.retained.value > s.limits.retained_bytes ||
        stage.peak.value > s.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        stage.tids.reserve(workers.size());
        for (const auto& row : workers) stage.tids.push_back(row.tid.value());
        std::ranges::sort(stage.tids);
        if (std::adjacent_find(stage.tids.begin(), stage.tids.end()) != stage.tids.end())
            return reject(ErrorCode::identity);
        ReadinessEvent candidate{seq, time, {workers.begin(), workers.end()}};
        s.readiness.emplace(std::move(candidate));
        s.usage.retained = stage.retained.value;
        s.sequence = seq; ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::finalization_event() const noexcept -> const FinalizationEvent* {
    return state_ && state_->finalization ? &*state_->finalization : nullptr;
}
auto PairPrefix::terminal_event() const noexcept -> const TerminalEvent* {
    return state_ && state_->terminal ? &*state_->terminal : nullptr;
}
auto PairPrefix::prepare_impl(ExpectedPairContext&& context, Domain domain, PairLimits limits,
    std::optional<runtime::StatisticsProductionPolicy> statistics,
    std::optional<measurement::RepetitionPolicy> repetition, bool terminal_mode, bool finalization_mode,
    bool readiness_mode, bool memory_mode, bool control_mode, bool monitoring_mode,
    bool preparation_mode, bool outcomes_mode) -> std::expected<PairPrefix, Error> {
    auto result=prepare_impl(PairFactsLease{context},domain,limits,statistics,repetition,
        terminal_mode,finalization_mode,readiness_mode,memory_mode,control_mode,monitoring_mode,
        preparation_mode,outcomes_mode);
    if(result)context.state_.reset();
    return result;
}
auto PairPrefix::prepare_impl(PairFactsLease&& context, Domain domain, PairLimits limits,
    std::optional<runtime::StatisticsProductionPolicy> statistics,
    std::optional<measurement::RepetitionPolicy> repetition, bool terminal_mode, bool finalization_mode, bool readiness_mode, bool memory_mode, bool control_mode, bool monitoring_mode, bool preparation_mode, bool outcomes_mode)
    -> std::expected<PairPrefix, Error> {
    if (!context.available() ||
        (context.origin()==PairFactsLease::Origin::decoded_evidence &&
            context.decoded_domain()!=std::optional{domain==Domain::supplied_observation?process_subject::Domain::supplied_observation:process_subject::Domain::synthetic_execution}) ||
        (domain != Domain::supplied_observation && domain != Domain::synthetic_execution))
        return error(ErrorCode::identity, Phase::context);
    if (preparation_mode && !context.complete_preparation()) return error(ErrorCode::identity, Phase::context);
    if (monitoring_mode && !context.monitoring_preparation()) return error(ErrorCode::identity, Phase::context);
    if (control_mode && !context.control_assignment()) return error(ErrorCode::identity, Phase::context);
    if (limits.root_depth > 64U || limits.component.depth > 64U || limits.component.events > 6U ||
        limits.blocks > std::vector<Slot>{}.max_size() ||
        (statistics && limits.blocks > std::vector<StoppingEvent>{}.max_size()) ||
        limits.observations > std::vector<std::uint64_t>{}.max_size() ||
        limits.terminal_errors > std::vector<runtime::FinalizedEvidenceError>{}.max_size())
        return error(ErrorCode::capacity, Phase::context);
    if (readiness_mode && !std::ranges::equal(context.assignments(false), context.assignments(true)))
        return error(ErrorCode::identity, Phase::context);
    Sum retained{sizeof(PairPrefix) + sizeof(State)};
    retained.add(limits.blocks, sizeof(Slot));
    if (statistics) retained.add(limits.blocks, sizeof(StoppingEvent));
    retained.add(limits.observations, sizeof(std::uint64_t));
    retained.add(context.usage().retained_bytes - sizeof(PairFactsLease));
    if (!retained.valid || retained.value > limits.retained_bytes ||
        retained.value > limits.simultaneous_bytes)
        return error(ErrorCode::capacity, Phase::context);
    try {
        auto state = std::make_unique<State>();
        state->blocks.resize(limits.blocks);
        state->ids.reserve(limits.observations);
        if (statistics) {
            state->stopping.reserve(limits.blocks);
            state->statistics.emplace(*statistics);
            state->repetition.emplace(*repetition);
        }
        state->terminal_mode = terminal_mode;
        state->finalization_mode = finalization_mode;
        state->readiness_mode = readiness_mode;
        state->memory_mode = memory_mode;
        state->control_mode = control_mode;
        state->monitoring_mode = monitoring_mode;
        state->preparation_mode = preparation_mode;
        state->outcomes_mode = outcomes_mode;
        state->limits = limits;
        state->domain = domain;
        state->usage.retained = retained.value;
        state->context.emplace(std::move(context));
        return PairPrefix{std::move(state)};
    } catch (const std::bad_alloc&) {
        return error(ErrorCode::allocation_failure, Phase::context);
    } catch (const std::length_error&) {
        return error(ErrorCode::capacity, Phase::context);
    }
}
auto PairPrefix::open(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    return append(seq, time, bytes, wire::EvidenceComponentKind::block_plan);
}
auto PairPrefix::factual(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    return append(seq, time, bytes, wire::EvidenceComponentKind::factual_observation);
}
auto PairPrefix::failed(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    return append(seq, time, bytes, wire::EvidenceComponentKind::failed_observation);
}
auto PairPrefix::factual(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes,
    observation_timing::Input& timing) -> std::expected<void, Error> {
    return append(seq, time, bytes, wire::EvidenceComponentKind::factual_observation, &timing);
}
auto PairPrefix::failed(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes,
    observation_timing::Input& timing) -> std::expected<void, Error> {
    return append(seq, time, bytes, wire::EvidenceComponentKind::failed_observation, &timing);
}
namespace {
struct TimingForwardingCells final {
    std::uint64_t sequence;
    const journal::Timestamp* time;
    std::string_view bytes;
    observation_timing::Input* input;
};
struct PairTimingCells final {
    std::uint64_t sequence;
    const journal::Timestamp* time;
    std::string_view bytes;
    wire::EvidenceComponentKind kind;
    observation_timing::Input* input;
    PairPrefix::State *state, *reject_capture;
    bool opening;
    std::expected<observation_timing::Usage, Error> measured;
    std::optional<observation_timing::Usage> cumulative;
    std::size_t reserve, fixed, added_base, before, other, original_peak;
    std::optional<Accumulator> candidate;
    Accumulator* child;
    std::optional<std::size_t> base;
    std::size_t shared;
    const PairFactsLease* base_context;
    Sum base_sum;
    ContextUsage base_usage, shared_usage;
    std::optional<std::size_t> base_return;
    Sum total, publication;
    std::size_t phase_maximum_values[4];
    std::initializer_list<std::size_t> phase_maximum_view;
    PairFactsLease lease;
    PairFactsLease lease_parameter;
    ExpectedPairContext actual_lease_constructor;
    std::shared_ptr<const ExpectedPairContext::State> actual_shared_parameter;
    Limits limits;
    std::expected<Accumulator, Error> made;
    std::expected<Component, Error> component;
    const measurement::MeasurementBlockPlan* plan;
    const runtime::detail::PreparationGraphPolicies* policy;
    std::optional<std::uint64_t> id;
    ComponentUsage component_usage;
    std::expected<void, Error> appended, returned;
    ErrorCode reject_code;
    ErrorCode helper_code;
    Phase helper_phase;
    Error rejection;
    std::unexpected<Error> returned_rejection;
};
struct TimingArithmeticCells final {
    Sum* self;
    std::size_t n, size, multiply_count, multiply_size, add_current, add_additional;
    std::expected<std::size_t, d::WireJsonError> product, sum, multiply_return, add_return;
    d::WireJsonError multiply_error, add_error;
    std::unexpected<d::WireJsonError> multiply_unexpected, add_unexpected;
};
struct LegacyPairTimingCells final {
    observation_timing::Input* input;
    std::expected<observation_timing::Usage, Error> measured;
    std::optional<observation_timing::Usage> cumulative;
    std::size_t reserve, original_peak;
};
struct TimingCalibrationCells final {
    const CalibrationEvent *calibration, *selected_work_parameter;
    const runtime::FactualObservationEvidence* factual;
    const void *selected_outcome, *estimate_outcome, *observation, *worker;
    std::optional<std::uint64_t> selected, selected_return, visit_return;
    std::optional<kernel::SequentialReadBandwidthPassCount> bandwidth_condition, bandwidth_value;
    std::optional<kernel::DependentLoadLatencyStepCount> latency_condition, latency_value;
    measurement::TimerOverheadEstimate calibration_estimate, validity_estimate;
    core::Duration overhead, visit_duration, calibration_duration, validity_duration;
    const std::optional<std::uint64_t> *observation_capture, *worker_capture;
    kernel::SequentialReadBandwidthPassCount observed_passes;
    kernel::DependentLoadLatencyStepCount observed_steps;
    std::span<const measurement::DependentLoadLatencyWorkerObservation> workers;
};
struct TimingMonitoringCells final {
    const MonitoringPreparationEvent* event;
    const runtime::FactualObservationEvidence* factual;
    const void *capture, *evidence, *raw, *activity, *prepared, *metadata, *acquisition;
    std::array<monitoring::MonitoringSignalState, 4> states;
    std::array<bool, 4> populated, acquisitions;
    std::size_t channel_index, domain_index, final_index;
    monitoring::MonitoringSignalState expected;
    decltype(std::declval<const monitoring::MonitoringCaptureResult&>().raw().frequency()) frequency_return;
    decltype(std::declval<const monitoring::MonitoringCaptureResult&>().raw().frequency_metadata()) metadata_return;
};
}
auto PairPrefix::timing_staging_bytes() noexcept -> std::size_t {
    return sizeof(TimingForwardingCells) + sizeof(PairTimingCells) + sizeof(TimingArithmeticCells) + sizeof(TimingCalibrationCells) + sizeof(TimingMonitoringCells) +
        Accumulator::timing_staging_bytes() + observation_timing::Owner::staging_bytes();
}
auto PairPrefix::append(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes,
                        wire::EvidenceComponentKind kind, observation_timing::Input* timing) -> std::expected<void, Error> {
    if (consumed())
        return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) {
        s.consumed = true;
        return error(code);
    };
    if (s.terminal || s.finalization) return reject(ErrorCode::state);
    if ((kind != wire::EvidenceComponentKind::block_plan && s.timing_mode != bool(timing)) ||
        (timing && kind == wire::EvidenceComponentKind::block_plan)) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty())
        return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events)
        return reject(ErrorCode::capacity);
    const bool opening = kind == wire::EvidenceComponentKind::block_plan;
    if (opening && s.outcomes_mode && (!s.calibration || !selected_work(*s.calibration) ||
        (s.preparation->graph.policies()->warmup.maximum_trials() && !s.warmup))) return reject(ErrorCode::association);
    if (opening && s.preparation_mode && !s.preparation) return reject(ErrorCode::state);
    if (opening && s.monitoring_mode && !s.monitoring) return reject(ErrorCode::state);
    if (opening && s.readiness_mode && !s.readiness) return reject(ErrorCode::state);
    if (opening && s.memory_mode && (!s.memory[0] || !s.memory[1])) return reject(ErrorCode::state);
    if (opening ? s.active : !s.active)
        return reject(ErrorCode::state);
    if (opening && !s.stopping.empty() && s.stopping.back().evidence.subsequent_disposition !=
            runtime::RepetitionBoundaryDisposition::continued)
        return reject(ErrorCode::state);
    if (opening && s.count == s.limits.blocks)
        return reject(ErrorCode::capacity);
    if (!opening && s.ids.size() == s.limits.observations)
        return reject(ErrorCode::capacity);
    std::expected<observation_timing::Usage, Error> measured;
    std::optional<observation_timing::Usage> cumulative;
    std::size_t reserve{sizeof(LegacyPairTimingCells) + Accumulator::legacy_timing_staging_bytes()};
    if (timing) {
        measured = observation_timing::detail::measure(*timing, *s.context, s.domain);
        if (!measured) { s.consumed = true; return std::unexpected{measured.error()}; }
        cumulative = observation_timing::detail::accumulate(s.timing_usage, *measured);
        if (!cumulative || !observation_timing::detail::fits(*cumulative, s.timing_limits)) return reject(ErrorCode::capacity);
        reserve = measured->error_retained_bytes;
        if (!observation_timing::detail::add(reserve, timing_staging_bytes())) return reject(ErrorCode::capacity);
    }
    try {
        std::optional<Accumulator> candidate;
        Accumulator* child{};
        std::size_t added_base{};
        if (opening) {
            const auto base = Accumulator::base_stock(*s.context);
            const auto shared = s.context->usage().retained_bytes - sizeof(PairFactsLease);
            if (!base || *base < shared + sizeof(Accumulator))
                return reject(ErrorCode::capacity);
            added_base = *base - shared - sizeof(Accumulator);
            Sum total{s.usage.retained};
            total.add(added_base);
            if (!total.valid || total.value > s.limits.retained_bytes ||
                total.value > s.limits.simultaneous_bytes)
                return reject(ErrorCode::capacity);
            auto lease=s.context->retain();
            auto limits = s.limits.component;
            const auto other = total.value - *base;
            limits.typed_bytes = std::min(limits.typed_bytes, s.limits.retained_bytes - other);
            limits.simultaneous_bytes =
                std::min(limits.simultaneous_bytes, s.limits.simultaneous_bytes - other);
            auto made = Accumulator::prepare(std::move(lease), s.domain, limits);
            if (!made) {
                s.consumed = true;
                return std::unexpected{made.error()};
            }
            candidate.emplace(std::move(*made));
            child = &*candidate;
        } else
            child = &*s.blocks[s.count - 1U];
        const auto before = child->retained_bytes();
        const auto other = s.usage.retained + added_base - before;
        child->residual_limits(
            std::min(s.limits.component.typed_bytes, s.limits.retained_bytes - other),
                std::min(s.limits.component.simultaneous_bytes, s.limits.simultaneous_bytes - other));
        const auto original_peak = std::min(s.limits.component.simultaneous_bytes, s.limits.simultaneous_bytes - other);
        if (reserve > original_peak || (timing && (other > s.timing_limits.simultaneous_bytes ||
            reserve > s.timing_limits.simultaneous_bytes - other))) return reject(ErrorCode::capacity);
        child->residual_limits(std::min(s.limits.component.typed_bytes, s.limits.retained_bytes - other),
            timing ? std::min(original_peak - reserve, s.timing_limits.simultaneous_bytes - other - reserve)
                   : original_peak - reserve);
        auto component = opening ? child->admit_plan(bytes)
                         : kind == wire::EvidenceComponentKind::factual_observation
                             ? child->admit_factual(bytes)
                             : child->admit_failed(bytes);
        if (!component) {
            s.consumed = true;
            return std::unexpected{component.error()};
        }
        if (s.outcomes_mode) {
            if (opening) {
                const auto& plan=*component->plan();
                const auto& policy=*s.preparation->graph.policies();
                if (plan.comparison_order()!=policy.comparison ||
                    plan.preparation().calibration_required()!=(s.count==0U) ||
                    plan.preparation().warmup_required()!=(s.count==0U && policy.warmup.maximum_trials()!=0U))
                    return reject(ErrorCode::association);
            } else if (component->factual() && !matches_selected_fact(*s.calibration,*component->factual()))
                return reject(ErrorCode::association);
        }
        if (s.monitoring_mode && component->factual() &&
            (!s.monitoring || !matches_monitoring(*s.monitoring, *component->factual()))) return reject(ErrorCode::association);
        std::optional<std::uint64_t> id;
        if (opening && s.count &&
            component->plan()->block_id().value() <=
                s.blocks[s.count - 1U]->events().front().component.plan()->block_id().value())
            return reject(ErrorCode::association);
        if (!opening) {
            id = component->factual() ? component->factual()->observation_id.value()
                                      : component->failed()->observation_id.value();
            if (std::binary_search(s.ids.begin(), s.ids.end(), *id))
                return reject(ErrorCode::association);
        }
        if (timing) {
            if (!s.calibration || !observation_timing::detail::matches(*timing, component->factual(), component->failed()))
                return reject(ErrorCode::association);
            Sum publication{s.usage.retained};
            publication.add(component->usage().retained_bytes);
            publication.add(measured->retained_bytes);
            publication.add(time.text().size());
            if (!publication.valid || publication.value > s.limits.retained_bytes) return reject(ErrorCode::capacity);
            publication.add(timing_staging_bytes());
            if (!publication.valid || publication.value > s.limits.simultaneous_bytes ||
                publication.value > s.timing_limits.simultaneous_bytes ||
                publication.value < other || publication.value - other > original_peak) return reject(ErrorCode::capacity);
            measured->simultaneous_bytes = std::max({publication.value,
                other + component->usage().phase1 + reserve,
                other + component->usage().phase2a + reserve,
                other + component->usage().phase2b + reserve});
            cumulative->simultaneous_bytes = std::max(cumulative->simultaneous_bytes, measured->simultaneous_bytes);
            child->residual_limits(std::min(s.limits.component.typed_bytes, s.limits.retained_bytes - other), original_peak);
        }
        if (!timing) child->residual_limits(std::min(s.limits.component.typed_bytes, s.limits.retained_bytes - other),
            original_peak - sizeof(LegacyPairTimingCells));
        auto appended = timing ? child->append_impl(seq, time, std::move(*component), timing, &*measured, s.calibration->sequence)
                               : child->append(seq, time, std::move(*component));
        if (!appended) {
            s.consumed = true;
            return std::unexpected{appended.error()};
        }
        s.usage.retained += added_base + child->retained_bytes() - before;
        if (id)
            s.ids.insert(std::lower_bound(s.ids.begin(), s.ids.end(), *id), *id);
        if (opening) {
            s.blocks[s.count++].emplace(std::move(*candidate));
            s.active = true;
        }
        s.sequence = seq;
        ++s.event_count;
        if (timing) s.timing_usage = *cumulative;
        return {};
    } catch (const std::bad_alloc&) {
        return reject(ErrorCode::allocation_failure);
    } catch (const std::length_error&) {
        return reject(ErrorCode::capacity);
    }
}
auto PairPrefix::close(std::uint64_t seq, const journal::Timestamp& time,
                       runtime::BlockAttemptCompletion completion) -> std::expected<void, Error> {
    if (consumed())
        return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) {
        s.consumed = true;
        return error(code);
    };
    if (!s.active)
        return reject(ErrorCode::state);
    if (s.terminal || s.finalization) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty())
        return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events)
        return reject(ErrorCode::capacity);
    auto& child = *s.blocks[s.count - 1U];
    const auto before = child.retained_bytes();
    const auto other = s.usage.retained - before;
    child.residual_limits(
        std::min(s.limits.component.typed_bytes, s.limits.retained_bytes - other),
        std::min(s.limits.component.simultaneous_bytes, s.limits.simultaneous_bytes - other));
    auto result = child.close(seq, time, completion);
    if (!result) {
        s.consumed = true;
        return std::unexpected{result.error()};
    }
    s.usage.retained += child.retained_bytes() - before;
    s.active = false;
    s.sequence = seq;
    ++s.event_count;
    return {};
}
auto PairPrefix::stopping(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code, Phase phase = Phase::event) {
        s.consumed = true;
        return error(code, phase);
    };
    if (!s.statistics || s.active || !s.count) return reject(ErrorCode::state);
    if (s.terminal || s.finalization) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events || s.stopping.size() == s.limits.blocks)
        return reject(ErrorCode::capacity);
    d::StoppingPrefixCounts prefix{};
    for (std::size_t i{}; i < s.count; ++i) {
        const auto events = s.blocks[i]->events();
        const auto completion = *events.back().completion;
        if (completion != runtime::BlockAttemptCompletion::operationally_incomplete) ++prefix.completed;
        if (completion == runtime::BlockAttemptCompletion::complete_valid) ++prefix.valid;
        prefix.boundary = events.front().component.plan()->block_id().value();
        prefix.boundary_found = completion != runtime::BlockAttemptCompletion::operationally_incomplete;
    }
    if (!prefix.boundary_found || (!s.stopping.empty() &&
        s.stopping.back().evidence.boundary_block_id.value() >= prefix.boundary))
        return reject(ErrorCode::association);
    // Direct eligibility predicate from Measurement repetition, not state-machine replay.
    if (!s.repetition->stopping_enabled() || !prefix.completed ||
        prefix.completed < s.repetition->minimum_blocks() || prefix.valid > UINT64_MAX / 2U ||
        2U * prefix.valid < s.repetition->minimum_valid_observations_per_variant())
        return reject(ErrorCode::association);
    const auto& limits = s.limits.component;
    if (bytes.size() > limits.canonical_bytes) return reject(ErrorCode::capacity, Phase::parse);
    const auto shape = d::count_component_wire_shape(bytes, limits.depth);
    if (!shape) return reject(shape.error().code == d::WireJsonFailure::malformed
        ? ErrorCode::malformed : ErrorCode::capacity, Phase::parse);
    if (shape->nodes > limits.nodes || shape->strings > limits.strings || shape->elements > limits.array_elements)
        return reject(ErrorCode::capacity, Phase::parse);
    const auto j = d::component_dom_stock(shape->nodes, shape->strings);
    const auto p = d::component_parser_stock(shape->bytes, shape->depth);
    const auto q = d::component_renderer_stock(shape->depth, shape->members, shape->bytes);
    if (!j || !p || !q) return reject(ErrorCode::capacity, Phase::parse);
    // At most eleven persistent scalar/policy results plus caller/return staging;
    // sixteen maximum-sized expected cells also cover the nested bootstrap helper.
    // Eight fixed path cells cover parameter, concatenation and error-return staging.
    constexpr auto diagnostic_work =
        16U * sizeof(std::expected<runtime::RunnerStoppingEvaluationEvidence, d::FixedTypedReconstructionError>) +
        8U * sizeof(d::ReconstructionPath) + 8U * sizeof(std::string_view);
    constexpr auto headers = diagnostic_work + fixed_stock + sizeof(StoppingEvent) +
        sizeof(d::StoppingPrefixCounts) + sizeof(d::FixedTypedReconstructionError) +
        sizeof(d::ComponentWireShape) + 4U * sizeof(Sum) +
        2U * sizeof(runtime::RunnerStoppingEvaluationEvidence);
    Sum retained{s.usage.retained}; retained.add(bytes.size()); retained.add(time.text().size());
    Sum base{s.usage.retained}; base.add(headers); base.add(time.text().size());
    Sum phase1{base.value}; phase1.add(2U, bytes.size()); phase1.add(2U, *j);
    phase1.add(*p); phase1.add(2U, *q);
    if (!base.valid || !retained.valid || !phase1.valid ||
        retained.value - s.usage.retained > limits.typed_bytes || retained.value > s.limits.retained_bytes ||
        retained.value > s.limits.simultaneous_bytes ||
        phase1.value > std::min(limits.simultaneous_bytes, s.limits.simultaneous_bytes))
        return reject(ErrorCode::capacity, Phase::parse);
    try {
        auto logical = wire::decode_evidence_component(wire::EvidenceComponentKind::stopping_evaluation,
            wire::EvidenceComponentSide::first, bytes, s.context->capacity());
        if (!logical) return reject(logical.error().code == wire::LogicalCodecErrorCode::allocation_failure
            ? ErrorCode::allocation_failure : ErrorCode::malformed, Phase::parse);
        auto value = d::fixed_reconstruct_stopping_component(logical->value(), *s.statistics, *s.repetition,
            prefix, s.stopping.empty() ? std::nullopt :
                std::optional{s.stopping.back().evidence.boundary_block_id.value()}, "stopping_evaluation");
        if (!value) {
            s.consumed = true;
            Error failure{ErrorCode::reconstruction, Phase::reconstruct};
            failure.detail_code = static_cast<std::uint8_t>(value.error().code);
            const auto path = value.error().path.view();
            if (!value.error().path.fixed() || value.error().path.overflowed() ||
                path.size() > failure.diagnostic.identity.size()) failure.code = ErrorCode::diagnostic_overflow;
            else {
                std::copy(path.begin(), path.end(), failure.diagnostic.identity.begin());
                failure.diagnostic.identity_size = path.size();
            }
            return std::unexpected{failure};
        }
        const auto count = d::count_stopping_component(*value);
        if (count.overflow || count.invalid || count.nodes > limits.nodes || count.strings > limits.strings ||
            count.elements > limits.array_elements || count.depth > limits.depth || count.bytes != bytes.size())
            return reject(ErrorCode::projection, Phase::project);
        const auto jp = d::component_dom_stock(count.nodes, count.strings);
        const auto pp = d::component_parser_stock(count.bytes, count.depth);
        const auto qp = d::component_renderer_stock(count.depth, count.members, count.bytes);
        if (!jp || !pp || !qp) return reject(ErrorCode::capacity, Phase::project);
        Sum phase2{base.value}; phase2.add(2U, bytes.size()); phase2.add(*j);
        phase2.add(3U, *jp); phase2.add(*pp); phase2.add(2U, *qp); phase2.add(3U, count.bytes);
        phase2.add(count.depth, sizeof(d::JsonRetirementFrame));
        if (!phase2.valid || phase2.value > std::min(limits.simultaneous_bytes, s.limits.simultaneous_bytes))
            return reject(ErrorCode::capacity, Phase::project);
        d::WireJson projected{count.depth};
        projected.value = d::project_stopping_component(*value);
        auto canonical = d::render_wire_json(projected.value, count.depth, count.nodes, count.bytes);
        if (!canonical || *canonical != bytes) return reject(ErrorCode::projection, Phase::project);
        StoppingEvent event{seq, time, std::move(*value), std::move(*canonical),
            {phase1.value, phase2.value, bytes.size() + time.text().size()}};
        static_assert(std::is_nothrow_move_constructible_v<StoppingEvent>);
        s.stopping.push_back(std::move(event));
        s.sequence = seq;
        ++s.event_count;
        s.usage.retained = retained.value;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::finalization_staging_bytes() noexcept -> std::size_t {
    constexpr auto cell = sizeof(std::expected<memory::RegionFinalizationDiagnostic, d::FixedTypedReconstructionError>) +
        sizeof(std::expected<memory::RegionRequest, core::Error>) +
        sizeof(std::expected<runtime::PairwiseFinalizationEvidence, d::FixedTypedReconstructionError>);
    // §19.4 sums every branch, including moved argument and delegated return headers.
    static_assert(sizeof(std::expected<memory::RegionPlacementObservation, d::FixedTypedReconstructionError>) <= cell);
    static_assert(sizeof(std::expected<memory::RegionPlacementEvaluation, d::FixedTypedReconstructionError>) <= cell);
    static_assert(sizeof(std::expected<memory::PreRunPlacementDiagnostic, d::FixedTypedReconstructionError>) <= cell);
    static_assert(sizeof(std::optional<memory::PostRunPlacementEvidence>) <= cell);
    static_assert(sizeof(std::expected<runtime::FinalizedVariantEvidence, d::FixedTypedReconstructionError>) <= cell);
    static_assert(sizeof(std::expected<std::optional<std::uint64_t>, d::FixedTypedReconstructionError>) <= cell);
    static_assert(sizeof(std::array<std::uintptr_t, 32>) <= cell); // largest scalar/reference bundle
    static_assert(sizeof(Error) <= cell);
    // All fixed value/header alternatives in the §19.4 inventory. Expected
    // wrappers are checked separately because their alignment/discriminant is real.
    []<class... T>() { static_assert(((sizeof(T) <= cell) && ...)); }
        .template operator()<memory::RegionRequestInput, memory::RegionRequest,
            memory::ExactRatio, memory::RegionNumaTarget, memory::LinuxNodePageCount,
            memory::RegionPlacementObservation, memory::RegionPlacementEvaluation,
            memory::PreRunPlacementDiagnostic, memory::PostRunPlacementEvidence,
            memory::AvailableRegionFinalizationDiagnostic, memory::UnavailableRegionFinalizationDiagnostic,
            memory::RegionFinalizationDiagnostic, memory::RegionBindingEvidence,
            memory::RegionInitializationEvidence, memory::RegionProtectionEvidence,
            memory::RegionCleanupEvidence, runtime::FinalizedWorkerEvidence,
            runtime::FinalizedVariantEvidence, runtime::PairwiseFinalizationEvidence,
            std::vector<memory::RegionNumaTarget>, std::vector<memory::LinuxNodePageCount>,
            std::vector<runtime::FinalizedWorkerEvidence>, std::vector<std::uint64_t>,
            std::vector<std::uint8_t>, std::string,
            std::expected<wire::LogicalBody, wire::LogicalCodecError>,
            std::expected<std::optional<memory::RegionPlacementObservation>, d::FixedTypedReconstructionError>,
            std::expected<std::optional<memory::RegionPlacementEvaluation>, d::FixedTypedReconstructionError>,
            std::expected<memory::PlacementQualificationState, d::FixedTypedReconstructionError>,
            std::expected<memory::ExactRatio, d::FixedTypedReconstructionError>,
            std::expected<std::uint64_t, d::FixedTypedReconstructionError>,
            std::optional<memory::PlacementQualificationState>,
            std::optional<memory::RegionProtectionEvidence>, d::FixedTypedReconstructionError>();
    return sizeof(FinalizationEvent) + sizeof(wire::LogicalBody) + sizeof(d::ComponentWireShape) +
        sizeof(d::ComponentProjectionCount) + sizeof(FinalizationUsage) + 12U * sizeof(Sum) +
        176U * cell + 36U * sizeof(d::ReconstructionPath);
}
auto PairPrefix::finalization(std::uint64_t seq, const journal::Timestamp& time, std::string_view bytes)
    -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code, Phase phase = Phase::event) {
        s.consumed = true;
        return error(code, phase);
    };
    if (!s.finalization_mode || s.finalization || s.terminal || s.active) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events) return reject(ErrorCode::capacity);
    const auto& limits = s.limits.component;
    const auto ceiling = std::min(limits.simultaneous_bytes, s.limits.simultaneous_bytes);
    if (bytes.size() > limits.canonical_bytes) return reject(ErrorCode::capacity, Phase::parse);
    const auto shape = d::count_component_wire_shape(bytes, limits.depth);
    if (!shape) return reject(shape.error().code == d::WireJsonFailure::malformed
        ? ErrorCode::malformed : ErrorCode::capacity, Phase::parse);
    if (shape->nodes > limits.nodes || shape->strings > limits.strings || shape->elements > limits.array_elements)
        return reject(ErrorCode::capacity, Phase::parse);
    const auto j = d::component_dom_stock(shape->nodes, shape->strings);
    const auto p = d::component_parser_stock(shape->bytes, shape->depth);
    const auto q = d::component_renderer_stock(shape->depth, shape->members, shape->bytes);
    if (!j || !p || !q) return reject(ErrorCode::capacity, Phase::parse);
    Sum base{s.usage.retained}; base.add(finalization_staging_bytes()); base.add(time.text().size());
    Sum phase1{base.value}; phase1.add(2U, bytes.size()); phase1.add(2U, *j);
    phase1.add(*p); phase1.add(2U, *q); phase1.add(shape->depth, sizeof(d::JsonRetirementFrame));
    if (!base.valid || !phase1.valid || phase1.value > ceiling) return reject(ErrorCode::capacity, Phase::parse);
    try {
        auto logical = wire::decode_evidence_component(wire::EvidenceComponentKind::finalization,
            wire::EvidenceComponentSide::first, bytes, s.context->capacity());
        if (!logical) return reject(logical.error().code == wire::LogicalCodecErrorCode::allocation_failure
            ? ErrorCode::allocation_failure : ErrorCode::malformed, Phase::parse);
        const auto stock = d::typed_finalization_stocks(logical->value());
        const auto& cap = s.limits.finalization;
        if (!stock.owned.valid || !stock.scratch.valid || !stock.workers.valid || !stock.targets.valid ||
            !stock.pages.valid || stock.workers.value > cap.workers || stock.targets.value > cap.targets ||
            stock.pages.value > cap.pages || stock.owned.value > cap.typed_payload ||
            stock.scratch.value > cap.reconstruction_scratch ||
            stock.workers.value > std::vector<runtime::FinalizedWorkerEvidence>{}.max_size() ||
            stock.targets.value > std::vector<memory::RegionNumaTarget>{}.max_size() ||
            stock.pages.value > std::vector<memory::LinuxNodePageCount>{}.max_size() ||
            stock.workers.value > std::vector<std::uint64_t>{}.max_size() / 2U)
            return reject(ErrorCode::capacity, Phase::reconstruct);
        Sum retained{s.usage.retained}; retained.add(bytes.size()); retained.add(time.text().size());
        retained.add(stock.owned.value);
        Sum phase2a{base.value}; phase2a.add(2U, bytes.size()); phase2a.add(*j);
        phase2a.add(shape->depth, sizeof(d::JsonRetirementFrame));
        phase2a.add(2U, stock.owned.value); phase2a.add(stock.scratch.value);
        if (!retained.valid || !phase2a.valid || retained.value > s.limits.retained_bytes ||
            phase2a.value > ceiling) return reject(ErrorCode::capacity, Phase::reconstruct);
        auto value = d::fixed_reconstruct_finalization_component(logical->value(), std::nullopt);
        if (!value) {
            s.consumed = true;
            Error failure{ErrorCode::reconstruction, Phase::reconstruct};
            failure.detail_code = static_cast<std::uint8_t>(value.error().code);
            const auto path = value.error().path.view();
            if (!value.error().path.fixed() || value.error().path.overflowed() ||
                path.size() > failure.diagnostic.identity.size()) failure.code = ErrorCode::diagnostic_overflow;
            else {
                std::copy(path.begin(), path.end(), failure.diagnostic.identity.begin());
                failure.diagnostic.identity_size = path.size();
            }
            return std::unexpected{failure};
        }
        if ((s.memory[0] && !matches_preparation(*s.memory[0], value->first)) ||
            (s.memory[1] && !matches_preparation(*s.memory[1], value->second))) return reject(ErrorCode::association);
        for (const auto* variant : {&value->first, &value->second}) {
            if (variant->workers.size() != s.context->workers().size()) return reject(ErrorCode::association);
            for (std::size_t i{}; i < variant->workers.size(); ++i)
                if (variant->workers[i].worker_id != s.context->workers()[i]) return reject(ErrorCode::association);
        }
        for (std::size_t block{}; block < s.count; ++block)
            for (const auto& event : s.blocks[block]->events()) {
                const auto* factual = event.component.factual();
                if (!factual) continue;
                const auto& workers = factual->slot.comparison_variant() == measurement::ComparisonVariantSlot::first
                    ? value->first.workers : value->second.workers;
                if (factual->placements.size() != workers.size() || factual->migrations.size() != workers.size())
                    return reject(ErrorCode::association);
                for (std::size_t i{}; i < workers.size(); ++i) {
                    if (factual->placements[i].worker_id() != workers[i].worker_id ||
                        factual->migrations[i].worker_id() != workers[i].worker_id) return reject(ErrorCode::association);
                    const auto* region = workers[i].region.available();
                    if (!region) continue;
                    const auto qualification = region->pre_run().qualification_state();
                    const auto state = factual->placements[i].state();
                    if ((region->request().verify_before() && (!qualification ||
                            region->pre_run().state() != memory::PreRunDiagnosticState::observed)) ||
                        (!region->request().verify_before() && (qualification ||
                            region->pre_run().state() != memory::PreRunDiagnosticState::skipped)) ||
                        (qualification == memory::PlacementQualificationState::qualified &&
                            state != measurement::WorkerPlacementValidityState::qualified) ||
                        (qualification == memory::PlacementQualificationState::not_qualified &&
                            state != measurement::WorkerPlacementValidityState::not_qualified) ||
                        (!qualification && state != measurement::WorkerPlacementValidityState::not_applicable))
                        return reject(ErrorCode::association);
                }
            }
        const auto count = d::count_pairwise_finalization_evidence(*value);
        if (count.overflow || count.invalid || count.nodes > limits.nodes || count.strings > limits.strings ||
            count.elements > limits.array_elements || count.depth > limits.depth || count.bytes != bytes.size())
            return reject(ErrorCode::projection, Phase::project);
        const auto jp = d::component_dom_stock(count.nodes, count.strings);
        const auto pp = d::component_parser_stock(count.bytes, count.depth);
        const auto qp = d::component_renderer_stock(count.depth, count.members, count.bytes);
        if (!jp || !pp || !qp) return reject(ErrorCode::capacity, Phase::project);
        Sum phase2b{base.value}; phase2b.add(2U, bytes.size()); phase2b.add(*j);
        phase2b.add(shape->depth, sizeof(d::JsonRetirementFrame)); phase2b.add(stock.owned.value);
        phase2b.add(3U, *jp); phase2b.add(*pp); phase2b.add(2U, *qp); phase2b.add(3U, count.bytes);
        phase2b.add(count.depth, sizeof(d::JsonRetirementFrame));
        if (!phase2b.valid || phase2b.value > ceiling) return reject(ErrorCode::capacity, Phase::project);
        d::WireJson projected{count.depth};
        projected.value = d::project_pairwise_finalization_evidence(*value);
        auto canonical = d::render_wire_json(projected.value, count.depth, count.nodes, count.bytes);
        if (!canonical || *canonical != bytes) return reject(ErrorCode::projection, Phase::project);
        FinalizationEvent candidate{seq, time, std::move(*value), std::move(*canonical),
            {phase1.value, phase2a.value, phase2b.value, stock.owned.value, stock.scratch.value,
             stock.workers.value, stock.targets.value, stock.pages.value}};
        static_assert(std::is_nothrow_move_constructible_v<FinalizationEvent>);
        s.finalization.emplace(std::move(candidate));
        s.sequence = seq; ++s.event_count; s.usage.retained = retained.value;
        return {};
    } catch (const std::bad_alloc&) { return reject(ErrorCode::allocation_failure); }
      catch (const std::length_error&) { return reject(ErrorCode::capacity); }
}
auto PairPrefix::terminal(std::uint64_t seq, const journal::Timestamp& time,
                          const runtime::PairwiseRunTerminalEvidence& evidence)
    -> std::expected<void, Error> {
    if (consumed()) return error(ErrorCode::state);
    auto& s = *state_;
    auto reject = [&](ErrorCode code) {
        s.consumed = true;
        return error(code);
    };
    if (!s.terminal_mode || s.terminal || s.active) return reject(ErrorCode::state);
    if (!seq || seq <= s.sequence || time.text().empty()) return reject(ErrorCode::sequence);
    if (s.event_count == s.limits.events) return reject(ErrorCode::capacity);
    if (evidence.valueless_by_exception()) return reject(ErrorCode::malformed);
    std::uint64_t completed{}, valid{};
    bool incomplete{};
    for (std::size_t i{}; i < s.count; ++i) {
        const auto completion = *s.blocks[i]->events().back().completion;
        if (completion == runtime::BlockAttemptCompletion::operationally_incomplete) incomplete = true;
        else ++completed;
        if (completion == runtime::BlockAttemptCompletion::complete_valid) ++valid;
    }
    if (valid > UINT64_MAX / 2U) return reject(ErrorCode::capacity);
    std::size_t secondary{};
    if (const auto* normal = std::get_if<runtime::NormalRunTerminalEvidence>(&evidence)) {
        if ((s.preparation_mode && !s.preparation) || (s.monitoring_mode && !s.monitoring) || (s.control_mode && !s.control) || (s.memory_mode && (!s.memory[0] || !s.memory[1]))) return reject(ErrorCode::association);
        const auto& out = normal->outcome;
        if (incomplete || out.experiment_id() != s.context->provenance().experiment_id ||
            out.policy() != *s.repetition || out.completed_blocks() != completed ||
            out.complete_valid_blocks() != valid || out.valid_first_variant_observations() != valid * 2U ||
            out.valid_second_variant_observations() != valid * 2U)
            return reject(ErrorCode::association);
        using D = measurement::RepetitionDisposition;
        switch (out.disposition()) {
        case D::selected_work_unavailable:
            if (s.count || !s.stopping.empty()) return reject(ErrorCode::association);
            break;
        case D::maximum_blocks_reached:
            if (completed != s.repetition->maximum_blocks()) return reject(ErrorCode::association);
            break;
        case D::precision_target_reached: break;
        default: return reject(ErrorCode::malformed);
        }
        if (s.outcomes_mode) {
            const auto& policy=*s.preparation->graph.policies();
            if (!policy.repetition.maximum_blocks()) {
                if (s.calibration || s.warmup || out.disposition()!=D::maximum_blocks_reached)
                    return reject(ErrorCode::association);
            } else if (out.disposition()==D::selected_work_unavailable) {
                if (s.warmup || (policy.calibration_enabled ? !s.calibration || bool(selected_work(*s.calibration)) : bool(s.calibration)))
                    return reject(ErrorCode::association);
            } else if (!s.calibration || !selected_work(*s.calibration) ||
                (policy.warmup.maximum_trials() && !s.warmup)) return reject(ErrorCode::association);
        }
        const bool eligible = s.repetition->stopping_enabled() && completed &&
            completed >= s.repetition->minimum_blocks() &&
            valid * 2U >= s.repetition->minimum_valid_observations_per_variant();
        if (eligible || out.disposition() == D::precision_target_reached) {
            if (s.stopping.empty() || !s.count ||
                s.stopping.back().evidence.boundary_block_id !=
                    s.blocks[s.count - 1U]->events().front().component.plan()->block_id())
                return reject(ErrorCode::association);
            const auto wanted = out.disposition() == D::precision_target_reached
                ? runtime::RepetitionBoundaryDisposition::precision_target_reached
                : runtime::RepetitionBoundaryDisposition::maximum_blocks_reached;
            if (s.stopping.back().evidence.subsequent_disposition != wanted)
                return reject(ErrorCode::association);
        }
    } else {
        const auto& out = std::get<runtime::OperationalRunTerminalEvidence>(evidence);
        secondary = out.secondary_errors.size();
        if (secondary > s.limits.terminal_errors) return reject(ErrorCode::capacity);
        if (!valid_error(out.primary_error) ||
            !std::ranges::all_of(out.secondary_errors, valid_error)) return reject(ErrorCode::malformed);
        if (out.active_block_id && (!s.count ||
            s.blocks[s.count - 1U]->events().back().completion !=
                runtime::BlockAttemptCompletion::operationally_incomplete ||
            s.blocks[s.count - 1U]->events().front().component.plan()->block_id() != *out.active_block_id))
            return reject(ErrorCode::association);
    }
    if (s.finalization_mode) {
        if (!s.finalization) {
            if (std::holds_alternative<runtime::NormalRunTerminalEvidence>(evidence) ||
                s.count || !s.stopping.empty() || (s.outcomes_mode && (s.calibration || s.warmup))) return reject(ErrorCode::association);
        } else {
            const bool normal = std::holds_alternative<runtime::NormalRunTerminalEvidence>(evidence);
            if (normal && !d::validate_normal_finalization(s.finalization->evidence))
                return reject(ErrorCode::association);
            if (!normal && secondary == SIZE_MAX) return reject(ErrorCode::capacity);
            const auto count = normal ? 0U : secondary + 1U;
            for (const auto* variant : {&s.finalization->evidence.first, &s.finalization->evidence.second})
                for (const auto& worker : variant->workers)
                    if ((worker.primary_error_ordinal && *worker.primary_error_ordinal >= count) ||
                        (worker.cleanup_error_ordinal && *worker.cleanup_error_ordinal >= count))
                        return reject(ErrorCode::association);
        }
    }
    Sum retained{s.usage.retained};
    retained.add(secondary, sizeof(runtime::FinalizedEvidenceError));
    retained.add(time.text().size());
    Sum peak{retained.value};
    peak.add(terminal_staging_bytes());
    if (!retained.valid || !peak.valid || retained.value > s.limits.retained_bytes ||
        peak.value > s.limits.simultaneous_bytes) return reject(ErrorCode::capacity);
    try {
        TerminalEvent candidate{seq, time, evidence};
        s.terminal.emplace(std::move(candidate));
        s.usage.retained = retained.value;
        s.sequence = seq;
        ++s.event_count;
        return {};
    } catch (const std::bad_alloc&) {
        return reject(ErrorCode::allocation_failure);
    } catch (const std::length_error&) {
        return reject(ErrorCode::capacity);
    }
}
namespace {
// Borrowed from the independently admitted root (legacy) or request header and
// accepted prefix (compact). No borrowed dependency escapes assembly.
struct RootParts final {
    const runtime::PairwiseRunProvenance& provenance;
    const runtime::StatisticsProductionPolicy& statistics;
    const monitoring::MonitoringExecutionPolicy& monitoring;
    const measurement::RepetitionPolicy& repetition;
    const measurement::ComparisonOrderPolicy& order;
    const runtime::PairwiseCalibrationEvidence* calibration;
    const runtime::PairwiseWarmupEvidence* warmup;
    std::span<const runtime::RunnerStoppingEvaluationEvidence> stopping;
    const runtime::PairwiseFinalizationEvidence* finalization;
    const runtime::PairwiseRunTerminalEvidence& terminal;
};
auto assemble_prefix_root(PairPrefix::State& s, const RootParts& parts)
    -> std::expected<runtime::FinalizedPairwiseRunEvidence, Error> {
    // All owning argument copies precede noexcept ledger operations. Both
    // reconciliation routes use this same real ledger assembly.
    const auto stopping_count=s.statistics?s.stopping.size():parts.stopping.size();
    const auto stopping_value=[&](std::size_t index)
        -> const runtime::RunnerStoppingEvaluationEvidence& {
        return s.statistics?s.stopping[index].evidence:parts.stopping[index];
    };
    auto provenance=parts.provenance;
    auto storage=runtime::detail::PreparedPairwiseLedgerStorage::prepare(
        static_cast<std::size_t>(parts.repetition.maximum_blocks()));
    if(!storage)return error(ErrorCode::allocation_failure,Phase::handoff);
    auto ledger=runtime::detail::PairwiseLedgerPreparedAccess::create(
        std::move(provenance),parts.statistics,parts.monitoring,parts.repetition,
        parts.order,std::move(*storage));
    if(!ledger)return error(ErrorCode::association,Phase::handoff);
    if(parts.calibration) {
        auto copy=s.outcomes_mode?s.calibration->evidence:*parts.calibration;
        if(!ledger->associate_calibration(std::move(copy)))return error(ErrorCode::association,Phase::handoff);
    }
    if(parts.warmup) {
        auto copy=s.outcomes_mode?s.warmup->evidence:*parts.warmup;
        if(!ledger->associate_warmup(std::move(copy)))return error(ErrorCode::association,Phase::handoff);
    }
    std::size_t stopping{};
    for(std::size_t i{};i<s.count;++i) {
        auto copied=s.blocks[i]->copy_closed_block();
        if(!copied)return std::unexpected{copied.error()};
        const auto id=copied->plan.block_id();
        if(!ledger->append_block(std::move(*copied)))return error(ErrorCode::association,Phase::handoff);
        while(stopping<stopping_count&&stopping_value(stopping).boundary_block_id==id) {
            auto copy=stopping_value(stopping++);
            if(!ledger->append_stopping_evaluation(std::move(copy)))return error(ErrorCode::association,Phase::handoff);
        }
    }
    if(stopping!=stopping_count)return error(ErrorCode::association,Phase::handoff);
    if(parts.finalization) {
        auto copy=s.finalization?s.finalization->evidence:*parts.finalization;
        if(!ledger->associate_finalization(std::move(copy)))return error(ErrorCode::association,Phase::handoff);
    }
    auto terminal=s.terminal?s.terminal->evidence:parts.terminal;
    const auto accepted=std::visit([&](auto& value) {
        if constexpr(std::same_as<std::remove_cvref_t<decltype(value)>,runtime::NormalRunTerminalEvidence>)
            return ledger->record_normal_terminal(std::move(value));
        else return ledger->record_operational_terminal(std::move(value));
    },terminal);
    if(!accepted)return error(ErrorCode::association,Phase::handoff);
    auto assembled=std::move(*ledger).finalize();
    if(!assembled)return error(ErrorCode::association,Phase::handoff);
    return std::move(*assembled);
}
auto root_failure(const d::RootReconstructionFailure& source) noexcept -> Error {
    Error failure{source.code == d::RootReconstructionFailureCode::allocation_failure
                      ? ErrorCode::allocation_failure
                      : ErrorCode::reconstruction,
                  Phase::reconstruct};
    failure.detail_code = static_cast<std::uint8_t>(source.detail.code);
    const auto path = source.detail.path.view();
    if (!source.detail.path.fixed() || source.detail.path.overflowed() ||
        path.size() > failure.diagnostic.identity.size())
        failure.code = ErrorCode::diagnostic_overflow;
    else {
        auto& diagnostic = failure.diagnostic;
        for (std::size_t i{}; i < path.size();) {
            if (path[i] != '[') {
                diagnostic.identity[diagnostic.identity_size++] = path[i++];
                continue;
            }
            const auto end = path.find(']', i + 1U);
            std::uint64_t ordinal{};
            const auto parsed =
                end == std::string_view::npos
                    ? std::from_chars_result{}
                    : std::from_chars(path.data() + i + 1U, path.data() + end, ordinal);
            if (end == std::string_view::npos || parsed.ec != std::errc{} ||
                parsed.ptr != path.data() + end ||
                diagnostic.ordinal_count == diagnostic.ordinals.size()) {
                failure.code = ErrorCode::diagnostic_overflow;
                break;
            }
            diagnostic.identity[diagnostic.identity_size++] = '[';
            diagnostic.identity[diagnostic.identity_size++] = ']';
            diagnostic.ordinals[diagnostic.ordinal_count++] = ordinal;
            i = end + 1U;
        }
    }
    return failure;
}
} // namespace
auto PairPrefix::reconcile(std::string_view bytes) -> std::expected<void, Error> {
    if (consumed())
        return error(ErrorCode::state, Phase::handoff);
    auto& s = *state_;
    s.consumed = true;
    if (s.active || (s.terminal_mode && !s.terminal))
        return error(ErrorCode::state, Phase::handoff);
    auto phase = [&](std::initializer_list<std::size_t> values, std::size_t& observed) {
        Sum sum{s.usage.retained};
        sum.add(fixed_stock + sizeof(RootParts));
        for (const auto value : values)
            sum.add(value);
        observed = sum.value;
        return sum.valid && sum.value <= s.limits.simultaneous_bytes;
    };
    try {
        if (bytes.size() > s.limits.root_bytes)
            return error(ErrorCode::capacity, Phase::parse);
        const auto shape = d::count_component_wire_shape(bytes, s.limits.root_depth);
        if (!shape)
            return error(shape.error().code == d::WireJsonFailure::overflow ||
                                 shape.error().code == d::WireJsonFailure::byte_limit
                             ? ErrorCode::capacity
                             : ErrorCode::malformed,
                         Phase::parse);
        if (shape->nodes > s.limits.root_nodes || shape->strings > s.limits.root_strings ||
            shape->elements > s.limits.root_elements)
            return error(ErrorCode::capacity, Phase::parse);
        const auto j = d::component_dom_stock(shape->nodes, shape->strings);
        const auto p = d::component_parser_stock(shape->bytes, shape->depth);
        const auto q = d::component_renderer_stock(shape->depth, shape->members, shape->bytes);
        const auto retirement =
            d::multiply_component_stock(shape->depth, sizeof(d::JsonRetirementFrame));
        if (!j || !p || !q || !retirement || !phase({bytes.size(), *j, *j, *p, *q}, s.usage.parse))
            return error(ErrorCode::capacity, Phase::parse);
        auto logical = wire::decode_evidence_body(bytes, s.context->capacity());
        if (!logical)
            return error(logical.error().code == wire::LogicalCodecErrorCode::allocation_failure
                             ? ErrorCode::allocation_failure
                             : ErrorCode::malformed,
                         Phase::parse);
        auto stock = d::count_root_reconstruction(logical->value());
        if (!stock)
            return error(stock.error().code == d::RootReconstructionFailureCode::arithmetic
                             ? ErrorCode::capacity
                             : ErrorCode::reconstruction,
                         Phase::reconstruct);
        if (!d::check_root_reconstruction_limits(*stock, s.limits.root) ||
            !phase({bytes.size(), *j, *retirement, stock->typed, stock->scratch, stock->ledger},
                   s.usage.reconstruct))
            return error(ErrorCode::capacity, Phase::reconstruct);
        auto root =
            d::reconstruct_finalized_evidence_bounded(*logical, s.context->cpus(), s.limits.root);
        if(!root)return std::unexpected{root_failure(root.error())};
        const auto& actual = root->provenance();
        const auto& expected = s.context->provenance();
        if (actual.experiment_id != expected.experiment_id ||
            actual.scenario_profile != expected.scenario_profile ||
            actual.scenario_digest != expected.scenario_digest ||
            actual.resolved_plan_digest != expected.resolved_plan_digest ||
            actual.workload_id != expected.workload_id ||
            actual.first_variant_id != expected.first_variant_id ||
            actual.second_variant_id != expected.second_variant_id)
            return error(ErrorCode::identity, Phase::handoff);
        if (s.statistics && (!exact_policy(*s.statistics, root->statistics_policy()) ||
            *s.repetition != root->repetition_policy() || s.stopping.size() != root->stopping_evaluations().size()))
            return error(ErrorCode::association, Phase::handoff);
        if (root->blocks().size() != s.count)
            return error(ErrorCode::association, Phase::handoff);
        if (s.readiness) {
            const auto resolves = [&](core::WorkerId id) noexcept {
                const auto index = static_cast<std::size_t>(id.value());
                return index < s.readiness->workers.size() &&
                    s.readiness->workers[index].worker_id == id &&
                    s.readiness->workers[index].requested_cpu == s.context->cpus()[index];
            };
            for (const auto& block : root->blocks()) {
                for (const auto& factual : block.observations) {
                    const bool known = std::visit([&](const auto& observation) {
                        return std::ranges::all_of(observation.workers(),
                            [&](const auto& worker) { return resolves(worker.worker_id()); });
                    }, factual.observation);
                    if (!known || !std::ranges::all_of(factual.placements,
                            [&](const auto& value) { return resolves(value.worker_id()); }) ||
                        !std::ranges::all_of(factual.migrations,
                            [&](const auto& value) { return resolves(value.worker_id()); }))
                        return error(ErrorCode::association, Phase::handoff);
                }
            }
        }
        if (s.outcomes_mode && (s.calibration.has_value()!=root->calibration().has_value() ||
            s.warmup.has_value()!=root->warmup().has_value())) return error(ErrorCode::association,Phase::handoff);
        if (s.preparation_mode && (root->calibration() || root->warmup()) && !s.preparation)
            return error(ErrorCode::association, Phase::handoff);
        if (s.preparation) {
            const auto& policy=*s.preparation->graph.policies();
            if (root->comparison_order()!=policy.comparison ||
                (root->calibration() && root->calibration()->index()!=policy.kernel.index()) ||
                (root->warmup() && root->warmup()->index()!=policy.kernel.index())) return error(ErrorCode::association,Phase::handoff);
            if (root->calibration() && !std::visit([&](const auto& outcome) {
                    const auto& outcome_policy=outcome.duration_policy();
                    return policy.calibration_enabled && outcome_policy.minimum_duration()==policy.calibration.minimum_duration() &&
                        outcome_policy.target_duration()==policy.calibration.target_duration() && outcome_policy.maximum_duration()==policy.calibration.maximum_duration();
                },*root->calibration())) return error(ErrorCode::association,Phase::handoff);
            if (root->warmup() && !std::visit([&](const auto& outcome) {
                    const auto& outcome_policy=outcome.policy();
                    return outcome_policy.minimum_trials()==policy.warmup.minimum_trials() && outcome_policy.maximum_trials()==policy.warmup.maximum_trials() &&
                        outcome_policy.stability_window()==policy.warmup.stability_window() &&
                        outcome_policy.stability_threshold().numerator()==policy.warmup.stability_threshold().numerator() &&
                        outcome_policy.stability_threshold().denominator()==policy.warmup.stability_threshold().denominator();
                },*root->warmup())) return error(ErrorCode::association,Phase::handoff);
        }
        if (s.monitoring_mode) {
            if (root->monitoring_policy() != s.context->monitoring_preparation()->policy ||
                (std::holds_alternative<runtime::NormalRunTerminalEvidence>(root->terminal()) && !s.monitoring))
                return error(ErrorCode::association, Phase::handoff);
            for (const auto& block : root->blocks()) for (const auto& factual : block.observations)
                if (!s.monitoring || !matches_monitoring(*s.monitoring, factual)) return error(ErrorCode::association, Phase::handoff);
        }
        if (s.memory_mode && std::holds_alternative<runtime::NormalRunTerminalEvidence>(root->terminal()) &&
            (!s.memory[0] || !s.memory[1])) return error(ErrorCode::association, Phase::handoff);
        if (root->finalization() &&
            ((s.memory[0] && !matches_preparation(*s.memory[0], root->finalization()->first)) ||
             (s.memory[1] && !matches_preparation(*s.memory[1], root->finalization()->second))))
            return error(ErrorCode::association, Phase::handoff);
        if (s.finalization_mode && s.finalization.has_value() != root->finalization().has_value())
            return error(ErrorCode::association, Phase::handoff);
        if (s.terminal && !same_terminal(s.terminal->evidence, root->terminal()))
            return error(ErrorCode::association, Phase::handoff);
        Sum v{stock->typed};
        v.add(stock->ledger);
        Sum a{v.value};
        a.add(2U, sizeof(runtime::PairwiseBlockAttemptEvidence));
        if (!v.valid || !a.valid ||
            !phase({bytes.size(), *j, *retirement, v.value, *q}, s.usage.fragments))
            return error(ErrorCode::capacity, Phase::handoff);
        if (s.outcomes_mode) {
            for (const auto& [key, retained_canonical] : {
                std::pair{"calibration",s.calibration?&s.calibration->canonical:nullptr},
                std::pair{"warmup",s.warmup?&s.warmup->canonical:nullptr}}) {
                if (!retained_canonical) continue;
                auto canonical=d::render_wire_json(logical->value().at(key),shape->depth,shape->nodes,shape->bytes);
                if (!canonical || *canonical!=*retained_canonical) return error(ErrorCode::association,Phase::handoff);
            }
        }
        if (s.finalization) {
            auto canonical = d::render_wire_json(logical->value().at("finalization"),
                shape->depth, shape->nodes, shape->bytes);
            if (!canonical || *canonical != s.finalization->canonical)
                return error(ErrorCode::association, Phase::handoff);
        }
        if (s.statistics) {
            const auto& records = logical->value().at("stopping_evaluations");
            for (std::size_t i{}; i < s.stopping.size(); ++i) {
                auto canonical = d::render_wire_json(records[i], shape->depth, shape->nodes, bytes.size());
                if (!canonical || *canonical != s.stopping[i].canonical)
                    return error(ErrorCode::association, Phase::handoff);
            }
        }
        for (std::size_t i{}; i < s.count; ++i) {
            const auto& block = logical->value().at("blocks")[i];
            const auto expected_facts = static_cast<std::size_t>(
                std::count_if(s.blocks[i]->events().begin(), s.blocks[i]->events().end(),
                              [](const Event& event) { return event.kind == EventKind::factual; }));
            if (expected_facts != root->blocks()[i].observations.size())
                return error(ErrorCode::association, Phase::handoff);
            const bool expected_failed =
                std::any_of(s.blocks[i]->events().begin(), s.blocks[i]->events().end(),
                            [](const Event& event) { return event.kind == EventKind::failed; });
            if (expected_failed != root->blocks()[i].failed_observation.has_value())
                return error(ErrorCode::association, Phase::handoff);
            std::size_t factual_index{};
            for (const auto& event : s.blocks[i]->events()) {
                if (event.kind == EventKind::closed) {
                    if (*event.completion != root->blocks()[i].completion)
                        return error(ErrorCode::association, Phase::handoff);
                    continue;
                }
                const auto& value = event.kind == EventKind::opened ? block.at("plan")
                                    : event.kind == EventKind::factual
                                        ? block.at("observations")[factual_index++]
                                        : block.at("failed_observation");
                auto canonical =
                    d::render_wire_json(value, shape->depth, shape->nodes, bytes.size());
                if (!canonical)
                    return error(ErrorCode::projection, Phase::handoff);
                if (*canonical != event.component.canonical())
                    return error(ErrorCode::association, Phase::handoff);
            }
            if (factual_index != root->blocks()[i].observations.size())
                return error(ErrorCode::association, Phase::handoff);
        }
        if (!phase({bytes.size(), *j, *retirement, v.value, a.value}, s.usage.assembly))
            return error(ErrorCode::capacity, Phase::handoff);
        const RootParts parts{root->provenance(),root->statistics_policy(),root->monitoring_policy(),
            root->repetition_policy(),root->comparison_order(),
            root->calibration()?&*root->calibration():nullptr,root->warmup()?&*root->warmup():nullptr,
            root->stopping_evaluations(),root->finalization()?&*root->finalization():nullptr,root->terminal()};
        auto assembled=assemble_prefix_root(s,parts);
        if(!assembled)return std::unexpected{assembled.error()};
        const auto projected = d::count_finalized_evidence(*assembled);
        if (projected.invalid || projected.overflow)
            return error(ErrorCode::projection, Phase::project);
        const auto jp = d::component_dom_stock(projected.nodes, projected.strings);
        const auto pp = d::component_parser_stock(projected.bytes, projected.depth);
        const auto qp =
            d::component_renderer_stock(projected.depth, projected.members, projected.bytes);
        const auto ip =
            d::multiply_component_stock(projected.depth, sizeof(d::JsonRetirementFrame));
        if (!jp || !pp || !qp || !ip ||
            !phase({bytes.size(), *j, *retirement, v.value, a.value, *jp, *jp, *jp, *ip, *pp, *qp,
                    *qp, projected.bytes, projected.bytes, projected.bytes},
                   s.usage.projection))
            return error(ErrorCode::capacity, Phase::project);
        Sum retained{s.usage.retained};
        retained.add(projected.bytes);
        if (!retained.valid || retained.value > s.limits.retained_bytes ||
            projected.bytes > s.limits.root_bytes)
            return error(ErrorCode::capacity, Phase::project);
        auto encoded = d::encode_finalized_evidence(*assembled, s.context->capacity());
        if (!encoded)
            return error(encoded.error().code == wire::LogicalCodecErrorCode::allocation_failure
                             ? ErrorCode::allocation_failure
                             : ErrorCode::projection,
                         Phase::project);
        if (*encoded != bytes)
            return error(ErrorCode::association, Phase::project);
        s.root = std::move(*encoded);
        s.usage.retained = retained.value;
        return {};
    } catch (const std::bad_alloc&) {
        return error(ErrorCode::allocation_failure, Phase::handoff);
    } catch (const std::length_error&) {
        return error(ErrorCode::capacity, Phase::handoff);
    }
}
auto PairPrefix::reconcile_digest(const core::Sha256Digest& expected_digest,
    const d::ReconstructedEvidenceHeader& header) -> std::expected<void,Error> {
    if(consumed())return error(ErrorCode::state,Phase::handoff);
    auto& s=*state_;s.consumed=true;
    // Compact publication requires the complete factual path. Older generic
    // modes still reconcile their independently supplied complete root.
    if(s.active||!s.outcomes_mode||!s.statistics||!s.repetition||!s.terminal||
       !s.context->monitoring_preparation()||!s.context->complete_preparation())
        return error(ErrorCode::state,Phase::handoff);
    const auto& provenance=s.context->provenance();
    const auto& actual=header.provenance;
    if(actual.experiment_id!=provenance.experiment_id||actual.scenario_profile!=provenance.scenario_profile||
       actual.scenario_digest!=provenance.scenario_digest||actual.resolved_plan_digest!=provenance.resolved_plan_digest||
       actual.workload_id!=provenance.workload_id||actual.first_variant_id!=provenance.first_variant_id||
       actual.second_variant_id!=provenance.second_variant_id)
        return error(ErrorCode::identity,Phase::handoff);
    const auto& preparation=*s.context->complete_preparation();
    if(!exact_policy(*s.statistics,header.statistics_policy)||*s.repetition!=header.repetition_policy||
       header.monitoring_policy!=s.context->monitoring_preparation()->policy||
       header.comparison_order.comparison_order_seed()!=preparation.comparison_seed)
        return error(ErrorCode::association,Phase::handoff);
    const auto order=header.comparison_order.strategy();
    switch(preparation.measurement.comparison_order()) {
    case scenario::MatrixComparisonOrder::randomized_abba:
        if(order!=measurement::ComparisonOrderStrategy::randomized_abba)return error(ErrorCode::association,Phase::handoff);
        break;
    case scenario::MatrixComparisonOrder::randomized_baab:
        if(order!=measurement::ComparisonOrderStrategy::randomized_baab)return error(ErrorCode::association,Phase::handoff);
        break;
    case scenario::MatrixComparisonOrder::randomized_balanced:
        if(order!=measurement::ComparisonOrderStrategy::randomized_balanced)return error(ErrorCode::association,Phase::handoff);
        break;
    }
    if(s.preparation&&header.comparison_order!=s.preparation->graph.policies()->comparison)
        return error(ErrorCode::association,Phase::handoff);
    const auto phase=[&](std::initializer_list<std::size_t> values,std::size_t& observed) {
        Sum sum{s.usage.retained};sum.add(fixed_stock+sizeof(RootParts)+sizeof(header));
        for(const auto value:values)sum.add(value);
        observed=sum.value;return sum.valid&&sum.value<=s.limits.simultaneous_bytes;
    };
    try {
        // Each copied typed member is already charged in the accepted prefix.
        // Two prefix stocks conservatively cover the final owning graph plus
        // the largest in-flight block/optional argument copy. Reserve the real
        // maximum-block ledger vectors separately before their allocation.
        Sum copies;copies.add(2U,s.usage.retained);
        Sum ledger;ledger.add(static_cast<std::size_t>(header.repetition_policy.maximum_blocks()),
            sizeof(runtime::PairwiseBlockAttemptEvidence)+sizeof(runtime::RunnerStoppingEvaluationEvidence));
        ledger.add(sizeof(runtime::PairwiseRunEvidenceLedger)+sizeof(runtime::FinalizedPairwiseRunEvidence));
        if(!copies.valid||!ledger.valid||copies.value>s.limits.root.typed||ledger.value>s.limits.root.ledger||
           !phase({copies.value,ledger.value},s.usage.assembly))
            return error(ErrorCode::capacity,Phase::handoff);
        const RootParts parts{header.provenance,header.statistics_policy,header.monitoring_policy,
            header.repetition_policy,header.comparison_order,s.calibration?&s.calibration->evidence:nullptr,
            s.warmup?&s.warmup->evidence:nullptr,{},s.finalization?&s.finalization->evidence:nullptr,s.terminal->evidence};
        auto assembled=assemble_prefix_root(s,parts);
        if(!assembled)return std::unexpected{assembled.error()};
        const auto projected=d::count_finalized_evidence(*assembled);
        if(projected.invalid||projected.overflow)return error(ErrorCode::projection,Phase::project);
        const auto j=d::component_dom_stock(projected.nodes,projected.strings);
        const auto p=d::component_parser_stock(projected.bytes,projected.depth);
        const auto q=d::component_renderer_stock(projected.depth,projected.members,projected.bytes);
        const auto retirement=d::multiply_component_stock(projected.depth,sizeof(d::JsonRetirementFrame));
        Sum retained{s.usage.retained};retained.add(projected.bytes);
        if(!j||!p||!q||!retirement||!retained.valid||retained.value>s.limits.retained_bytes||
           projected.bytes>s.limits.root_bytes||projected.nodes>s.limits.root_nodes||
           projected.strings>s.limits.root_strings||projected.depth>s.limits.root_depth||
           !phase({copies.value,ledger.value,*j,*j,*j,*retirement,*p,*q,*q,
                   projected.bytes,projected.bytes,projected.bytes},s.usage.projection))
            return error(ErrorCode::capacity,Phase::project);
        auto encoded=d::encode_finalized_evidence(*assembled,s.context->capacity());
        if(!encoded)return error(encoded.error().code==wire::LogicalCodecErrorCode::allocation_failure?
            ErrorCode::allocation_failure:ErrorCode::projection,Phase::project);
        if(core::sha256(*encoded)!=expected_digest)return error(ErrorCode::association,Phase::project);
        // Apply the same complete-root logical/typed validators as the legacy
        // route, in addition to every already admitted component association.
        // The digest never substitutes for independent classifier replay.
        const auto shape=d::count_component_wire_shape(*encoded,s.limits.root_depth);
        if(!shape||shape->elements>s.limits.root_elements)return error(ErrorCode::capacity,Phase::parse);
        if(!phase({copies.value,ledger.value,projected.bytes,*j,*j,*p,*q},s.usage.parse))
            return error(ErrorCode::capacity,Phase::parse);
        auto logical=wire::decode_evidence_body(*encoded,s.context->capacity());
        if(!logical)return error(logical.error().code==wire::LogicalCodecErrorCode::allocation_failure?
            ErrorCode::allocation_failure:ErrorCode::malformed,Phase::parse);
        const auto stock=d::count_root_reconstruction(logical->value());
        if(!stock||!d::check_root_reconstruction_limits(*stock,s.limits.root)||
           !phase({copies.value,ledger.value,projected.bytes,*j,*retirement,
                   stock->typed,stock->scratch,stock->ledger},s.usage.reconstruct))
            return error(ErrorCode::capacity,Phase::reconstruct);
        auto verified=d::reconstruct_finalized_evidence_bounded(*logical,s.context->cpus(),s.limits.root);
        if(!verified)return std::unexpected{root_failure(verified.error())};
        s.root=std::move(*encoded);s.usage.retained=retained.value;
        return {};
    } catch(const std::bad_alloc&) {return error(ErrorCode::allocation_failure,Phase::handoff);}
      catch(const std::length_error&) {return error(ErrorCode::capacity,Phase::handoff);}
}
static_assert(std::is_nothrow_move_constructible_v<Accumulator>);
static_assert(std::is_nothrow_move_assignable_v<Accumulator>);
static_assert(std::is_nothrow_move_constructible_v<ExpectedPairContext>);
} // namespace epycbench::results::block_evidence

static_assert(std::is_nothrow_move_constructible_v<epycbench::results::block_evidence::TerminalEvent>);
