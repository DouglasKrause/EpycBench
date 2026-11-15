cmake_minimum_required(VERSION 3.25)
project(EpycBenchExampleKernel LANGUAGES CXX)

set(EPYCBENCH_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH
    "Directory containing epycbench/kernel/plugin.h")
if(NOT EXISTS "${EPYCBENCH_INCLUDE_DIR}/epycbench/kernel/plugin.h")
    message(FATAL_ERROR "Set EPYCBENCH_INCLUDE_DIR to the EpycBench include directory")
endif()

add_library(epycbench_example MODULE kernel.cpp)
target_compile_features(epycbench_example PRIVATE cxx_std_23)
target_include_directories(epycbench_example PRIVATE "${EPYCBENCH_INCLUDE_DIR}")
set_target_properties(epycbench_example PROPERTIES PREFIX "" OUTPUT_NAME "example-bandwidth")
target_compile_options(epycbench_example PRIVATE
    "$<$<CXX_COMPILER_ID:GNU>:-fno-lto;-fno-tree-loop-vectorize;-fno-tree-slp-vectorize;-fno-unroll-loops>"
    "$<$<CXX_COMPILER_ID:Clang>:-fno-lto;-fno-vectorize;-fno-slp-vectorize;-fno-unroll-loops>")
target_compile_definitions(epycbench_example PRIVATE
    EPYCBENCH_PLUGIN_BUILD_INFO="standalone-example-${CMAKE_BUILD_TYPE}-C++23")

# A full source checkout supplies the same permissive checker used by the two
# standard modules. A header-only SDK remains sufficient to compile the module.
set(EPYCBENCH_KERNEL_CHECKER "${EPYCBENCH_INCLUDE_DIR}/../tools/check-kernel-code.py")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND EXISTS "${EPYCBENCH_KERNEL_CHECKER}")
    set_property(TARGET epycbench_example APPEND PROPERTY LINK_DEPENDS "${EPYCBENCH_KERNEL_CHECKER}")
    add_custom_command(TARGET epycbench_example POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -B "${EPYCBENCH_KERNEL_CHECKER}"
            --module "$<TARGET_FILE:epycbench_example>" --family bandwidth
            --symbol epycbench_kernel_execute_v1
            --output "${CMAKE_CURRENT_BINARY_DIR}/example-bandwidth.kernel-check.json"
        COMMENT "Checking example bandwidth module" VERBATIM)
else()
    message(STATUS "Basic kernel inspection unavailable; the example can still be built")
endif()
