C++20 Modules Support
=====================

> **Warning**
> The C++20 support is still under development! and you would need to compile the used tools
> and compiler from source.

C++20 modules support needs following requisites:

* ninja 1.11
* CMake 3.25
* [Clang with p1689r5 support](https://github.com/tchaikov/llvm-project/releases/tag/p1689r5-1)
* [GCC with p1685p5 support](https://patchwork.sourceware.org/project/gcc/list/?series=16313)

make sure the building tools and compiler are in place:
```console
$ ninja --version
1.12.0.git
$ cmake --version
cmake version 3.25.20230127-g7ac338b

CMake suite maintained and supported by Kitware (kitware.com/cmake).
$ clang++ --version
clang version 17.0.0 (git@github.com:llvm/llvm-project.git d214ee93e076eec0c097f18fa48cb6520f0f911b)
Target: x86_64-unknown-linux-gnu
Thread model: posix
InstalledDir: /home/kefu/.local/bin
```

configure the building system and build the C++20 modules:
``` console
$ cmake -DCMAKE_CXX_COMPILER=clang++ \
        -DSeastar_CXX_MODULES=ON \
        -GNinja -B build
$ cmake --build build --target test_cxx_modules
...
FAILED: tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o 
/home/kefu/.local/bin/clang++ -DFMT_LOCALE -DFMT_SHARED -DSEASTAR_API_LEVEL=6 -DSEASTAR_DEBUG -DSEASTAR_DEBUG_SHARED_PTR -DSEASTAR_DEFAULT_ALLOCATOR -DSEASTAR_SCHEDULING_GROUPS_COUNT=16 -DSEASTAR_SHUFFLE_TASK_QUEUE -DSEASTAR_TYPE_ERASE_MORE -I/home/kefu/dev/seastar/include -I/home/kefu/dev/seastar/build/gen/include -g -std=c++20 -U_FORTIFY_SOURCE -DSEASTAR_SSTRING -Wno-error=unused-result "-Wno-error=#warnings" -fstack-clash-protection -MD -MT tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o -MF tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o.d @tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o.modmap -o tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o -c /home/kefu/dev/seastar/tests/unit/test_cxx_modules.cc
In module 'seastar:core':
/home/kefu/dev/seastar/include/seastar/core/future.hh:1914:25: error: cannot befriend target of using declaration
    friend future<U...> make_ready_future(A&&... value) noexcept;
                        ^
...
/home/kefu/dev/seastar/tests/unit/test_cxx_modules.cc:26:23: note: while substituting deduced template arguments into function template 'function' [with _Functor = (lambda at /home/kefu/dev/seastar/tests/unit/test_cxx_modules.cc:26:23), _Constraints = (no value)]
  app.run(argc, argv, [] () -> seastar::future<> {
                      ^
/home/kefu/dev/seastar/include/seastar/core/future.hh:2062:14: note: target of using declaration
future<T...> make_ready_future(A&&... value) noexcept {
             ^
/home/kefu/dev/seastar/modules/seastar-core.cc:292:22: note: using declaration
    using ::seastar::make_ready_future;
                     ^
```

the general structure in CMake is structured following the [blog post from CMake](https://www.kitware.com/import-cmake-c20-modules/). 
and the command line options passed to `clang-scan-deps` are updated according to latest LLVM patches which are still being reviewed.
