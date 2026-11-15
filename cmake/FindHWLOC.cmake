include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_HWLOC QUIET IMPORTED_TARGET GLOBAL hwloc)
endif()

find_package_handle_standard_args(
    HWLOC
    REQUIRED_VARS PC_HWLOC_LINK_LIBRARIES
    VERSION_VAR PC_HWLOC_VERSION
)

if(HWLOC_FOUND AND NOT TARGET HWLOC::HWLOC)
    add_library(HWLOC::HWLOC INTERFACE IMPORTED)
    set_property(TARGET HWLOC::HWLOC PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::PC_HWLOC)
endif()

set(HWLOC_VERSION "${PC_HWLOC_VERSION}")
