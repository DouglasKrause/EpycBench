add_library(epycbench_warnings INTERFACE)
add_library(EpycBench::warnings ALIAS epycbench_warnings)

target_compile_options(
    epycbench_warnings
    INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
            -Wundef
        >
)

function(epycbench_enable_warnings target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot enable warnings for unknown target: ${target}")
    endif()

    get_target_property(is_imported "${target}" IMPORTED)
    if(is_imported)
        message(FATAL_ERROR "EpycBench warning policy must not be applied to imported target: ${target}")
    endif()

    target_link_libraries("${target}" PRIVATE EpycBench::warnings)
endfunction()
