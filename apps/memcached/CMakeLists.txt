seastar_ragel_generate (ascii.rl memcached_ragel_sources)

add_executable (memcached memcache.cc "${memcached_ragel_sources}")
target_link_libraries (memcached seastar)
