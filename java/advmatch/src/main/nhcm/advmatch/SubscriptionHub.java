package nhcm.advmatch;

import nhcm.advmatch.command.read.ReadChanges;
import nhcm.advmatch.internal.ResponseCodec;
import nhcm.advmatch.internal.Wire;
import nhcm.advmatch.internal.WireReader;
import nhcm.advmatch.model.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.LongAdder;
import java.util.function.Consumer;

/** Demand-driven shared snapshots. One source per query and at most one queued notification per listener. */
public class SubscriptionHub implements AutoCloseable
{
    private final Exchange exchange;
    private final SubscriptionChannel remote;
    private final Executor callbacks;
    private final ScheduledExecutorService scheduler;
    private final Map<ObservationQuery, Feed> feeds = new LinkedHashMap<>();
    private final int batchQueries;
    private final long batchScan;
    private final AtomicInteger batches = new AtomicInteger();
    private final LongAdder checks = new LongAdder(), snapshots = new LongAdder(), failures = new LongAdder(), deferred = new LongAdder();
    private int cursor;
    private long reservedRows;
    private boolean closed;

    public record Metrics(int activeQueries, int pendingBatches, long reservedRows, long conditionalChecks, long changedSnapshots,
                          long failedBatches, long deferredTicks) {}

    public synchronized Metrics metrics()
    {
        return new Metrics(feeds.size(), batches.get(), reservedRows, checks.sum(), snapshots.sum(), failures.sum(), deferred.sum());
    }

    public SubscriptionHub(Exchange exchange, SubscriptionChannel remote, Executor callbacks)
    {
        this(exchange, remote, callbacks, 16, 65536);
    }

    public SubscriptionHub(Exchange exchange, SubscriptionChannel remote, Executor callbacks, int batchQueries, long batchScan)
    {
        this.exchange = exchange;
        this.remote = remote;
        this.callbacks = callbacks;
        this.batchQueries = Math.max(1, Math.min(256, batchQueries));
        this.batchScan = batchScan;
        scheduler = Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("advmatch-subscriptions").factory());
        scheduler.scheduleWithFixedDelay(this::tick, 0, 25, TimeUnit.MILLISECONDS);
    }

    public synchronized ObservationSubscription subscribe(ObservationQuery query, Duration interval,
                                                           Consumer<SubscriptionSnapshot> receiver)
    {
        Objects.requireNonNull(query);
        Objects.requireNonNull(receiver);
        long millis = interval.toMillis();
        if (closed || millis < 100 || millis > 60_000 || query.limit() < 1 || query.limit() > 1024)
        {
            throw new IllegalArgumentException("Closed hub or invalid subscription interval/row limit");
        }
        if (query.cursor().started())
        {
            throw new IllegalArgumentException("Subscribe to a fresh bounded snapshot, not a pagination cursor");
        }
        var feed = feeds.get(query);
        if (feed == null)
        {
            if (feeds.size() >= 16384 || reservedRows + query.limit() > 1048576)
            {
                throw new IllegalStateException("Subscription query limit reached");
            }
            feed = new Feed(query, millis);
            feeds.put(query, feed);
            reservedRows += query.limit();
            if (remote != null)
            {
                var target = feed;
                try
                {
                    feed.upstream = remote.listen(query, Duration.ofMillis(millis), target::publish);
                }
                catch (RuntimeException error)
                {
                    feeds.remove(query);
                    reservedRows -= query.limit();
                    throw error;
                }
            }
        }
        feed.interval = Math.min(feed.interval, millis);
        var handle = new Handle(feed, receiver);
        feed.listeners.add(handle);
        if (feed.snapshot != null)
        {
            handle.signal();
        }
        return handle;
    }

    public synchronized int activeQueries()
    {
        return feeds.size();
    }

    private void tick()
    {
        List<Feed> active;
        synchronized (this)
        {
            if (closed || remote != null)
            {
                return;
            }
            active = List.copyOf(feeds.values());
        }
        long now = System.nanoTime();
        if (active.isEmpty()) return;
        // Background reads may occupy only a small part of native admission.
        // Rotate admission so a busy market cannot starve a quiet account.
        if (exchange.outstandingRequests() >= 128 || batches.get() >= 8)
        {
            deferred.increment();
            return;
        }
        var selected = new ArrayList<Feed>();
        long scan = 0;
        int visited = 0;
        while (visited++ < active.size() && batches.get() < 8)
        {
            var feed = active.get(Math.floorMod(cursor++, active.size()));
            if (now < feed.retryAfter || now - feed.queried < feed.interval * 1_000_000L || feed.pending.get())
            {
                continue;
            }
            if (!selected.isEmpty() && (selected.size() >= batchQueries || scan + feed.query.scanBudget() > batchScan))
            {
                readChanges(selected);
                selected = new ArrayList<>();
                scan = 0;
                if (batches.get() >= 8) break;
            }
            if (!feed.pending.compareAndSet(false, true)) continue;
            feed.queried = now;
            selected.add(feed);
            scan += feed.query.scanBudget();
        }
        if (!selected.isEmpty()) readChanges(selected);
    }

    private void readChanges(List<Feed> selected)
    {
        var batch = List.copyOf(selected);
        batches.incrementAndGet();
        checks.add(batch.size());
        var queries = batch.stream().map(feed -> feed.query).toList();
        var known = batch.stream().map(feed -> feed.known).toList();
        exchange.submit(new RawCommand(new ReadChanges(queries, known).encode())).whenComplete((bytes, error) -> {
            try
            {
                if (error != null) throw new CompletionException(error);
                var reader = WireReader.response(bytes, 3);
                var state = ReadState.fromCode(reader.u8());
                var stamp = Wire.readReadStamp(reader);
                if (state != ReadState.Success) throw new IllegalStateException("Conditional snapshot read failed: " + state);
                var images = new HashMap<ObservationQuery, SubscriptionSnapshot>();
                reader.list(() -> {
                    int start = reader.offset();
                    var image = ResponseCodec.observation(reader);
                    byte[] wire = new byte[reader.offset() - start + 3];
                    wire[0] = 1;
                    wire[2] = 2;
                    System.arraycopy(bytes, start, wire, 3, wire.length - 3);
                    images.put(image.query(), new SubscriptionSnapshot(SubscriptionSnapshot.State.LIVE, image, null,
                                                                        System.currentTimeMillis(), wire));
                    return image;
                });
                reader.finish();
                for (var feed : batch)
                {
                    feed.known = stamp;
                    feed.retryAfter = 0;
                    var image = images.get(feed.query);
                    if (image != null)
                    {
                        snapshots.increment();
                        feed.publish(image);
                    }
                }
            }
            catch (RuntimeException failure)
            {
                failures.increment();
                for (var feed : batch)
                {
                    feed.known = new ReadStamp();
                    feed.retryAfter = System.nanoTime() + 1_000_000_000L;
                    feed.publish(new SubscriptionSnapshot(SubscriptionSnapshot.State.STALE,
                                                           feed.snapshot == null ? null : feed.snapshot.result(), failure,
                                                           System.currentTimeMillis(), null));
                }
            }
            finally
            {
                batch.forEach(feed -> feed.pending.set(false));
                batches.decrementAndGet();
            }
        });
    }

    private class Feed
    {
        private final ObservationQuery query;
        private final Set<Handle> listeners = new HashSet<>();
        private final AtomicBoolean pending = new AtomicBoolean();
        private volatile SubscriptionSnapshot snapshot;
        private volatile long queried, interval;
        private volatile long retryAfter;
        private volatile ReadStamp known = new ReadStamp();
        private AutoCloseable upstream;

        Feed(ObservationQuery query, long interval)
        {
            this.query = query;
            this.interval = interval;
        }

        void publish(SubscriptionSnapshot next)
        {
            List<Handle> notify;
            synchronized (SubscriptionHub.this)
            {
                if (closed || feeds.get(query) != this)
                {
                    return;
                }
                var previous = snapshot;
                snapshot = next;
                boolean same = previous != null && previous.state() == next.state()
                               && previous.result() != null && next.result() != null
                               && previous.result().page().stamp().session() == next.result().page().stamp().session()
                               && previous.result().page().state() == next.result().page().state()
                               && previous.result().page().hasMore() == next.result().page().hasMore()
                               && Objects.equals(previous.result().data(), next.result().data());
                if (same)
                {
                    return;
                }
                notify = List.copyOf(listeners);
            }
            notify.forEach(Handle::signal);
        }
    }

    private class Handle implements ObservationSubscription
    {
        private final Feed feed;
        private final Consumer<SubscriptionSnapshot> receiver;
        private final AtomicBoolean queued = new AtomicBoolean();
        private final CompletableFuture<SubscriptionSnapshot> first = new CompletableFuture<>();
        private volatile boolean disposed;

        Handle(Feed feed, Consumer<SubscriptionSnapshot> receiver)
        {
            this.feed = feed;
            this.receiver = receiver;
        }

        @Override
        public SubscriptionSnapshot latest()
        {
            if (disposed)
            {
                return new SubscriptionSnapshot(SubscriptionSnapshot.State.CLOSED, null, null, System.currentTimeMillis(), null);
            }
            return feed.snapshot;
        }

        @Override
        public CompletableFuture<SubscriptionSnapshot> first()
        {
            return first.copy();
        }

        void signal()
        {
            if (disposed || !queued.compareAndSet(false, true))
            {
                return;
            }
            Runnable deliver = () -> {
                var image = feed.snapshot;
                try
                {
                    if (!disposed && image != null)
                    {
                        if (image.state() == SubscriptionSnapshot.State.LIVE)
                        {
                            first.complete(image);
                        }
                        else if (image.error() != null)
                        {
                            first.completeExceptionally(image.error());
                        }
                        receiver.accept(image);
                    }
                }
                finally
                {
                    queued.set(false);
                    if (!disposed && feed.snapshot != image)
                    {
                        signal();
                    }
                }
            };
            try
            {
                callbacks.execute(deliver);
            }
            catch (RuntimeException rejected)
            {
                ForkJoinPool.commonPool().execute(deliver);
            }
        }

        @Override
        public void close()
        {
            synchronized (SubscriptionHub.this)
            {
                if (disposed)
                {
                    return;
                }
                disposed = true;
                first.completeExceptionally(new CancellationException("Subscription closed"));
                feed.listeners.remove(this);
                if (feed.listeners.isEmpty())
                {
                    feeds.remove(feed.query);
                    reservedRows -= feed.query.limit();
                    if (feed.upstream != null)
                    {
                        try { feed.upstream.close(); }
                        catch (Exception ignored) { /* Disconnect already marks the feed stale. */ }
                    }
                }
            }
        }
    }

    @Override
    public synchronized void close()
    {
        if (closed)
        {
            return;
        }
        for (var feed : List.copyOf(feeds.values()))
        {
            for (var handle : List.copyOf(feed.listeners))
            {
                handle.close();
            }
        }
        closed = true;
        scheduler.shutdownNow();
    }
}
