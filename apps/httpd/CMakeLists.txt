seastar_swagger_generate (demo.json httpd_swagger_sources)
add_executable (httpd main.cc "${httpd_swagger_sources}")
target_link_libraries (httpd seastar)
