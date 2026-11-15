#pragma once

#include "platform_linux/clock_operations.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace epycbench::platform_linux::detail {

enum class RunStartStage { inputs, clock, entropy, format };
enum class RunStartReason {
    invalid_operations,
    native_failure,
    unexpected_native_result,
    invalid_timestamp,
    timestamp_overflow,
    short_entropy,
    overreported_entropy,
    formatting_failure
};
struct RunStartFailure {
    RunStartStage stage;
    RunStartReason reason;
    int error_number{};
};
struct RawEntropyResult {
    std::ptrdiff_t count;
    int error_number;
};
struct EntropyOperations {
    void* context;
    RawEntropyResult (*read)(void*, std::span<std::byte>, unsigned int) noexcept;
};
class RunStartIdentityAccess;
class RunStartIdentity final {
  public:
    [[nodiscard]] static auto acquire() noexcept
        -> std::expected<RunStartIdentity, RunStartFailure>;
    [[nodiscard]] auto run_id() const noexcept -> std::string_view {
        return {uuid_.data(), 36};
    }
    [[nodiscard]] auto utc_start() const noexcept -> std::string_view {
        return {utc_.data(), 30};
    }

  private:
    RunStartIdentity(std::array<char, 37> uuid, std::array<char, 31> utc) noexcept
        : uuid_{uuid}, utc_{utc} {}
    std::array<char, 37> uuid_;
    std::array<char, 31> utc_;
    friend class RunStartIdentityAccess;
};
class RunStartIdentityAccess final {
  public:
    [[nodiscard]] static auto acquire(ClockOperations, EntropyOperations) noexcept
        -> std::expected<RunStartIdentity, RunStartFailure>;
};

// One current realtime reading, using the identical accepted calendar formatter.
// This does not acquire entropy or mint another run identity.
[[nodiscard]] auto acquire_event_utc(ClockOperations) noexcept
    -> std::expected<std::array<char, 30>, RunStartFailure>;
[[nodiscard]] auto acquire_event_utc() noexcept
    -> std::expected<std::array<char, 30>, RunStartFailure>;

} // namespace epycbench::platform_linux::detail
