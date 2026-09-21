# AdvMatch

C++20 spot matching engine with a reusable single-thread consumer built on a
multi-producer Disruptor queue, a C ABI scaffold, and an optional JNI entry point.
Matching is implemented in the C++ core. The C/JNI interfaces currently expose
only the ABI version.

## Requirements

- CMake 3.20+, Git, Ninja, and a C++20 compiler (MSVC 2019+, GCC 11+, or Clang 14+).
- Network access on first configure for fmt, spdlog, Boost.Container,
  unordered_dense, and Abseil unless installed packages or FetchContent source
  overrides are supplied. Disruptor is included in the repository.
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
- [Boost.Container](https://github.com/boostorg/container) 1.92.0 for inline
  balance storage.
- [unordered_dense](https://github.com/martinus/unordered_dense) 5.0.1 for
  large balance sets.
- [Abseil](https://github.com/abseil/abseil-cpp) 20250814.2, exposing
  `flat_hash_map` as an alternative flat hash container.

CMake first uses installed packages, then fetches pinned versions. Disruptor is
built from the vendored headers without a network request or its upstream
global compiler flags, tests, benchmarks, or examples. The
`AdvMatch::disruptor` target is available to the C++ core. Downloads live under
the ignored build directory.

`advmatch/util/Containers.h` provides the project aliases `SmallVector`,
`DenseMap`, `DenseSet`, `FlatHashMap`, and `NodeHashMap`. Managers use
`NodeHashMap` for objects whose addresses are returned to callers, while their
secondary indexes use contiguous dense containers. A `User` currently keeps its first six asset
balances inside the object through `SmallVector`; exceeding the configured
inline capacity migrates the entries once to `DenseMap`. Existing balance APIs
do not expose the selected storage mode. Balance element addresses are not
stable across insertion, deletion, promotion, or hash-table reallocation. Code
must retain `AssetId` rather than an `AssetBalance` pointer or reference and
perform another lookup after a balance mutation.

`MatchingEngine` stores monotonically numbered orders and books in vectors and
uses `id - 1` as a direct index. The pointed-to `OrderNode` and `OrderBook`
objects remain stable when those vectors grow. `SpotOrderBook` keeps price
levels in `std::map` for ordered traversal and stable node addresses, with a
`DenseMap` secondary index for exact-price lookup. The spot risk hold table
keeps `std::unordered_map`: its settlement workload erases entries frequently,
and local A/B measurements showed that avoiding dense-vector relocation was
faster for this access pattern.

## Matching engine

`MatchingEngine` owns orders and symbol books. `SpotOrderBook` maintains price
priority across levels and FIFO within each level. Limit GTC orders can rest;
IOC cancels the unfilled remainder; FOK checks available crossing liquidity
before matching. Market orders require IOC or FOK. Trades execute at the resting
order's price. Increasing an order's quantity moves it to the end of its price
level; reducing quantity keeps its position. Changing price removes the order
from its old level and tries to match it at the new price.

Call `MatchingEngine` directly from one writer thread. For multiple producer
threads, derive from `consuming::SingleThreadConsumer<Command, Response>` and
own the matching engine in the derived class. Register books before starting:

```cpp
#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"

struct PlaceCommand
{
    advmatch::UserId userId;
    advmatch::SymbolId symbolId;
    advmatch::Price price;
    advmatch::Quantity quantity;
};

class PlaceConsumer final : public advmatch::consuming::SingleThreadConsumer<PlaceCommand, advmatch::PlaceOrderResult>
{
    advmatch::MatchingEngine engine;

protected:
    advmatch::PlaceOrderResult consume(PlaceCommand&& command) override
    {
        std::vector<advmatch::Trade> trades;
        return this->engine.placeOrder(command.userId, command.symbolId, advmatch::OrderSide::Bid,
            advmatch::OrderType::Limit, advmatch::TimeInForce::GTC, command.price, command.quantity, 1, trades);
    }

public:
    PlaceConsumer(advmatch::UserManager& users, advmatch::SymbolManager& symbols)
        : engine(users, symbols)
    {}

    ~PlaceConsumer() override
    {
        stop();
    }

    bool addSpotBook(advmatch::SymbolId symbolId)
    {
        return this->engine.addSpotBook(symbolId);
    }
};

advmatch::UserManager users;
advmatch::SymbolManager symbols;
const auto buyer = users.createUser();
const auto* symbol = symbols.createSymbol("BTC/USDT", advmatch::SymbolType::Spot, 1, 2);
users.getUser(buyer)->updateBalance(2, 1000);
PlaceConsumer engine(users, symbols);
engine.addSpotBook(symbol->symbolId);
engine.start(1024); // power-of-two ring capacity

std::future<advmatch::PlaceOrderResult> response = engine.post({buyer, symbol->symbolId, 100, 5});
auto result = response.get();
if (result)
{
    // result.orderId is available here
}
engine.stop();
```

`post()` is safe to call from multiple producer threads and returns immediately
with a `std::future<Response>`. The worker invokes `consume()` in queue order;
exceptions are delivered through the future. `stop()` drains accepted commands
and waits for the worker. Call `stop()` in the derived destructor, before its
matching state is destroyed. The consumer cannot restart after stopping, and
its worker thread cannot call `post()` or `stop()` on itself. Register symbols
and deposit balances before starting the worker. Do not mutate `UserManager`
or `SymbolManager` concurrently with matching. `queryOrder` returns a copy of the order. User ID,
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

## TPS benchmark

Build and run the standalone benchmark in Release mode:

```powershell
cmake --build build/release --target advmatch_tps_benchmark
.\build\release\advmatch_tps_benchmark.exe --orders 100000 --producers 4 --batch-size 256 --ring-size 1024
```

`--batch-size` controls how many asynchronous responses each producer keeps in
flight before waiting. `--publish-batch` controls how many adjacent Disruptor
entries are claimed and published with one cursor operation; omission selects a
bounded value from the ring size and producer count. For example:

```powershell
.\build\release\advmatch_tps_benchmark.exe --orders 1000000 --producers 8 --batch-size 256 --publish-batch 16 --ring-size 512
```

More producer threads do not imply more matching throughput because all of them
contend for one multi-producer cursor and feed one matching thread. Leave at
least one logical CPU for the consumer and usually start with two to four
producer threads. Ring capacity absorbs bursts; it does not increase steady
state processing capacity. Start with 32-256 entries for a few producers and
512-1024 for a larger ingress pool, then measure on the deployment hardware.
Avoid assigning one producer thread per connection. Network event-loop threads
should aggregate client commands before posting them to the matching queue.

The program first posts one resting sell order per requested trade. Preloading
is reported separately and excluded from TPS. During the timed phase, producer
threads post limit IOC buys through `SingleThreadConsumer`; its one worker runs
matching, risk checks, and settlement. Each producer awaits responses in
bounded batches. TPS is completed trades divided by wall time from producer
release to the last response. The program checks every fill and final balances
before reporting success. Command allocation, futures, and producer contention
are included; process startup, preload, and shutdown are excluded. Run it
several times for a representative figure. The engine retains all orders in
memory, so larger `--orders` values also increase memory use.

## Scale benchmark

The scale benchmark has a small default smoke profile and an explicit target
profile matching 3 million users, 10 million asset-balance records, 100,000
spot books, and 4 million resting orders:

```powershell
cmake --build build/release --target advmatch_scale_benchmark
.\build\release\advmatch_scale_benchmark.exe --profile target
```

The current model has no separate account object, so an asset-balance record
is the closest available account proxy. The program verifies those counts
before measuring. Each of three default rounds posts 100,000 IOC buys and
100,000 replacement asks through `SingleThreadConsumer`, leaving the 4 million
resting orders in place. It reports trade and command TPS, submission-to-consume
latency percentiles, process memory after each round, and final counts. Setup
time is separate. `--cycles`, `--rounds`, `--producers`, `--batch-size`, and
`--ring-size` are configurable. The program checks available physical memory
against a rough estimate before the target run; `--force` bypasses that check.
Repeated rounds expose memory growth because terminal orders are retained.
A finite run does not establish 24/7 durability or pause-free operation.

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
