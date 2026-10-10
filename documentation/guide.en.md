# AdvMatch integration guide

[README](../README.md) · [简体中文](guide.zh-CN.md)

## API map

| Manager | Responsibilities |
| --- | --- |
| `assets()`, `users()`, `accounts()` | Asset definitions, identities and balances |
| `symbols()`, `books()`, `orders()` | Markets, depth and orders |
| `positions()`, `contracts()` | Margin, prices, funding, liquidation and settlement |
| `conditionalOrders()` | Stop-loss, take-profit and OCO |
| `rules()`, `trading()`, `audit()` | Fees, risk limits, halts and conservation checks |

C++ futures use `.get()`; Java uses `.join()` or asynchronous chaining. Query results are independent snapshots. A successful submission can represent an unfilled or partially filled order.

## Trading rules

Use exact integer units and the configured asset/symbol scales. Prices and trigger prices must align with `priceTick`; quantities must satisfy `quantityStep`, `minQuantity` and minimum notional. Extra risk limits set to zero are disabled. Existing contract risk checks still apply.

Configure rules through `rules().configure(...)`. Most changes require no open orders or positions. A change limited to `tickAwarePriceBand` is allowed on a trading contract with exposure. Read the current rules and modify only the intended fields.

Spot fees are deducted from the received asset (base for buyers, quote for sellers), rounded down per fill. Contract fees use quote and round up. Rates use millionths; negative rebates are unsupported. Spot fee balances can be queried and withdrawn through the account commands.

Self-trade protection compares exchange **UserId**, using the incoming order's policy: Allow, CancelTaker, CancelMaker or CancelBoth. Default is Allow; contracts also forbid matching the same account. PostOnly rejects immediate execution. FOK simulates execution first; failure neither fills nor cancels makers.

Market orders need liquidity and valid price protection. A contract maker outside the mark-based band may be cancelled during matching. `tickAwarePriceBand` is off by default; enable it to include at least one tick around the mark. It does not change the mark or require consecutive trades to move by one tick.

## Contract reference prices

Bind a matching base/quote spot market with `contracts().bindSpot(...)`, or configure weighted spot/external sources with `configureReference(...)`. Publish external prices with `publishPrice(...)` and periodically call `refresh(...)`.

Refresh updates reference prices and handles configured funding and expiry settlement. It requires valid source data; it does not reconstruct old settlements from today's price. A failed refresh may already have updated prices, so read state before deciding what to do next. See the [reference example](../examples/contract-reference/main.cpp).

## Conditional orders and halts

Conditional orders can trigger from traded price, mark or index; spot supports traded price only. Waiting orders reserve no funds and run risk checks at activation. Inspect their status and rejection reason. OCO cancels the paired waiting order when a child is accepted, which does not guarantee a fill.

Exchange and symbol halts are independent. Halts block new orders, modifications and matching, while cancellation and queries remain available. Halting does not automatically cancel orders or release reservations. Resume evaluates waiting conditions against the latest prices.

## Atomic balances and durability

`BalanceTransaction` accepts 1–256 distinct `(accountId, assetId)` changes. With `conserve=true`, each asset's total change must be zero. With `conserve=false`, external net flows enter the audit ledger; the host application must authorize these operations. All legs are validated before committing.

Atomicity covers the engine only. Item delivery, databases and application logs have separate boundaries. An accepted receipt is not proof of disk durability: submit `Flush` and check `enabled`, `faulted` and `durableThrough` against the receipt sequence. Durable request IDs are deduplicated within the configured retention window; do not blindly repeat an unknown result.

Recovery verification is enabled by default. `audit().report()` checks balances, reservations and economic conservation; use it for maintenance or tests rather than on every trade. Back up the entire persistence directory while the engine is stopped. Keep Java and native versions matched; schema 2 logs require the new engine.

## Tests and benchmarks

```powershell
ctest --preset release
.\java\gradlew.bat -p java :advmatch:build -PskipNativeBuild
.\build\release\advmatch_tps_benchmark.exe --orders 100000 --producers 4 --ring-size 32
```

Java benchmark tasks include `benchmarkTps`, `benchmarkScale`, `benchmarkPerpetual`, `benchmarkLiquidation`, `benchmarkObserve` and `benchmarkBigAllocs`. Pass options with `-PbenchmarkArgs=...`; matching C++ programs are in `build/release/`.
