#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <expected>
#include <span>

namespace epycbench::platform_linux {

namespace detail {
struct RawMappingStatusResult;
class AnonymousMappingAccess;
} // namespace detail

enum class MemoryProtection {
    none,
    read_only,
    read_write,
};

class AnonymousMapping {
  public:
    [[nodiscard]] static auto create(std::size_t length)
        -> std::expected<AnonymousMapping, core::Error>;

    AnonymousMapping(AnonymousMapping&& other) noexcept;
    auto operator=(AnonymousMapping&& other) noexcept -> AnonymousMapping&;

    AnonymousMapping(const AnonymousMapping&) = delete;
    auto operator=(const AnonymousMapping&) -> AnonymousMapping& = delete;

    ~AnonymousMapping() noexcept;

    [[nodiscard]] auto data() noexcept -> std::byte*;
    [[nodiscard]] auto data() const noexcept -> const std::byte*;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto writable_bytes() noexcept -> std::span<std::byte>;
    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto protection() const noexcept -> MemoryProtection;

    [[nodiscard]] auto protect(MemoryProtection protection)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto disable_transparent_huge_pages()
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto release() -> std::expected<void, core::Error>;

    // Pointers and spans are invalid after release, move, or destruction. Every successful
    // protection change invalidates prior mutable access; returning to read_write requires a new
    // writable_bytes() call. One owner requires external synchronization between const access and
    // protection, advice, release, move, destruction, or writable access.

  private:
    using UnmapFunction = detail::RawMappingStatusResult (*)(void*, void*, std::size_t) noexcept;
    using ProtectFunction =
        detail::RawMappingStatusResult (*)(void*, void*, std::size_t, int) noexcept;
    using AdviseFunction = detail::RawMappingStatusResult (*)(void*, void*, std::size_t, int) noexcept;

    AnonymousMapping(void* address, std::size_t length, void* operation_context,
                     UnmapFunction unmap, ProtectFunction protect_function,
                     AdviseFunction advise) noexcept;

    void best_effort_release() noexcept;
    void clear() noexcept;

    void* address_{nullptr};
    std::size_t length_{0};
    MemoryProtection protection_{MemoryProtection::none};
    void* operation_context_{nullptr};
    UnmapFunction unmap_{nullptr};
    ProtectFunction protect_{nullptr};
    AdviseFunction advise_{nullptr};

    friend class detail::AnonymousMappingAccess;
};

} // namespace epycbench::platform_linux
