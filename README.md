# AdvMatch

C++20 spot matching engine with a single matching writer, a multi-producer
Disruptor command queue, a C ABI scaffold, and an optional JNI entry point.
Matching is implemented in the C++ core. The C/JNI interfaces currently expose
only the ABI version.

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

For MSYS2 MinGW, CMake generates a local `.clangd` file from the selected GCC
toolchain. It gives CLion's Clangd the correct target and GCC private include
directory; `compile_commands.json` also records an explicit C++20 mode. If
CLion still shows old standard-library errors after changing the toolchain,
reload the CMake project so both files are regenerated.

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

## Matching engine

`MatchingEngine` owns orders and symbol books. `SpotOrderBook` maintains price
priority across levels and FIFO within each level. Limit GTC orders can rest;
IOC cancels the unfilled remainder; FOK checks available crossing liquidity
before matching. Market orders require IOC or FOK. Trades execute at the resting
order's price. Increasing an order's quantity moves it to the end of its price
level; reducing quantity keeps its position. Changing price removes the order
from its old level and tries to match it at the new price.

Call `MatchingEngine` directly from one writer thread, or use
`QueuedMatchingEngine` to accept requests from multiple producers and process
them on one worker thread. Register symbol books before starting the queue:

```cpp
#include "advmatch/matching/QueuedMatchingEngine.h"

advmatch::UserManager users;
advmatch::SymbolManager symbols;
const auto buyer = users.createUser();
const auto* symbol = symbols.createSymbol("BTC/USDT", advmatch::SymbolType::Spot, 1, 2);
users.getUser(buyer)->updateBalance(2, 1000);
advmatch::QueuedMatchingEngine engine(users, symbols);
engine.addSpotBook(symbol->symbolId);
engine.start(1024); // power-of-two ring capacity

auto outcome = engine.placeOrder(buyer, symbol->symbolId, advmatch::OrderSide::Bid,
    advmatch::OrderType::Limit, advmatch::TimeInForce::GTC,
    100, 5, 1); // price and quantity are integer units
if (outcome.result)
{
    // outcome.result.orderId and outcome.trades are available here
}
engine.stop();
```

The queued API waits for each command's result. Register symbols and deposit
balances before starting the worker. Do not mutate `UserManager` or
`SymbolManager` concurrently with matching. `queryOrder` returns a copy of the order. User ID,
order ID, symbol, and exact-price lookups use hash tables and are average O(1); best
bid/ask reads are O(1). Adding or removing a price level costs O(log P) for P
active levels, and matching costs at least O(number of fills). These are not
worst-case O(1) guarantees. Accepted terminal orders remain queryable in memory
for the engine's lifetime; production retention or archival is not yet wired.
The spot risk engine reserves quote assets for bids and base assets for asks.
Limit bids reserve `price * quantity`; market bids reserve the cost of the
currently available ask liquidity they can fill. Each fill debits the reserved
assets and credits the counter asset at the resting order's price. Price
improvement, cancelled remainder, and successful reductions release unused
funds. Price and quantity use integer units, and `price * quantity` is the
quote asset amount; asset scale conversion and fees are not implemented.
This is an in-memory matching prototype: account changes and book changes do
not yet have a transactional rollback path for exceptional failures during
settlement, so it is not ready for production custody of funds.

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
