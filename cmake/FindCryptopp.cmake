find_package (PkgConfig)

pkg_check_modules (PC_Cryptopp QUIET cryptopp)

find_path (Cryptopp_INCLUDE_DIR
  NAMES default.h
  PATHS ${PC_Cryptopp_INCLUDE_DIRS}
  PATH_SUFFIXES cryptopp)

find_library (Cryptopp_LIBRARY
  NAMES cryptopp
  PATHS ${PC_Cryptopp_LIBRARY_DIRS})

set (Cryptopp_VERSION ${PC_Cryptopp_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Cryptopp
  FOUND_VAR Cryptopp_FOUND
  REQUIRED_VARS
    Cryptopp_LIBRARY
    Cryptopp_INCLUDE_DIR
  VERSION_VAR Cryptopp_VERSION)

if (Cryptopp_FOUND)
  set (Cryptopp_LIBRARIES ${Cryptopp_LIBRARY})
  set (Cryptopp_INCLUDE_DIRS ${Cryptopp_INCLUDE_DIR})
  set (Cryptopp_DEFINITIONS ${PC_Cryptopp_CFLAGS_OTHER})
endif ()

if (Cryptopp_FOUND AND NOT TARGET Cryptopp::cryptopp)
  add_library (Cryptopp::cryptopp UNKNOWN IMPORTED)

  set_target_properties (Cryptopp::cryptopp PROPERTIES
    IMPORTED_LOCATION "${Cryptopp_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_Cryptopp_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${Cryptopp_INCLUDE_DIR}")
endif ()
