list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

find_package(HWLOC REQUIRED)
find_package(NUMA REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFYAML REQUIRED IMPORTED_TARGET "libfyaml>=0.9.6")
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(ftxui 6.1.9 EXACT CONFIG QUIET)
if(NOT TARGET ftxui::component)
    include(FetchContent)
    set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "Install the bundled FTXUI dependency")
    FetchContent_Declare(ftxui
        URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6
        URL_HASH SHA256=7a61f2b5519ce42840da9f035297c5e61e4ed8210b9ed990a0138bf31e48ba59
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    FetchContent_MakeAvailable(ftxui)
endif()
if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
else()
    # Keep the fixed Build manifest dependency vocabulary without requiring a
    # test framework in a production-only source build.
    set(Catch2_VERSION "disabled")
endif()

set(
    EPYCBENCH_REQUIRED_DEPENDENCY_TARGETS
    HWLOC::HWLOC
    NUMA::NUMA
    PkgConfig::LIBFYAML
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    fmt::fmt
)

if(BUILD_TESTING)
    list(APPEND EPYCBENCH_REQUIRED_DEPENDENCY_TARGETS Catch2::Catch2WithMain)
endif()

foreach(dependency_target IN LISTS EPYCBENCH_REQUIRED_DEPENDENCY_TARGETS)
    if(NOT TARGET "${dependency_target}")
        message(FATAL_ERROR "Required dependency target is unavailable: ${dependency_target}")
    endif()

    get_target_property(is_imported "${dependency_target}" IMPORTED)
    if(NOT is_imported)
        message(FATAL_ERROR "Dependency target must be imported: ${dependency_target}")
    endif()
endforeach()

set(EPYCBENCH_DEPENDENCY_HWLOC_VERSION "${HWLOC_VERSION}" CACHE INTERNAL "hwloc version")
set(EPYCBENCH_DEPENDENCY_NUMA_VERSION "${NUMA_VERSION}" CACHE INTERNAL "libnuma version")
set(EPYCBENCH_DEPENDENCY_LIBFYAML_VERSION "${LIBFYAML_VERSION}" CACHE INTERNAL "libfyaml version")
set(EPYCBENCH_DEPENDENCY_CLI11_VERSION "${CLI11_VERSION}" CACHE INTERNAL "CLI11 version")
set(
    EPYCBENCH_DEPENDENCY_NLOHMANN_JSON_VERSION
    "${nlohmann_json_VERSION}"
    CACHE INTERNAL
    "nlohmann/json version"
)
set(EPYCBENCH_DEPENDENCY_FMT_VERSION "${fmt_VERSION}" CACHE INTERNAL "fmt version")
set(
    EPYCBENCH_DEPENDENCY_CATCH2_VERSION
    "${Catch2_VERSION}"
    CACHE INTERNAL
    "Catch2 version"
)

set(
    EPYCBENCH_DEPENDENCY_NAMES
    "hwloc;libnuma;libfyaml;CLI11;nlohmann-json;fmt;Catch2"
    CACHE INTERNAL
    "Dependencies included in EpycBench build metadata"
)

set(
    EPYCBENCH_DEPENDENCY_VERSION_VARIABLES
    EPYCBENCH_DEPENDENCY_HWLOC_VERSION
    EPYCBENCH_DEPENDENCY_NUMA_VERSION
    EPYCBENCH_DEPENDENCY_LIBFYAML_VERSION
    EPYCBENCH_DEPENDENCY_CLI11_VERSION
    EPYCBENCH_DEPENDENCY_NLOHMANN_JSON_VERSION
    EPYCBENCH_DEPENDENCY_FMT_VERSION
    EPYCBENCH_DEPENDENCY_CATCH2_VERSION
)

foreach(version_variable IN LISTS EPYCBENCH_DEPENDENCY_VERSION_VARIABLES)
    if("${${version_variable}}" STREQUAL "")
        message(FATAL_ERROR "Required dependency version is unavailable: ${version_variable}")
    endif()
endforeach()

set(
    EPYCBENCH_DEPENDENCY_RECORDS
    "hwloc=${EPYCBENCH_DEPENDENCY_HWLOC_VERSION};libnuma=${EPYCBENCH_DEPENDENCY_NUMA_VERSION};libfyaml=${EPYCBENCH_DEPENDENCY_LIBFYAML_VERSION};CLI11=${EPYCBENCH_DEPENDENCY_CLI11_VERSION};nlohmann-json=${EPYCBENCH_DEPENDENCY_NLOHMANN_JSON_VERSION};fmt=${EPYCBENCH_DEPENDENCY_FMT_VERSION};Catch2=${EPYCBENCH_DEPENDENCY_CATCH2_VERSION}"
    CACHE INTERNAL
    "Dependency names and versions for the build manifest"
)

message(STATUS "EpycBench dependencies:")
message(STATUS "  hwloc: ${EPYCBENCH_DEPENDENCY_HWLOC_VERSION}")
message(STATUS "  libnuma: ${EPYCBENCH_DEPENDENCY_NUMA_VERSION}")
message(STATUS "  libfyaml: ${EPYCBENCH_DEPENDENCY_LIBFYAML_VERSION}")
message(STATUS "  CLI11: ${EPYCBENCH_DEPENDENCY_CLI11_VERSION}")
message(STATUS "  nlohmann/json: ${EPYCBENCH_DEPENDENCY_NLOHMANN_JSON_VERSION}")
message(STATUS "  fmt: ${EPYCBENCH_DEPENDENCY_FMT_VERSION}")
message(STATUS "  Catch2: ${EPYCBENCH_DEPENDENCY_CATCH2_VERSION}")
