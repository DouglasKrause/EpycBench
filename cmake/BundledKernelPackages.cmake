include(GNUInstallDirs)

install(TARGETS epycbench RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${PROJECT_SOURCE_DIR}/include/epycbench/kernel/package.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/epycbench/kernel")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/modules/package-example/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/epycbench/examples/kernel-package"
    FILES_MATCHING PATTERN "CMakeLists.txt" PATTERN "README.md" PATTERN "*.hpp" PATTERN "*.cpp" PATTERN "*.yaml")

target_sources(epycbench_kernel_task_domain PRIVATE src/kernel/gather_inspection.cpp)

set(EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/kernel-packages")
set(package_install_directory "${CMAKE_INSTALL_BINDIR}/kernel-packages")
add_custom_target(epycbench_bundled_kernel_packages)
add_dependencies(epycbench epycbench_bundled_kernel_packages)
foreach(demo IN ITEMS demo-bandwidth.scenario.yaml demo-latency.scenario.yaml)
    set(source "${PROJECT_SOURCE_DIR}/examples/scenarios/packages/${demo}")
    configure_file("${source}" "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}/${demo}" COPYONLY)
    install(FILES "${source}" DESTINATION "${package_install_directory}")
endforeach()
function(epycbench_install_package_examples folder schema)
    file(GLOB_RECURSE examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/${folder}/*.yaml")
    foreach(example IN LISTS examples)
        file(STRINGS "${example}" package_schema LIMIT_COUNT 1 REGEX "^schema: ${schema}$")
        if(package_schema)
            cmake_path(RELATIVE_PATH example BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE relative)
            cmake_path(GET relative PARENT_PATH destination)
            install(FILES "${example}" DESTINATION "${CMAKE_INSTALL_DATADIR}/epycbench/${destination}")
        endif()
    endforeach()
endfunction()
epycbench_install_package_examples(scenarios "epycbench[.]scenario/packages-v1")
epycbench_install_package_examples(configuration-spaces "epycbench[.]configuration-space/v3")

function(epycbench_stage_kernel_package_manifest basename)
    set(source "${PROJECT_SOURCE_DIR}/modules/packages/${basename}")
    configure_file("${source}" "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}/${basename}" COPYONLY)
    install(FILES "${source}" DESTINATION "${package_install_directory}")
endfunction()

function(epycbench_add_bundled_task stem source)
    set(oracle "epycbench_package_${stem}_oracle")
    epycbench_add_kernel_module(${oracle} "${source}" "${stem}-oracle"
        "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}")
    target_include_directories(${oracle} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${oracle} PRIVATE ${ARGN})
    add_dependencies(epycbench_bundled_kernel_packages ${oracle})
    install(TARGETS ${oracle} LIBRARY DESTINATION "${package_install_directory}")
    epycbench_stage_kernel_package_manifest("${stem}.task.yaml")
endfunction()

function(epycbench_stage_bundled_implementation module basename family symbol)
    set_property(TARGET ${module} APPEND PROPERTY LINK_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/check-kernel-code.py"
        "${PROJECT_SOURCE_DIR}/tools/kernel-object-audit/kernel_object_audit/elf.py")
    add_custom_command(TARGET ${module} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/check-kernel-code.py"
            --module "$<TARGET_FILE:${module}>" --family "${family}" --symbol "${symbol}"
            --output "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}/${basename}.kernel-check.json"
        COMMENT "Checking ${basename} package implementation" VERBATIM)
    add_dependencies(epycbench_bundled_kernel_packages ${module})
    install(TARGETS ${module} LIBRARY DESTINATION "${package_install_directory}")
    epycbench_stage_kernel_package_manifest("${basename}.kernel.yaml")
endfunction()

function(epycbench_add_bundled_implementation basename source adapter family symbol)
    set(module "epycbench_package_${basename}")
    epycbench_add_kernel_module(${module} "${source}" "${basename}"
        "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}")
    target_sources(${module} PRIVATE "${adapter}")
    target_compile_definitions(${module} PRIVATE ${ARGN})
    target_link_options(${module} PRIVATE "LINKER:-Bsymbolic-functions")
    epycbench_stage_bundled_implementation(${module} ${basename} ${family} ${symbol})
endfunction()

epycbench_add_bundled_task(sum-u64 modules/packages/task_u64_oracle.cpp)
epycbench_add_bundled_task(xor-u64 modules/packages/task_u64_oracle.cpp EPYCBENCH_BUNDLED_XOR_TASK)
epycbench_add_bundled_task(copy-bytes modules/packages/copy_bytes_oracle.cpp)
foreach(variant IN ITEMS single four)
    epycbench_add_bundled_implementation(sum-u64-${variant} modules/sum_u64_${variant}.cpp
        modules/packages/sum_u64_implementation.cpp task-sum-u64 epycbench_kernel_execute_v2)
    epycbench_add_bundled_implementation(xor-u64-${variant} modules/xor_u64_${variant}.cpp
        modules/packages/map_implementation.cpp task-map-xor-u64 epycbench_kernel_execute_v3)
endforeach()

function(epycbench_add_cooperative_bundle stem profile first second)
    epycbench_add_bundled_task(${stem} modules/packages/cooperative_oracle.cpp
        EPYCBENCH_BUNDLED_COOPERATIVE=${profile})
    target_link_libraries(epycbench_package_${stem}_oracle PRIVATE EpycBench::kernel_task_domain)
    target_link_options(epycbench_package_${stem}_oracle PRIVATE "LINKER:--exclude-libs,ALL,--gc-sections")
    foreach(variant IN ITEMS ${first} ${second})
        set(basename "${stem}-${variant}")
        set(module "epycbench_package_${basename}")
        epycbench_add_kernel_module(${module} modules/packages/cooperative_implementation.cpp "${basename}"
            "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}")
        target_include_directories(${module} PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(${module} PRIVATE EPYCBENCH_BUNDLED_COOPERATIVE=${profile}
            EPYCBENCH_BUNDLED_ALTERNATE=$<STREQUAL:${variant},${second}>)
        target_link_libraries(${module} PRIVATE EpycBench::kernel_task_domain)
        target_link_options(${module} PRIVATE "LINKER:--exclude-libs,ALL,--gc-sections")
        epycbench_stage_bundled_implementation(${module} ${basename} task-shared-state
            epycbench_package_execute_cooperative_v1)
    endforeach()
endfunction()

epycbench_add_cooperative_bundle(shared-atomic-counter atomic_counter shared distinct)
epycbench_add_cooperative_bundle(false-sharing-counter false_sharing adjacent separated)
epycbench_add_cooperative_bundle(lock-counter lock_counter tas ttas)
epycbench_add_cooperative_bundle(two-rank-barrier barrier central flags)
epycbench_add_cooperative_bundle(spsc-queue queue direct cached)

epycbench_add_bundled_task(two_rank_histogram modules/packages/two_rank_histogram_oracle.cpp)
target_link_libraries(epycbench_package_two_rank_histogram_oracle PRIVATE EpycBench::kernel_task_domain)
target_link_options(epycbench_package_two_rank_histogram_oracle PRIVATE "LINKER:--exclude-libs,ALL,--gc-sections")
foreach(variant IN ITEMS shared_atomic private_partials)
    set(basename "two_rank_histogram-${variant}")
    set(module "epycbench_package_${basename}")
    epycbench_add_kernel_module(${module} modules/packages/two_rank_histogram_implementation.cpp "${basename}"
        "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}")
    target_include_directories(${module} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${module} PRIVATE EPYCBENCH_HISTOGRAM_PRIVATE=$<STREQUAL:${variant},private_partials>)
    epycbench_stage_bundled_implementation(${module} ${basename} task-histogram-u64
        epycbench_package_histogram_rank_v1)
    add_custom_command(TARGET ${module} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/check-kernel-code.py"
            --module "$<TARGET_FILE:${module}>" --family task-histogram-u64
            --symbol epycbench_package_histogram_finalize_v1
            --output "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}/${basename}-finalize.kernel-check.json"
        COMMENT "Checking ${basename} ordered finalization" VERBATIM)
endforeach()

foreach(isa IN ITEMS avx2 avx512f)
    epycbench_add_bundled_implementation(sum-u64-${isa} modules/sum_u64_${isa}.cpp
        modules/packages/sum_u64_implementation.cpp task-sum-u64 epycbench_sum_u64_${isa}_body)
endforeach()
foreach(variant IN ITEMS scalar avx2)
    epycbench_add_bundled_implementation(copy-bytes-${variant} modules/copy_bytes_${variant}.cpp
        modules/packages/map_implementation.cpp task-copy-bytes epycbench_copy_bytes_${variant}_body
        EPYCBENCH_BUNDLED_COPY_TASK)
endforeach()

# Oracles reuse the independently maintained task generators and references.
set_target_properties(epycbench_core epycbench_kernel_task_domain PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(epycbench_kernel_task_domain PRIVATE -ffunction-sections -fdata-sections)
function(epycbench_add_algorithm_oracle stem task_definition)
    epycbench_add_bundled_task(${stem} modules/packages/algorithm_oracle.cpp ${task_definition})
    target_link_libraries(epycbench_package_${stem}_oracle PRIVATE EpycBench::kernel_task_domain)
    target_link_options(epycbench_package_${stem}_oracle PRIVATE "LINKER:--exclude-libs,ALL,--gc-sections")
endfunction()

epycbench_add_algorithm_oracle(affine-f64 EPYCBENCH_BUNDLED_AFFINE_TASK)
epycbench_add_algorithm_oracle(gather-u64 EPYCBENCH_BUNDLED_GATHER_TASK)
epycbench_add_algorithm_oracle(gather-page-set-u64 EPYCBENCH_BUNDLED_GATHER_TASK)
epycbench_add_algorithm_oracle(gemm-u64 EPYCBENCH_BUNDLED_GEMM_TASK)
epycbench_add_algorithm_oracle(histogram-u64 EPYCBENCH_BUNDLED_HISTOGRAM_TASK)
epycbench_add_algorithm_oracle(scatter-u64 EPYCBENCH_BUNDLED_SCATTER_TASK)

foreach(variant IN ITEMS scalar avx2)
    epycbench_add_bundled_implementation(affine-f64-${variant} modules/affine_f64_${variant}.cpp
        modules/packages/algorithm_implementation.cpp task-map-affine-f64 epycbench_affine32_f64_${variant}_body
        EPYCBENCH_BUNDLED_AFFINE_TASK)
    target_compile_options(epycbench_package_affine-f64-${variant} PRIVATE -fno-fast-math -ffp-contract=off)
endforeach()
foreach(variant IN ITEMS single four)
    epycbench_add_bundled_implementation(gather-u64-${variant} modules/gather_u64_${variant}.cpp
        modules/packages/algorithm_implementation.cpp task-gather-u64 epycbench_gather_u64_${variant}_body
        EPYCBENCH_BUNDLED_GATHER_TASK)
    epycbench_add_bundled_implementation(gather-cache-set-u64-${variant} modules/gather_u64_${variant}.cpp
        modules/packages/algorithm_implementation.cpp task-gather-u64 epycbench_gather_u64_${variant}_body
        EPYCBENCH_BUNDLED_GATHER_TASK EPYCBENCH_GATHER_CACHE_SETS=1)
    epycbench_add_bundled_implementation(gather-matched-pages-u64-${variant} modules/gather_u64_${variant}.cpp
        modules/packages/algorithm_implementation.cpp task-gather-u64 epycbench_gather_u64_${variant}_body
        EPYCBENCH_BUNDLED_GATHER_TASK EPYCBENCH_GATHER_MATCHED_PAGES=1)
endforeach()
foreach(variant IN ITEMS ijk blocked)
    epycbench_add_bundled_implementation(gemm-u64-${variant} modules/gemm_u64_${variant}.cpp
        modules/packages/algorithm_implementation.cpp task-gemm-u64 epycbench_gemm_u64_${variant}_body
        EPYCBENCH_BUNDLED_GEMM_TASK)
endforeach()
foreach(variant IN ITEMS direct hash)
    epycbench_add_bundled_implementation(histogram-u64-${variant} modules/histogram_u64.cpp
        modules/packages/algorithm_implementation.cpp task-histogram-u64 epycbench_histogram_u64_body
        EPYCBENCH_BUNDLED_HISTOGRAM_TASK EPYCBENCH_HISTOGRAM_HASH=$<STREQUAL:${variant},hash>)
endforeach()
foreach(variant IN ITEMS scalar unroll4)
    epycbench_add_bundled_implementation(scatter-u64-${variant} modules/scatter_u64.cpp
        modules/packages/algorithm_implementation.cpp task-scatter-u64 epycbench_scatter_u64_body
        EPYCBENCH_BUNDLED_SCATTER_TASK EPYCBENCH_SCATTER_UNROLL4=$<STREQUAL:${variant},unroll4>)
endforeach()

epycbench_add_bundled_task(bandwidth modules/packages/memory_oracle.cpp)
epycbench_add_bundled_task(latency modules/packages/memory_oracle.cpp EPYCBENCH_PACKAGE_LATENCY)
target_link_libraries(epycbench_package_latency_oracle PRIVATE EpycBench::core)
target_link_options(epycbench_package_latency_oracle PRIVATE "LINKER:--exclude-libs,ALL")
epycbench_add_bundled_implementation(bandwidth-read modules/bandwidth.cpp modules/packages/memory_implementation.cpp
    bandwidth epycbench_kernel_execute_v1)
epycbench_add_bundled_implementation(latency-dependent modules/latency.cpp modules/packages/memory_implementation.cpp
    latency epycbench_kernel_execute_v1 EPYCBENCH_PACKAGE_LATENCY)

epycbench_add_bundled_task(collective-sum-u64 modules/packages/task_u64_oracle.cpp EPYCBENCH_BUNDLED_COLLECTIVE_SUM)
target_sources(epycbench_package_collective-sum-u64_oracle PRIVATE
    modules/packages/collective_sum_oracle.cpp src/kernel/reduction_tree.cpp)
foreach(variant IN ITEMS single four avx2 avx512f)
    epycbench_add_bundled_implementation(collective-sum-u64-${variant} modules/sum_u64_${variant}.cpp
        modules/packages/collective_sum_implementation.cpp task-shared-state
        epycbench_package_execute_collective_sum_v1)
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/AdditionalKernelPackages.cmake")
epycbench_bundle_mapping_tasks()
