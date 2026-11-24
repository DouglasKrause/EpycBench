#pragma once

#include "epycbench/platform_linux/memory_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace epycbench::platform_linux::detail {

struct RawMapResult {
    void* address;
    int error_number;
};

struct RawMappingStatusResult {
    int value;
    int error_number;
};

struct MappingOperations {
    void* context;
    RawMapResult (*map_anonymous)(void*, std::size_t, int, int, int, std::int64_t) noexcept;
    RawMappingStatusResult (*unmap)(void*, void*, std::size_t) noexcept;
    RawMappingStatusResult (*protect)(void*, void*, std::size_t, int) noexcept;
    RawMappingStatusResult (*advise)(void*, void*, std::size_t, int) noexcept;
};

using RangeValidator = void (*)(void*, std::size_t);

[[nodiscard]] auto production_mapping_operations() noexcept -> MappingOperations;

class AnonymousMappingAccess {
  public:
    [[nodiscard]] static auto adopt(void* address, std::size_t length,
                                    const MappingOperations& operations) noexcept
        -> AnonymousMapping {
        return AnonymousMapping{address, length, operations.context, operations.unmap,
                                operations.protect, operations.advise};
    }
    [[nodiscard]] static auto operation_context(const AnonymousMapping& mapping) noexcept
        -> void* {
        return mapping.operation_context_;
    }
    [[nodiscard]] static auto address(const AnonymousMapping& mapping) noexcept -> void* {
        return mapping.address_;
    }
    [[nodiscard]] static auto unmap(const AnonymousMapping& mapping) noexcept
        -> AnonymousMapping::UnmapFunction {
        return mapping.unmap_;
    }
    [[nodiscard]] static auto protect(const AnonymousMapping& mapping) noexcept
        -> AnonymousMapping::ProtectFunction {
        return mapping.protect_;
    }
    [[nodiscard]] static auto advise(const AnonymousMapping& mapping) noexcept
        -> AnonymousMapping::AdviseFunction {
        return mapping.advise_;
    }
    static void set_protection(AnonymousMapping& mapping, MemoryProtection protection) noexcept {
        mapping.protection_ = protection;
    }
    static void clear(AnonymousMapping& mapping) noexcept { mapping.clear(); }
    [[nodiscard]] static auto create(std::size_t length, const MappingOperations& operations,
                                     RangeValidator validate_range = validate_address_range)
        -> std::expected<AnonymousMapping, core::Error>;
    [[nodiscard]] static auto create_explicit_hugetlb_2m(
        std::size_t length, const MappingOperations& operations = production_mapping_operations())
        -> std::expected<AnonymousMapping, core::Error>;

  private:
    static void validate_address_range(void* address, std::size_t length);
};

} // namespace epycbench::platform_linux::detail
