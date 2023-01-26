add_library (seastar_module)
target_sources (seastar_module
  PUBLIC
    FILE_SET CXX_MODULES
      FILES "seastar.cc")
  target_link_libraries (seastar_module
    INTERFACE seastar)
target_compile_features (seastar_module
  PUBLIC cxx_std_20)
install (
  TARGETS seastar_module
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  CXX_MODULES_BMI
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cxx/bmi"
    COMPONENT "bmi"
  FILE_SET CXX_MODULES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxx/modules"
    COMPONENT "modules")
export (
  TARGETS
    seastar_module
  NAMESPACE Seastar::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/SeastarModules.cmake"
  CXX_MODULES_DIRECTORY "cxx-modules")

function (seastar_add_module_partition partition)
  cmake_parse_arguments (parsed_args
    ""
    "MODULE;BASE_DIR"
    "FILES"
    ${ARGN})
  if (parsed_args_MODULE)
    set (module "seastar")
  else ()
    set (module "${parsed_args_MODULE}")
  endif ()
  set (target "module-${module}-${partition}")
  add_library (${target})
  foreach (fn ${parsed_args_FILES})
    get_filename_component(file_ext ${fn} EXT)
    if (NOT file_ext STREQUAL ".hh")
      # TODO: build modules from proper modularized source files in a single
      # pass, instead of building header files separately just for the module
      # interface files (.pcm or .gcm).
      message (SEND_ERROR "Only headers is used for generating C++ modules")
    endif ()
    get_filename_component(file_name ${fn} NAME_WE)
    set (header_src "${parsed_args_BASE_DIR}/${fn}")
    set (module_src
      "${CMAKE_CURRENT_BINARY_DIR}/${module}-${partition}-${file_name}.cc")
    # CMake refuses to compile .hh files as modules, so we need to rename them
    # first. and more importantly, we cannot put "module;" at the top of module
    # source file using macro, as C++ module *containerize* macros, and "module"
    # cannot be declared using a macro.
    add_custom_command (
      OUTPUT ${module_src}
      DEPENDS ${header_src}
      # silence "-Wpragma-once-outside-header"
      COMMAND sed
            -e "s/^#pragma once/module;/"
            -e "s/^SEASTAR_EXPORT_MODULE(\\(.*\\))/export module \\1/"
            -e "s/^SEASTAR_EXPORT/export/"
            "${parsed_args_BASE_DIR}/${fn}" > "${module_src}"
      VERBATIM)
    list (APPEND module_srcs "${module_src}")
  endforeach ()
  target_sources (${target}
    PUBLIC
      FILE_SET "module_${partition}"
      TYPE CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
      FILES "${module_srcs}")
  target_link_libraries (${target}
    PUBLIC seastar)
  export (
    TARGETS ${target}
    NAMESPACE Seastar::
    APPEND FILE "${CMAKE_CURRENT_BINARY_DIR}/SeastarModules.cmake"
    CXX_MODULES_DIRECTORY "cxx-modules")
  target_link_libraries (seastar_module
    PRIVATE ${target})
endfunction ()

seastar_add_module_partition (core
  MODULE seastar
  BASE_DIR ${CMAKE_SOURCE_DIR}/include/seastar/core
  FILES
    abort_on_ebadf.hh
    abort_on_expiry.hh)
