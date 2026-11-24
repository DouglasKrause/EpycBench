#pragma once
#include <cstdint>

// Private borrowed operands for the bundled timed leaves.
struct EpycBenchKernelRequestV1 {
    const unsigned char* data;
    std::uint64_t bytes;
    std::uint64_t work_units;
    std::uint64_t start_index;
};

struct EpycBenchKernelRequestV2 {
    const unsigned char* data;
    std::uint64_t bytes;
    std::uint64_t repetitions;
};

struct EpycBenchKernelRequestV3 {
    const unsigned char* input;
    std::uint64_t input_bytes;
    unsigned char* output;
    std::uint64_t output_bytes;
    std::uint64_t repetitions;
    std::uint64_t xor_mask;
};
