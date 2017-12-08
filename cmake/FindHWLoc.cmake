find_package (PkgConfig)

pkg_check_modules (PC_HWLoc QUIET hwloc)

find_path (HWLoc_INCLUDE_DIR
  NAMES hwloc.h
  PATHS ${PC_HWLoc_INCLUDE_DIRS})

find_library (HWLoc_LIBRARY
  NAMES hwloc
  PATHS ${PC_HWLoc_LIBRARY_DIRS})

set (HWLoc_VERSION ${PC_HWLoc_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (HWLoc
  FOUND_VAR HWLoc_FOUND
  REQUIRED_VARS
    HWLoc_LIBRARY
    HWLoc_INCLUDE_DIR
  VERSION_VAR HWLoc_VERSION)

if (HWLoc_FOUND)
  set (HWLoc_LIBRARIES ${HWLoc_LIBRARY})
  set (HWLoc_INCLUDE_DIRS ${HWLoc_INCLUDE_DIR})
  set (HWLoc_DEFINITIONS ${PC_HWLoc_CFLAGS_OTHER})
endif ()

if (HWLoc_FOUND AND NOT TARGET HWLoc::hwloc)
  add_library (HWLoc::hwloc UNKNOWN IMPORTED)

  set_target_properties (HWLoc::hwloc PROPERTIES
    IMPORTED_LOCATION "${HWLoc_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_HWLoc_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${HWLoc_INCLUDE_DIR}")
endif ()
