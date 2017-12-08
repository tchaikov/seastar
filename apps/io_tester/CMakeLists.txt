add_executable (io_tester io_tester.cc)
target_link_libraries (io_tester seastar Yaml-cpp::yaml-cpp)
