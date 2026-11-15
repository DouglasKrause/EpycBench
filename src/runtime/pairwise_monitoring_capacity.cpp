#include "runtime/pairwise_monitoring_capacity.hpp"
#include "monitoring/interruptible_raw_scheduler_kernel.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <ranges>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto preparation_failure() noexcept -> PairwiseRuntimeFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] auto storage_capacity_failure(std::uint64_t blocks) noexcept
    -> PairwiseRuntimeFailure {
    constexpr auto maximum_blocks =
        std::numeric_limits<std::uint64_t>::max() / std::uint64_t{4U};
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::monitoring_storage_capacity_exhausted,
        ExpectedObservedCountContext{PairwiseCount{maximum_blocks},
                                     PairwiseCount{blocks}});
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] constexpr auto add(std::uint64_t left, std::uint64_t right) noexcept
    -> std::optional<std::uint64_t> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::nullopt;
    return left + right;
}

[[nodiscard]] constexpr auto multiply(std::uint64_t left, std::uint64_t right) noexcept
    -> std::optional<std::uint64_t> {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return std::nullopt;
    return left * right;
}

template <class... Values>
[[nodiscard]] constexpr auto product(Values... values) noexcept
    -> std::optional<std::uint64_t> {
    std::optional<std::uint64_t> result{1U};
    ((result = result ? multiply(*result, static_cast<std::uint64_t>(values))
                      : std::nullopt), ...);
    return result;
}

[[nodiscard]] constexpr auto requested(
    const monitoring::MonitoringExecutionPolicy& policy,
    monitoring::MonitoringChannel channel) noexcept -> bool {
    return policy.disposition(channel) != monitoring::MonitoringChannelDisposition::disabled;
}

} // namespace

PairwiseAdmittedMonitoringCapacity::PairwiseAdmittedMonitoringCapacity(
    PairwiseMonitoringHostCapacityAdmission admission,
    std::uint64_t workers, std::uint64_t blocks, std::uint64_t captures,
    std::uint64_t rounds, std::uint64_t base_page, std::uint64_t cpu_count,
    std::uint64_t enumeration, std::uint64_t present,
    std::uint64_t domain_maximum, std::uint64_t membership_maximum,
    std::uint64_t ring, std::uint64_t mapping, std::uint64_t events,
    bool page, bool context, bool temperature, bool frequency,
    std::array<std::uint64_t,
               static_cast<std::size_t>(PairwiseMonitoringCapacitySlot::count)> slots,
    std::uint64_t preparation_rollback_values,
    std::uint64_t completion_abort_values,
    std::uint64_t whole_stop_values) noexcept
    : admission_{std::move(admission)}, workers_{workers}, blocks_{blocks},
      captures_{captures}, rounds_{rounds}, base_page_{base_page}, cpu_count_{cpu_count},
      enumeration_{enumeration}, present_{present}, domain_maximum_{domain_maximum},
      membership_maximum_{membership_maximum}, ring_{ring}, mapping_{mapping}, events_{events},
      page_{page}, context_{context}, temperature_{temperature}, frequency_{frequency},
      asynchronous_{context || temperature || frequency}, slots_{slots},
      preparation_rollback_values_{preparation_rollback_values},
      completion_abort_values_{completion_abort_values}, whole_stop_values_{whole_stop_values} {}

auto PairwiseMonitoringCapacityAccess::project(
    PairwiseMonitoringHostCapacityAdmission admission) noexcept
    -> std::expected<PairwiseAdmittedMonitoringCapacity, PairwiseRuntimeFailure> {
    try {
        const auto& authority = admission.authority();
        const auto& policy = admission.monitoring_policy();
        const auto workers = admission.worker_count();
        const auto blocks = admission.maximum_blocks();
        const auto duration = admission.maximum_observation_nanoseconds();
        const auto base_page = authority.base_page_bytes().value();
        const auto present = static_cast<std::uint64_t>(authority.present_cpus().size());
        if (!authority.authentically_bound() || workers == 0U || blocks == 0U || duration == 0U ||
            base_page == 0U || present == 0U || admission.worker_cpus().size() != workers ||
            present > static_cast<std::uint64_t>(authority.kernel_maximum_cpu()) + 1U)
            return std::unexpected{preparation_failure()};
        const auto worker_cpus = admission.worker_cpus();
        const bool ordered_workers = std::ranges::is_sorted(worker_cpus);
        if (!std::ranges::is_sorted(authority.present_cpus()) ||
            std::ranges::adjacent_find(authority.present_cpus()) != authority.present_cpus().end() ||
            (authority.provenance().kind() != platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::host_qualification_plan &&
             !ordered_workers) ||
            (ordered_workers && std::ranges::adjacent_find(worker_cpus) != worker_cpus.end()) ||
            !std::ranges::all_of(admission.worker_cpus(), [&](core::LinuxCpuId cpu) {
                return std::ranges::binary_search(authority.present_cpus(), cpu);
            }))
            return std::unexpected{preparation_failure()};

        // Host-plan workers follow their authored stable-core order. Preserve
        // that indexed association while validating the entire CPU set.
        if (!ordered_workers)
            for (std::size_t index{}; index < worker_cpus.size(); ++index)
                if (std::ranges::find(worker_cpus.first(index), worker_cpus[index]) !=
                    worker_cpus.first(index).end())
                    return std::unexpected{preparation_failure()};

        const auto cpu_count = add(authority.kernel_maximum_cpu(), 1U);
        const auto enumeration = cpu_count ? add(*cpu_count, 1U) : std::nullopt;
        const auto captures = multiply(4U, blocks);
        if (!captures)
            return std::unexpected{storage_capacity_failure(blocks)};
        const bool page = requested(policy, monitoring::MonitoringChannel::page_faults);
        const bool context = requested(policy, monitoring::MonitoringChannel::context_switches);
        const bool temperature = requested(policy, monitoring::MonitoringChannel::temperature);
        const bool frequency = requested(policy, monitoring::MonitoringChannel::frequency);
        const bool asynchronous = context || temperature || frequency;
        std::optional<std::uint64_t> rounds;
        if (!asynchronous) {
            rounds = 0U;
        } else if (context && !temperature && !frequency && !admission.interval_nanoseconds()) {
            rounds = 1U;
        } else if (admission.interval_nanoseconds() && *admission.interval_nanoseconds() != 0U) {
            const auto period = temperature || frequency
                ? monitoring::detail::point_acquisition_period_nanoseconds(
                      *admission.interval_nanoseconds())
                : *admission.interval_nanoseconds();
            rounds = add(2U, (duration - 1U) / period);
        }
        const auto ring = multiply(8U, base_page);
        const auto mapping = multiply(9U, base_page);
        if (!cpu_count || !enumeration || !rounds || !ring || !mapping ||
            *ring == 0U || *mapping == 0U)
            return std::unexpected{preparation_failure()};
        const auto events = *ring / 16U;
        if (events == 0U)
            return std::unexpected{preparation_failure()};
        const auto domains = std::min(workers, present);
        const auto memberships = present;
        const auto xp = static_cast<std::uint64_t>(page);
        const auto xc = static_cast<std::uint64_t>(context);
        const auto xt = static_cast<std::uint64_t>(temperature);
        const auto xf = static_cast<std::uint64_t>(frequency);
        const auto xa = static_cast<std::uint64_t>(asynchronous);
        const auto r_plus_one = add(*rounds, 1U);
        const auto r_plus_three = add(*rounds, 3U);
        const auto c_plus_two = add(*captures, 2U);
        const auto w_plus_one = add(workers, 1U);
        if (!r_plus_one || !r_plus_three || !c_plus_two || !w_plus_one)
            return std::unexpected{preparation_failure()};

        using Slot = PairwiseMonitoringCapacitySlot;
        std::array<std::uint64_t, static_cast<std::size_t>(Slot::count)> slots{};
        const auto set = [&](Slot slot, std::optional<std::uint64_t> value) {
            if (!value)
                return false;
            slots[static_cast<std::size_t>(slot)] = *value;
            return true;
        };
        if (!set(Slot::raw_page_before, product(*captures, xp, workers)) ||
            !set(Slot::raw_page_after, product(*captures, xp, workers)) ||
            !set(Slot::raw_context, product(*captures, xc, workers, *r_plus_one)) ||
            !set(Slot::context_offsets, product(*captures, xc, *w_plus_one)) ||
            !set(Slot::context_events,
                 product(*captures, xc, workers, *r_plus_one, events)) ||
            !set(Slot::raw_temperature, product(*captures, xt, *rounds)) ||
            !set(Slot::raw_frequency_domains, product(*captures, xf, domains)) ||
            !set(Slot::raw_frequency_acquisitions,
                 product(*captures, xf, domains, *rounds)) ||
            !set(Slot::classified_page_workers, product(*captures, xp, workers)) ||
            !set(Slot::classified_context_workers, product(*captures, xc, workers)) ||
            !set(Slot::classified_frequency_domains, product(*captures, xf, domains)) ||
            !set(Slot::frequency_nested_cpu_ids,
                 product(*captures, xf, memberships, *r_plus_three)) ||
            !set(Slot::page_pair_scratch, product(xp, workers)) ||
            !set(Slot::context_worker_scratch, product(xc, workers)) ||
            !set(Slot::context_view_scratch, product(xc, workers)) ||
            !set(Slot::context_membership_scratch, product(xc, workers)) ||
            !set(Slot::context_lookup_scratch, product(xc, workers)) ||
            !set(Slot::temperature_view_scratch, product(xt, *rounds)) ||
            !set(Slot::frequency_view_scratch, product(xf, *rounds)) ||
            !set(Slot::frequency_result_scratch, product(xf, domains)) ||
            !set(Slot::persistent_worker_scopes, workers) ||
            !set(Slot::persistent_page_sources, product(xp, workers)) ||
            !set(Slot::persistent_context_sources, product(xc, workers)) ||
            !set(Slot::persistent_context_state, product(3U, xc, workers)) ||
            !set(Slot::persistent_temperature_sources, xt) ||
            !set(Slot::persistent_frequency_sources, product(xf, domains)) ||
            !set(Slot::persistent_frequency_metadata, product(xf, domains)) ||
            !set(Slot::persistent_frequency_cpu_ids, product(2U, xf, memberships)) ||
            !set(Slot::retained_frequency_raw_bytes,
                 product(2U, base_page, xf, domains, *c_plus_two)) ||
            !set(Slot::retained_frequency_escaped_bytes,
                 product(4U, base_page, xf, domains, *c_plus_two)))
            return std::unexpected{preparation_failure()};

        const auto context_cleanup = product(xc, workers);
        const auto two_context_cleanup = product(2U, xc, workers);
        const auto three_context_cleanup = product(3U, xc, workers);
        const auto rollback = two_context_cleanup ? add(1U, *two_context_cleanup) : std::nullopt;
        const auto completion = context_cleanup ? add(1U, *context_cleanup) : std::nullopt;
        const auto whole_without_service =
            three_context_cleanup ? add(1U, *three_context_cleanup) : std::nullopt;
        const auto whole = whole_without_service ? add(*whole_without_service, xa) : std::nullopt;
        if (!rollback || !completion || !whole)
            return std::unexpected{preparation_failure()};

        return PairwiseAdmittedMonitoringCapacity{
            std::move(admission), workers, blocks, *captures, *rounds, base_page,
            *cpu_count, *enumeration, present, domains, memberships, *ring, *mapping,
            events, page, context, temperature, frequency, slots, *rollback,
            *completion, *whole};
    } catch (const std::exception&) {
        return std::unexpected{preparation_failure()};
    } catch (...) {
        return std::unexpected{preparation_failure()};
    }
}

} // namespace epycbench::runtime::detail
