cooking_ingredient (Seastar
  SOURCE_DIR $ENV{SEASTAR_SOURCE_DIR}
  COOKING_RECIPE dev
  CMAKE_ARGS
    # Not `lib64`.
    -DCMAKE_INSTALL_LIBDIR=lib
    -DSeastar_APPS=OFF
    -DSeastar_DOCS=OFF
    -DSeastar_DEMOS=OFF
    -DSeastar_DPDK=ON
    -DSeastar_TESTING=OFF)
