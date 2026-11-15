include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_NUMA QUIET IMPORTED_TARGET GLOBAL numa)
endif()

find_package_handle_standard_args(
    NUMA
    REQUIRED_VARS PC_NUMA_LINK_LIBRARIES
    VERSION_VAR PC_NUMA_VERSION
)

if(NUMA_FOUND AND NOT TARGET NUMA::NUMA)
    add_library(NUMA::NUMA INTERFACE IMPORTED)
    set_property(TARGET NUMA::NUMA PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::PC_NUMA)
endif()

set(NUMA_VERSION "${PC_NUMA_VERSION}")
