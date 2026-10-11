package nhcm.advmatch.remote;

import nhcm.advmatch.Exchange;
import nhcm.advmatch.NativeException;
import nhcm.advmatch.ObservationSubscription;
import nhcm.advmatch.model.*;
import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

public class AdmissionTest
{
    private static void require(boolean condition, String message)
    {
        if (!condition) throw new AssertionError(message);
    }

    private static void await(BooleanSupplier condition) throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean())
        {
            require(System.nanoTime() < deadline, "admission timeout");
            Thread.sleep(5);
        }
    }

    public static void main(String[] args) throws Exception
    {
        try (var scheduler = new FairRequestScheduler())
        {
            var blocked = new CompletableFuture<byte[]>();
            var work = new ArrayList<CompletableFuture<byte[]>>();
            for (int index = 0; index < 100; index++) work.add(scheduler.submit("busy", true, () -> blocked));
            await(() -> scheduler.metrics().running() == 8);
            require(scheduler.submit("other", false, () -> CompletableFuture.completedFuture(new byte[]{1}))
                             .get(1, TimeUnit.SECONDS)[0] == 1, "one busy gateway cannot monopolize admission");
            blocked.complete(new byte[0]);
            CompletableFuture.allOf(work.toArray(CompletableFuture[]::new)).get(5, TimeUnit.SECONDS);
        }
        try (var scheduler = new FairRequestScheduler())
        {
            var blocked = new CompletableFuture<byte[]>();
            for (int index = 0; index < 40; index++) scheduler.submit("reader-" + index, true, () -> blocked);
            await(() -> scheduler.metrics().runningReads() == 32);
            scheduler.submit("trader", false, () -> CompletableFuture.completedFuture(new byte[0])).get(1, TimeUnit.SECONDS);
            require(scheduler.metrics().runningReads() == 32, "reads have a reserved limit");
            blocked.complete(new byte[0]);
        }
        queueLimits();
        var mailbox = new OutboundMailbox();
        for (int index = 0; index < 1000; index++) require(mailbox.offer(new NetworkFrames.Frame(NetworkFrames.UPDATE, 9, new byte[]{(byte) index})), "coalescing admission");
        require(mailbox.coalesced() == 999, "latest snapshot replaces obsolete data");
        require(mailbox.offer(new NetworkFrames.Frame(NetworkFrames.RESPONSE, 1, new byte[]{42})), "reply reserve");
        require(mailbox.poll(0).type() == NetworkFrames.RESPONSE, "responses precede snapshot traffic");
        require(mailbox.poll(0).payload()[0] == (byte) 999, "most recent snapshot");
        require(mailbox.poll(0) == null, "obsolete updates must be absent");
        for (int index = 0; index < 256; index++) require(mailbox.offer(new NetworkFrames.Frame(NetworkFrames.RESPONSE, index, new byte[0])), "bounded reply admission");
        require(!mailbox.offer(new NetworkFrames.Frame(NetworkFrames.RESPONSE, 257, new byte[0])), "slow consumer must not use unbounded memory");
        mailbox.close();
        subscriptionBurst();
        System.out.println("Gateway admission: fairness, trading reserve, snapshot coalescing and bounded slow-reader memory passed");
    }

    private static void rejected(CompletableFuture<byte[]> result) throws Exception
    {
        try
        {
            result.get(1, TimeUnit.SECONDS);
            throw new AssertionError("Overload was admitted");
        }
        catch (ExecutionException failure)
        {
            require(failure.getCause() instanceof NativeException error && error.status() == NativeException.QUEUE_FULL,
                    "Overload must fail before execution with QUEUE_FULL");
        }
    }

    private static void queueLimits() throws Exception
    {
        try (var scheduler = new FairRequestScheduler())
        {
            var blocked = new CompletableFuture<byte[]>();
            for (int index = 0; index < 8; index++) scheduler.submit("busy", false, () -> blocked);
            await(() -> scheduler.metrics().running() == 8);
            var queued = new ArrayList<CompletableFuture<byte[]>>();
            for (int index = 0; index < 256; index++) queued.add(scheduler.submit("busy", false, () -> blocked));
            rejected(scheduler.submit("busy", false, () -> {
                throw new AssertionError("Rejected work executed");
            }));
            require(scheduler.metrics().queued() == 256, "Per-gateway waiting capacity");
            blocked.complete(new byte[0]);
            CompletableFuture.allOf(queued.toArray(CompletableFuture[]::new)).get(5, TimeUnit.SECONDS);
        }
        try (var scheduler = new FairRequestScheduler())
        {
            var blocked = new CompletableFuture<byte[]>();
            for (int index = 0; index < 128; index++) scheduler.submit("running-" + index % 16, false, () -> blocked);
            await(() -> scheduler.metrics().running() == 128);
            var queued = new ArrayList<CompletableFuture<byte[]>>();
            for (int index = 0; index < 4096; index++) queued.add(scheduler.submit("queued-" + index % 32, false, () -> blocked));
            rejected(scheduler.submit("overflow", false, () -> {
                throw new AssertionError("Rejected work executed");
            }));
            require(scheduler.metrics().queued() == 4096, "Global waiting capacity");
            scheduler.close();
            require(queued.stream().allMatch(CompletableFuture::isCompletedExceptionally), "Shutdown must reject every queued result");
            blocked.complete(new byte[0]);
        }
    }

    private static void subscriptionBurst() throws Exception
    {
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false));
        var handles = new ArrayList<ObservationSubscription>();
        String token = "burst-test-012345678901234567890123456789";
        try (var owner = new Exchange(config);
             var server = new ExchangeServer(owner, new ExchangeServer.Options("127.0.0.1", 0, token, "burst-ledger", false, 4, 64, 4194304));
             var remote = new Exchange(new RemoteChannel(new RemoteOptions("127.0.0.1", server.port(), "burst", token)), ForkJoinPool.commonPool()))
        {
            var user = owner.users().create().join();
            for (int index = 1; index <= 300; index++)
            {
                var query = new ObservationQuery().withKind(ReadKind.Balances).withUserId(user.id()).withLimit(index);
                handles.add(remote.subscriptions().subscribe(query, Duration.ofSeconds(1), ignored -> {}));
            }
            CompletableFuture.allOf(handles.stream().map(ObservationSubscription::first).toArray(CompletableFuture[]::new)).get(20, TimeUnit.SECONDS);
            require(server.metrics().connections() == 1 && owner.subscriptions().activeQueries() == 300,
                    "a subscription burst must not overflow reply capacity or disconnect");
            handles.forEach(ObservationSubscription::close);
            await(() -> owner.subscriptions().activeQueries() == 0);
        }
        finally
        {
            handles.forEach(ObservationSubscription::close);
        }
    }
}
