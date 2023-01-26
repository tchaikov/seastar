add_library (cxx_modules)
target_sources (cxx_modules
  PUBLIC
    FILE_SET CXX_MODULES
      FILES
        seastar.cc
        seastar-core.cc)
target_compile_features (cxx_modules
  PUBLIC cxx_std_20)

target_link_libraries (cxx_modules
  PUBLIC seastar)
install (
  TARGETS cxx_modules
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  CXX_MODULES_BMI
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cxx/bmi"
    COMPONENT "bmi"
  FILE_SET CXX_MODULES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxx/modules"
    COMPONENT "modules")
export (
  TARGETS cxx_modules
  NAMESPACE Seastar::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/SeastarModules.cmake"
  CXX_MODULES_DIRECTORY "cxx-modules")
