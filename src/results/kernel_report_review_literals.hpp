// Versioned producer review literals; no producer execution or runtime dependency.
#pragma once
#include <array>
#include <string_view>
namespace epycbench::results::kernel_report::detail {
struct ReviewLiteral {
    std::string_view id, question;
};
inline constexpr std::array<ReviewLiteral, 12> latency_reviews{{
    {"latency.v1.review.candidate_loop", "Candidate loop is the timed dependent-load loop."},
    {"latency.v1.review.one_load", "Exactly one benchmark successor load occurs per logical step."},
    {"latency.v1.review.load_form", "The successor is one 64-bit scalar memory-to-GPR load."},
    {"latency.v1.review.successor_only", "The load touches only successor bytes 0 through 7."},
    {"latency.v1.review.dependency", "The loaded successor is the sole next-iteration node index."},
    {"latency.v1.review.single_chain", "No second, lookahead, or interleaved chain exists."},
    {"latency.v1.review.no_bounds", "No successor bounds validation is present."},
    {"latency.v1.review.one_step", "One back-edge iteration equals one logical step."},
    {"latency.v1.review.no_stores", "No benchmark-region write is present."},
    {"latency.v1.review.scalar_traversal", "Traversal uses no SIMD, gather, or prefetch."},
    {"latency.v1.review.terminal", "The final loaded/current node reaches the ABI scalar return."},
    {"latency.v1.review.setup", "Setup metadata loads are outside successor-load semantics."},
}};
inline constexpr std::array<ReviewLiteral, 17> bandwidth_reviews{{
    {"bandwidth.v1.review.traversal", "Selected control flow is the timed bandwidth traversal."},
    {"bandwidth.v1.review.eight_loads",
     "Exactly eight benchmark loads occur per logical 64-byte block."},
    {"bandwidth.v1.review.load_form",
     "Every benchmark load is one 64-bit scalar memory-to-GPR operation."},
    {"bandwidth.v1.review.offsets", "Logical offsets are exactly 0,8,16,24,32,40,48,56."},
    {"bandwidth.v1.review.load_order",
     "Benchmark-load program order is ascending logical word order."},
    {"bandwidth.v1.review.lane_mapping",
     "Each logical word maps to its corresponding accumulator lane."},
    {"bandwidth.v1.review.eight_chains",
     "Exactly eight independent persistent accumulator chains exist."},
    {"bandwidth.v1.review.persistence", "Accumulator chains persist across every block and pass."},
    {"bandwidth.v1.review.block_stride",
     "Block stride is 64 bytes with complete ascending coverage."},
    {"bandwidth.v1.review.pass_major", "All blocks complete before the next pass begins."},
    {"bandwidth.v1.review.pass_restart",
     "Each pass restarts at region start without resetting lanes."},
    {"bandwidth.v1.review.no_extra_access", "No hidden extra benchmark access exists."},
    {"bandwidth.v1.review.no_stores", "No benchmark-region write occurs."},
    {"bandwidth.v1.review.scalar_only",
     "Traversal has no vector, gather, prefetch, call, or helper logic."},
    {"bandwidth.v1.review.reduction", "Final scalar reduction includes every lane exactly once."},
    {"bandwidth.v1.review.terminal", "The reduced terminal reaches the ABI scalar return."},
    {"bandwidth.v1.review.setup",
     "Setup, metadata, and stack accesses are not benchmark accesses."},
}};
} // namespace epycbench::results::kernel_report::detail
