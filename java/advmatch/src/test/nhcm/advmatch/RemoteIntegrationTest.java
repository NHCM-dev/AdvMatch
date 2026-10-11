package nhcm.advmatch;

import nhcm.advmatch.command.account.UpdateBalance;
import nhcm.advmatch.command.persistence.Flush;
import nhcm.advmatch.model.*;
import nhcm.advmatch.remote.*;

import java.nio.file.Files;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;

public class RemoteIntegrationTest
{
    private static final String TOKEN = "integration-token-012345678901234567890123456789";

    private static void require(boolean value, String message)
    {
        if (!value) throw new AssertionError(message);
    }

    private static void await(BooleanSupplier ready, String message) throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (!ready.getAsBoolean())
        {
            if (System.nanoTime() > deadline) throw new AssertionError(message);
            Thread.sleep(20);
        }
    }

    public static void main(String[] args) throws Exception
    {
        var directory = Files.createTempDirectory("advmatch-remote-");
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(true).withDirectory(directory.toString()));
        String identity = UUID.randomUUID().toString();
        try (var owner = new Exchange(config))
        {
            var options = new ExchangeServer.Options("127.0.0.1", 0, TOKEN, identity, false, 8, 64, 4194304);
            var server = new ExchangeServer(owner, options);
            try (var a = new Exchange(new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "a", TOKEN)), ForkJoinPool.commonPool());
                 var channelB = new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "b", TOKEN));
                 var b = new Exchange(channelB, ForkJoinPool.commonPool()))
            {
                var base = a.assets().create("remote_base", "Base", 0).join();
                var quote = a.assets().create("remote_quote", "Quote", 0).join();
                var maker = a.users().create().join();
                var taker = b.users().create().join();
                var makerAccount = maker.spotAccount().join();
                var takerAccount = taker.spotAccount().join();
                require(a.submit(new UpdateBalance(makerAccount.id(), base.id(), 100, 11)).join().result().accepted(), "maker funding");
                require(b.submit(new UpdateBalance(takerAccount.id(), quote.id(), 1000, 12)).join().result().accepted(), "taker funding");
                var market = a.symbols().create("BASE/QUOTE", SymbolType.Spot, base, quote).join();
                a.books().addSpot(market, OrderBookImplementation.Radix).join();
                var query = new ObservationQuery().withKind(ReadKind.Depth).withSymbolId(market.id()).withSide(OrderSide.Ask).withLimit(64);
                try (var subA = a.subscriptions().subscribe(query, Duration.ofMillis(100), ignored -> {});
                     var subB = b.subscriptions().subscribe(query, Duration.ofMillis(100), ignored -> {}))
                {
                    subA.first().get(10, TimeUnit.SECONDS);
                    subB.first().get(10, TimeUnit.SECONDS);
                    require(owner.subscriptions().activeQueries() == 1, "two clients must share one source query");
                    a.matching().place(maker, market, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 10, 8, 1, null).join();
                    await(() -> !subA.latest().result().typed(DepthView.class).values().isEmpty(), "ask subscription update");
                    b.matching().place(taker, b.symbols().get(market.id()), OrderSide.Bid, OrderType.Market, TimeInForce.IOC, 0, 3, 2, null).join();
                    await(() -> subB.latest().result().typed(DepthView.class).values().stream().anyMatch(level -> level.quantity() == 5), "cross-client fill update");
                    require(takerAccount.balances(new ObservationQuery().withAssetId(base.id())).join().values()
                             .stream().anyMatch(balance -> balance.active() == 3), "shared taker balance");
                    var audit = owner.audit().report(16).join();
                    require(audit.conserved() && audit.consistent(), "cross-client ledger audit");
                }
                await(() -> owner.subscriptions().activeQueries() == 0, "subscriptions must release shared query");
                try (var wrong = new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "wrong", TOKEN + "x")))
                {
                    boolean rejected = false;
                    try { wrong.request(new nhcm.advmatch.command.read.ReadAssets().encode()).get(5, TimeUnit.SECONDS); }
                    catch (ExecutionException expected) { rejected = true; }
                    require(rejected, "bad authentication must fail");
                }
                try (var slot = b.createObservationSlot(query).join())
                {
                    require(slot.refresh().join().typed(DepthView.class).values().getFirst().quantity() == 5, "remote observation slot");
                    require(slot.load() != null, "slot load must use a local immutable image");
                }
                try (var duplicate = new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "b", TOKEN));
                     var differentLedger = new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "pinned", TOKEN,
                             false, false, 5000, 30000, 64, 4194304, UUID.randomUUID().toString())))
                {
                    for (var rejected : new RemoteChannel[]{duplicate, differentLedger})
                    {
                        boolean failed = false;
                        try { rejected.request(new nhcm.advmatch.command.read.ReadAssets().encode()).get(5, TimeUnit.SECONDS); }
                        catch (ExecutionException expected) { failed = true; }
                        require(failed, "duplicate instance or wrong exchange identity must fail");
                    }
                }
                try (var reconnecting = a.subscriptions().subscribe(query, Duration.ofMillis(100), ignored -> {}))
                {
                    reconnecting.first().get(5, TimeUnit.SECONDS);
                    int port = server.port();
                    server.close();
                    await(() -> reconnecting.latest().state() == SubscriptionSnapshot.State.STALE, "disconnect must mark cached data stale");
                    server = new ExchangeServer(owner, new ExchangeServer.Options("127.0.0.1", port, TOKEN, identity, false, 8, 64, 4194304));
                    owner.matching().place(owner.users().get(maker.id()), owner.symbols().get(market.id()),
                                           OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 11, 1, 3, null).join();
                    await(() -> reconnecting.latest().state() == SubscriptionSnapshot.State.LIVE
                                && reconnecting.latest().result().typed(DepthView.class).values().stream().anyMatch(level -> level.price() == 11),
                          "subscriptions must restore after endpoint reconnect");
                }
                var entered = new CountDownLatch(1);
                var responseGate = new CompletableFuture<byte[]>();
                server.extensions((context, operation, payload) -> owner.submit(new UpdateBalance(takerAccount.id(), quote.id(), 7, 99))
                        .thenCompose(receipt -> { entered.countDown(); return responseGate; }), ignored -> {});
                var uncertain = channelB.extension(1, new byte[0]);
                require(entered.await(5, TimeUnit.SECONDS), "test command must reach the owner");
                server.close();
                boolean unknown = false;
                try { uncertain.get(5, TimeUnit.SECONDS); }
                catch (ExecutionException error) { unknown = error.getCause() instanceof RemoteFailure failure && failure.mayHaveExecuted(); }
                require(unknown, "lost submitted response must be marked uncertain");
                responseGate.complete(new byte[0]);
                require(owner.submit(new Flush()).join().durableThrough() > 0, "closing clients must not close the owner");
                System.out.println("Remote integration: shared matching, subscriptions, authentication, slots and uncertain responses passed");
            }
            finally
            {
                server.close();
            }
        }
    }
}
