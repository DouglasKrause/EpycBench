#pragma once
#include "platform_linux/process_ipc_operations.hpp"
#include <cstddef>
#include <cstdint>
namespace epycbench::platform_linux::journal_file::detail {
struct Metadata {
    std::uint64_t device{}, inode{}, owner{}, links{};
    std::int64_t size{}, modified_seconds{}, modified_nanos{}, changed_seconds{}, changed_nanos{};
    std::uint32_t permissions{};
    bool regular{}, directory{};
    auto operator==(const Metadata&) const -> bool = default;
};
using Raw = platform_linux::detail::RawStatusResult;
struct Operations {
    void* context{};
    Raw (*open_at)(void*, int, const char*, int, unsigned) noexcept {};
    Raw (*stat)(void*, int, Metadata*) noexcept {};
    Raw (*read)(void*, int, void*, std::size_t) noexcept {};
    Raw (*write)(void*, int, const void*, std::size_t) noexcept {};
    Raw (*sync)(void*, int) noexcept {};
    Raw (*close)(void*, int) noexcept {};
    Raw (*effective_uid)(void*) noexcept {};
    Raw (*rename_no_replace)(void*, int, const char*, const char*) noexcept {};
};
[[nodiscard]] auto production_operations() noexcept -> Operations;
class Access;
} // namespace epycbench::platform_linux::journal_file::detail
