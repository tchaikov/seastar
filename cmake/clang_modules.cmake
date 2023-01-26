if(NOT CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
  string(REGEX MATCH "^([0-9]+)" major_version
    "${CMAKE_CXX_COMPILER_VERSION}")
  find_program(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS
    NAMES
      "clang-scan-deps-${CMAKE_CXX_COMPILER_VERSION}"
      "clang-scan-deps-${major_version}"
      "clang-scan-deps")
endif()

string(CONCAT CMAKE_EXPERIMENTAL_CXX_SCANDEP_SOURCE
  "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}"
  " -format=p1689 "
  " --"
  " <CMAKE_CXX_COMPILER> <DEFINES> <INCLUDES> <FLAGS>"
  " -x c++ <SOURCE> -c -o <OBJECT>"
  " -MT <DYNDEP_FILE>"
  " -MD -MF <DEP_FILE>"
  " > <DYNDEP_FILE>")
set(CMAKE_EXPERIMENTAL_CXX_MODULE_MAP_FORMAT "clang")
set(CMAKE_EXPERIMENTAL_CXX_MODULE_MAP_FLAG "@<MODULE_MAP_FILE>")
# Default to C++ extensions being off. Clang's modules support have trouble
# with extensions right now. Clang cannot find the partition partition with
# -std=gnu++20
set(CMAKE_CXX_EXTENSIONS OFF)
