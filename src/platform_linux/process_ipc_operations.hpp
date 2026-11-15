#pragma once

#include "epycbench/platform_linux/child_process.hpp"

#include <cstddef>
#include <expected>

namespace epycbench::platform_linux::detail {

struct RawStatusResult { long value; int error_number; };
struct RawSpawnResult { int value; int error_number; };

struct ImageMetadata {
    std::uint64_t device{}, inode{}, mode{}, uid{}, gid{};
    std::int64_t size{}, modified_seconds{}, modified_nanoseconds{};
    // Included for test evidence; deliberately not part of image identity.
    std::uint64_t links{};
    std::int64_t changed_seconds{}, changed_nanoseconds{};
};
struct ImageOperations {
    void* context;
    RawStatusResult (*current_pid)(void*);
    RawStatusResult (*open_image)(void*, const char*, int);
    RawStatusResult (*stat_image)(void*, int, ImageMetadata*);
};
[[nodiscard]] auto production_image_operations() noexcept -> ImageOperations;

struct DescriptorOperations {
    void* context;
    RawStatusResult (*close)(void*, int) noexcept;
    RawStatusResult (*duplicate_cloexec)(void*, int, int) noexcept;
};

struct IpcOperations {
    void* context;
    RawStatusResult (*socket_pair)(void*, int, int, int, int[2]) noexcept;
    RawStatusResult (*receive)(void*, int, void*, std::size_t, int) noexcept;
    RawStatusResult (*send)(void*, int, const void*, std::size_t, int) noexcept;
    RawStatusResult (*poll)(void*, int, short, int, short*) noexcept;
    RawStatusResult (*shutdown)(void*, int, int) noexcept{nullptr};
};

struct SpawnOperations {
    void* context;
    RawSpawnResult (*initialize)(void*, void**) noexcept;
    RawSpawnResult (*add_dup2)(void*, void*, int, int) noexcept;
    RawSpawnResult (*add_close_from)(void*, void*, int) noexcept;
    RawSpawnResult (*destroy)(void*, void*) noexcept;
    RawSpawnResult (*spawn)(void*, int*, const char*, void*, char* const*, char* const*) noexcept;
};

struct ProcessOperations {
    void* context;
    RawStatusResult (*wait_pid)(void*, int, int*, int) noexcept;
    RawStatusResult (*send_signal)(void*, int, int) noexcept;
};

[[nodiscard]] auto production_descriptor_operations() noexcept -> DescriptorOperations;
[[nodiscard]] auto production_ipc_operations() noexcept -> IpcOperations;
[[nodiscard]] auto production_spawn_operations() noexcept -> SpawnOperations;
[[nodiscard]] auto production_process_operations() noexcept -> ProcessOperations;

class DescriptorAccess {
  public:
    [[nodiscard]] static auto close_raw(FileDescriptor& descriptor) noexcept -> RawStatusResult;
    [[nodiscard]] static auto adopt(int descriptor, const DescriptorOperations& operations)
        -> std::expected<FileDescriptor, core::Error>;
    [[nodiscard]] static auto duplicate_at_least(const FileDescriptor& descriptor, int minimum,
                                                 const DescriptorOperations& operations)
        -> std::expected<FileDescriptor, core::Error>;
};

enum class FixedProcessFailureKind : unsigned char {
    invalid_state, invalid_argument, malformed_result, native_failure,
    interrupted, would_block, wrong_child, invalid_status
};
struct FixedProcessFailure final {
    FixedProcessFailureKind kind{};
    int error_number{};
};
struct FixedReadiness final {
    bool timed_out{}, ready{}, hangup{}, error{};
};

// The source invocation owns raw PIDs in fixed, immovable launch slots. Share
// the terminal-status predicates without constructing a second ChildProcess.
[[nodiscard]] auto parse_fixed_process_status(int) noexcept
    -> std::expected<ProcessExitStatus, FixedProcessFailure>;

class LocalIpcAccess {
  public:
    [[nodiscard]] static auto try_receive(LocalIpcEndpoint&, std::span<std::byte>) noexcept
        -> std::expected<std::size_t, FixedProcessFailure>;
    [[nodiscard]] static auto try_send(LocalIpcEndpoint&, std::span<const std::byte>) noexcept
        -> std::expected<std::size_t, FixedProcessFailure>;
    [[nodiscard]] static auto poll(LocalIpcEndpoint&, bool writing, int timeout_ms) noexcept
        -> std::expected<FixedReadiness, FixedProcessFailure>;
    // One no-descriptor wait using retained operations after explicit close.
    [[nodiscard]] static auto pause(LocalIpcEndpoint&, int timeout_ms) noexcept
        -> std::expected<void, FixedProcessFailure>;
    [[nodiscard]] static auto close_fixed(LocalIpcEndpoint&) noexcept
        -> std::expected<void, FixedProcessFailure>;
    [[nodiscard]] static auto adopt(int descriptor,
                                    const DescriptorOperations& descriptors,
                                    const IpcOperations& ipc)
        -> std::expected<LocalIpcEndpoint, core::Error>;
    [[nodiscard]] static auto create(const DescriptorOperations& descriptors,
                                     const IpcOperations& ipc)
        -> std::expected<LocalIpcChannels, core::Error>;
};

class ProcessAccess {
  public:
    // One attempt, including blocking exact reap. The lifecycle owns EINTR retry.
    [[nodiscard]] static auto wait_fixed(ChildProcess&, bool blocking) noexcept
        -> std::expected<std::optional<ProcessExitStatus>, FixedProcessFailure>;
    [[nodiscard]] static auto signal_fixed(ChildProcess&, ProcessSignal) noexcept
        -> std::expected<void, FixedProcessFailure>;
    [[nodiscard]] static auto spawn(const ChildSpawnRequest& request,
                                    const RunnerIpcEndpoints& endpoints,
                                    const SpawnOperations& spawn,
                                    const ProcessOperations& process)
        -> std::expected<ChildProcess, core::Error>;
    [[nodiscard]] static auto make_child(LinuxProcessId pid,
                                         const ProcessOperations& operations) noexcept
        -> ChildProcess;
};

} // namespace epycbench::platform_linux::detail
