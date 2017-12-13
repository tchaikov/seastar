protobuf_generate_cpp(srcs hdrs metrics2.proto)
add_library(metrics_proto OBJECT ${srcs} ${hdrs})
target_include_directories(metrics_proto PUBLIC ${PROTOBUF_INCLUDE_DIRS})
