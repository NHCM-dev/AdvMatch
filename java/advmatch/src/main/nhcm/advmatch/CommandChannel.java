package nhcm.advmatch;

import java.util.concurrent.CompletableFuture;

/** An owned wire request/response channel; closing a client never closes its server. */
public interface CommandChannel extends AutoCloseable
{
    CompletableFuture<byte[]> request(byte[] request);

    int outstandingRequests();

    CompletableFuture<Void> closeAsync();

    @Override
    default void close()
    {
        closeAsync().join();
    }
}
