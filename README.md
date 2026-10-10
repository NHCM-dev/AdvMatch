# AdvMatch 1.2.0

English · [简体中文](README.zh-CN.md)

A C++20 exchange engine with C++ and Java SDKs for **spot, linear futures and perpetual contracts**.

## Features

- Single-threaded matching, concurrent command submission and asynchronous results.
- Accounts, balances, reservations, orders, trades and immutable snapshots.
- Cross / isolated margin, leverage, liquidation, funding and cash settlement.
- Stop-loss, take-profit and OCO orders; fees, risk limits and self-trade protection.
- Atomic balance transactions, market halts, conservation audits and WAL recovery.
- Integer monetary units with asset and symbol scales.

## Build

Requires a C++20 compiler, **CMake 3.20+**, Ninja and Git. Java integration requires **JDK 21**. The first build downloads dependencies.

```powershell
.\build.cmd release
```

On Linux / macOS, use `./build.sh release`. Both scripts build and test; release builds also create a ZIP in `build/release/`.

For the Java SDK:

```powershell
.\java\gradlew.bat -p java :advmatch:build :advmatch:runExample
```

The SDK is `java/advmatch/build/libs/advmatch-1.2.0.jar`, including native libraries for the build platform. Use `-PskipNativeBuild` when compatible native libraries are already built.

## Quick start

```java
import nhcm.advmatch.MatchingExchange;
import nhcm.advmatch.model.ExchangeConfig;
import nhcm.advmatch.model.PersistenceConfig;

var config = new ExchangeConfig()
    .withPersistence(new PersistenceConfig().withEnabled(false));
try (var exchange = new MatchingExchange(config)) {
    var user = exchange.users().create().join();
    var account = user.spotAccount().join();
    System.out.println(account.id());
}
```

This example runs in memory. Persistence is enabled by default; set its directory for a durable exchange. Entity managers return futures, and `submit()` accepts commands. Create an order book after creating a symbol to enable matching.

[C++ trading example](examples/cpp-sdk/main.cpp) · [Java trading example](java/advmatch/src/main/nhcm/advmatch/example/SpotExample.java) · [Contract references](examples/contract-reference/main.cpp)

To install the C++ SDK, run `cmake --install build/release --prefix build/sdk`, then link `AdvMatch::sdk` with `find_package(AdvMatch CONFIG REQUIRED)`. Use compatible compiler/runtime settings and deploy the SDK's shared library with your application.

## Integration guide

[English guide](documentation/guide.en.md) · [中文接入说明](documentation/guide.zh-CN.md)

The guide covers trading rules, contract prices, conditional orders, atomic transfers, recovery and benchmarks.

## Compatibility

Update the Java SDK and native libraries together. Command records use schema 2; schema 1 records retain their historical replay behavior. Engines without the same command set cannot read newer operations; use a complete compatible backup for downgrades.

`TradingRules.tickAwarePriceBand` is opt-in. It lets the contract protection band cover at least one price tick on either side of the mark. Changing only this flag preserves open orders, positions and fee settings.

`contracts().configureProtection(symbol, rate)` sets the execution band in millionths: `100000` is 10%, `0` disables it. Explicit market caps, collateral checks and bounded liquidation execution remain active.

Development assistance: GPT-5.6 and GPT-6.1.
