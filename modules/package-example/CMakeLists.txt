cmake_minimum_required(VERSION 3.25)
project(EpycBenchPackageExample LANGUAGES CXX)

set(EPYCBENCH_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH
    "Directory containing epycbench/kernel/package.h")
if(NOT EXISTS "${EPYCBENCH_INCLUDE_DIR}/epycbench/kernel/package.h")
    message(FATAL_ERROR "Set EPYCBENCH_INCLUDE_DIR to the EpycBench include directory")
endif()

set(EPYCBENCH_EXAMPLE_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package" CACHE PATH
    "Directory to receive the runnable example package")

function(add_package_binary suffix source)
    add_library(epycbench_package_example_${suffix} MODULE ${source})
    target_include_directories(epycbench_package_example_${suffix} PRIVATE "${EPYCBENCH_INCLUDE_DIR}")
    target_compile_features(epycbench_package_example_${suffix} PRIVATE cxx_std_23)
    set_target_properties(epycbench_package_example_${suffix} PROPERTIES
        PREFIX "" OUTPUT_NAME "${suffix}" LIBRARY_OUTPUT_DIRECTORY "${EPYCBENCH_EXAMPLE_PACKAGE_DIRECTORY}")
    if(COMMAND epycbench_enable_warnings)
        epycbench_enable_warnings(epycbench_package_example_${suffix})
        epycbench_enable_build_options(epycbench_package_example_${suffix})
    endif()
endfunction()

add_package_binary(byte-transform-task task.cpp)
add_package_binary(byte-transform-scalar implementation.cpp)
add_package_binary(byte-transform-blocked implementation.cpp)
target_compile_definitions(epycbench_package_example_byte-transform-blocked PRIVATE EXAMPLE_BLOCKED)

foreach(manifest IN ITEMS byte-transform.task.yaml scalar.kernel.yaml blocked.kernel.yaml)
    configure_file("${manifest}" "${EPYCBENCH_EXAMPLE_PACKAGE_DIRECTORY}/${manifest}" COPYONLY)
endforeach()
