#pragma once

#include "task_u64_support.hpp"

#include <atomic>

namespace epycbench::bundled_packages::collective_sum {

inline constexpr std::size_t line = 64;
inline constexpr std::size_t maximum_stages = 4;
inline constexpr std::uint32_t maximum_ranks = 1024;

struct alignas(line) Control {
    std::uint64_t arrivals{}, epoch{}, cancelled{}, combined{}, checksum{};
};
static_assert(sizeof(Control) == line);

struct Schedule {
    std::uint32_t ranks{}, stages{}, result_slot{};
};

struct Merge {
    std::uint32_t first_source{}, source_count{}, destination{};
};

constexpr auto state_bytes(std::uint32_t ranks) noexcept -> std::size_t {
    return sizeof(Control) + line * (2U * ranks - 1U);
}

constexpr auto schedule_bytes(std::uint32_t ranks) noexcept -> std::size_t {
    return sizeof(Schedule) + maximum_stages * ranks * (sizeof(Merge) + sizeof(std::uint32_t));
}

template <typename Byte>
auto merge_bytes(Byte* data, std::size_t index) noexcept -> Byte* {
    return data + sizeof(Schedule) + index * sizeof(Merge);
}

template <typename Byte>
auto source_bytes(Byte* data, std::uint32_t ranks, std::size_t index) noexcept -> Byte* {
    return data + sizeof(Schedule) + maximum_stages * ranks * sizeof(Merge) + index * sizeof(std::uint32_t);
}

inline auto valid_team(const EpycBenchPackageTaskRequestV1* request) noexcept -> bool {
    if (!request || request->rank_count == 0 || request->rank_count > maximum_ranks ||
        request->rank >= request->rank_count || !request->locality || request->locality_count != request->rank_count ||
        request->repetitions == 0 || request->repetitions > UINT64_MAX / (maximum_stages + 2U))
        return false;
    const auto elements = unsigned_parameter(*request, "element_count");
    return elements && *elements >= request->rank_count && *elements <= UINT64_MAX / request->repetitions;
}

inline void store_partial(unsigned char* state, std::uint32_t slot, std::uint64_t value) noexcept {
    std::memcpy(state + sizeof(Control) + line * slot, &value, sizeof(value));
}

inline auto load_partial(const unsigned char* state, std::uint32_t slot) noexcept -> std::uint64_t {
    std::uint64_t value{};
    std::memcpy(&value, state + sizeof(Control) + line * slot, sizeof(value));
    return value;
}

} // namespace epycbench::bundled_packages::collective_sum
