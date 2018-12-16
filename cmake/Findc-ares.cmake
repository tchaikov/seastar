find_package(PkgConfig QUIET)

pkg_search_module(_cares
  libcares)

set(_cares_ROOT_HINTS
  ${CARES_ROOT_DIR}
  $ENV{CARES_ROOT_DIR})

find_path(c-ares_INCLUDE_DIR
  NAMES ares_dns.h
  HINTS ${_cares_INCLUDE_DIRS} ${_cares_ROOT_HINTS}
  PATH_SUFFIXES include)

find_library(c-ares_LIBRARY
  NAMES cares
  HINTS ${_cares_LIBRARY_DIRS} ${_cares_ROOT_HINTS}
  PATH_SUFFIXES lib/${CMAKE_LIBRARY_ARCHITECTURE} lib)

set(c-ares_VERSION ${_cares_VERSION})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(c-ares
  REQUIRED_VARS
    c-ares_INCLUDE_DIR
    c-ares_LIBRARY
  VERSION_VAR c-ares_VERSION)

if(c-ares_FOUND AND NOT (TARGET c-ares::c-ares))
  add_library(c-ares::c-ares UNKNOWN IMPORTED)
  set_target_properties(c-ares::c-ares PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${c-ares_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${c-ares_LIBRARY}")
endif()
