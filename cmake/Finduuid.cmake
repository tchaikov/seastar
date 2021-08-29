if (uuid_INCLUDE_DIR AND uuid_LIBRARIES)
  set (uuid_FIND_QUIETLY TRUE)
endif ()

find_path (uuid_INCLUDE_DIR NAMES uuid/uuid.h)
find_library (uuid_LIBRARIES NAMES uuid)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(uuid DEFAULT_MSG
  uuid_LIBRARIES uuid_INCLUDE_DIR)

mark_as_advanced(uuid_LIBRARIES uuid_INCLUDE_DIR)

if (uuid_FOUND AND NOT (TARGET uuid::uuid))
  add_library (uuid::uuid UNKNOWN IMPORTED)
  set_target_properties (uuid::uuid PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${uuid_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${uuid_LIBRARIES}")
endif()
