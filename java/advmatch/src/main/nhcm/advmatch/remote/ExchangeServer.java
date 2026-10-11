package nhcm.advmatch.remote;

import nhcm.advmatch.*;
import nhcm.advmatch.command.read.Read;
import nhcm.advmatch.internal.Wire;
import nhcm.advmatch.internal.WireReader;
import nhcm.advmatch.model.*;

import javax.net.ServerSocketFactory;
import javax.net.ssl.SSLContext;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Consumer;
import java.util.function.BiConsumer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** One native owner, multiple trusted Minecraft gateways. Does not own the Exchange lifecycle. */
public class ExchangeServer implements AutoCloseable
{
    public record Options(String bindAddress, int port, String token, String exchangeId,
                          boolean allowPlaintextNetwork, int maxConnections, int maxInFlight, int maxFrameBytes)
    {
        public Options
        {
            if (token == null || token.length() < 32 || exchangeId == null || exchangeId.isBlank()
                || port < 0 || port > 65535 || maxConnections < 1 || maxConnections > 256
                || maxInFlight < 1 || maxInFlight > 4096 || maxFrameBytes < 1024 || maxFrameBytes > 16777216)
            {
                throw new IllegalArgumentException("Invalid exchange endpoint options");
            }
        }

        @Override
        public String toString()
        {
            return "ExchangeServer.Options[bindAddress=" + bindAddress + ", port=" + port + "]";
        }
    }

    public record ClientContext(String instanceId, String connectionId, AtomicBoolean active)
    {
        public ClientContext(String instanceId)
        {
            this(instanceId, UUID.randomUUID().toString(), new AtomicBoolean(true));
        }
    }

    public interface ExtensionHandler
    {
        CompletableFuture<byte[]> handle(ClientContext client, int operation, byte[] payload);
    }

    private final Exchange exchange;
    private final Options options;
    private final ServerSocket listener;
    private final Set<Connection> connections = ConcurrentHashMap.newKeySet();
    private final Map<String, Connection> activeInstances = new ConcurrentHashMap<>();
    private final Semaphore capacity;
    private volatile boolean closed;
    private volatile ExtensionHandler extensions;
    private volatile Consumer<ClientContext> disconnected = ignored -> {};
    private volatile BiConsumer<ClientContext, byte[]> validator = (context, packet) -> {};
    private final Set<String> reservedInstances = ConcurrentHashMap.newKeySet();

    public ExchangeServer(Exchange exchange, Options options, SSLContext tls) throws IOException
    {
        this.exchange = Objects.requireNonNull(exchange);
        this.options = options;
        var address = InetAddress.getByName(options.bindAddress());
        if (tls == null && !address.isLoopbackAddress() && !options.allowPlaintextNetwork())
        {
            throw new IllegalArgumentException("Non-loopback endpoints require TLS or explicit trusted-network plaintext configuration");
        }
        capacity = new Semaphore(options.maxConnections());
        ServerSocketFactory factory = tls == null ? ServerSocketFactory.getDefault() : tls.getServerSocketFactory();
        listener = factory.createServerSocket();
        listener.setReuseAddress(true);
        listener.bind(new InetSocketAddress(address, options.port()));
        Thread.ofPlatform().daemon().name("advmatch-endpoint").start(this::accept);
    }

    public ExchangeServer(Exchange exchange, Options options) throws IOException
    {
        this(exchange, options, null);
    }

    public int port()
    {
        return listener.getLocalPort();
    }

    public void extensions(ExtensionHandler handler, Consumer<ClientContext> onDisconnect)
    {
        extensions = Objects.requireNonNull(handler);
        disconnected = Objects.requireNonNull(onDisconnect);
    }

    public void commandValidator(BiConsumer<ClientContext, byte[]> validator)
    {
        this.validator = Objects.requireNonNull(validator);
    }

    public void reserveInstance(String instanceId)
    {
        reservedInstances.add(instanceId);
    }

    private void accept()
    {
        while (!closed)
        {
            try
            {
                Socket socket = listener.accept();
                if (!capacity.tryAcquire())
                {
                    socket.close();
                    continue;
                }
                var connection = new Connection(socket);
                connections.add(connection);
                Thread.ofVirtual().name("advmatch-client").start(connection::read);
            }
            catch (IOException error)
            {
                if (!closed)
                {
                    System.getLogger(ExchangeServer.class.getName()).log(System.Logger.Level.WARNING, "Exchange accept failed", error);
                }
            }
        }
    }

    private class Connection implements AutoCloseable
    {
        private final Socket socket;
        private final ArrayBlockingQueue<NetworkFrames.Frame> outbound = new ArrayBlockingQueue<>(256);
        private final AtomicLong queuedBytes = new AtomicLong();
        private final Semaphore inFlight = new Semaphore(options.maxInFlight());
        private final Map<Long, ObservationSubscription> subscriptions = new ConcurrentHashMap<>();
        private final Set<Long> slots = ConcurrentHashMap.newKeySet();
        private volatile boolean ended;
        private ClientContext context;

        Connection(Socket socket)
        {
            this.socket = socket;
        }

        void read()
        {
            try
            {
                socket.setTcpNoDelay(true);
                socket.setSoTimeout(30000);
                var input = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
                var output = new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
                var hello = NetworkFrames.read(input, options.maxFrameBytes());
                if (hello.type() != NetworkFrames.HELLO)
                {
                    throw new IOException("Missing handshake");
                }
                var body = NetworkFrames.input(hello.payload());
                int version = body.readInt();
                String node = NetworkFrames.string(body, 64);
                String token = NetworkFrames.string(body, 1024);
                String expected = NetworkFrames.string(body, 128);
                if (version != NetworkFrames.VERSION || !node.matches("[a-zA-Z0-9_-]{1,64}") || body.available() != 0
                    || !MessageDigest.isEqual(token.getBytes(StandardCharsets.UTF_8), options.token().getBytes(StandardCharsets.UTF_8))
                    || !expected.isEmpty() && !expected.equals(options.exchangeId()))
                {
                    throw new IOException("Handshake rejected");
                }
                if (reservedInstances.contains(node) || activeInstances.putIfAbsent(node, this) != null)
                {
                    throw new IOException("Instance ID is already connected");
                }
                context = new ClientContext(node);
                NetworkFrames.write(output, new NetworkFrames.Frame(NetworkFrames.RESPONSE, 0,
                        NetworkFrames.encode(out -> { out.writeInt(NetworkFrames.VERSION); NetworkFrames.string(out, options.exchangeId()); })), options.maxFrameBytes());
                Thread.ofVirtual().name("advmatch-client-writer").start(() -> write(output));
                while (!ended)
                {
                    var frame = NetworkFrames.read(input, options.maxFrameBytes());
                    if (!inFlight.tryAcquire())
                    {
                        error(frame.id(), new NativeException(NativeException.QUEUE_FULL, "Endpoint request limit reached"));
                        continue;
                    }
                    // Admit commands in wire order; completion remains asynchronous.
                    handle(frame);
                }
            }
            catch (IOException | RuntimeException ignored)
            {
                // Authentication and request payloads must never be logged.
            }
            finally
            {
                close();
            }
        }

        private void handle(NetworkFrames.Frame frame)
        {
            CompletableFuture<byte[]> result;
            try
            {
                switch (frame.type())
                {
                    case NetworkFrames.COMMAND ->
                    {
                        validator.accept(context, frame.payload());
                        if (frame.payload().length < 3)
                        {
                            throw new IllegalArgumentException("Truncated binding command");
                        }
                        int operation = frame.payload()[2] & 255;
                        if (operation == 7)
                        {
                            var reader = new WireReader(frame.payload());
                            reader.skip(3);
                            if (!slots.contains(reader.u64()))
                            {
                                throw new IllegalArgumentException("Observation slot belongs to another connection");
                            }
                        }
                        result = exchange.submit(new RawCommand(frame.payload()));
                        if (operation == 6)
                        {
                            result = result.thenApply(bytes -> {
                                var reader = WireReader.response(bytes, 6);
                                long slot = reader.u64();
                                synchronized (Connection.this)
                                {
                                    if (ended)
                                    {
                                        exchange.releaseSlot(slot);
                                    }
                                    else
                                    {
                                        slots.add(slot);
                                    }
                                }
                                return bytes;
                            });
                        }
                    }
                    case NetworkFrames.EXTENSION ->
                    {
                        var reader = NetworkFrames.input(frame.payload());
                        int operation = reader.readInt();
                        var handler = extensions;
                        if (handler == null)
                        {
                            throw new IllegalStateException("Application extension is unavailable");
                        }
                        result = handler.handle(context, operation, reader.readAllBytes());
                    }
                    case NetworkFrames.SUBSCRIBE ->
                    {
                        var reader = NetworkFrames.input(frame.payload());
                        int interval = reader.readInt();
                        var encoded = new WireReader(reader.readAllBytes());
                        if (encoded.u16() != 1 || encoded.u8() != 1)
                        {
                            throw new IllegalArgumentException("Subscription must be an immutable read");
                        }
                        var query = Wire.readObservationQuery(encoded);
                        encoded.finish();
                        if (subscriptions.size() >= 1024 || subscriptions.containsKey(frame.id()))
                        {
                            throw new IllegalStateException("Connection subscription limit or duplicate ID");
                        }
                        var subscription = exchange.subscriptions().subscribe(query, Duration.ofMillis(interval), snapshot -> {
                            byte[] wire = snapshot.wire();
                            var payload = NetworkFrames.encode(out -> {
                                out.writeBoolean(snapshot.state() == SubscriptionSnapshot.State.LIVE && wire != null);
                                if (wire != null && snapshot.state() == SubscriptionSnapshot.State.LIVE)
                                {
                                    out.write(wire);
                                }
                            });
                            send(new NetworkFrames.Frame(NetworkFrames.UPDATE, frame.id(), payload));
                        });
                        subscriptions.put(frame.id(), subscription);
                        if (ended)
                        {
                            subscription.close();
                        }
                        result = CompletableFuture.completedFuture(new byte[0]);
                    }
                    case NetworkFrames.UNSUBSCRIBE ->
                    {
                        var subscription = subscriptions.remove(frame.id());
                        if (subscription != null)
                        {
                            subscription.close();
                        }
                        result = CompletableFuture.completedFuture(new byte[0]);
                    }
                    case NetworkFrames.RELEASE_SLOT ->
                    {
                        long slot = NetworkFrames.input(frame.payload()).readLong();
                        if (slots.remove(slot))
                        {
                            exchange.releaseSlot(slot);
                        }
                        result = CompletableFuture.completedFuture(new byte[0]);
                    }
                    case NetworkFrames.PING -> result = CompletableFuture.completedFuture(new byte[0]);
                    default -> throw new IllegalArgumentException("Unknown network operation");
                }
            }
            catch (Throwable failure)
            {
                error(frame.id(), failure);
                inFlight.release();
                return;
            }
            result.whenComplete((bytes, failure) -> {
                try
                {
                    if (failure != null)
                    {
                        error(frame.id(), failure);
                    }
                    else
                    {
                        send(new NetworkFrames.Frame(NetworkFrames.RESPONSE, frame.id(), bytes));
                    }
                }
                finally
                {
                    inFlight.release();
                }
            });
        }

        private void error(long id, Throwable failure)
        {
            while (failure.getCause() != null)
            {
                failure = failure.getCause();
            }
            int status = failure instanceof NativeException nativeError ? nativeError.status() : -1;
            String publicKey = failure.getMessage();
            String message = failure instanceof NativeException ? publicKey
                    : publicKey != null && publicKey.matches("(ui|cli)\\.[A-Za-z0-9_.]+") ? publicKey
                    : "Endpoint operation rejected: " + failure.getClass().getSimpleName();
            send(new NetworkFrames.Frame(NetworkFrames.ERROR, id,
                                         NetworkFrames.encode(out -> { out.writeInt(status); NetworkFrames.string(out, message); })));
        }

        private void send(NetworkFrames.Frame frame)
        {
            if (ended) return;
            long bytes = frame.payload().length + 13L;
            if (queuedBytes.addAndGet(bytes) > 16777216 || frame.payload().length + 9 > options.maxFrameBytes() || !outbound.offer(frame))
            {
                queuedBytes.addAndGet(-bytes);
                close(); // A slow reader loses its connection, never unbounded memory.
            }
        }

        private void write(DataOutputStream output)
        {
            try
            {
                while (!ended)
                {
                    var frame = outbound.poll(1, TimeUnit.SECONDS);
                    if (frame != null)
                    {
                        queuedBytes.addAndGet(-(frame.payload().length + 13L));
                        NetworkFrames.write(output, frame, options.maxFrameBytes());
                    }
                }
            }
            catch (IOException | InterruptedException ignored)
            {
                close();
            }
        }

        @Override
        public synchronized void close()
        {
            if (ended)
            {
                return;
            }
            ended = true;
            try { socket.close(); }
            catch (IOException ignored) {}
            subscriptions.values().forEach(ObservationSubscription::close);
            subscriptions.clear();
            slots.forEach(exchange::releaseSlot);
            slots.clear();
            if (context != null)
            {
                context.active().set(false);
                activeInstances.remove(context.instanceId(), this);
                try { disconnected.accept(context); }
                catch (RuntimeException ignored) {}
            }
            connections.remove(this);
            capacity.release();
        }
    }

    @Override
    public void close()
    {
        closed = true;
        try { listener.close(); }
        catch (IOException ignored) {}
        List.copyOf(connections).forEach(Connection::close);
    }
}
