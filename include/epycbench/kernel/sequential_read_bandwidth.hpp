#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/plugin.h"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::kernel {

namespace detail {
struct KernelLeafAccess;
}

class SequentialReadBandwidthInput {
  public:
    SequentialReadBandwidthInput(const SequentialReadBandwidthInput&) noexcept = default;
    SequentialReadBandwidthInput(SequentialReadBandwidthInput&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthInput&)
        -> SequentialReadBandwidthInput& = delete;
    auto operator=(SequentialReadBandwidthInput&&)
        -> SequentialReadBandwidthInput& = delete;
    ~SequentialReadBandwidthInput() noexcept = default;

    [[nodiscard]] static auto
    create(SequentialReadBandwidthConfig configuration,
           std::span<const std::byte> logical_region) noexcept
        -> std::expected<SequentialReadBandwidthInput, core::Error>;

    [[nodiscard]] auto configuration() const noexcept
        -> const SequentialReadBandwidthConfig& {
        return configuration_;
    }

    [[nodiscard]] auto logical_region() const noexcept
        -> std::span<const std::byte> {
        return logical_region_;
    }

  private:
    friend struct detail::KernelLeafAccess;
    SequentialReadBandwidthInput(SequentialReadBandwidthConfig configuration,
                                 std::span<const std::byte> logical_region) noexcept;

    const SequentialReadBandwidthConfig configuration_;
    const std::span<const std::byte> logical_region_;
};

class PreparedSequentialReadBandwidth {
  public:
    PreparedSequentialReadBandwidth(const PreparedSequentialReadBandwidth&) noexcept = default;
    PreparedSequentialReadBandwidth(PreparedSequentialReadBandwidth&&) noexcept = default;
    auto operator=(const PreparedSequentialReadBandwidth&)
        -> PreparedSequentialReadBandwidth& = delete;
    auto operator=(PreparedSequentialReadBandwidth&&)
        -> PreparedSequentialReadBandwidth& = delete;
    ~PreparedSequentialReadBandwidth() noexcept = default;

    [[nodiscard]] static auto prepare(SequentialReadBandwidthInput input) noexcept
        -> PreparedSequentialReadBandwidth;

    [[nodiscard]] auto input() const noexcept -> const SequentialReadBandwidthInput& {
        return input_;
    }

    [[nodiscard]] auto word_count() const noexcept -> std::size_t { return word_count_; }

    // Bound once during untimed preparation; the session owns the module.
    void bind_module(const EpycBenchKernelV1& api) noexcept { module_ = &api; }
    [[nodiscard]] auto module() const noexcept -> const EpycBenchKernelV1* { return module_; }

  private:
    PreparedSequentialReadBandwidth(SequentialReadBandwidthInput input,
                                    std::size_t word_count) noexcept;

    const SequentialReadBandwidthInput input_;
    const std::size_t word_count_;
    const EpycBenchKernelV1* module_{};
};

class SequentialReadBandwidthPassCount {
  public:
    SequentialReadBandwidthPassCount(const SequentialReadBandwidthPassCount&) noexcept = default;
    SequentialReadBandwidthPassCount(SequentialReadBandwidthPassCount&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthPassCount&)
        -> SequentialReadBandwidthPassCount& = delete;
    auto operator=(SequentialReadBandwidthPassCount&&)
        -> SequentialReadBandwidthPassCount& = delete;
    ~SequentialReadBandwidthPassCount() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t value) noexcept
        -> std::expected<SequentialReadBandwidthPassCount, core::Error>;

    [[nodiscard]] auto value() const noexcept -> std::uint64_t { return value_; }

    auto operator<=>(const SequentialReadBandwidthPassCount&) const = default;

  private:
    friend struct detail::KernelLeafAccess;
    explicit SequentialReadBandwidthPassCount(std::uint64_t value) noexcept;

    const std::uint64_t value_;
};

class SequentialReadBandwidthTerminalValue;

[[nodiscard]]
[[gnu::noinline]]
auto execute_sequential_read_bandwidth(
    const PreparedSequentialReadBandwidth& prepared,
    SequentialReadBandwidthPassCount passes) noexcept
    -> SequentialReadBandwidthTerminalValue;

class SequentialReadBandwidthTerminalValue {
  public:
    SequentialReadBandwidthTerminalValue(
        const SequentialReadBandwidthTerminalValue&) noexcept = default;
    SequentialReadBandwidthTerminalValue(
        SequentialReadBandwidthTerminalValue&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthTerminalValue&)
        -> SequentialReadBandwidthTerminalValue& = delete;
    auto operator=(SequentialReadBandwidthTerminalValue&&)
        -> SequentialReadBandwidthTerminalValue& = delete;
    ~SequentialReadBandwidthTerminalValue() noexcept = default;

    [[nodiscard]] auto value() const noexcept -> std::uint64_t { return value_; }

  private:
    friend struct detail::KernelLeafAccess;
    explicit SequentialReadBandwidthTerminalValue(std::uint64_t value) noexcept;

    friend auto execute_sequential_read_bandwidth(
        const PreparedSequentialReadBandwidth& prepared,
        SequentialReadBandwidthPassCount passes) noexcept
        -> SequentialReadBandwidthTerminalValue;

    const std::uint64_t value_;
};

class SequentialReadBandwidthWork;

[[nodiscard]] auto account_sequential_read_bandwidth_work(
    const PreparedSequentialReadBandwidth& prepared,
    SequentialReadBandwidthPassCount passes) noexcept
    -> std::expected<SequentialReadBandwidthWork, core::Error>;

class SequentialReadBandwidthWork {
  public:
    SequentialReadBandwidthWork(const SequentialReadBandwidthWork&) noexcept = default;
    SequentialReadBandwidthWork(SequentialReadBandwidthWork&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthWork&)
        -> SequentialReadBandwidthWork& = delete;
    auto operator=(SequentialReadBandwidthWork&&)
        -> SequentialReadBandwidthWork& = delete;
    ~SequentialReadBandwidthWork() noexcept = default;

    [[nodiscard]] auto logical_bytes_read() const noexcept -> core::ByteCount {
        return logical_bytes_read_;
    }

  private:
    friend struct detail::KernelLeafAccess;
    explicit SequentialReadBandwidthWork(core::ByteCount logical_bytes_read) noexcept;

    friend auto account_sequential_read_bandwidth_work(
        const PreparedSequentialReadBandwidth& prepared,
        SequentialReadBandwidthPassCount passes) noexcept
        -> std::expected<SequentialReadBandwidthWork, core::Error>;

    const core::ByteCount logical_bytes_read_;
};

class SequentialReadBandwidthExpectedTerminalValue;

class PreparedSequentialReadBandwidthChecksum {
  public:
    PreparedSequentialReadBandwidthChecksum(
        const PreparedSequentialReadBandwidthChecksum&) noexcept = default;
    PreparedSequentialReadBandwidthChecksum(
        PreparedSequentialReadBandwidthChecksum&&) noexcept = default;
    auto operator=(const PreparedSequentialReadBandwidthChecksum&)
        -> PreparedSequentialReadBandwidthChecksum& = delete;
    auto operator=(PreparedSequentialReadBandwidthChecksum&&)
        -> PreparedSequentialReadBandwidthChecksum& = delete;
    ~PreparedSequentialReadBandwidthChecksum() noexcept = default;

    [[nodiscard]] static auto prepare(
        const PreparedSequentialReadBandwidth& prepared) noexcept
        -> PreparedSequentialReadBandwidthChecksum;

    [[nodiscard]] auto expected_terminal(
        SequentialReadBandwidthPassCount passes) const noexcept
        -> SequentialReadBandwidthExpectedTerminalValue;

  private:
    explicit PreparedSequentialReadBandwidthChecksum(
        std::uint64_t one_pass_expected) noexcept;

    const std::uint64_t one_pass_expected_;
};

class SequentialReadBandwidthExpectedTerminalValue {
  public:
    SequentialReadBandwidthExpectedTerminalValue(
        const SequentialReadBandwidthExpectedTerminalValue&) noexcept = default;
    SequentialReadBandwidthExpectedTerminalValue(
        SequentialReadBandwidthExpectedTerminalValue&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthExpectedTerminalValue&)
        -> SequentialReadBandwidthExpectedTerminalValue& = delete;
    auto operator=(SequentialReadBandwidthExpectedTerminalValue&&)
        -> SequentialReadBandwidthExpectedTerminalValue& = delete;
    ~SequentialReadBandwidthExpectedTerminalValue() noexcept = default;

    [[nodiscard]] auto value() const noexcept -> std::uint64_t { return value_; }

  private:
    friend struct detail::KernelLeafAccess;
    explicit SequentialReadBandwidthExpectedTerminalValue(std::uint64_t value) noexcept;

    friend class PreparedSequentialReadBandwidthChecksum;

    const std::uint64_t value_;
};

class SequentialReadBandwidthVerification;

[[nodiscard]] auto verify_sequential_read_bandwidth_checksum(
    SequentialReadBandwidthExpectedTerminalValue expected,
    SequentialReadBandwidthTerminalValue actual) noexcept
    -> SequentialReadBandwidthVerification;

class SequentialReadBandwidthVerification {
  public:
    SequentialReadBandwidthVerification(
        const SequentialReadBandwidthVerification&) noexcept = default;
    SequentialReadBandwidthVerification(
        SequentialReadBandwidthVerification&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthVerification&)
        -> SequentialReadBandwidthVerification& = delete;
    auto operator=(SequentialReadBandwidthVerification&&)
        -> SequentialReadBandwidthVerification& = delete;
    ~SequentialReadBandwidthVerification() noexcept = default;

    [[nodiscard]] auto expected() const noexcept
        -> SequentialReadBandwidthExpectedTerminalValue {
        return expected_;
    }

    [[nodiscard]] auto actual() const noexcept -> SequentialReadBandwidthTerminalValue {
        return actual_;
    }

    [[nodiscard]] auto matched() const noexcept -> bool {
        return actual_.value() == expected_.value();
    }

  private:
    friend struct detail::KernelLeafAccess;
    SequentialReadBandwidthVerification(
        SequentialReadBandwidthExpectedTerminalValue expected,
        SequentialReadBandwidthTerminalValue actual) noexcept;

    friend auto verify_sequential_read_bandwidth_checksum(
        SequentialReadBandwidthExpectedTerminalValue expected,
        SequentialReadBandwidthTerminalValue actual) noexcept
        -> SequentialReadBandwidthVerification;

    const SequentialReadBandwidthExpectedTerminalValue expected_;
    const SequentialReadBandwidthTerminalValue actual_;
};

} // namespace epycbench::kernel
