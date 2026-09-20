# AdvMatch

C++20 matching engine project scaffold. The current code establishes the build,
third-party libraries, a C ABI, and an optional JNI entry point. Order book and
matching rules have not been implemented.

## Requirements

- CMake 3.20+, Git, Ninja, and a C++20 compiler (MSVC 2019+, GCC 11+, or Clang 14+).
- Network access on first configure for fmt/spdlog unless they are installed or
  supplied through `FETCHCONTENT_SOURCE_DIR_FMT` and
  `FETCHCONTENT_SOURCE_DIR_SPDLOG`. Disruptor is included in the repository.
- A JDK only when `ADVMATCH_BUILD_JNI=ON`.

## CLion

Open the repository as a CMake project. If it was already open as a plain
directory, right-click the root `CMakeLists.txt` and choose **Load CMake
Project**. Wait for CMake reload and indexing to finish. Otherwise CLion cannot
associate headers with targets or resolve project include paths, even when the
command-line build succeeds. The public headers are explicitly listed in their
CMake targets for IDE indexing.

JNI is enabled by default on a fresh configure when CMake finds a JDK. In an
existing CLion profile that has JNI disabled, add `-DADVMATCH_BUILD_JNI=ON` to
CMake options and set `JAVA_HOME` to the JDK in the profile environment.
On Windows, select the same MinGW installation for CLion and the command-line
build. Mixing CLion's bundled MinGW compiler with MSYS2 UCRT64 runtime DLLs can
make a successfully built executable fail when launched.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Use `release` instead of `debug` for optimized builds. The presets use the
compiler found in the current environment. On Windows, run them in an MSVC
Developer PowerShell or an MSYS2 UCRT64 shell. Keep the compiler and C runtime
consistent across all native libraries and language bindings.

To build JNI, configure with `cmake --preset release -DADVMATCH_BUILD_JNI=ON`
and set `JAVA_HOME` to the JDK if CMake cannot locate it. The C ABI library is
`advmatch` (`advmatch.dll` on Windows); the optional JNI library is
`advmatch_jni`. Install the C ABI with `cmake --install build/release --prefix <dir>`.

## Dependencies

- [SkynetNext/disruptor-cpp](https://github.com/SkynetNext/disruptor-cpp), a
  C++20 port of the LMAX Disruptor with single and multi-producer support.
  Headers and license are vendored in `third_party/disruptor-cpp` from commit
  `473687201f80851ba4fae376c8b075f1ffaf26e0`.
- [{fmt}](https://github.com/fmtlib/fmt) 11.2.0 for formatting.
- [spdlog](https://github.com/gabime/spdlog) 1.15.3 for diagnostics. Keep
  formatting and logging outside latency-critical matching paths.

CMake first uses installed fmt/spdlog packages, then fetches the pinned
versions. Disruptor is built from the vendored headers without a network
request or its upstream global compiler flags, tests, benchmarks, or examples.
The `AdvMatch::disruptor` target is available to the C++ core. Any fmt/spdlog
downloads live under the ignored build directory.

## Language boundary

`include/advmatch/advmatch.h` is the stable C entry point for P/Invoke and
other FFI clients. It currently exports only `advmatch_abi_version()`; no order
submission or matching semantics are claimed yet. When JNI is enabled,
`advmatch_jni` registers `org.advmatch.AdvMatch.abiVersion()` from `JNI_OnLoad`
through `RegisterNatives`; native methods do not rely on `Java_*` symbol names.
New API types should
use fixed-width C integers, opaque handles, and explicit ownership/error codes;
never expose C++ templates or exceptions across the ABI.

Minimal callers are in `examples/java` and `examples/csharp`. For Java, compile
with `javac -d build/java examples/java/org/advmatch/AdvMatch.java`, then run
with `java "-Djava.library.path=build/debug" -cp build/java org.advmatch.AdvMatch`.
For C#, place the native library on the process library search path and run
`dotnet run --project examples/csharp/AdvMatchExample.csproj`. On Windows/MSYS2,
the UCRT64 runtime directory must also be on `PATH`.
