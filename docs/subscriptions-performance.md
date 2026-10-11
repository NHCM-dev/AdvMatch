# Bounded reads and remote admission

[Remote setup](java-remote.md) · [README](../README.md)

Subscriptions are sampled immutable state, not a durable event stream. Consumers must use receipts, persistence and reconciliation for financial decisions.

## Conditional reads

`ReadChanges(queries, knownStamps)` uses wire opcode 11 and returns an observation batch containing only queries that may have changed, plus the current batch stamp. Advance every query's known stamp after a successful batch, even if its image is omitted. Start with a fresh stamp and cursor; honor the configured row, query and aggregate scan budgets. Session changes and persistence faults invalidate old stamps.

User orders, balances, positions and conditional orders use user-scoped invalidation, including both sides of fills and affected contract users. Books use symbol-scoped invalidation. Global configuration, risk and less narrowly classified operations use conservative invalidation. Without the user-order index, invalidation is conservative. Private conditional queries use per-user history/waiting indexes; global queries still have bounded scans.

The Java hub shares identical queries, batches conditional checks and retains unchanged snapshots. It allows at most eight background batches and pauses admission at 128 outstanding native requests. Limits are 16,384 unique queries, 1,048,576 reserved rows and 1,024 rows per query. Batch limits also respect the native observation configuration. Intervals range from 100 ms to 60 s. Callbacks coalesce and must return promptly.

## TCP flow control

Remote command admission rotates between gateways, preserving mutation order per gateway while allowing reads to be reordered. Global limits are 4,096 waiting and 128 running commands; each gateway has 256 waiting and eight running commands. Explicit reads may use at most 32 running slots. Pre-admission overload returns `QUEUE_FULL`; sent requests with lost replies remain uncertain and are never replayed automatically.

Each connection has a separate reply FIFO (256 frames / 8 MiB) and latest-snapshot map (1,024 entries / 8 MiB). Replies receive priority, with an update opportunity after 16 replies. Overflow disconnects a slow reader. Control-message registration is paced to 32 unacknowledged messages per client channel. These limits bound queued state, not total process memory.

Native flushes force pending committed records. If the durable sequence already covers all records, subsequent gateway flushes reuse that boundary without forcing an unchanged WAL. Recovery still forces its repaired journal, and a failed force leaves the session faulted with its previous durability boundary.

`SubscriptionHub.metrics()` and `ExchangeServer.metrics()` expose active/reserved queries, conditional checks, changed snapshots, failures, deferred ticks, queue pressure, rejected commands and coalescing. Counters are process-local and reset on restart.

## Durable gateway request IDs

Zero remains a request without deduplication. Nonzero IDs with the high bit clear retain the legacy globally increasing rule. A high-bit ID opts into a 15-bit namespace and 48-bit counter. The embedding application must durably allocate exclusive namespaces and counter ranges; the SDK does not allocate them automatically. Java represents these IDs as negative `long` values. Treat them as opaque IDs, never require positivity.

Retained requests accept exact duplicate intents and reject conflicting intents. Namespaced requests may arrive out of order while above their namespace's retirement watermark. Eviction raises that watermark; an absent ID at or below it is expired, even if it was never executed. This conservatively prevents re-execution after deduplication retention expires. Checkpoint/WAL replay reconstructs the watermarks. Keep retention large enough for expected in-flight requests and reconciliation; expiration requires review, not a new-ID retry.

For namespaced place/modify/cancel order and conditional commands, execution timestamps are clamped to the existing symbol command/price clocks. Original intent stays in the WAL and deduplication cache. Replay derives the same timestamp from preceding ledger state, without consulting wall time. Legacy request behavior and market-data timestamp validation remain unchanged.

Deploy the Java SDK and native library together on the owner and gateways. Older binaries do not support opcode 11 or namespaced replay semantics. Keep complete compatible backups before upgrading; do not downgrade a ledger containing operations the older engine cannot replay.

## Validation and remaining limits

`check` includes conditional subscription isolation, maker/taker updates, third-party conditional rejection, private conditional indexing, gateway fairness, read reservation, reply coalescing, subscription bursts, TLS, disconnect uncertainty and existing SDK integration tests. Native persistence tests cover namespace interleaving, conflicts, eviction, timestamp reordering, checkpoint and restart.

Admission and subscriptions reduce unnecessary work; matching and financial state still have one owner. Expensive reads, concentrated risk work and history replay can limit throughput. This transport provides no replication, automatic failover or sharded matching. Benchmark representative workloads and recovery on deployment hardware.
