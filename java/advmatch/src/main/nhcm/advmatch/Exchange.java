package nhcm.advmatch;

import nhcm.advmatch.command.Command;
import nhcm.advmatch.command.read.CreateObservationSlot;
import nhcm.advmatch.internal.NativeBindings;
import nhcm.advmatch.internal.ResponseCodec;
import nhcm.advmatch.internal.Wire;
import nhcm.advmatch.internal.WireWriter;
import nhcm.advmatch.model.ExchangeConfig;
import nhcm.advmatch.model.ObservationQuery;
import nhcm.advmatch.model.ObservationResult;
import nhcm.advmatch.manager.*;

import java.util.Objects;
import java.util.List;
import java.util.ArrayList;
import java.nio.charset.StandardCharsets;
import nhcm.advmatch.internal.WireReader;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.CompletableFuture;
import java.util.HashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.locks.LockSupport;

public class Exchange implements AutoCloseable
{
    private final AssetManager assets = new AssetManager(this);
    private final UserManager users = new UserManager(this);
    private final SymbolManager symbols = new SymbolManager(this);
    private final AccountManager accounts = new AccountManager(this);
    private final OrderManager orders = new OrderManager(this);
    private final ConditionalOrderManager conditionalOrders = new ConditionalOrderManager(this);
    private final PositionManager positions = new PositionManager(this);
    private final OrderBookManager books = new OrderBookManager(this);
    private final ContractManager contracts = new ContractManager(this);
    private final MatchingEngine matching = new MatchingEngine(this);

    public final AssetManager assets()
    {
        return this.assets;
    }

    public final UserManager users()
    {
        return this.users;
    }

    public final SymbolManager symbols()
    {
        return this.symbols;
    }

    public final AccountManager accounts()
    {
        return this.accounts;
    }

    public final OrderManager orders()
    {
        return this.orders;
    }

    public final ConditionalOrderManager conditionalOrders()
    {
        return this.conditionalOrders;
    }

    public final PositionManager positions()
    {
        return this.positions;
    }

    public final OrderBookManager books()
    {
        return this.books;
    }

    public final ContractManager contracts()
    {
        return this.contracts;
    }

    public final MatchingEngine matching()
    {
        return this.matching;
    }


    private final Object admission = new Object();
    private final HashMap<Long, Entry<?>> pending = new HashMap<>();
    private volatile int nativePending;
    private final TransportOptions transport;
    private final AtomicInteger outstanding = new AtomicInteger();
    private final Executor callbacks;
    private final Thread responses;
    private long session;
    private volatile long closeTicket;
    private CompletableFuture<Void> closeFuture;

    private final class RequestFuture<T> extends CompletableFuture<T>
    {
        private final Entry<T> entry;

        private RequestFuture(Entry<T> entry)
        {
            this.entry = entry;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning)
        {
            synchronized (Exchange.this.admission)
            {
                synchronized (this.entry)
                {
                    if (this.entry.released || this.isDone() || !NativeBindings.cancel(Exchange.this.session, this.entry.ticket))
                    {
                        return false;
                    }

                    return super.cancel(false);
                }
            }
        }
    }

    private final class Entry<T>
    {
        private long ticket;
        private final Command<T> command;
        private final RequestFuture<T> future;
        private boolean released;

        private Entry(long ticket, Command<T> command)
        {
            this.ticket = ticket;
            this.command = command;
            this.future = new RequestFuture<>(this);
        }

        private Runnable response(byte[] bytes, int offset, int size, Throwable error)
        {
            synchronized (this)
            {
                this.released = true;
            }

            return () ->
            {
                // Release Java capacity before user continuations run. Native
                // tickets were released by drain, independently of callbacks.
                Exchange.this.outstanding.decrementAndGet();
                if (error != null)
                {
                    this.future.completeExceptionally(error);
                    return;
                }

                try
                {
                    this.future.complete(this.command.decode(bytes, offset, size));
                }
                catch (Throwable decodeError)
                {
                    this.future.completeExceptionally(decodeError);
                }
            };
        }
    }

    public Exchange()
    {
        this(new ExchangeConfig());
    }

    public Exchange(ExchangeConfig config)
    {
        this(config, ForkJoinPool.commonPool());
    }

    // Executors must enqueue work asynchronously. Completion decoding is
    // dispatched here; non-Async CF continuations may also run on an attaching
    // caller if the Future was already complete (standard CompletableFuture).
    public Exchange(ExchangeConfig config, Executor callbacks)
    {
        this(config, callbacks, new TransportOptions(256, 64, 1_000, (int)Math.min(config.maxPendingRequests(), Integer.MAX_VALUE)));
    }

    public Exchange(ExchangeConfig config, Executor callbacks, TransportOptions transport)
    {
        this.transport = Objects.requireNonNull(transport);
        this.callbacks = Objects.requireNonNull(callbacks);
        var writer = new WireWriter();
        writer.u16(1);
        Wire.writeExchangeConfig(writer, Objects.requireNonNull(config));
        this.session = NativeBindings.create(writer.bytes());
        this.responses = new Thread(this::pump, "advmatch-responses");
        this.responses.setDaemon(true);
        try
        {
            this.responses.start();
        }
        catch (Throwable error)
        {
            NativeBindings.destroy(this.session);
            throw error;
        }
    }

    public <T> CompletableFuture<T> submit(Command<T> command)
    {
        try
        {
            var packet = Objects.requireNonNull(command).encode();
            synchronized (this.admission)
            {
                if (this.closeFuture != null)
                {
                    throw new IllegalStateException("Exchange is closing or closed");
                }

                this.reserveOutstanding(1);
                Entry<T> entry;
                try
                {
                    entry = new Entry<>(0, command);
                    entry.ticket = NativeBindings.submit(this.session, packet);
                    this.pending.put(entry.ticket, entry);
                    ++this.nativePending;
                }
                catch (Throwable error)
                {
                    this.outstanding.decrementAndGet();
                    throw error;
                }
                LockSupport.unpark(this.responses);
                return entry.future;
            }
        }
        catch (Throwable error)
        {
            return CompletableFuture.failedFuture(error);
        }
    }

    // One JNI crossing and one native queue task. Individual commands retain
    // their own typed results, cancellation and business rejection semantics.
    // Order is preserved within a batch; a batch is not a database transaction.
    public <T> List<CompletableFuture<T>> submitBatch(List<? extends Command<T>> commands)
    {
        Objects.requireNonNull(commands);
        if (commands.isEmpty())
        {
            return List.of();
        }

        if (commands.size() > 1024)
        {
            throw new IllegalArgumentException("A native batch supports at most 1024 commands");
        }

        var entries = new ArrayList<Entry<T>>(commands.size());
        var futures = new ArrayList<CompletableFuture<T>>(commands.size());
        try
        {
            var writer = new WireWriter(Math.addExact(4, Math.multiplyExact(commands.size(), 96)));
            writer.u32(commands.size());
            for (var command : commands)
            {
                var frame = writer.beginFrame();
                Objects.requireNonNull(command).encodeTo(writer);
                writer.endFrame(frame);
                var entry = new Entry<T>(0, command);
                entries.add(entry);
                futures.add(entry.future);
            }

            var packet = writer.bytes();
            synchronized (this.admission)
            {
                if (this.closeFuture != null)
                {
                    throw new IllegalStateException("Exchange is closing or closed");
                }

                this.reserveOutstanding(entries.size());
                try
                {
                    var tickets = NativeBindings.submitBatch(this.session, packet, entries.size());
                    for (int index = 0; index < entries.size(); ++index)
                    {
                        var entry = entries.get(index);
                        entry.ticket = tickets[index];
                        this.pending.put(entry.ticket, entry);
                        ++this.nativePending;
                    }
                }
                catch (Throwable error)
                {
                    this.outstanding.addAndGet(-entries.size());
                    throw error;
                }

                LockSupport.unpark(this.responses);
            }
        }
        catch (Throwable error)
        {
            // Also cover encode failure before all entries were constructed.
            futures.clear();
            for (int index = 0; index < commands.size(); ++index)
            {
                futures.add(CompletableFuture.failedFuture(error));
            }
        }

        return List.copyOf(futures);
    }

    public int pendingRequests()
    {
        return this.nativePending;
    }

    public int outstandingRequests()
    {
        return this.outstanding.get();
    }

    private void reserveOutstanding(int count)
    {
        // Called under admission; completions only decrease the counter.
        if (count > this.transport.maxOutstanding() - this.outstanding.get())
        {
            throw new NativeException(NativeException.QUEUE_FULL, "Java outstanding response limit reached");
        }

        this.outstanding.addAndGet(count);
    }

    public CompletableFuture<ObservationSlot> createObservationSlot(ObservationQuery query)
    {
        return this.submit(new CreateObservationSlot(query)).thenApply(id -> new ObservationSlot(this, id));
    }

    public CompletableFuture<Void> closeAsync()
    {
        synchronized (this.admission)
        {
            if (this.closeFuture != null)
            {
                return this.closeFuture;
            }

            try
            {
                var ticket = NativeBindings.close(this.session);
                this.closeFuture = new CompletableFuture<>()
                {
                    @Override
                    public boolean cancel(boolean mayInterruptIfRunning)
                    {
                        return false;
                    }
                };
                this.closeTicket = ticket;
                LockSupport.unpark(this.responses);
                return this.closeFuture;
            }
            catch (Throwable error)
            {
                return CompletableFuture.failedFuture(error);
            }
        }
    }

    @Override
    public void close()
    {
        this.closeAsync().join();
    }

    ObservationResult loadSlot(long id)
    {
        synchronized (this.admission)
        {
            if (this.session == 0)
            {
                return null;
            }

            var bytes = NativeBindings.loadSlot(this.session, id);
            return bytes == null ? null : ResponseCodec.read(bytes);
        }
    }

    void releaseSlot(long id)
    {
        synchronized (this.admission)
        {
            if (this.session != 0)
            {
                NativeBindings.releaseSlot(this.session, id);
            }
        }
    }

    private void complete(Runnable action)
    {
        try
        {
            this.callbacks.execute(action);
        }
        catch (RuntimeException rejected)
        {
            // Keep accepted responses observable even if a custom executor
            // was shut down too early; the fallback also queues asynchronously.
            ForkJoinPool.commonPool().execute(action);
        }
    }

    private void pump()
    {
        boolean closed = false;
        Throwable closeError = null;
        int idleSpins = 0;
        for (;;)
        {
            boolean progress = false;
            ArrayList<Runnable> actions = null;
            synchronized (this.admission)
            {
                if (this.nativePending != 0)
                {
                    try
                    {
                        var bytes = NativeBindings.drain(this.session, this.transport.responseBatchSize());
                        if (bytes != null)
                        {
                            var reader = new WireReader(bytes);
                            var count = Math.toIntExact(reader.u32());
                            actions = new ArrayList<>(count);
                            for (int index = 0; index < count; ++index)
                            {
                                var id = reader.u64();
                                var status = reader.i32();
                                var size = Math.toIntExact(reader.u32());
                                var offset = reader.offset();
                                reader.skip(size);
                                var entry = this.pending.remove(id);
                                if (entry == null)
                                {
                                    throw new IllegalStateException("Unknown drained request " + id);
                                }

                                --this.nativePending;
                                Throwable error = null;
                                if (status != 0)
                                {
                                    var message = new String(bytes, offset, size, StandardCharsets.UTF_8);
                                    error = status == -2 ? new CancellationException(message) : new NativeException(status, message);
                                }

                                actions.add(entry.response(bytes, offset, size, error));
                            }

                            reader.finish();
                            progress = count != 0;
                        }
                    }
                    catch (Throwable error)
                    {
                        // A broken transport cannot silently strand Futures.
                        if (actions == null)
                        {
                            actions = new ArrayList<>(this.pending.size());
                        }

                        for (var entry : this.pending.values())
                        {
                            actions.add(entry.response(null, 0, 0, error));
                        }

                        this.pending.clear();
                        this.nativePending = 0;
                        closeError = error;
                        if (this.closeTicket == 0)
                        {
                            this.closeAsync();
                        }
                    }
                }
            }

            if (actions != null && !actions.isEmpty())
            {
                var completed = actions;
                this.complete(() -> completed.forEach(Runnable::run));
            }

            var ticket = this.closeTicket;
            if (ticket != 0 && !closed)
            {
                try
                {
                    closed = NativeBindings.poll(this.session, ticket) != null;
                }
                catch (Throwable error)
                {
                    closeError = error;
                    closed = true;
                }

                if (closed)
                {
                    NativeBindings.release(this.session, ticket);
                }
            }

            if (closed && this.nativePending == 0)
            {
                synchronized (this.admission)
                {
                    try
                    {
                        NativeBindings.destroy(this.session);
                    }
                    catch (Throwable error)
                    {
                        if (closeError == null)
                        {
                            closeError = error;
                        }
                    }

                    this.session = 0;
                }

                var error = closeError;
                this.complete(() ->
                {
                    if (error == null)
                    {
                        this.closeFuture.complete(null);
                    }
                    else
                    {
                        this.closeFuture.completeExceptionally(error);
                    }
                });
                return;
            }

            if (progress)
            {
                idleSpins = 0;
            }
            else
            {
                if ((this.nativePending != 0 || ticket != 0) && idleSpins++ < this.transport.spinCount())
                {
                    Thread.onSpinWait();
                }
                else
                {
                    if (this.nativePending != 0)
                    {
                        // Native condition-variable wakeup avoids Windows Java
                        // timer granularity on serial/small-ring workloads.
                        // Never hold Java admission while waiting: producers
                        // must be able to register new tickets and close.
                        try
                        {
                            NativeBindings.awaitResponses(this.session, 10);
                        }
                        catch (Throwable error)
                        {
                            closeError = error;
                            this.closeAsync();
                        }
                    }
                    else
                    {
                        LockSupport.parkNanos(ticket == 0 ? 10_000_000 : this.transport.activeParkNanos());
                    }

                    idleSpins = 0;
                }
            }
        }
    }
}
