#pragma once

#include "epycbench/platform_linux/file_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::platform_linux {

namespace detail {
struct IpcOperations;
class LocalIpcAccess;
class ProcessAccess;
} // namespace detail

struct EndpointReadiness {
    bool timed_out{};
    bool readable{};
    bool hangup{};
    bool error{};
    auto operator==(const EndpointReadiness&) const noexcept -> bool = default;
};

struct WritableReadiness {
    bool timed_out{};
    bool writable{};
    bool hangup{};
    bool error{};
    auto operator==(const WritableReadiness&) const noexcept -> bool = default;
};

class LocalIpcEndpoint {
  public:
    [[nodiscard]] static auto adopt(int descriptor)
        -> std::expected<LocalIpcEndpoint, core::Error>;

    LocalIpcEndpoint(LocalIpcEndpoint&&) noexcept = default;
    auto operator=(LocalIpcEndpoint&&) noexcept -> LocalIpcEndpoint& = default;
    LocalIpcEndpoint(const LocalIpcEndpoint&) = delete;
    auto operator=(const LocalIpcEndpoint&) -> LocalIpcEndpoint& = delete;
    ~LocalIpcEndpoint() noexcept = default;

    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto receive(std::span<std::byte> buffer)
        -> std::expected<std::size_t, core::Error>;
    [[nodiscard]] auto send(std::span<const std::byte> buffer)
        -> std::expected<std::size_t, core::Error>;
    [[nodiscard]] auto readiness(std::int64_t timeout_milliseconds)
        -> std::expected<EndpointReadiness, core::Error>;
    [[nodiscard]] auto observe_writable(std::int64_t timeout_milliseconds)
        -> std::expected<WritableReadiness, core::Error>;
    // May run concurrently with one blocked receive while this endpoint's owner, storage, and
    // descriptor remain alive. Close, move, and destruction must not run concurrently.
    [[nodiscard]] auto shutdown_read() -> std::expected<void, core::Error>;
    [[nodiscard]] auto close() -> std::expected<void, core::Error>;

  private:
    LocalIpcEndpoint(FileDescriptor descriptor, const detail::IpcOperations& operations) noexcept;

    FileDescriptor descriptor_;
    void* operation_context_{nullptr};
    detail::RawStatusResult (*receive_)(void*, int, void*, std::size_t, int) noexcept{nullptr};
    detail::RawStatusResult (*send_)(void*, int, const void*, std::size_t, int) noexcept{nullptr};
    detail::RawStatusResult (*poll_)(void*, int, short, int, short*) noexcept{nullptr};
    detail::RawStatusResult (*shutdown_)(void*, int, int) noexcept{nullptr};
    bool read_shutdown_{};

    friend class detail::LocalIpcAccess;
    friend class detail::ProcessAccess;
};

struct ControllerIpcEndpoints {
    LocalIpcEndpoint control_writer;
    LocalIpcEndpoint event_reader;
};

struct RunnerIpcEndpoints {
    LocalIpcEndpoint control_reader;
    LocalIpcEndpoint event_writer;
};

struct LocalIpcChannels {
    ControllerIpcEndpoints controller;
    RunnerIpcEndpoints runner;
};

[[nodiscard]] auto create_local_ipc_channels()
    -> std::expected<LocalIpcChannels, core::Error>;

} // namespace epycbench::platform_linux
