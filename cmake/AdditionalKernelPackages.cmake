# Preserve the separately checked legacy timing bodies behind package entry points.
function(epycbench_add_algorithm_task stem source selector)
    set(definitions EPYCBENCH_BUNDLED_ALGORITHM EPYCBENCH_ALGORITHM_${selector}
        EPYCBENCH_ALGORITHM_HEADER=\"epycbench/kernel/${source}_task.hpp\")
    if(selector MATCHES "^((MIN|MAX|CHASE4)_U64|VARINT14|(SUM|DOT|NORM_L1)_F64)$")
        list(APPEND definitions EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    endif()
    epycbench_add_bundled_task(${stem} modules/packages/algorithm_oracle.cpp ${definitions})
    target_link_libraries(epycbench_package_${stem}_oracle PRIVATE EpycBench::kernel_task_domain)
    target_link_options(epycbench_package_${stem}_oracle PRIVATE "LINKER:--exclude-libs,ALL,--gc-sections")
endfunction()

function(epycbench_add_algorithm_implementation stem header source selector variant timed_symbol)
    set(family "task-${stem}")
    if(stem STREQUAL "varint14")
        set(family "task-varint14-u64")
    endif()
    epycbench_add_bundled_implementation(${stem}-${variant} modules/${source}.cpp
        modules/packages/algorithm_implementation.cpp ${family} ${timed_symbol}
        EPYCBENCH_BUNDLED_ALGORITHM EPYCBENCH_ALGORITHM_${selector}
        EPYCBENCH_ALGORITHM_HEADER=\"epycbench/kernel/${header}_task.hpp\" ${ARGN})
endfunction()

epycbench_add_algorithm_task(bfs-u64 bfs_u64 BFS_U64)
epycbench_add_algorithm_implementation(bfs-u64 bfs_u64 bfs_u64 BFS_U64 scalar
    epycbench_bfs_u64_body EPYCBENCH_BFS_UNROLL4=0)
epycbench_add_algorithm_implementation(bfs-u64 bfs_u64 bfs_u64 BFS_U64 unroll4
    epycbench_bfs_u64_body EPYCBENCH_BFS_UNROLL4=1)

epycbench_add_algorithm_task(csr-u64 csr_u64 CSR_U64)
epycbench_add_algorithm_implementation(csr-u64 csr_u64 csr_u64 CSR_U64 scalar
    epycbench_csr_u64_body EPYCBENCH_CSR_UNROLL4=0)
epycbench_add_algorithm_implementation(csr-u64 csr_u64 csr_u64 CSR_U64 unroll4
    epycbench_csr_u64_body EPYCBENCH_CSR_UNROLL4=1)

epycbench_add_algorithm_task(compact-u64 compact_u64 COMPACT_U64)
epycbench_add_algorithm_implementation(compact-u64 compact_u64 compact_u64 COMPACT_U64 scalar
    epycbench_compact_u64_body EPYCBENCH_COMPACT_MASK4=0)
epycbench_add_algorithm_implementation(compact-u64 compact_u64 compact_u64 COMPACT_U64 mask4
    epycbench_compact_u64_body EPYCBENCH_COMPACT_MASK4=1)

epycbench_add_algorithm_task(transpose-u64 transpose_u64 TRANSPOSE_U64)
epycbench_add_algorithm_implementation(transpose-u64 transpose_u64 transpose_u64 TRANSPOSE_U64 scalar
    epycbench_transpose_u64_body EPYCBENCH_TRANSPOSE_TILED=0)
epycbench_add_algorithm_implementation(transpose-u64 transpose_u64 transpose_u64 TRANSPOSE_U64 tiled8
    epycbench_transpose_u64_body EPYCBENCH_TRANSPOSE_TILED=1)

epycbench_add_algorithm_task(stencil-u64 stencil_u64 STENCIL_U64)
epycbench_add_algorithm_implementation(stencil-u64 stencil_u64 stencil_u64 STENCIL_U64 row
    epycbench_stencil_u64_body EPYCBENCH_STENCIL_COLUMN=0)
epycbench_add_algorithm_implementation(stencil-u64 stencil_u64 stencil_u64 STENCIL_U64 column
    epycbench_stencil_u64_body EPYCBENCH_STENCIL_COLUMN=1)

epycbench_add_algorithm_task(convolution-u64 convolution_u64 CONVOLUTION_U64)
epycbench_add_algorithm_implementation(convolution-u64 convolution_u64 convolution_u64 CONVOLUTION_U64 scalar
    epycbench_convolution_u64_body EPYCBENCH_CONVOLUTION_BLOCK4=0)
epycbench_add_algorithm_implementation(convolution-u64 convolution_u64 convolution_u64 CONVOLUTION_U64 block4
    epycbench_convolution_u64_body EPYCBENCH_CONVOLUTION_BLOCK4=1)

epycbench_add_algorithm_task(stable-key-sort-u64 stable_key_sort_u64 STABLE_KEY_SORT_U64)
epycbench_add_algorithm_implementation(stable-key-sort-u64 stable_key_sort_u64 stable_key_sort_u64 STABLE_KEY_SORT_U64 scan
    epycbench_stable_key_sort_u64_body EPYCBENCH_STABLE_KEY_SORT_COUNT=0)
epycbench_add_algorithm_implementation(stable-key-sort-u64 stable_key_sort_u64 stable_key_sort_u64 STABLE_KEY_SORT_U64 count
    epycbench_stable_key_sort_u64_body EPYCBENCH_STABLE_KEY_SORT_COUNT=1)

epycbench_add_algorithm_task(search-tree-u64 search_tree_u64 SEARCH_TREE_U64)
epycbench_add_algorithm_implementation(search-tree-u64 search_tree_u64 search_tree_u64 SEARCH_TREE_U64 binary
    epycbench_search_tree_u64_body EPYCBENCH_SEARCH_TREE_STEPS=0)
epycbench_add_algorithm_implementation(search-tree-u64 search_tree_u64 search_tree_u64 SEARCH_TREE_U64 steps
    epycbench_search_tree_u64_body EPYCBENCH_SEARCH_TREE_STEPS=1)

epycbench_add_algorithm_task(serialize-u64 serialize_u64 SERIALIZE_U64)
epycbench_add_algorithm_implementation(serialize-u64 serialize_u64 serialize_u64 SERIALIZE_U64 bytes
    epycbench_serialize_u64_body EPYCBENCH_SERIALIZE_BSWAP=0)
epycbench_add_algorithm_implementation(serialize-u64 serialize_u64 serialize_u64 SERIALIZE_U64 bswap
    epycbench_serialize_u64_body EPYCBENCH_SERIALIZE_BSWAP=1)

epycbench_add_algorithm_task(pack-u4 pack_u4 PACK_U4)
epycbench_add_algorithm_implementation(pack-u4 pack_u4 pack_u4 PACK_U4 pair
    epycbench_pack_u4_body EPYCBENCH_PACK_UNROLL4=0)
epycbench_add_algorithm_implementation(pack-u4 pack_u4 pack_u4 PACK_U4 unroll4
    epycbench_pack_u4_body EPYCBENCH_PACK_UNROLL4=1)

epycbench_add_algorithm_task(merge-u64 merge_u64 MERGE_U64)
epycbench_add_algorithm_implementation(merge-u64 merge_u64 merge_u64_scalar MERGE_U64 scalar
    epycbench_merge_u64_scalar_body)
epycbench_add_algorithm_implementation(merge-u64 merge_u64 merge_u64_run_copy MERGE_U64 run-copy
    epycbench_merge_u64_run_copy_body)

epycbench_add_algorithm_task(prefix-scan-u64 prefix_scan_u64 PREFIX_SCAN_U64)
epycbench_add_algorithm_implementation(prefix-scan-u64 prefix_scan_u64 prefix_scan_u64_scalar PREFIX_SCAN_U64 scalar
    epycbench_prefix_scan_u64_scalar_body)
epycbench_add_algorithm_implementation(prefix-scan-u64 prefix_scan_u64 prefix_scan_u64_grouped4 PREFIX_SCAN_U64 grouped4
    epycbench_prefix_scan_u64_grouped4_body)

epycbench_add_algorithm_task(varint14 varint14 VARINT14)
epycbench_add_algorithm_implementation(varint14 varint14 varint14_branch VARINT14 branch
    epycbench_varint14_branch_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
epycbench_add_algorithm_implementation(varint14 varint14 varint14_masked VARINT14 masked
    epycbench_varint14_masked_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)

epycbench_add_algorithm_task(chase4-u64 chase4_u64 CHASE4_U64)
epycbench_add_algorithm_implementation(chase4-u64 chase4_u64 chase4_u64_serialized CHASE4_U64 serialized
    epycbench_chase4_u64_serialized_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
epycbench_add_algorithm_implementation(chase4-u64 chase4_u64 chase4_u64_interleaved CHASE4_U64 interleaved
    epycbench_chase4_u64_interleaved_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)

epycbench_add_algorithm_task(min-u64 min_u64 MIN_U64)
epycbench_add_algorithm_implementation(min-u64 min_u64 min_u64_scalar MIN_U64 scalar
    epycbench_min_u64_scalar_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
epycbench_add_algorithm_implementation(min-u64 min_u64 min_u64_simd MIN_U64 avx2
    epycbench_min_u64_avx2_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)

epycbench_add_algorithm_task(max-u64 max_u64 MAX_U64)
epycbench_add_algorithm_implementation(max-u64 max_u64 max_u64_scalar MAX_U64 scalar
    epycbench_max_u64_scalar_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
epycbench_add_algorithm_implementation(max-u64 max_u64 max_u64_simd MAX_U64 avx2
    epycbench_max_u64_avx2_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)

epycbench_add_algorithm_task(sum-f64 sum_f64 SUM_F64)
epycbench_add_algorithm_implementation(sum-f64 sum_f64 sum_f64_scalar SUM_F64 scalar
    epycbench_sum_f64_scalar_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_sum-f64-scalar PRIVATE -fno-fast-math -ffp-contract=off)
epycbench_add_algorithm_implementation(sum-f64 sum_f64 sum_f64_simd SUM_F64 avx
    epycbench_sum_f64_avx_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_sum-f64-avx PRIVATE -fno-fast-math -ffp-contract=off)

epycbench_add_algorithm_task(dot-f64 dot_f64 DOT_F64)
epycbench_add_algorithm_implementation(dot-f64 dot_f64 dot_f64_scalar DOT_F64 scalar
    epycbench_dot_f64_scalar_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_dot-f64-scalar PRIVATE -fno-fast-math -ffp-contract=off)
epycbench_add_algorithm_implementation(dot-f64 dot_f64 dot_f64_simd DOT_F64 avx
    epycbench_dot_f64_avx_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_dot-f64-avx PRIVATE -fno-fast-math -ffp-contract=off)

epycbench_add_algorithm_task(norm-l1-f64 norm_l1_f64 NORM_L1_F64)
epycbench_add_algorithm_implementation(norm-l1-f64 norm_l1_f64 norm_l1_f64_scalar NORM_L1_F64 scalar
    epycbench_norm_l1_f64_scalar_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_norm-l1-f64-scalar PRIVATE -fno-fast-math -ffp-contract=off)
epycbench_add_algorithm_implementation(norm-l1-f64 norm_l1_f64 norm_l1_f64_simd NORM_L1_F64 avx
    epycbench_norm_l1_f64_avx_body EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
target_compile_options(epycbench_package_norm-l1-f64-avx PRIVATE -fno-fast-math -ffp-contract=off)

