#include "epycbench/measurement/block_scheduler.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace epycbench::measurement {

ComparisonOrderPolicy::ComparisonOrderPolicy(ComparisonOrderStrategy strategy,
                                             std::uint64_t comparison_order_seed) noexcept
    : strategy_{strategy}, comparison_order_seed_{comparison_order_seed} {}

auto ComparisonOrderPolicy::create(ComparisonOrderStrategy strategy,
                                   std::uint64_t comparison_order_seed) noexcept
    -> ComparisonOrderPolicy {
    return {strategy, comparison_order_seed};
}

auto ComparisonOrderPolicy::strategy() const noexcept -> ComparisonOrderStrategy {
    return strategy_;
}

auto ComparisonOrderPolicy::comparison_order_seed() const noexcept -> std::uint64_t {
    return comparison_order_seed_;
}

BlockPreparationPlan::BlockPreparationPlan(bool calibration_required,
                                           bool warmup_required) noexcept
    : calibration_required_{calibration_required}, warmup_required_{warmup_required} {}

auto BlockPreparationPlan::create(bool calibration_required, bool warmup_required) noexcept
    -> BlockPreparationPlan {
    return {calibration_required, warmup_required};
}

auto BlockPreparationPlan::calibration_required() const noexcept -> bool {
    return calibration_required_;
}

auto BlockPreparationPlan::warmup_required() const noexcept -> bool {
    return warmup_required_;
}

ScheduledObservationSlot::ScheduledObservationSlot(
    kernel::DependentLoadLatencyBlockId block_id, std::uint8_t slot_index,
    ComparisonVariantSlot comparison_variant) noexcept
    : block_id_{block_id}, slot_index_{slot_index}, comparison_variant_{comparison_variant} {}

auto ScheduledObservationSlot::block_id() const noexcept
    -> kernel::DependentLoadLatencyBlockId {
    return block_id_;
}

auto ScheduledObservationSlot::slot_index() const noexcept -> std::uint8_t {
    return slot_index_;
}

auto ScheduledObservationSlot::comparison_variant() const noexcept -> ComparisonVariantSlot {
    return comparison_variant_;
}

MeasurementBlockPlan::MeasurementBlockPlan(
    kernel::DependentLoadLatencyBlockId block_id, ComparisonOrderPolicy comparison_order,
    BlockPreparationPlan preparation,
    std::array<ScheduledObservationSlot, 4> observations) noexcept
    : block_id_{block_id}, comparison_order_{comparison_order}, preparation_{preparation},
      observations_{std::move(observations)} {}

auto MeasurementBlockPlan::create(kernel::DependentLoadLatencyBlockId block_id,
                                  ComparisonOrderPolicy comparison_order,
                                  BlockPreparationPlan preparation) noexcept
    -> MeasurementBlockPlan {
    const auto swap_assignment = (comparison_order.comparison_order_seed() & 1U) != 0U;
    const auto a = swap_assignment ? ComparisonVariantSlot::second
                                   : ComparisonVariantSlot::first;
    const auto b = swap_assignment ? ComparisonVariantSlot::first
                                   : ComparisonVariantSlot::second;

    bool use_baab = false;
    switch (comparison_order.strategy()) {
    case ComparisonOrderStrategy::randomized_abba: use_baab = false; break;
    case ComparisonOrderStrategy::randomized_baab: use_baab = true; break;
    case ComparisonOrderStrategy::randomized_balanced: {
        const auto start_with_baab =
            ((comparison_order.comparison_order_seed() >> 1U) & 1U) != 0U;
        const auto odd_block = (block_id.value() & 1U) != 0U;
        use_baab = start_with_baab != odd_block;
        break;
    }
    }

    const std::array<ComparisonVariantSlot, 4> variants =
        use_baab ? std::array{b, a, a, b} : std::array{a, b, b, a};
    std::array<ScheduledObservationSlot, 4> observations{
        ScheduledObservationSlot{block_id, 0U, variants[0]},
        ScheduledObservationSlot{block_id, 1U, variants[1]},
        ScheduledObservationSlot{block_id, 2U, variants[2]},
        ScheduledObservationSlot{block_id, 3U, variants[3]},
    };

    return MeasurementBlockPlan{block_id, std::move(comparison_order), std::move(preparation),
                                std::move(observations)};
}

auto MeasurementBlockPlan::block_id() const noexcept
    -> kernel::DependentLoadLatencyBlockId {
    return block_id_;
}

auto MeasurementBlockPlan::comparison_order() const noexcept -> const ComparisonOrderPolicy& {
    return comparison_order_;
}

auto MeasurementBlockPlan::preparation() const noexcept -> const BlockPreparationPlan& {
    return preparation_;
}

auto MeasurementBlockPlan::observations() const noexcept
    -> const std::array<ScheduledObservationSlot, 4>& {
    return observations_;
}

} // namespace epycbench::measurement
