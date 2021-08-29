if(archive_INCLUDE_DIR AND archive_LIBRARIES)
  set(archive_FIND_QUIETLY TRUE)
endif()

find_path(archive_INCLUDE_DIR archive.h)
find_library(archive_LIBRARIES archive)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(archive
  DEFAULT_MSG archive_INCLUDE_DIR archive_LIBRARIES)

mark_as_advanced(archive_INCLUDE_DIR archive_LIBRARIES)
