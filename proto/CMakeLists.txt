protobuf_generate_cpp (proto_sources proto_headers
  metrics2.proto)

set (SEASTAR_PROTO_SOURCES "${proto_sources}" PARENT_SCOPE)
set (SEASTAR_PROTO_HEADERS "${proto_headers}" PARENT_SCOPE)
set (SEASTAR_PROTO_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_target (seastar-protobuf
  DEPENDS
    ${proto_sources}
    ${proto_headers})

add_dependencies (seastar seastar-protobuf)
