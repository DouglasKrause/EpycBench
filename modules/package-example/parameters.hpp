#pragma once

#include "epycbench/kernel/package.h"

#include <cstdint>
#include <string_view>

namespace byte_transform {
inline auto view(EpycBenchPackageTextV1 text) noexcept -> std::string_view {
    return {text.data, text.bytes};
}

inline auto named(const EpycBenchPackageValueV1* values, std::uint64_t count, std::string_view name)
    noexcept -> const EpycBenchPackageValueV1* {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (view(values[i].name) == name) return &values[i];
    }
    return nullptr;
}

inline auto parameter(const EpycBenchPackageTaskRequestV1& request, std::string_view name)
    noexcept -> const EpycBenchPackageValueV1& {
    // The manifest requires or defaults these values and validates their types.
    return *named(request.parameters, request.parameter_count, name);
}
} // namespace byte_transform
