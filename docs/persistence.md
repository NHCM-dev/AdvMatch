# WAL and state snapshots

AdvMatch persists fixed-width binary command/result records and explicit binary state snapshots. Financial objects, links and pools are reconstructed from stable IDs; no raw memory or pointer addresses are saved.

## Recovery and history

Startup loads the required state snapshot, restores managers, balances, reservations, books/FIFO, risk, contracts, conditional trigger priority and request deduplication, then replays only the newer active WAL. Cost is proportional to retained state plus that tail, not total historical commands. `PersistentTradingEngine::replayedCommands()` reports commands executed during recovery.

`FilePersistenceStore` keeps `journal.wal`, `checkpoint.head`, `snapshots/snapshot-{sequence}.bin`, and immutable `archive/segment-{first}-{last}.wal`. Snapshot retention never removes archive segments. Archives preserve the full committed outbox but are not read at startup. Outbox queries skip segments entirely covered by their cursor; requesting full history still reads full history. Archive size grows over time.

Legacy replay-image snapshots are replayed once, converted to state snapshots and archived before pruning active WAL. Legacy root snapshots remain discoverable. Restore a complete compatible backup for a downgrade; old engines cannot read the new state snapshot schema.

## Configuration and boundaries

`PersistenceConfig.checkpointEveryCommands` defaults to **10000**; `0` disables automatic snapshots for SDK users. `retainedCheckpoints` controls snapshot retention. Configure `directory`, record/snapshot size limits, tail recovery and flush policy for deployment storage. CapitalMC exposes its thresholds in its persistence configuration.

A checkpoint forces active WAL, durably archives covered commands/results, publishes a checksummed snapshot and manifest, then prunes covered WAL. Snapshot creation and force are synchronous owner-thread work; larger intervals reduce frequency and increase tail recovery. No replication or automatic failover is provided.

Batch receipts can have `sequence > durableSequence`; `flush()` advances the durable boundary. A physically incomplete final WAL record may be truncated under `TruncateIncomplete`. Complete corruption, unsupported schemas, missing required snapshots/manifests and sequence gaps fail closed. A lost response is an unknown result, not permission to resubmit under a new request ID.

Custom `PersistenceStore` implementations must implement durable `archiveRecords` and cursor-aware `replayArchive`. Implementations lacking durable archives reject checkpoint creation. They must preserve append order and force/publish guarantees. Back up the complete stopped directory, including manifest, snapshots, WAL and archives; never run concurrent writers on one ledger.

## Order construction

`BasicOrderChecker` validates order fields and market-buy overflow without constructing an order. Matching constructs the checked `BasicOrder` directly inside the pool-owned `OrderNode` through `BigAllocs.emplace` and `std::construct_at`. Snapshot restore uses the same direct constructor. The copy constructor path remains available for isolated settlement simulation.
