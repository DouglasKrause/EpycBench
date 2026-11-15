#pragma once

#include "epycbench/platform_linux/local_ipc.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "runtime/controller_clock.hpp"

#include <cstddef>
#include <string_view>

namespace epycbench::runtime::detail {

enum class ControllerInterruptionPolicy {
    preserve_factual,
    stop_after_frame_progress,
};

class DeadlineControlWriter final : public ipc::ByteWriter {
  public:
    DeadlineControlWriter(platform_linux::LocalIpcEndpoint& endpoint, ControllerClock clock,
                          const ControllerTimePoint& deadline,
                          std::string_view timeout_classification,
                        ControllerInterruptionPolicy interruption_policy =
                            ControllerInterruptionPolicy::preserve_factual) noexcept;
    DeadlineControlWriter(const DeadlineControlWriter&) = delete;
    auto operator=(const DeadlineControlWriter&) -> DeadlineControlWriter& = delete;
    DeadlineControlWriter(DeadlineControlWriter&&) = delete;
    auto operator=(DeadlineControlWriter&&) -> DeadlineControlWriter& = delete;

    auto write(std::span<const std::byte> bytes)
        -> std::expected<std::size_t, core::Error> override;
    [[nodiscard]] auto progress() const noexcept -> std::size_t;
    void reset_progress() noexcept;

  private:
    platform_linux::LocalIpcEndpoint* endpoint_;
    ControllerClock clock_;
    const ControllerTimePoint* deadline_;
    std::string_view timeout_classification_;
    ControllerInterruptionPolicy interruption_policy_;
    std::size_t progress_{};
};

class DeadlineEventReader final : public ipc::ByteReader {
  public:
    DeadlineEventReader(platform_linux::LocalIpcEndpoint& endpoint, ControllerClock clock,
                        const ControllerTimePoint& deadline,
                        std::string_view timeout_classification,
                        ControllerInterruptionPolicy interruption_policy =
                            ControllerInterruptionPolicy::preserve_factual) noexcept;
    DeadlineEventReader(const DeadlineEventReader&) = delete;
    auto operator=(const DeadlineEventReader&) -> DeadlineEventReader& = delete;
    DeadlineEventReader(DeadlineEventReader&&) = delete;
    auto operator=(DeadlineEventReader&&) -> DeadlineEventReader& = delete;

    auto read(std::span<std::byte> bytes)
        -> std::expected<std::size_t, core::Error> override;
    [[nodiscard]] auto progress() const noexcept -> std::size_t;
    void reset_progress() noexcept;

  private:
    platform_linux::LocalIpcEndpoint* endpoint_;
    ControllerClock clock_;
    const ControllerTimePoint* deadline_;
    std::string_view timeout_classification_;
    ControllerInterruptionPolicy interruption_policy_;
    std::size_t progress_{};
};

[[nodiscard]] auto checked_deadline(ControllerClock clock, std::chrono::milliseconds duration)
    -> std::expected<ControllerTimePoint, core::Error>;
[[nodiscard]] auto poll_timeout(ControllerClock clock, ControllerTimePoint deadline)
    -> std::expected<int, core::Error>;

} // namespace epycbench::runtime::detail
