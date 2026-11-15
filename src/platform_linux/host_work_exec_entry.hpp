#pragma once

#include "platform_linux/host_work_admission.hpp"

namespace epycbench::platform_linux::host_work {

enum class ExecPhase : unsigned char {
    preparation, identity, target, projection, limits, recheck, flags, execute, state,
    stage, entry
};
enum class ExecReason : unsigned char {
    invalid_input, capacity, authentication, native_failure, clock, deadline,
    cancelled, limit_failure, exec_returned, invalid_state
};
struct ExecFailure final {
    ExecPhase phase{};
    ExecReason reason{};
    std::optional<Reason> limit_reason;
    std::array<detail::RawStatusResult, 4> cleanup{};
    std::size_t cleanup_count{};
};
struct ExecOperations final {
    void* context{};
    detail::RawStatusResult (*metadata)(void*, int, detail::ImageMetadata*) noexcept{};
    detail::RawStatusResult (*file_flags)(void*, int) noexcept{};
    // The raw xattr-size query: exactly {-1, ENODATA} proves absence.
    detail::RawStatusResult (*capability_size)(void*, int) noexcept{};
    detail::RawStatusResult (*cloexec)(void*, int, bool) noexcept{};
    detail::RawStatusResult (*execute)(void*, int, char* const*) noexcept{};
    detail::RawStatusResult (*close)(void*, int) noexcept{};
    detail::RawStatusResult (*running_metadata)(void*, detail::ImageMetadata*) noexcept{};
    detail::RawStatusResult (*socket_endpoint)(void*, int) noexcept{};
    // Source transport is connected AF_UNIX/SOCK_SEQPACKET. The legacy stage
    // remains a stream socket and uses socket_endpoint above.
    detail::RawStatusResult (*source_socket_endpoint)(void*, int) noexcept{};
};

// Owns the child's inherited copies. It never owns the parent's descriptors.
class HostWorkExecEntry final {
  public:
    HostWorkExecEntry(const HostWorkExecEntry&) = delete;
    auto operator=(const HostWorkExecEntry&) -> HostWorkExecEntry& = delete;
    HostWorkExecEntry(HostWorkExecEntry&&) noexcept;
    auto operator=(HostWorkExecEntry&&) -> HostWorkExecEntry& = delete;
    ~HostWorkExecEntry() noexcept;
    [[nodiscard]] static auto prepare(int expected_parent, int target, int projection,
        std::span<char* const> arguments, ExecOperations, LimitOperations) noexcept
        -> std::expected<HostWorkExecEntry, ExecFailure>;
    [[nodiscard]] static auto prepare_extended(int expected_parent, int target, int projection,
        int stage, int held_entry, std::span<char* const> arguments,
        ExecOperations, LimitOperations) noexcept
        -> std::expected<HostWorkExecEntry, ExecFailure>;
    // The source parent owns these RAW-clock limits from before launch. This
    // handoff adds no producer authority and never renews the entry interval.
    [[nodiscard]] static auto prepare_source_extended(int expected_parent, int target,
        int projection, int stage, int held_entry, std::uint64_t original_origin,
        std::uint64_t original_end, std::span<char* const> arguments,
        ExecOperations, LimitOperations) noexcept
        -> std::expected<HostWorkExecEntry, ExecFailure>;
    // A pure source producer has an authenticated stage and held input, but
    // no writable projection. It retains the same original launch fence.
    [[nodiscard]] static auto prepare_source_without_projection(int expected_parent,
        int target, int stage, int held_entry, std::uint64_t original_origin,
        std::uint64_t original_end, std::span<char* const> arguments,
        ExecOperations, LimitOperations) noexcept
        -> std::expected<HostWorkExecEntry, ExecFailure>;
    // Successful native exec never returns. All returned outcomes are terminal.
    [[nodiscard]] auto run() noexcept -> ExecFailure;
  private:
    struct SourceDeadline final { std::uint64_t origin{}, end{}; };
    [[nodiscard]] static auto prepare_impl(int, int, int, int, int,
        std::span<char* const>, ExecOperations, LimitOperations,
        std::optional<SourceDeadline>, bool without_projection) noexcept
        -> std::expected<HostWorkExecEntry, ExecFailure>;
    HostWorkExecEntry(int, int, int, int, int, std::array<char*, 33>, ExecOperations,
        LimitOperations, std::optional<SourceDeadline>) noexcept;
    int parent_{}, target_{-1}, projection_{-1};
    int stage_{-1}, entry_{-1};
    std::array<char*, 33> arguments_{};
    ExecOperations operations_;
    LimitOperations limits_;
    std::optional<SourceDeadline> source_deadline_;
};
[[nodiscard]] auto production_exec_operations() noexcept -> ExecOperations;
[[nodiscard]] auto production_source_exec_limit_operations() noexcept -> LimitOperations;

} // namespace epycbench::platform_linux::host_work
