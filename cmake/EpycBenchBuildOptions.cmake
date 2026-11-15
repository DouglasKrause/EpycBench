option(EPYCBENCH_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(EPYCBENCH_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(EPYCBENCH_NATIVE_REFERENCE "Enable reference-host native architecture tuning" OFF)

if(EPYCBENCH_ENABLE_ASAN_UBSAN AND EPYCBENCH_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled in the same build")
endif()

add_library(epycbench_build_options INTERFACE)
add_library(EpycBench::build_options ALIAS epycbench_build_options)

if(EPYCBENCH_ENABLE_ASAN_UBSAN)
    target_compile_options(
        epycbench_build_options
        INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined;-fno-omit-frame-pointer>
    )
    target_link_options(
        epycbench_build_options
        INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined;-fno-omit-frame-pointer>
    )
endif()

if(EPYCBENCH_ENABLE_TSAN)
    target_compile_options(
        epycbench_build_options
        INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread;-fno-omit-frame-pointer>
    )
    target_link_options(
        epycbench_build_options
        INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread;-fno-omit-frame-pointer>
    )
endif()

if(EPYCBENCH_NATIVE_REFERENCE)
    target_compile_options(
        epycbench_build_options
        INTERFACE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>
    )
endif()

function(epycbench_enable_build_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot enable build options for unknown target: ${target}")
    endif()

    get_target_property(is_imported "${target}" IMPORTED)
    if(is_imported)
        message(FATAL_ERROR "EpycBench build options must not be applied to imported target: ${target}")
    endif()

    target_link_libraries("${target}" PRIVATE EpycBench::build_options)
endfunction()
