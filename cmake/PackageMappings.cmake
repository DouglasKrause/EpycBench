target_sources(epycbench_package_execution PRIVATE src/platform_linux/package_mapping.cpp)

function(epycbench_bundle_mapping_tasks)
    epycbench_add_bundled_task(first-touch modules/packages/fresh_mapping_oracle.cpp)
    epycbench_add_bundled_task(allocation-latency modules/packages/fresh_mapping_oracle.cpp EPYCBENCH_ALLOCATION_MAPPING)
    foreach(stem IN ITEMS first-touch-byte-per-page allocation-latency-anonymous-mmap)
        set(module "epycbench_package_${stem}")
        epycbench_add_kernel_module(${module} modules/packages/fresh_mapping_implementation.cpp "${stem}"
            "${EPYCBENCH_BUNDLED_PACKAGE_DIRECTORY}")
        if(stem STREQUAL "first-touch-byte-per-page")
            epycbench_stage_bundled_implementation(${module} ${stem} first-touch
                epycbench_package_touch_fresh_pages)
        else()
            target_compile_definitions(${module} PRIVATE EPYCBENCH_ALLOCATION_MAPPING)
            epycbench_stage_bundled_implementation(${module} ${stem} allocation-mapping
                epycbench_package_request_anonymous_mapping)
        endif()
    endforeach()
endfunction()

