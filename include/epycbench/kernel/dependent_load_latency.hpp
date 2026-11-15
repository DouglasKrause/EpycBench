#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/kernel/configuration.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::kernel {

namespace detail {
struct KernelLeafAccess;
}

class DependentLoadLatencyInput {
  public:
    DependentLoadLatencyInput(const DependentLoadLatencyInput&) noexcept = default;
    DependentLoadLatencyInput(DependentLoadLatencyInput&&) noexcept = default;
    auto operator=(const DependentLoadLatencyInput&)
        -> DependentLoadLatencyInput& = delete;
    auto operator=(DependentLoadLatencyInput&&)
        -> DependentLoadLatencyInput& = delete;
    ~DependentLoadLatencyInput() noexcept = default;

    [[nodiscard]] static auto
    create(DependentLoadLatencyConfig configuration,
           std::span<const std::byte> logical_region) noexcept
        -> std::expected<DependentLoadLatencyInput, core::Error>;

    [[nodiscard]] auto configuration() const noexcept
        -> const DependentLoadLatencyConfig& {
        return configuration_;
    }

    [[nodiscard]] auto logical_region() const noexcept
        -> std::span<const std::byte> {
        return logical_region_;
    }

  private:
    friend struct detail::KernelLeafAccess;
    DependentLoadLatencyInput(DependentLoadLatencyConfig configuration,
                              std::span<const std::byte> logical_region) noexcept;

    const DependentLoadLatencyConfig configuration_;
    const std::span<const std::byte> logical_region_;
};

class PreparedDependentLoadLatency {
  public:
    PreparedDependentLoadLatency(const PreparedDependentLoadLatency&) noexcept = default;
    PreparedDependentLoadLatency(PreparedDependentLoadLatency&&) noexcept = default;
    auto operator=(const PreparedDependentLoadLatency&)
        -> PreparedDependentLoadLatency& = delete;
    auto operator=(PreparedDependentLoadLatency&&)
        -> PreparedDependentLoadLatency& = delete;
    ~PreparedDependentLoadLatency() noexcept = default;

    [[nodiscard]] static auto prepare(DependentLoadLatencyInput input) noexcept
        -> PreparedDependentLoadLatency;

    [[nodiscard]] auto input() const noexcept -> const DependentLoadLatencyInput& {
        return input_;
    }

    [[nodiscard]] auto node_count() const noexcept -> std::size_t { return node_count_; }

  private:
    PreparedDependentLoadLatency(DependentLoadLatencyInput input,
                                 std::size_t node_count) noexcept;

    const DependentLoadLatencyInput input_;
    const std::size_t node_count_;
};

class DependentLoadLatencyStepCount {
  public:
    DependentLoadLatencyStepCount(const DependentLoadLatencyStepCount&) noexcept = default;
    DependentLoadLatencyStepCount(DependentLoadLatencyStepCount&&) noexcept = default;
    auto operator=(const DependentLoadLatencyStepCount&)
        -> DependentLoadLatencyStepCount& = delete;
    auto operator=(DependentLoadLatencyStepCount&&)
        -> DependentLoadLatencyStepCount& = delete;
    ~DependentLoadLatencyStepCount() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t value) noexcept
        -> std::expected<DependentLoadLatencyStepCount, core::Error>;

    [[nodiscard]] auto value() const noexcept -> std::uint64_t { return value_; }

    auto operator<=>(const DependentLoadLatencyStepCount&) const = default;

  private:
    friend struct detail::KernelLeafAccess;
    explicit DependentLoadLatencyStepCount(std::uint64_t value) noexcept;

    const std::uint64_t value_;
};

class DependentLoadLatencyBlockId {
  public:
    constexpr explicit DependentLoadLatencyBlockId(std::uint64_t value) noexcept
        : value_{value} {}

    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }

    auto operator<=>(const DependentLoadLatencyBlockId&) const = default;

  private:
    const std::uint64_t value_;
};

class DependentLoadLatencySchedule {
  public:
    DependentLoadLatencySchedule(const DependentLoadLatencySchedule&) noexcept = default;
    DependentLoadLatencySchedule(DependentLoadLatencySchedule&&) noexcept = default;
    auto operator=(const DependentLoadLatencySchedule&)
        -> DependentLoadLatencySchedule& = delete;
    auto operator=(DependentLoadLatencySchedule&&)
        -> DependentLoadLatencySchedule& = delete;
    ~DependentLoadLatencySchedule() noexcept = default;

    [[nodiscard]] static auto create(
        const PreparedDependentLoadLatency& prepared, std::uint64_t root_seed,
        core::ExperimentId experiment_id, DependentLoadLatencyBlockId block_id,
        DependentLoadLatencyStepCount step_count) noexcept
        -> std::expected<DependentLoadLatencySchedule, core::Error>;

    [[nodiscard]] auto start_index() const noexcept -> std::size_t { return start_index_; }

    [[nodiscard]] auto step_count() const noexcept -> DependentLoadLatencyStepCount {
        return step_count_;
    }

  private:
    friend struct detail::KernelLeafAccess;
    DependentLoadLatencySchedule(std::size_t start_index,
                                 DependentLoadLatencyStepCount step_count) noexcept;

    const std::size_t start_index_;
    const DependentLoadLatencyStepCount step_count_;
};

class DependentLoadLatencyTerminalValue;

[[nodiscard]]
[[gnu::noinline]]
auto execute_dependent_load_latency(
    const PreparedDependentLoadLatency& prepared,
    DependentLoadLatencySchedule schedule) noexcept
    -> DependentLoadLatencyTerminalValue;

class DependentLoadLatencyTerminalValue {
  public:
    DependentLoadLatencyTerminalValue(
        const DependentLoadLatencyTerminalValue&) noexcept = default;
    DependentLoadLatencyTerminalValue(
        DependentLoadLatencyTerminalValue&&) noexcept = default;
    auto operator=(const DependentLoadLatencyTerminalValue&)
        -> DependentLoadLatencyTerminalValue& = delete;
    auto operator=(DependentLoadLatencyTerminalValue&&)
        -> DependentLoadLatencyTerminalValue& = delete;
    ~DependentLoadLatencyTerminalValue() noexcept = default;

    [[nodiscard]] auto value() const noexcept -> std::size_t { return value_; }

  private:
    friend struct detail::KernelLeafAccess;
    explicit DependentLoadLatencyTerminalValue(std::size_t value) noexcept;

    friend auto execute_dependent_load_latency(
        const PreparedDependentLoadLatency& prepared,
        DependentLoadLatencySchedule schedule) noexcept
        -> DependentLoadLatencyTerminalValue;

    const std::size_t value_;
};

class DependentLoadLatencyExpectedTerminalValue;

class PreparedDependentLoadLatencyVerification {
  public:
    PreparedDependentLoadLatencyVerification(
        const PreparedDependentLoadLatencyVerification&) = delete;
    auto operator=(const PreparedDependentLoadLatencyVerification&)
        -> PreparedDependentLoadLatencyVerification& = delete;
    PreparedDependentLoadLatencyVerification(
        PreparedDependentLoadLatencyVerification&&) noexcept = default;
    auto operator=(PreparedDependentLoadLatencyVerification&&) noexcept
        -> PreparedDependentLoadLatencyVerification& = default;
    ~PreparedDependentLoadLatencyVerification() noexcept = default;

    [[nodiscard]] static auto prepare(
        const PreparedDependentLoadLatency& prepared,
        std::uint64_t resolved_permutation_seed) noexcept
        -> std::expected<PreparedDependentLoadLatencyVerification, core::Error>;

    [[nodiscard]] auto expected_terminal(
        const DependentLoadLatencySchedule& schedule) const noexcept
        -> DependentLoadLatencyExpectedTerminalValue;

  private:
    friend struct detail::KernelLeafAccess;
    PreparedDependentLoadLatencyVerification(
        std::vector<std::uint64_t> permutation,
        std::vector<std::size_t> inverse_position) noexcept;

    std::vector<std::uint64_t> permutation_;
    std::vector<std::size_t> inverse_position_;
};

class DependentLoadLatencyExpectedTerminalValue {
  public:
    DependentLoadLatencyExpectedTerminalValue(
        const DependentLoadLatencyExpectedTerminalValue&) noexcept = default;
    DependentLoadLatencyExpectedTerminalValue(
        DependentLoadLatencyExpectedTerminalValue&&) noexcept = default;
    auto operator=(const DependentLoadLatencyExpectedTerminalValue&)
        -> DependentLoadLatencyExpectedTerminalValue& = delete;
    auto operator=(DependentLoadLatencyExpectedTerminalValue&&)
        -> DependentLoadLatencyExpectedTerminalValue& = delete;
    ~DependentLoadLatencyExpectedTerminalValue() noexcept = default;

    [[nodiscard]] auto value() const noexcept -> std::size_t { return value_; }

  private:
    friend struct detail::KernelLeafAccess;
    explicit DependentLoadLatencyExpectedTerminalValue(std::size_t value) noexcept;

    friend class PreparedDependentLoadLatencyVerification;
    friend class DependentLoadLatencyVerification;

    const std::size_t value_;
};

class DependentLoadLatencyVerification;

[[nodiscard]] auto verify_dependent_load_latency_terminal(
    DependentLoadLatencyExpectedTerminalValue expected,
    DependentLoadLatencyTerminalValue actual) noexcept
    -> DependentLoadLatencyVerification;

class DependentLoadLatencyVerification {
  public:
    DependentLoadLatencyVerification(
        const DependentLoadLatencyVerification&) noexcept = default;
    DependentLoadLatencyVerification(
        DependentLoadLatencyVerification&&) noexcept = default;
    auto operator=(const DependentLoadLatencyVerification&)
        -> DependentLoadLatencyVerification& = delete;
    auto operator=(DependentLoadLatencyVerification&&)
        -> DependentLoadLatencyVerification& = delete;
    ~DependentLoadLatencyVerification() noexcept = default;

    [[nodiscard]] auto expected() const noexcept
        -> DependentLoadLatencyExpectedTerminalValue {
        return expected_;
    }
    [[nodiscard]] auto actual() const noexcept -> DependentLoadLatencyTerminalValue {
        return actual_;
    }
    [[nodiscard]] auto matched() const noexcept -> bool {
        return actual_.value() == expected_.value();
    }

  private:
    friend struct detail::KernelLeafAccess;
    DependentLoadLatencyVerification(
        DependentLoadLatencyExpectedTerminalValue expected,
        DependentLoadLatencyTerminalValue actual) noexcept;

    friend auto verify_dependent_load_latency_terminal(
        DependentLoadLatencyExpectedTerminalValue expected,
        DependentLoadLatencyTerminalValue actual) noexcept
        -> DependentLoadLatencyVerification;

    const DependentLoadLatencyExpectedTerminalValue expected_;
    const DependentLoadLatencyTerminalValue actual_;
};

} // namespace epycbench::kernel
