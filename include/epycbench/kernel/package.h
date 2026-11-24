#ifndef EPYCBENCH_KERNEL_PACKAGE_H
#define EPYCBENCH_KERNEL_PACKAGE_H

#include <stdint.h>

#define EPYCBENCH_PACKAGE_ABI_VERSION 1U
#define EPYCBENCH_PACKAGE_MAX_REGIONS 32U
#define EPYCBENCH_PACKAGE_REGION_NAME_CAPACITY 64U

#define EPYCBENCH_PACKAGE_UINT64 1U
#define EPYCBENCH_PACKAGE_INT64 2U
#define EPYCBENCH_PACKAGE_FLOAT64 3U
#define EPYCBENCH_PACKAGE_BOOL 4U
#define EPYCBENCH_PACKAGE_ENUM 5U

#define EPYCBENCH_PACKAGE_INPUT 1U
#define EPYCBENCH_PACKAGE_OUTPUT 2U
#define EPYCBENCH_PACKAGE_SCRATCH 3U
#define EPYCBENCH_PACKAGE_SHARED 4U
#define EPYCBENCH_PACKAGE_SHARED_INPUT 5U

#ifdef __cplusplus
#define EPYCBENCH_PACKAGE_NOEXCEPT noexcept
extern "C" {
#else
#define EPYCBENCH_PACKAGE_NOEXCEPT
#endif

/* Text has an explicit byte count and may be read only during the call. */
typedef struct EpycBenchPackageTextV1 {
    const char* data;
    uint64_t bytes;
} EpycBenchPackageTextV1;

typedef struct EpycBenchPackageValueV1 {
    EpycBenchPackageTextV1 name;
    uint32_t type;
    union {
        uint64_t unsigned_value;
        int64_t signed_value;
        double float_value;
        uint32_t bool_value;
        EpycBenchPackageTextV1 enum_value;
    } value;
} EpycBenchPackageValueV1;

typedef struct EpycBenchPackageLocalityV1 {
    uint32_t cpu;
    uint32_t core;
    uint32_t linux_numa_node;
    uint32_t memory_node;
    uint32_t package;
    uint32_t llc;
} EpycBenchPackageLocalityV1;

typedef struct EpycBenchPackageTaskRequestV1 {
    const EpycBenchPackageValueV1* parameters;
    uint64_t parameter_count;
    uint64_t seed;
    uint64_t repetitions;
    uint32_t rank;
    uint32_t rank_count;
    /* A logical range in the task's indexing domain, separate from the byte
     * extents below. The task defines the meaning of an empty range. */
    uint64_t partition_begin;
    uint64_t partition_count;
    /* Zero-based measured block, shared by all roles in one comparison. */
    uint64_t block;
    /* Ordered by rank. Missing optional topology membership uses UINT32_MAX.
     * A direct caller may supply no topology and leave the count at zero. */
    const EpycBenchPackageLocalityV1* locality;
    uint32_t locality_count;
} EpycBenchPackageTaskRequestV1;

typedef struct EpycBenchPackageRegionRequirementV1 {
    char name[EPYCBENCH_PACKAGE_REGION_NAME_CAPACITY];
    uint32_t kind;
    uint64_t bytes;
    uint64_t alignment;
} EpycBenchPackageRegionRequirementV1;

typedef struct EpycBenchPackageLayoutV1 {
    uint32_t region_count;
    EpycBenchPackageRegionRequirementV1 regions[EPYCBENCH_PACKAGE_MAX_REGIONS];
    /* Report the effective task partition to the host. Subsequent callbacks
     * receive this range. Preserve a nonempty range supplied in the request. */
    uint64_t partition_begin;
    uint64_t partition_count;
    /* Maximum additional temporary heap bytes used by this layout's untimed
     * callbacks, excluding the declared regions and their reference copies. */
    uint64_t preparation_bytes
#ifdef __cplusplus
        = 0
#endif
        ;
} EpycBenchPackageLayoutV1;

typedef struct EpycBenchPackageReadRegionV1 {
    const char* name;
    const unsigned char* data;
    uint64_t bytes;
} EpycBenchPackageReadRegionV1;

typedef struct EpycBenchPackageWriteRegionV1 {
    const char* name;
    unsigned char* data;
    uint64_t bytes;
} EpycBenchPackageWriteRegionV1;

typedef struct EpycBenchPackageWorkV1 {
    uint64_t repetitions;
    uint64_t work_units;
    uint64_t output_count;
} EpycBenchPackageWorkV1;

typedef struct EpycBenchPackageCancellationV1 {
    void* context;
    uint32_t (*requested)(void*) EPYCBENCH_PACKAGE_NOEXCEPT;
} EpycBenchPackageCancellationV1;

typedef struct EpycBenchPackageCallV1 {
    const EpycBenchPackageTaskRequestV1* task;
    const EpycBenchPackageValueV1* tuning;
    uint64_t tuning_count;
    const EpycBenchPackageReadRegionV1* inputs;
    uint32_t input_count;
    const EpycBenchPackageWriteRegionV1* outputs;
    uint32_t output_count;
    const EpycBenchPackageWriteRegionV1* scratch;
    uint32_t scratch_count;
    const EpycBenchPackageWriteRegionV1* shared;
    uint32_t shared_count;
    const EpycBenchPackageReadRegionV1* shared_inputs;
    uint32_t shared_input_count;
    EpycBenchPackageCancellationV1 cancellation;
} EpycBenchPackageCallV1;

typedef struct EpycBenchPackageCollectiveInputV1 {
    const EpycBenchPackageTaskRequestV1* task;
    const EpycBenchPackageReadRegionV1* inputs;
    uint32_t input_count;
} EpycBenchPackageCollectiveInputV1;

typedef struct EpycBenchPackageRankReceiptV1 {
    uint32_t rank;
    uint32_t executed;
    int execute_status;
    EpycBenchPackageWorkV1 actual;
} EpycBenchPackageRankReceiptV1;

/* Bounded UTF-8 JSON object owned by the host. Callbacks write only to data,
 * report the number of bytes written, and never alter its pointer or capacity. */
typedef struct EpycBenchPackageJsonV1 {
    char* data;
    uint32_t capacity;
    uint32_t bytes_written;
} EpycBenchPackageJsonV1;

typedef struct EpycBenchPackageInspectionRequestV1 {
    EpycBenchPackageTextV1 workload;
    EpycBenchPackageTextV1 worker;
    EpycBenchPackageTextV1 l1_cache;
    /* Zero means the corresponding topology quantity was unavailable. */
    uint64_t base_page_bytes;
    uint64_t l1_capacity_bytes;
    uint64_t l1_line_bytes;
    uint64_t l1_ways;
} EpycBenchPackageInspectionRequestV1;

/* Rank zero owns one set of shared regions for the team. layout only declares
 * regions of kind EPYCBENCH_PACKAGE_SHARED or EPYCBENCH_PACKAGE_SHARED_INPUT.
 * The initialization and oracle callbacks can access all regions in declared
 * order. During timed execution a shared input has a read-only view. Ranks must
 * synchronize conflicting accesses to writable regions and check cancellation
 * during all bounded peer waits. The host sets
 * cancellation when any timed rank fails. Receipts retain completed prefixes.
 * The team oracle runs before timed execution and again after all ranks return.
 * Its reference produces independent expected final bytes and aggregate work.
 * Its compare checks the terminal shared state and derives actual aggregate work. */
typedef struct EpycBenchPackageCollectiveTaskV1 {
    int (*layout)(const EpycBenchPackageTaskRequestV1*, EpycBenchPackageLayoutV1*)
        EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*initialize)(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageWriteRegionV1*,
                      uint32_t) EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*reference)(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageCollectiveInputV1*,
                     uint32_t, const EpycBenchPackageWriteRegionV1*, uint32_t,
                     EpycBenchPackageWorkV1*) EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*compare)(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1*,
                   const EpycBenchPackageReadRegionV1*, uint32_t, const EpycBenchPackageRankReceiptV1*,
                   uint32_t, EpycBenchPackageWorkV1*) EPYCBENCH_PACKAGE_NOEXCEPT;
    /* Optional task-defined observation from quiescent verified shared state. */
    int (*observe)(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1*, uint32_t,
                   const EpycBenchPackageRankReceiptV1*, uint32_t, EpycBenchPackageJsonV1*) EPYCBENCH_PACKAGE_NOEXCEPT
#ifdef __cplusplus
        = nullptr
#endif
        ;
} EpycBenchPackageCollectiveTaskV1;

/* The task and implementation are separate shared objects. Their getters
 * return immutable descriptors valid until the corresponding object unloads.
 * Each callback returns zero on success. A nonzero status is a failed call.
 * The host owns all regions and views. Callbacks may neither retain pointers
 * nor access bytes beyond the views. Empty arrays have a count of zero.
 * The task declares positive byte extents, power-of-two alignments, and
 * distinct NUL-terminated region names. The arrays preserve declaration order
 * within each region kind. A task may supply an effective partition derived
 * from rank and rank_count when the request range is empty. The task and
 * implementation must allow concurrent calls with disjoint rank-local views.
 * Input preparation must be deterministic for the task parameters, seed and
 * partition. In a pipeline the host can replace a bound input after this step.
 * Reference and compare run
 * independently of the timed implementation and receive no tuning or scratch.
 * Reference reports total logical work, completed repetitions, and the logical
 * output count for the request. Compare checks the observed output against it.
 * The host poisons the actual output using the bitwise complement of the
 * reference output. An implementation reports the work it did. Logical
 * work units must be positive. A task may legitimately produce zero outputs.
 * Only execute is timed. It must use the supplied regions for working data. */
typedef struct EpycBenchPackageTaskV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*layout)(const EpycBenchPackageTaskRequestV1*, EpycBenchPackageLayoutV1*)
        EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*initialize)(const EpycBenchPackageTaskRequestV1*,
                      const EpycBenchPackageWriteRegionV1*, uint32_t) EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*reference)(const EpycBenchPackageTaskRequestV1*,
                     const EpycBenchPackageReadRegionV1*, uint32_t,
                     const EpycBenchPackageWriteRegionV1*, uint32_t,
                     EpycBenchPackageWorkV1*) EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*compare)(const EpycBenchPackageTaskRequestV1*,
                   const EpycBenchPackageReadRegionV1*, const EpycBenchPackageReadRegionV1*,
                   uint32_t) EPYCBENCH_PACKAGE_NOEXCEPT;
    const EpycBenchPackageCollectiveTaskV1* collective;
    /* Optional logical inspection against supplied placement geometry. */
    int (*inspect)(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageInspectionRequestV1*,
                   EpycBenchPackageJsonV1*) EPYCBENCH_PACKAGE_NOEXCEPT
#ifdef __cplusplus
        = nullptr
#endif
        ;
} EpycBenchPackageTaskV1;

typedef struct EpycBenchPackageImplementationV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*execute)(const EpycBenchPackageCallV1*, EpycBenchPackageWorkV1*)
        EPYCBENCH_PACKAGE_NOEXCEPT;
    /* Required only for a finalizing collective task. Runs on rank zero after
     * every rank completed its scan. Only shared and tuning views are present.
     * The work report describes the complete collective result. */
    int (*finalize)(const EpycBenchPackageCallV1*, const EpycBenchPackageRankReceiptV1*,
                    uint32_t, EpycBenchPackageWorkV1*) EPYCBENCH_PACKAGE_NOEXCEPT
#ifdef __cplusplus
        = nullptr
#endif
        ;
} EpycBenchPackageImplementationV1;

typedef const EpycBenchPackageTaskV1* (*EpycBenchPackageTaskEntryV1)(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;
typedef const EpycBenchPackageImplementationV1* (*EpycBenchPackageImplementationEntryV1)(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;

const EpycBenchPackageTaskV1* epycbench_package_task_v1(void) EPYCBENCH_PACKAGE_NOEXCEPT;
const EpycBenchPackageImplementationV1* epycbench_package_implementation_v1(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;

/* Fresh mapping tasks use these descriptors in place of the prepared-region
 * descriptors above. All mappings belong to the Runner. A first-touch module
 * sees the fresh span only during its callback. The oracle sees it read-only
 * after timing and reports the count of verified pages. An allocation module
 * requests the exact byte extent through the supplied operation once and
 * never receives a pointer to the allocation. */
typedef struct EpycBenchPackageMappingExtentV1 {
    uint64_t bytes;
    uint64_t page_bytes;
    uint64_t pages;
} EpycBenchPackageMappingExtentV1;

typedef struct EpycBenchPackageMappingServiceV1 {
    void* context;
    int (*allocate)(void*, uint64_t) EPYCBENCH_PACKAGE_NOEXCEPT;
} EpycBenchPackageMappingServiceV1;

typedef struct EpycBenchPackageMappingTaskV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*verify)(const EpycBenchPackageMappingExtentV1*, const unsigned char*, uint64_t*)
        EPYCBENCH_PACKAGE_NOEXCEPT;
} EpycBenchPackageMappingTaskV1;

typedef struct EpycBenchPackageMappingImplementationV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*first_touch)(const EpycBenchPackageMappingExtentV1*, volatile unsigned char*)
        EPYCBENCH_PACKAGE_NOEXCEPT;
    int (*allocate)(const EpycBenchPackageMappingExtentV1*, EpycBenchPackageMappingServiceV1)
        EPYCBENCH_PACKAGE_NOEXCEPT;
} EpycBenchPackageMappingImplementationV1;

typedef const EpycBenchPackageMappingTaskV1* (*EpycBenchPackageMappingTaskEntryV1)(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;
typedef const EpycBenchPackageMappingImplementationV1*
    (*EpycBenchPackageMappingImplementationEntryV1)(void) EPYCBENCH_PACKAGE_NOEXCEPT;

const EpycBenchPackageMappingTaskV1* epycbench_package_mapping_task_v1(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;
const EpycBenchPackageMappingImplementationV1* epycbench_package_mapping_implementation_v1(void)
    EPYCBENCH_PACKAGE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
