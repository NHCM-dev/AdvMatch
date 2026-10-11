package nhcm.advmatch.remote;

import nhcm.advmatch.*;
import nhcm.advmatch.command.read.Read;
import nhcm.advmatch.internal.ResponseCodec;
import nhcm.advmatch.internal.WireReader;
import nhcm.advmatch.model.*;

import javax.net.SocketFactory;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import java.io.*;
import java.net.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;

/** Multiplexed bounded client. Reconnects subscriptions, never replays submitted commands. */
public class RemoteChannel implements CommandChannel, SubscriptionChannel, RemoteSlotAccess
{
    private final RemoteOptions options;
    private final SSLSocketFactory tlsFactory;
    private final ArrayBlockingQueue<NetworkFrames.Frame> outbound;
    private final Map<Long, Pending> pending = new ConcurrentHashMap<>();
    private final Map<Long, Subscriber> subscribers = new ConcurrentHashMap<>();
    private final Map<Long, ObservationResult> slots = new ConcurrentHashMap<>();
    private final AtomicLong nextId = new AtomicLong();
    private final Semaphore capacity;
    private final Semaphore controlCapacity = new Semaphore(32);
    private final Map<Long, ArrayDeque<Connection>> controlPending = new HashMap<>();
    private final ScheduledExecutorService timer;
    private volatile Connection connection;
    private volatile String exchangeId;
    private volatile boolean closed;

    private static class Pending
    {
        private final CompletableFuture<byte[]> result = new CompletableFuture<>()
        {
            @Override
            public boolean cancel(boolean interrupt)
            {
                return false; // A sent remote command cannot be safely cancelled by abandoning its response.
            }
        };
        private final long created = System.nanoTime();
        private final byte[] packet;
        private final int type;
        private volatile boolean written;

        Pending(int type, byte[] packet)
        {
            this.type = type;
            this.packet = packet;
        }
    }

    private record Connection(Socket socket, DataInputStream input, DataOutputStream output) {}

    private class Subscriber
    {
        private final long id;
        private final byte[] packet;
        private final Consumer<SubscriptionSnapshot> receiver;
        private volatile SubscriptionSnapshot snapshot;
        private volatile Connection registered;

        Subscriber(long id, byte[] packet, Consumer<SubscriptionSnapshot> receiver)
        {
            this.id = id;
            this.packet = packet;
            this.receiver = receiver;
        }

        void stale(Throwable error)
        {
            registered = null;
            var old = snapshot;
            snapshot = new SubscriptionSnapshot(SubscriptionSnapshot.State.STALE, old == null ? null : old.result(),
                                                error, System.currentTimeMillis(), null);
            receiver.accept(snapshot);
        }
    }

    public RemoteChannel(RemoteOptions options)
    {
        this(options, null);
    }

    public RemoteChannel(RemoteOptions options, SSLSocketFactory tlsFactory)
    {
        if (tlsFactory != null && !options.tls()) throw new IllegalArgumentException("TLS socket factory requires TLS mode");
        this.options = options;
        this.tlsFactory = tlsFactory;
        exchangeId = options.expectedExchangeId();
        capacity = new Semaphore(options.maxOutstanding());
        outbound = new ArrayBlockingQueue<>(options.maxOutstanding() + 256);
        timer = Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("advmatch-remote-timer").factory());
        timer.scheduleWithFixedDelay(this::maintain, 1, 1, TimeUnit.SECONDS);
        Thread.ofPlatform().daemon().name("advmatch-remote-writer").start(this::write);
    }

    public String exchangeId()
    {
        return exchangeId;
    }

    @Override
    public CompletableFuture<byte[]> request(byte[] request)
    {
        return request(NetworkFrames.COMMAND, request);
    }

    public CompletableFuture<byte[]> extension(int operation, byte[] request)
    {
        return request(NetworkFrames.EXTENSION, NetworkFrames.encode(out -> { out.writeInt(operation); out.write(request); }));
    }

    private CompletableFuture<byte[]> request(int type, byte[] packet)
    {
        if (closed || packet.length + 9 > options.maxFrameBytes())
        {
            return CompletableFuture.failedFuture(new RemoteFailure("Client closed or request too large", false, null));
        }
        if (!capacity.tryAcquire())
        {
            return CompletableFuture.failedFuture(new NativeException(NativeException.QUEUE_FULL, "Remote outstanding limit reached"));
        }
        long id = nextId.incrementAndGet();
        var entry = new Pending(type, packet.clone());
        pending.put(id, entry);
        if (!outbound.offer(new NetworkFrames.Frame(type, id, entry.packet)))
        {
            finish(id, null, new RemoteFailure("Remote outbound queue is full", false, null));
        }
        return entry.result;
    }

    @Override
    public int outstandingRequests()
    {
        return options.maxOutstanding() - capacity.availablePermits();
    }

    private Connection connect() throws IOException
    {
        var current = connection;
        if (current != null)
        {
            return current;
        }
        var address = InetAddress.getByName(options.host());
        if (!options.tls() && !address.isLoopbackAddress() && !options.allowPlaintextNetwork())
        {
            throw new IOException("Remote non-loopback connections require TLS or explicit trusted-network plaintext configuration");
        }
        SocketFactory factory = options.tls() ? tlsFactory == null ? SSLSocketFactory.getDefault() : tlsFactory
                                             : SocketFactory.getDefault();
        Socket socket = factory.createSocket();
        try
        {
            socket.connect(new InetSocketAddress(address, options.port()), options.connectTimeoutMillis());
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(options.requestTimeoutMillis());
            if (socket instanceof SSLSocket tls)
            {
                var parameters = tls.getSSLParameters();
                parameters.setEndpointIdentificationAlgorithm("HTTPS");
                tls.setSSLParameters(parameters);
                tls.startHandshake();
            }
            var input = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
            var output = new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
            var hello = NetworkFrames.encode(out -> {
                out.writeInt(NetworkFrames.VERSION);
                NetworkFrames.string(out, options.instanceId());
                NetworkFrames.string(out, options.token());
                NetworkFrames.string(out, exchangeId);
            });
            NetworkFrames.write(output, new NetworkFrames.Frame(NetworkFrames.HELLO, 0, hello), options.maxFrameBytes());
            var response = NetworkFrames.read(input, options.maxFrameBytes());
            var body = NetworkFrames.input(response.payload());
            if (response.type() != NetworkFrames.RESPONSE || response.id() != 0 || body.readInt() != NetworkFrames.VERSION)
            {
                throw new IOException("Remote handshake rejected");
            }
            String identity = NetworkFrames.string(body, 128);
            if (body.available() != 0 || !exchangeId.isEmpty() && !exchangeId.equals(identity))
            {
                throw new IOException("Remote exchange identity changed");
            }
            exchangeId = identity;
            current = new Connection(socket, input, output);
            synchronized (this)
            {
                if (closed)
                {
                    throw new IOException("Remote client closed during connection");
                }
                connection = current;
            }
            var accepted = current;
            Thread.ofPlatform().daemon().name("advmatch-remote-reader").start(() -> read(accepted));
            for (var subscriber : subscribers.values())
            {
                writeControl(current, new NetworkFrames.Frame(NetworkFrames.SUBSCRIBE, subscriber.id, subscriber.packet));
                subscriber.registered = current;
            }
            return current;
        }
        catch (IOException | RuntimeException error)
        {
            socket.close();
            throw error;
        }
    }

    private void write()
    {
        while (!closed)
        {
            Connection current = null;
            NetworkFrames.Frame frame = null;
            try
            {
                frame = outbound.poll(1, TimeUnit.SECONDS);
                if (frame == null)
                {
                    continue;
                }
                var entry = pending.get(frame.id());
                if ((frame.type() == NetworkFrames.COMMAND || frame.type() == NetworkFrames.EXTENSION) && entry == null)
                {
                    continue;
                }
                current = connect();
                if (frame.type() == NetworkFrames.SUBSCRIBE)
                {
                    var subscriber = subscribers.get(frame.id());
                    if (subscriber == null || subscriber.registered == current)
                    {
                        continue;
                    }
                    subscriber.registered = current;
                }
                if (entry != null)
                {
                    synchronized (this)
                    {
                        if (closed || connection != current || pending.get(frame.id()) != entry) continue;
                        entry.written = true;
                    }
                }
                if (frame.type() == NetworkFrames.SUBSCRIBE || frame.type() == NetworkFrames.UNSUBSCRIBE || frame.type() == NetworkFrames.RELEASE_SLOT)
                    writeControl(current, frame);
                else NetworkFrames.write(current.output(), frame, options.maxFrameBytes());
            }
            catch (IOException | RuntimeException error)
            {
                disconnect(current, error);
                if (frame != null)
                {
                    var entry = pending.get(frame.id());
                    finish(frame.id(), null, new RemoteFailure("Remote request failed; inspect durability before retrying",
                                                                entry != null && entry.written, error));
                }
                try { Thread.sleep(250); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); return; }
            }
            catch (InterruptedException interrupted)
            {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    private void read(Connection current)
    {
        try
        {
            while (!closed && connection == current)
            {
                var frame = NetworkFrames.read(current.input(), options.maxFrameBytes());
                switch (frame.type())
                {
                    case NetworkFrames.RESPONSE -> {
                        acknowledgeControl(current, frame.id());
                        finish(frame.id(), frame.payload(), null);
                    }
                    case NetworkFrames.ERROR ->
                    {
                        acknowledgeControl(current, frame.id());
                        var reader = NetworkFrames.input(frame.payload());
                        var error = new NativeException(reader.readInt(), NetworkFrames.string(reader, 4096));
                        var subscriber = subscribers.get(frame.id());
                        if (subscriber != null)
                        {
                            subscriber.stale(error);
                        }
                        else
                        {
                            finish(frame.id(), null, error);
                        }
                    }
                    case NetworkFrames.UPDATE ->
                    {
                        var subscriber = subscribers.get(frame.id());
                        if (subscriber == null)
                        {
                            continue;
                        }
                        var reader = NetworkFrames.input(frame.payload());
                        if (!reader.readBoolean())
                        {
                            subscriber.stale(new RemoteFailure("Subscription source is stale", false, null));
                            continue;
                        }
                        byte[] bytes = reader.readAllBytes();
                        var result = ResponseCodec.read(bytes);
                        var snapshot = new SubscriptionSnapshot(SubscriptionSnapshot.State.LIVE, result, null, System.currentTimeMillis(), bytes);
                        subscriber.snapshot = snapshot;
                        subscriber.receiver.accept(snapshot);
                    }
                    default -> throw new IOException("Unexpected remote frame");
                }
            }
        }
        catch (IOException | RuntimeException error)
        {
            disconnect(current, error);
        }
    }

    private void finish(long id, byte[] bytes, Throwable error)
    {
        var entry = pending.remove(id);
        if (entry == null)
        {
            return;
        }
        // User continuations must not block socket reads (for example metadata IO waiting for another RPC).
        ForkJoinPool.commonPool().execute(() -> {
            capacity.release();
            complete(entry, bytes, error);
        });
    }

    private void writeControl(Connection current, NetworkFrames.Frame frame) throws IOException
    {
        try
        {
            if (!controlCapacity.tryAcquire(options.requestTimeoutMillis(), TimeUnit.MILLISECONDS))
                throw new IOException("Subscription control deadline exceeded");
        }
        catch (InterruptedException failure)
        {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("Subscription control interrupted");
        }
        synchronized (this)
        {
            if (closed || connection != current)
            {
                controlCapacity.release();
                throw new IOException("Connection changed before subscription control admission");
            }
            controlPending.computeIfAbsent(frame.id(), ignored -> new ArrayDeque<>()).addLast(current);
        }
        NetworkFrames.write(current.output(), frame, options.maxFrameBytes());
    }

    private synchronized void acknowledgeControl(Connection current, long id)
    {
        var queue = controlPending.get(id);
        if (queue != null && queue.peekFirst() == current)
        {
            queue.removeFirst();
            if (queue.isEmpty()) controlPending.remove(id);
            controlCapacity.release();
        }
    }

    private void complete(Pending entry, byte[] bytes, Throwable error)
    {
        if (error != null)
        {
            entry.result.completeExceptionally(error);
        }
        else
        {
            try
            {
                if (entry.type == NetworkFrames.COMMAND && entry.packet.length > 2 && entry.packet[2] == 7)
                {
                    var reader = new WireReader(entry.packet);
                    reader.skip(3);
                    slots.put(reader.u64(), ResponseCodec.read(bytes));
                }
                entry.result.complete(bytes);
            }
            catch (RuntimeException decodeFailure)
            {
                entry.result.completeExceptionally(decodeFailure);
            }
        }
    }

    private synchronized void disconnect(Connection current, Throwable error)
    {
        if (current != null && connection != current)
        {
            return;
        }
        var existing = connection;
        connection = null;
        if (existing != null)
        {
            try { existing.socket().close(); }
            catch (IOException ignored) {}
        }
        int controls = controlPending.values().stream().mapToInt(ArrayDeque::size).sum();
        controlPending.clear();
        controlCapacity.release(controls);
        slots.clear();
        for (var request : List.copyOf(pending.entrySet()))
        {
            finish(request.getKey(), null, new RemoteFailure("Remote connection lost; submitted results may be unknown", request.getValue().written, error));
        }
        subscribers.values().forEach(subscriber -> subscriber.stale(error));
    }

    private void maintain()
    {
        if (closed)
        {
            return;
        }
        long now = System.nanoTime();
        if (pending.values().stream().anyMatch(request -> now - request.created > options.requestTimeoutMillis() * 1_000_000L))
        {
            disconnect(connection, new TimeoutException("Remote response deadline exceeded"));
        }
        outbound.offer(new NetworkFrames.Frame(NetworkFrames.PING, 0, new byte[0]));
    }

    @Override
    public AutoCloseable listen(ObservationQuery query, Duration interval, Consumer<SubscriptionSnapshot> receiver)
    {
        if (closed || subscribers.size() >= 1024)
        {
            throw new IllegalStateException("Remote subscription limit or closed client");
        }
        long id = nextId.incrementAndGet();
        var packet = NetworkFrames.encode(out -> { out.writeInt(Math.toIntExact(interval.toMillis())); out.write(new Read(query).encode()); });
        var subscriber = new Subscriber(id, packet, receiver);
        subscribers.put(id, subscriber);
        if (!outbound.offer(new NetworkFrames.Frame(NetworkFrames.SUBSCRIBE, id, packet)))
        {
            subscribers.remove(id);
            throw new IllegalStateException("Remote subscription queue is full");
        }
        return () -> {
            subscribers.remove(id);
            if (!outbound.offer(new NetworkFrames.Frame(NetworkFrames.UNSUBSCRIBE, id, new byte[0])))
                disconnect(connection, new IOException("Subscription cleanup queue is full"));
        };
    }

    @Override
    public ObservationResult load(long id)
    {
        return slots.get(id);
    }

    @Override
    public void release(long id)
    {
        slots.remove(id);
        outbound.offer(new NetworkFrames.Frame(NetworkFrames.RELEASE_SLOT, nextId.incrementAndGet(), NetworkFrames.encode(out -> out.writeLong(id))));
    }

    @Override
    public CompletableFuture<Void> closeAsync()
    {
        closed = true;
        timer.shutdownNow();
        disconnect(connection, new IOException("Remote client closed"));
        subscribers.clear();
        outbound.clear();
        return CompletableFuture.completedFuture(null);
    }
}
