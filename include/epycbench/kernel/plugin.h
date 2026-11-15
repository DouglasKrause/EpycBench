#ifndef EPYCBENCH_KERNEL_PLUGIN_H
#define EPYCBENCH_KERNEL_PLUGIN_H

#include <stdint.h>

#define EPYCBENCH_KERNEL_ABI_VERSION 1U
#define EPYCBENCH_KERNEL_BANDWIDTH 1U
#define EPYCBENCH_KERNEL_LATENCY 2U

#ifdef __cplusplus
#define EPYCBENCH_KERNEL_NOEXCEPT noexcept
extern "C" {
#else
#define EPYCBENCH_KERNEL_NOEXCEPT
#endif

/* The host owns the aligned, initialized, read-only region. work_units means
 * complete passes for bandwidth and completed dependent loads for latency.
 * Latency nodes are 64 bytes, with a uint64_t next-node index in the first word.
 * start_index is zero for bandwidth. No pointer in this structure is retained
 * after the call. All preparation and checking happen outside timed execution. */
typedef struct EpycBenchKernelRequestV1 {
    const unsigned char* data;
    uint64_t bytes;
    uint64_t work_units;
    uint64_t start_index;
} EpycBenchKernelRequestV1;

typedef struct EpycBenchKernelV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t family;
    const char* kernel_id;
    const char* implementation_id;
    const char* version;
    const char* compiler;
    const char* build_info;
    const char* timed_symbol;

    /* Return 0 on success. Preparation validates the buffer with work_units=1
     * and start_index=0; execute receives the actual work and scheduled start.
     * V1 uses stateless functions and host-owned memory; cleanup releases only
     * module-wide resources after all workers stop. */
    int (*prepare)(const EpycBenchKernelRequestV1*) EPYCBENCH_KERNEL_NOEXCEPT;
    uint64_t (*execute)(const EpycBenchKernelRequestV1*) EPYCBENCH_KERNEL_NOEXCEPT;
    int (*verify)(uint64_t expected, uint64_t actual) EPYCBENCH_KERNEL_NOEXCEPT;
    void (*cleanup)(void) EPYCBENCH_KERNEL_NOEXCEPT;
} EpycBenchKernelV1;

typedef const EpycBenchKernelV1* (*EpycBenchKernelEntryV1)(void) EPYCBENCH_KERNEL_NOEXCEPT;

/* Export this symbol with default visibility. The returned descriptor and its
 * strings remain immutable and valid until the shared library is unloaded. */
const EpycBenchKernelV1* epycbench_kernel_v1(void) EPYCBENCH_KERNEL_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
