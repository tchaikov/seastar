if (uuid_INCLUDE_DIR AND uuid_LIBRARIES)
  set (uuid_FIND_QUIETLY TRUE)
endif ()

find_path (aio_INCLUDE_DIR libaio.h)

find_library (aio_LIBRARIES aio)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (aio DEFAULT_MSG
  aio_LIBRARIES aio_INCLUDE_DIR)

mark_as_advanced (aio_INCLUDE_DIR aio_LIBRARIES)

if (aio_FOUND AND NOT (TARGET aio::aio))
  add_library (aio::aio UNKNOWN IMPORTED)
  set_target_properties (aio::aio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${aio_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${aio_LIBRARIES}")
endif()
