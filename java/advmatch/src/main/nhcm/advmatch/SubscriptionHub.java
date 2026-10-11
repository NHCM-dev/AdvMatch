package nhcm.advmatch;

import nhcm.advmatch.command.read.Read;
import nhcm.advmatch.internal.ResponseCodec;
import nhcm.advmatch.model.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Demand-driven shared snapshots. One source per query and at most one queued notification per listener. */
public class SubscriptionHub implements AutoCloseable
{
    private final Exchange exchange;
    private final SubscriptionChannel remote;
    private final Executor callbacks;
    private final ScheduledExecutorService scheduler;
    private final Map<ObservationQuery, Feed> feeds = new HashMap<>();
    private boolean closed;

    public SubscriptionHub(Exchange exchange, SubscriptionChannel remote, Executor callbacks)
    {
        this.exchange = exchange;
        this.remote = remote;
        this.callbacks = callbacks;
        scheduler = Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("advmatch-subscriptions").factory());
        scheduler.scheduleWithFixedDelay(this::tick, 0, 50, TimeUnit.MILLISECONDS);
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
            if (feeds.size() >= 4096)
            {
                throw new IllegalStateException("Subscription query limit reached");
            }
            feed = new Feed(query, millis);
            feeds.put(query, feed);
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
        for (var feed : active)
        {
            if (now - feed.queried < feed.interval * 1_000_000L || !feed.pending.compareAndSet(false, true))
            {
                continue;
            }
            feed.queried = now;
            exchange.submit(new RawCommand(new Read(feed.query).encode())).whenComplete((bytes, error) -> {
                try
                {
                    if (error != null)
                    {
                        feed.publish(new SubscriptionSnapshot(SubscriptionSnapshot.State.STALE,
                                                               feed.snapshot == null ? null : feed.snapshot.result(), error,
                                                               System.currentTimeMillis(), null));
                        return;
                    }
                    var result = ResponseCodec.read(bytes);
                    if (result.page().state() != ReadState.Success && result.page().state() != ReadState.NotFound)
                    {
                        throw new BusinessException(result.page());
                    }
                    feed.publish(new SubscriptionSnapshot(SubscriptionSnapshot.State.LIVE, result, null,
                                                           System.currentTimeMillis(), bytes));
                }
                catch (RuntimeException failure)
                {
                    feed.publish(new SubscriptionSnapshot(SubscriptionSnapshot.State.STALE,
                                                           feed.snapshot == null ? null : feed.snapshot.result(), failure,
                                                           System.currentTimeMillis(), null));
                }
                finally
                {
                    feed.pending.set(false);
                }
            });
        }
    }

    private class Feed
    {
        private final ObservationQuery query;
        private final Set<Handle> listeners = new HashSet<>();
        private final AtomicBoolean pending = new AtomicBoolean();
        private volatile SubscriptionSnapshot snapshot;
        private volatile long queried, interval;
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
