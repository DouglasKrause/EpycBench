set(EPYCBENCH_GENERATED_SUM_ROOT "${PROJECT_BINARY_DIR}/generated-sum")
set(EPYCBENCH_GENERATED_SUM_COMPILER_ROOT "${PROJECT_BINARY_DIR}/generated-sum-compiler-search")
function(epycbench_add_generated_sum_module module root accumulators compiler_option)
    set(stem "a${accumulators}")
    set(option_args)
    if(NOT compiler_option STREQUAL "")
        set(option_args --compiler-option "${compiler_option}")
    endif()
    if(compiler_option STREQUAL "align32")
        string(APPEND stem "-align32")
    endif()
    set(source "${root}/sources/package/sum-${stem}.cpp")
    set(module_directory "${root}/kernel-packages")
    set(package_manifest "${module_directory}/sum-u64-generated-${stem}.kernel.yaml")
    set(check "${module_directory}/sum-u64-generated-${stem}.kernel-check.json")
    set(manifest "${module_directory}/sum-u64-generated-${stem}.candidate.json")
    add_custom_command(OUTPUT "${source}"
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/generate-sum-candidate.py"
            render --accumulators ${accumulators} ${option_args} --source "${source}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate-sum-candidate.py"
        COMMENT "Generating scalar Sum ${stem}" VERBATIM)
    epycbench_add_kernel_module(${module} "${source}" "sum-u64-generated-${stem}" "${module_directory}")
    set_target_properties(${module} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_include_directories(${module} PRIVATE "${PROJECT_SOURCE_DIR}/modules")
    target_sources(${module} PRIVATE modules/packages/sum_u64_implementation.cpp)
    target_link_options(${module} PRIVATE "LINKER:-Bsymbolic-functions")
    if(compiler_option STREQUAL "align32")
        target_compile_options(${module} PRIVATE -falign-loops=32)
    endif()
    set_property(TARGET ${module} APPEND PROPERTY LINK_DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/generate-sum-candidate.py"
        "${PROJECT_SOURCE_DIR}/tools/generated_sum_compiler_options.py"
        "${PROJECT_SOURCE_DIR}/tools/check-kernel-code.py"
        "${PROJECT_SOURCE_DIR}/tools/kernel-object-audit/kernel_object_audit/elf.py")
    add_custom_command(TARGET ${module} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/check-kernel-code.py"
            --module "$<TARGET_FILE:${module}>" --family task-sum-u64
            --symbol epycbench_kernel_execute_v2 --output "${check}"
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/generate-sum-candidate.py"
            manifest --accumulators ${accumulators} ${option_args} --source "${source}"
            --module "$<TARGET_FILE:${module}>" --check "${check}"
            --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
            --compiler "${CMAKE_CXX_COMPILER}" --compiler-id "${CMAKE_CXX_COMPILER_ID}"
            --compiler-version "${CMAKE_CXX_COMPILER_VERSION}" --compiler-target "${EPYCBENCH_COMPILER_TARGET}"
            --preset "${EPYCBENCH_BUILD_PRESET}" --output "${manifest}" --package-manifest "${package_manifest}"
        BYPRODUCTS "${check}" "${manifest}" ${package_manifest}
        COMMENT "Checking and recording generated Sum ${stem}" VERBATIM)
endfunction()
epycbench_add_generated_sum_module(epycbench_generated_sum_a2_module_package
    "${EPYCBENCH_GENERATED_SUM_ROOT}" 2 "")
epycbench_add_generated_sum_module(epycbench_generated_sum_a8_module_package
    "${EPYCBENCH_GENERATED_SUM_ROOT}" 8 "")
epycbench_add_generated_sum_module(epycbench_generated_sum_search_preset_default_module_package
    "${EPYCBENCH_GENERATED_SUM_COMPILER_ROOT}" 8 preset-default)
epycbench_add_generated_sum_module(epycbench_generated_sum_search_align32_module_package
    "${EPYCBENCH_GENERATED_SUM_COMPILER_ROOT}" 8 align32)
add_custom_target(epycbench_generated_sum_package_baselines
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EPYCBENCH_GENERATED_SUM_ROOT}/kernel-packages"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:epycbench_package_sum-u64_oracle>" "$<TARGET_FILE:epycbench_package_sum-u64-single>"
        "${PROJECT_SOURCE_DIR}/modules/packages/sum-u64.task.yaml"
        "${PROJECT_SOURCE_DIR}/modules/packages/sum-u64-single.kernel.yaml"
        "${PROJECT_BINARY_DIR}/kernel-packages/sum-u64-single.kernel-check.json"
        "${EPYCBENCH_GENERATED_SUM_ROOT}/kernel-packages"
    DEPENDS epycbench_package_sum-u64_oracle epycbench_package_sum-u64-single
    COMMENT "Staging the shared Sum oracle and baseline package" VERBATIM)

add_custom_target(epycbench_generated_sum_candidates DEPENDS
    epycbench_generated_sum_package_baselines epycbench_generated_sum_a2_module_package
    epycbench_generated_sum_a8_module_package)
