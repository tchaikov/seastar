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
[155/212] Building CXX object modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o
FAILED: modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o modules/CMakeFiles/seastar_module.dir/seastar-core.file_impl.pcm
FAILED: modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o modules/CMakeFiles/seastar_module.dir/seastar-core.file_impl.pcm
/home/kefu/.local/bin/clang++ -DFMT_LOCALE -DFMT_SHARED -DSEASTAR_API_LEVEL=6 -DSEASTAR_DEBUG -DSEASTAR_DEBUG_SHARED_PTR -DSEASTAR_DEFAULT_ALLOCATOR -DSEASTAR_DEFERRED_ACTION_REQUIRE_NOEXCEPT -DSEASTAR_HAS_MEMBARRIER -DSEASTAR_HAVE_ASAN_FIBER_SUPPORT -DSEASTAR_HAVE_HWLOC -DSEASTAR_HAVE_LZ4_COMPRESS_DEFAULT -DSEASTAR_HAVE_NUMA -DSEASTAR_HAVE_URING -DSEASTAR_SCHEDULING_GROUPS_COUNT=16 -DSEASTAR_SHUFFLE_TASK_QUEUE -DSEASTAR_THREAD_STACK_GUARDS -DSEASTAR_TYPE_ERASE_MORE -I/home/kefu/dev/seastar/include/seastar/core -I/home/kefu/dev/seastar/src/core -I/home/kefu/dev/seastar/include/seastar/core/internal -I/home/kefu/dev/seastar/include/seastar/net -I/home/kefu/dev/seastar/src/util -I/home/kefu/dev/seastar/include/seastar/util -I/home/kefu/dev/seastar/include/seastar/http -I/home/kefu/dev/seastar/src/http -I/home/kefu/dev/seastar/include -I/home/kefu/dev/seastar/build/gen/include -I/home/kefu/dev/seastar/src -g -std=c++20 -U_FORTIFY_SOURCE -DSEASTAR_SSTRING -Wno-error=unused-result "-Wno-error=#warnings" -fstack-clash-protection -fvisibility=hidden -UNDEBUG -Wall -Werror -Wno-array-bounds -Wno-error=deprecated-declarations -gz -MD -MT modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o -MF modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o.d @modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o.modmap -o modules/CMakeFiles/seastar_module.dir/src/core/file-impl.hh.cc.o -c /home/kefu/dev/seastar/build/modules/src/core/file-impl.hh.cc
In file included from /home/kefu/dev/seastar/build/modules/src/core/file-impl.hh.cc:25:
In file included from /home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/deque:69:
/home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/bits/deque.tcc:1307:5: error: inline declaration of '__lexicographical_compare_aux1' follows non-inline definition
    __lexicographical_compare_aux1(
    ^
/home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/bits/deque.tcc:1307:5: note: previous definition is here
    __lexicographical_compare_aux1(
    ^
In file included from /home/kefu/dev/seastar/build/modules/src/core/file-impl.hh.cc:25:
In file included from /home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/deque:69:
/home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/bits/deque.tcc:1316:5: error: inline declaration of '__lexicographical_compare_aux1' follows non-inline definition
    __lexicographical_compare_aux1(_Tp1* __first1, _Tp1* __last1,
    ^
/home/kefu/.local/bin/../lib/gcc/x86_64-pc-linux-gnu/13.0.1/../../../../include/c++/13.0.1/bits/deque.tcc:1316:5: note: previous definition is here
    __lexicographical_compare_aux1(_Tp1* __first1, _Tp1* __last1,
    ^
2 errors generated.
```

the general structure in CMake is structured following the [blog post from CMake](https://www.kitware.com/import-cmake-c20-modules/). 
and the command line options passed to `clang-scan-deps` are updated according to latest LLVM patches which are still being reviewed.
