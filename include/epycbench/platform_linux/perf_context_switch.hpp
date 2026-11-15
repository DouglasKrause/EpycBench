#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace epycbench::platform_linux {

namespace detail {
struct PerfContextSwitchOperations;
class PerfContextSwitchAccess;
}

enum class PerfContextSwitchKind { switch_in, voluntary_switch_out, involuntary_switch_out };
enum class PerfCaptureCompleteness { complete, incomplete };

class PerfContextSwitchRecord {
  public:
    [[nodiscard]] static constexpr auto create(RawMonotonicTimestamp timestamp,
                                               PerfContextSwitchKind kind) noexcept
        -> PerfContextSwitchRecord {
        return PerfContextSwitchRecord{timestamp, kind};
    }
    [[nodiscard]] constexpr auto timestamp() const noexcept -> RawMonotonicTimestamp {
        return timestamp_;
    }
    [[nodiscard]] constexpr auto kind() const noexcept -> PerfContextSwitchKind { return kind_; }
    constexpr auto operator==(const PerfContextSwitchRecord&) const noexcept -> bool = default;

  private:
    constexpr PerfContextSwitchRecord(RawMonotonicTimestamp timestamp,
                                      PerfContextSwitchKind kind) noexcept
        : timestamp_{timestamp}, kind_{kind} {}
    RawMonotonicTimestamp timestamp_;
    PerfContextSwitchKind kind_;
};

class PerfContextSwitchDrain {
  public:
    [[nodiscard]] static auto create(std::vector<PerfContextSwitchRecord> records,
                                     PerfCaptureCompleteness completeness)
        -> PerfContextSwitchDrain;
    [[nodiscard]] auto records() const noexcept -> std::span<const PerfContextSwitchRecord>;
    [[nodiscard]] auto completeness() const noexcept -> PerfCaptureCompleteness;
    auto operator==(const PerfContextSwitchDrain&) const noexcept -> bool = default;

  private:
    PerfContextSwitchDrain(std::vector<PerfContextSwitchRecord> records,
                           PerfCaptureCompleteness completeness) noexcept;
    std::vector<PerfContextSwitchRecord> records_;
    PerfCaptureCompleteness completeness_;
};

struct PerfContextSwitchConfiguration {
    std::uint32_t type;
    std::uint32_t size;
    std::uint64_t config;
    std::uint64_t sample_type;
    std::int32_t clock_id;
    bool sample_id_all;
    bool use_clock_id;
    bool context_switch;
    bool disabled;
    bool inherit;
    bool write_backward;
    auto operator==(const PerfContextSwitchConfiguration&) const noexcept -> bool = default;
};

class PerfContextSwitchSource {
  public:
    [[nodiscard]] static auto prepare(LinuxThreadId thread_id, std::size_t data_pages = 8U)
        -> std::expected<PerfContextSwitchSource, core::Error>;
    [[nodiscard]] static auto configuration() noexcept -> PerfContextSwitchConfiguration;
    [[nodiscard]] auto drain() noexcept -> std::expected<PerfContextSwitchDrain, core::Error>;

    PerfContextSwitchSource(PerfContextSwitchSource&&) noexcept;
    auto operator=(PerfContextSwitchSource&&) noexcept -> PerfContextSwitchSource&;
    PerfContextSwitchSource(const PerfContextSwitchSource&) = delete;
    auto operator=(const PerfContextSwitchSource&) -> PerfContextSwitchSource& = delete;
    ~PerfContextSwitchSource() noexcept;

  private:
    friend class detail::PerfContextSwitchAccess;
    struct State;
    [[nodiscard]] static auto prepare_with_operations(
        LinuxThreadId thread_id, std::size_t data_pages, std::size_t page_size,
        const detail::PerfContextSwitchOperations& operations)
        -> std::expected<PerfContextSwitchSource, core::Error>;
    explicit PerfContextSwitchSource(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace epycbench::platform_linux
