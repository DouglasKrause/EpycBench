#pragma once

#include "epycbench/kernel/package.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace epycbench::bundled_packages {

inline auto text_view(EpycBenchPackageTextV1 value) noexcept -> std::string_view {
    return value.data && value.bytes != 0 ? std::string_view{value.data, value.bytes} : std::string_view{};
}

inline auto write_json(std::string_view document, EpycBenchPackageJsonV1* out) noexcept -> int {
    if (!out || !out->data || document.size() > out->capacity) return 1;
    std::memcpy(out->data, document.data(), document.size());
    out->bytes_written = static_cast<std::uint32_t>(document.size());
    return 0;
}

struct Partition {
    std::uint64_t begin;
    std::uint64_t count;
};

inline auto unsigned_parameter(const EpycBenchPackageTaskRequestV1& request,
                               std::string_view name) noexcept -> std::optional<std::uint64_t> {
    if (!request.parameters) return std::nullopt;
    for (std::uint64_t index{}; index < request.parameter_count; ++index) {
        const auto& item = request.parameters[index];
        if (item.type == EPYCBENCH_PACKAGE_UINT64 && item.name.bytes == name.size() &&
            item.name.data && name == std::string_view{item.name.data, name.size()})
            return item.value.unsigned_value;
    }
    return std::nullopt;
}

inline auto logical_partition(const EpycBenchPackageTaskRequestV1& request, std::uint64_t total)
    noexcept -> std::optional<Partition> {
    if (total == 0 || request.repetitions == 0 || request.rank_count == 0 || request.rank >= request.rank_count)
        return std::nullopt;
    Partition result{request.partition_begin, request.partition_count};
    if (result.count == 0) {
        if (result.begin != 0) return std::nullopt;
        const auto workers = static_cast<std::uint64_t>(request.rank_count);
        const auto rank = static_cast<std::uint64_t>(request.rank);
        const auto base = total / workers;
        const auto remainder = total % workers;
        result = {rank * base + std::min(rank, remainder), base + (rank < remainder ? 1U : 0U)};
    }
    if (result.begin >= total || result.count == 0 || result.count > total - result.begin ||
        request.repetitions > std::numeric_limits<std::uint64_t>::max() / result.count)
        return std::nullopt;
    return result;
}

inline auto compare_outputs(const EpycBenchPackageReadRegionV1* observed,
                            const EpycBenchPackageReadRegionV1* expected, std::uint32_t count)
    noexcept -> int {
    if (!observed || !expected) return 1;
    for (std::uint32_t index{}; index < count; ++index) {
        if (observed[index].bytes != expected[index].bytes ||
            observed[index].bytes > std::numeric_limits<std::size_t>::max() ||
            !observed[index].data || !expected[index].data ||
            std::memcmp(observed[index].data, expected[index].data,
                        static_cast<std::size_t>(observed[index].bytes)) != 0)
            return 1;
    }
    return 0;
}

} // namespace epycbench::bundled_packages
