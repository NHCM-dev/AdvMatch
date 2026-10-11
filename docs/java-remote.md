# Java remote transport and subscriptions

The native engine has one owner. Java callers can use the existing `Exchange` object API through an authenticated TCP endpoint, without loading a native library on the client. CapitalMC provides an independently runnable owner and shared Minecraft account/configuration integration; see its cross-server deployment guide.

```java
var endpoint = new ExchangeServer(owner,
        new ExchangeServer.Options("127.0.0.1", 25570, token, stableExchangeId,
                                   false, 64, 256, 4194304));
var channel = new RemoteChannel(new RemoteOptions("127.0.0.1", 25570, "gateway-one", token));
var remote = new Exchange(channel, callbackExecutor);
// remote.assets(), users(), matching(), contracts(), audit(), persistence(), etc.
```

Imports: `nhcm.advmatch.Exchange`, `nhcm.advmatch.remote.*`. `token` needs at least 32 characters. Gateway instance IDs must be unique and stable; duplicate connected IDs are rejected. `stableExchangeId` identifies the persistent ledger, not a process start. Use `RemoteOptions.expectedExchangeId` to pin it across client restarts; once connected, a channel pins the observed ID for subsequent reconnects.

The SDK preserves native command/response codecs and wire order when admitting commands from a connection. Responses are correlated and may complete in a different order. Commands are bounded by frame and outstanding-request limits. Remote batches are ordered individual admissions, not atomic transactions. Authentication is for trusted application servers: a gateway token grants access to the exchange API, including privileged commands.

Non-loopback plaintext is rejected by default. Supply an `SSLContext` to the endpoint and enable TLS in `RemoteOptions`, or explicitly allow plaintext on a trusted private network. TLS clients validate the certificate and hostname using the default Java trust store. `Options.toString()` and `RemoteOptions.toString()` do not print tokens; authentication/payloads are not logged by the endpoint.

## State subscriptions

```java
var query = new ObservationQuery().withKind(ReadKind.Depth)
        .withSymbolId(symbolId).withSide(OrderSide.Ask).withLimit(64);
try (var subscription = remote.subscriptions().subscribe(query,
        Duration.ofMillis(250), image -> consume(image)))
{
    var initial = subscription.first().join(); // Never wait on a Minecraft server thread.
    var latest = subscription.latest();
}
```

Imports include `nhcm.advmatch.model.*` and `java.time.Duration`. Snapshots are immutable and carry LIVE/STALE/CLOSED state, read page metadata and a reception timestamp. Check state before treating data as current. A NotFound read is a live empty result. Query cursors must be fresh, row limits 1–1024 and requested intervals 100–60000 ms. `hasMore` is preserved; a bounded snapshot is not automatically a complete list.

Identical queries share an owner-side source. Native reads are scheduled on demand, with one query in flight at a time; changes coalesce into at most one queued notification per listener. Sequence-only changes do not resend identical rows. Closing the last listener releases its source. There are at most 4096 source queries per hub and 1024 subscriptions per connection. Consumers must return promptly and marshal game work to the game thread.

This is a sampled state subscription, not a durable event stream. It does not deliver every intermediate execution and cannot replace the financial WAL, retained history or reconciliation reads.

## Disconnection and lifecycle

Subscriptions reconnect automatically; mutations **never replay**. `RemoteFailure.mayHaveExecuted()` distinguishes a request that may have reached the owner from one rejected before transmission. Lost responses to financial changes require reconciliation using durable native request IDs. Remote cancellation does not stop submitted work. Timed-out or slow connections are closed; queues and packet sizes are bounded.

Closing a remote `Exchange` closes its channel and subscriptions. Closing `ExchangeServer` closes clients and their native observation slots, but leaves the owner's `Exchange` running. The application's lifecycle must separately close the native owner. Remote observation slots expose the most recently refreshed immutable client image; they are invalid after disconnect/reconnect.

Run `:advmatch:remoteIntegrationTest` for shared matching, subscription delivery, authentication, observation slots and response-loss checks; `check` includes it. Persistence and native matching integration tests continue to run through the same API.
