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
[189/189] Linking CXX executable tests/unit/test_cxx_modules
FAILED: tests/unit/test_cxx_modules
tests/unit/CMakeFiles/test_cxx_modules.dir/test_cxx_modules.cc.o: in function `main':
test_cxx_modules.cc:(.text+0x17): undefined reference to `_ZN7seastarW7seastar25is_abort_on_ebadf_enabledEv'
clang-17: error: linker command failed with exit code 1 (use -v to see invocation)
ninja: build stopped: subcommand failed.

$ nm build/libseastar.a | grep is_abort_on_ebadf_enabled | sort | uniq
0000000000002100 T _ZN7seastar25is_abort_on_ebadf_enabledEv
                 U _ZN7seastar25is_abort_on_ebadf_enabledEv
```

the general structure in CMake is structured following the [blog post from CMake](https://www.kitware.com/import-cmake-c20-modules/). 
and the command line options passed to `clang-scan-deps` are updated according to latest LLVM patches which are still being reviewed.
