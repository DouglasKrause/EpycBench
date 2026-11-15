#pragma once

#include "epycbench/kernel/dependent_load_latency.hpp"

#include <array>
#include <cstdint>

namespace epycbench::measurement {

enum class ComparisonVariantSlot {
    first,
    second,
};

enum class ComparisonOrderStrategy {
    randomized_abba,
    randomized_baab,
    randomized_balanced,
};

class ComparisonOrderPolicy {
  public:
    ComparisonOrderPolicy(const ComparisonOrderPolicy&) noexcept = default;
    ComparisonOrderPolicy(ComparisonOrderPolicy&&) noexcept = default;
    auto operator=(const ComparisonOrderPolicy&) -> ComparisonOrderPolicy& = delete;
    auto operator=(ComparisonOrderPolicy&&) -> ComparisonOrderPolicy& = delete;
    ~ComparisonOrderPolicy() noexcept = default;

    [[nodiscard]] static auto create(ComparisonOrderStrategy strategy,
                                     std::uint64_t comparison_order_seed) noexcept
        -> ComparisonOrderPolicy;

    [[nodiscard]] auto strategy() const noexcept -> ComparisonOrderStrategy;
    [[nodiscard]] auto comparison_order_seed() const noexcept -> std::uint64_t;
    auto operator==(const ComparisonOrderPolicy&) const -> bool = default;

  private:
    ComparisonOrderPolicy(ComparisonOrderStrategy strategy,
                          std::uint64_t comparison_order_seed) noexcept;

    const ComparisonOrderStrategy strategy_;
    const std::uint64_t comparison_order_seed_;
};

class BlockPreparationPlan {
  public:
    BlockPreparationPlan(const BlockPreparationPlan&) noexcept = default;
    BlockPreparationPlan(BlockPreparationPlan&&) noexcept = default;
    auto operator=(const BlockPreparationPlan&) -> BlockPreparationPlan& = delete;
    auto operator=(BlockPreparationPlan&&) -> BlockPreparationPlan& = delete;
    ~BlockPreparationPlan() noexcept = default;

    [[nodiscard]] static auto create(bool calibration_required, bool warmup_required) noexcept
        -> BlockPreparationPlan;

    [[nodiscard]] auto calibration_required() const noexcept -> bool;
    [[nodiscard]] auto warmup_required() const noexcept -> bool;
    auto operator==(const BlockPreparationPlan&) const -> bool = default;

  private:
    BlockPreparationPlan(bool calibration_required, bool warmup_required) noexcept;

    const bool calibration_required_;
    const bool warmup_required_;
};

class ScheduledObservationSlot {
  public:
    ScheduledObservationSlot(const ScheduledObservationSlot&) noexcept = default;
    ScheduledObservationSlot(ScheduledObservationSlot&&) noexcept = default;
    auto operator=(const ScheduledObservationSlot&) -> ScheduledObservationSlot& = delete;
    auto operator=(ScheduledObservationSlot&&) -> ScheduledObservationSlot& = delete;
    ~ScheduledObservationSlot() noexcept = default;

    [[nodiscard]] auto block_id() const noexcept -> kernel::DependentLoadLatencyBlockId;
    [[nodiscard]] auto slot_index() const noexcept -> std::uint8_t;
    [[nodiscard]] auto comparison_variant() const noexcept -> ComparisonVariantSlot;
    auto operator==(const ScheduledObservationSlot&) const -> bool = default;

  private:
    ScheduledObservationSlot(kernel::DependentLoadLatencyBlockId block_id,
                             std::uint8_t slot_index,
                             ComparisonVariantSlot comparison_variant) noexcept;

    const kernel::DependentLoadLatencyBlockId block_id_;
    const std::uint8_t slot_index_;
    const ComparisonVariantSlot comparison_variant_;

    friend class MeasurementBlockPlan;
};

class MeasurementBlockPlan {
  public:
    MeasurementBlockPlan(const MeasurementBlockPlan&) noexcept = default;
    MeasurementBlockPlan(MeasurementBlockPlan&&) noexcept = default;
    auto operator=(const MeasurementBlockPlan&) -> MeasurementBlockPlan& = delete;
    auto operator=(MeasurementBlockPlan&&) -> MeasurementBlockPlan& = delete;
    ~MeasurementBlockPlan() noexcept = default;

    [[nodiscard]] static auto create(kernel::DependentLoadLatencyBlockId block_id,
                                     ComparisonOrderPolicy comparison_order,
                                     BlockPreparationPlan preparation) noexcept
        -> MeasurementBlockPlan;

    [[nodiscard]] auto block_id() const noexcept -> kernel::DependentLoadLatencyBlockId;
    [[nodiscard]] auto comparison_order() const noexcept -> const ComparisonOrderPolicy&;
    [[nodiscard]] auto preparation() const noexcept -> const BlockPreparationPlan&;
    [[nodiscard]] auto observations() const noexcept
        -> const std::array<ScheduledObservationSlot, 4>&;
    auto operator==(const MeasurementBlockPlan&) const -> bool = default;

  private:
    MeasurementBlockPlan(kernel::DependentLoadLatencyBlockId block_id,
                         ComparisonOrderPolicy comparison_order,
                         BlockPreparationPlan preparation,
                         std::array<ScheduledObservationSlot, 4> observations) noexcept;

    const kernel::DependentLoadLatencyBlockId block_id_;
    const ComparisonOrderPolicy comparison_order_;
    const BlockPreparationPlan preparation_;
    const std::array<ScheduledObservationSlot, 4> observations_;
};

} // namespace epycbench::measurement
