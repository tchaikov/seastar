find_package (PkgConfig)

pkg_check_modules (PC_Yaml-cpp QUIET yaml-cpp)

find_path (Yaml-cpp_INCLUDE_DIR
  NAMES yaml-cpp/yaml.h
  PATHS ${PC_Yaml-cpp_INCLUDE_DIRS})

find_library (Yaml-cpp_LIBRARY
  NAMES yaml-cpp
  PATHS ${PC_Yaml-cpp_LIBRARY_DIRS})

set (Yaml-cpp_VERSION ${PC_Yaml-cpp_VERSION})

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Yaml-cpp
  FOUND_VAR Yaml-cpp_FOUND
  REQUIRED_VARS
    Yaml-cpp_LIBRARY
    Yaml-cpp_INCLUDE_DIR
  VERSION_VAR Yaml-cpp_VERSION)

if (Yaml-cpp_FOUND)
  set (Yaml-cpp_LIBRARIES ${Yaml-cpp_LIBRARY})
  set (Yaml-cpp_INCLUDE_DIRS ${Yaml-cpp_INCLUDE_DIR})
  set (Yaml-cpp_DEFINITIONS ${PC_Yaml-cpp_CFLAGS_OTHER})
endif ()

if (Yaml-cpp_FOUND AND NOT TARGET Yaml-cpp::yaml-cpp)
  add_library (Yaml-cpp::yaml-cpp UNKNOWN IMPORTED)

  set_target_properties (Yaml-cpp::yaml-cpp PROPERTIES
    IMPORTED_LOCATION "${Yaml-cpp_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${PC_Yaml-cpp_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${Yaml-cpp_INCLUDE_DIR}")
endif ()
