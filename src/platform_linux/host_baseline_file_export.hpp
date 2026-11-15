#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/platform_linux/file_descriptor.hpp"
#include "platform_linux/clock_operations.hpp"
#include "platform_linux/process_ipc_operations.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace epycbench::platform_linux::host_baseline_file {

enum class Reason : unsigned char {
    invalid_input, capacity, admission, metadata, access, identity,
    clock, deadline, write, progress, sync, close
};
struct Failure final {
    Reason primary{};
    std::optional<Reason> secondary;
    std::size_t confirmed_bytes{}, write_calls{};
};
struct Receipt final {
    core::ByteCount bytes_written;
    std::size_t write_calls{};
};
struct Limits final {
    std::size_t bytes{524288}, write_calls{1024};
    core::Duration duration{core::Duration::from_base_units(UINT64_C(5000000000))};
};
struct Metadata final {
    std::uint64_t device{}, inode{}, mode{}, uid{}, links{}, bytes{};
};
struct Operations final {
    void* context{};
    auto (*effective_uid)(void*) noexcept -> std::uint64_t;
    auto (*metadata)(void*, int, Metadata*) noexcept -> detail::RawStatusResult;
    auto (*access)(void*, int) noexcept -> detail::RawStatusResult;
    auto (*write_at)(void*, int, std::span<const std::byte>, std::uint64_t) noexcept
        -> detail::RawStatusResult;
    auto (*sync)(void*, int) noexcept -> detail::RawStatusResult;
    detail::ClockOperations clock;
};

// Consumes only this descriptor copy. Bytes are borrowed until it returns.
// This private byte writer establishes no Monitoring or publication authority.
[[nodiscard]] auto export_bytes(FileDescriptor, std::span<const std::byte>, Limits = {}) noexcept
    -> std::expected<Receipt, Failure>;
[[nodiscard]] auto export_bytes_with(FileDescriptor, std::span<const std::byte>,
    Limits, const Operations&) noexcept -> std::expected<Receipt, Failure>;
[[nodiscard]] auto production_operations() noexcept -> Operations;

} // namespace epycbench::platform_linux::host_baseline_file
