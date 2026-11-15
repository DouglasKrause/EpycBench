#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
namespace epycbench::monitoring::qualification::detail {
enum class Q2Winner : std::uint8_t { undecided, helper_first, ceiling_first };
struct Q2ControlledGenerationProtocol {
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> target_ready{};
    std::atomic<std::uint64_t> helper_ready{};
    std::atomic<std::uint64_t> target_running{};
    std::atomic<std::uint64_t> helper_running{};
    std::atomic<std::uint64_t> target_terminal{};
    std::atomic<std::uint64_t> helper_terminal{};
    std::atomic<std::uint64_t> helper_ceiling{};
    std::atomic<std::uint64_t> voluntary_ready{};
    std::atomic<std::uint64_t> voluntary_parked{};
    std::atomic<std::uint64_t> voluntary_resumed{};
    std::atomic<std::uint64_t> voluntary_terminal{};
    std::atomic<std::uint32_t> voluntary_command{};
    std::atomic<bool> stop{};
    std::atomic<Q2Winner> winner{Q2Winner::undecided};
    std::atomic<std::uint64_t> helper_iteration{};
    std::atomic<std::uint64_t> helper_side_effect_sink{};
    std::atomic<std::uint64_t> helper_test_stop_at{std::numeric_limits<std::uint64_t>::max()};
};
void run_q2_helper_event_loop(Q2ControlledGenerationProtocol&, std::uint64_t) noexcept;
} // namespace epycbench::monitoring::qualification::detail
