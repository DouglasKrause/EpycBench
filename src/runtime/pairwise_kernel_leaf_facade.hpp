#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/region_initialization.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/runtime/pairwise_evidence_failure.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::runtime::detail {

enum class PairwiseKernelLeafOperation : std::uint8_t {
    bandwidth_pass_count,
    bandwidth_accounting,
    bandwidth_validation,
    latency_validation,
    latency_step_count,
    latency_schedule,
    latency_verification,
    region_initializer,
};

using PairwiseKernelBeforeOperation = void (*)(void*, PairwiseKernelLeafOperation);

enum class PairwiseKernelDrawState : std::uint8_t { selected, rejected, exhausted };
struct PairwiseKernelDrawResult final {
    PairwiseKernelDrawState state;
    std::uint64_t index{};
};
enum class PairwiseKernelPermutationFailure : std::uint8_t { stream_exhausted };
using PairwiseKernelDrawOperation = PairwiseKernelDrawResult (*)(
    void*, std::uint64_t, std::uint64_t) noexcept;
using PairwiseKernelPermutationOperation =
    std::expected<std::vector<std::uint64_t>, PairwiseKernelPermutationFailure> (*)(
        void*, std::size_t, std::uint64_t);
using PairwiseKernelCheckedSizeOperation = std::optional<std::size_t> (*)(
    void*, std::uint64_t) noexcept;
using PairwiseKernelCycleValidationOperation = bool (*)(
    void*, std::span<const std::uint64_t>);

struct PairwiseKernelAlgorithms final {
    void* context{};
    PairwiseKernelDrawOperation draw{};
    PairwiseKernelPermutationOperation permutation{};
    PairwiseKernelCheckedSizeOperation checked_size{};
    PairwiseKernelCycleValidationOperation validate_cycle{};
};

class PairwisePreparedLatencyScheduleSeed final {
  public:
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }

  private:
    explicit constexpr PairwisePreparedLatencyScheduleSeed(std::uint64_t value) noexcept
        : value_{value} {}
    std::uint64_t value_{};
    friend class PairwiseKernelLeafFacade;
};

class PairwisePreparedRegionInitializer final {
  public:
    PairwisePreparedRegionInitializer(const PairwisePreparedRegionInitializer&) = delete;
    auto operator=(const PairwisePreparedRegionInitializer&)
        -> PairwisePreparedRegionInitializer& = delete;
    PairwisePreparedRegionInitializer(PairwisePreparedRegionInitializer&&) noexcept = default;
    auto operator=(PairwisePreparedRegionInitializer&&) noexcept
        -> PairwisePreparedRegionInitializer& = default;
    ~PairwisePreparedRegionInitializer() noexcept = default;

    [[nodiscard]] auto evidence() const noexcept -> const kernel::RegionContentEvidence& {
        return evidence_;
    }

  private:
    PairwisePreparedRegionInitializer(kernel::RegionContentEvidence evidence,
                                      std::vector<std::uint64_t> successors) noexcept
        : evidence_{evidence}, successors_{std::move(successors)} {}

    kernel::RegionContentEvidence evidence_;
    std::vector<std::uint64_t> successors_;
    friend class PairwiseKernelLeafFacade;
};

class PairwiseKernelLeafFacade final {
  public:
    constexpr PairwiseKernelLeafFacade() noexcept = default;
    constexpr PairwiseKernelLeafFacade(void* hook_context,
                                       PairwiseKernelBeforeOperation before_operation,
                                       PairwiseKernelAlgorithms algorithms = {}) noexcept
        : hook_context_{hook_context}, before_operation_{before_operation},
          algorithms_{algorithms} {}

    [[nodiscard]] auto bandwidth_configuration(core::ByteCount working_set) const noexcept
        -> std::expected<kernel::SequentialReadBandwidthConfig, PairwiseKernelFailure>;
    [[nodiscard]] auto latency_configuration(
        core::ByteCount working_set, kernel::PointerCycleSeedIntent seed) const noexcept
        -> std::expected<kernel::DependentLoadLatencyConfig, PairwiseKernelFailure>;
    [[nodiscard]] auto validate_configuration_identity(
        kernel::KernelId id, const kernel::KernelConfiguration& configuration) const noexcept
        -> std::expected<void, PairwiseKernelFailure>;
    [[nodiscard]] auto bandwidth_pass_count(std::uint64_t value) const noexcept
        -> std::expected<kernel::SequentialReadBandwidthPassCount, PairwiseKernelFailure>;
    [[nodiscard]] auto bandwidth_input(
        kernel::SequentialReadBandwidthConfig configuration,
        std::span<const std::byte> logical_region) const noexcept
        -> std::expected<kernel::SequentialReadBandwidthInput, PairwiseKernelFailure>;
    [[nodiscard]] auto bandwidth_work(
        const kernel::PreparedSequentialReadBandwidth& prepared,
        kernel::SequentialReadBandwidthPassCount passes) const noexcept
        -> std::expected<kernel::SequentialReadBandwidthWork, PairwiseKernelFailure>;
    [[nodiscard]] auto latency_input(
        kernel::DependentLoadLatencyConfig configuration,
        std::span<const std::byte> logical_region) const noexcept
        -> std::expected<kernel::DependentLoadLatencyInput, PairwiseKernelFailure>;
    [[nodiscard]] auto latency_step_count(std::uint64_t value) const noexcept
        -> std::expected<kernel::DependentLoadLatencyStepCount, PairwiseKernelFailure>;
    [[nodiscard]] auto latency_schedule(
        const kernel::PreparedDependentLoadLatency& prepared,
        PairwisePreparedLatencyScheduleSeed seed,
        kernel::DependentLoadLatencyStepCount step_count) const noexcept
        -> std::expected<kernel::DependentLoadLatencySchedule, PairwiseKernelFailure>;
    [[nodiscard]] auto prepare_latency_schedule_seed(
        std::uint64_t root_seed, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id) const noexcept
        -> std::expected<PairwisePreparedLatencyScheduleSeed, PairwiseKernelFailure>;
    [[nodiscard]] auto latency_verification(
        const kernel::PreparedDependentLoadLatency& prepared,
        std::uint64_t resolved_permutation_seed) const noexcept
        -> std::expected<kernel::PreparedDependentLoadLatencyVerification,
                         PairwiseKernelFailure>;
    [[nodiscard]] auto latency_verification_for_configuration(
        kernel::DependentLoadLatencyConfig configuration,
        std::uint64_t resolved_permutation_seed) const noexcept
        -> std::expected<kernel::PreparedDependentLoadLatencyVerification,
                         PairwiseKernelFailure>;
    [[nodiscard]] auto latency_region_spec(
        kernel::DependentLoadLatencyConfig configuration,
        std::uint64_t resolved_permutation_seed) const noexcept
        -> std::expected<kernel::RegionInitializationSpec, PairwiseKernelFailure>;
    [[nodiscard]] auto prepare_region_initializer(
        kernel::RegionInitializationSpec specification) const noexcept
        -> std::expected<PairwisePreparedRegionInitializer, PairwiseKernelFailure>;
    [[nodiscard]] auto initialize_region(const PairwisePreparedRegionInitializer& initializer,
                                         std::span<std::byte> logical_region) const noexcept
        -> std::expected<void, PairwiseKernelFailure>;
    [[nodiscard]] auto bounded_draw(std::span<const std::uint64_t> draws,
                                    std::size_t& cursor,
                                    std::uint64_t bound) const noexcept
        -> std::expected<std::uint64_t, PairwiseKernelFailure>;
    [[nodiscard]] auto canonical_permutation(std::size_t node_count,
                                             std::uint64_t seed) const noexcept
        -> std::expected<std::vector<std::uint64_t>, PairwiseKernelFailure>;

  private:
    void* hook_context_{};
    PairwiseKernelBeforeOperation before_operation_{};
    PairwiseKernelAlgorithms algorithms_{};
};

} // namespace epycbench::runtime::detail
