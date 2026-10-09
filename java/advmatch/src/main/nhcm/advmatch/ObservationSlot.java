package nhcm.advmatch;

import nhcm.advmatch.command.read.RefreshObservation;
import nhcm.advmatch.model.ObservationResult;
import java.util.concurrent.CompletableFuture;

public final class ObservationSlot implements AutoCloseable
{
    private final Exchange exchange;
    private final long id;
    private ObservationResult cached;
    private boolean closed;

    ObservationSlot(Exchange exchange, long id)
    {
        this.exchange = exchange;
        this.id = id;
    }

    public synchronized ObservationResult load()
    {
        if (!this.closed)
        {
            var image = this.exchange.loadSlot(this.id);
            if (image != null)
            {
                this.cached = image;
            }
        }

        return this.cached;
    }

    public synchronized CompletableFuture<ObservationResult> refresh()
    {
        if (this.closed)
        {
            return CompletableFuture.failedFuture(new IllegalStateException("Observation slot is closed"));
        }

        return this.exchange.submit(new RefreshObservation(this.id)).thenApply(image ->
        {
            synchronized (this)
            {
                this.cached = image;
            }

            return image;
        });
    }

    @Override
    public synchronized void close()
    {
        if (!this.closed)
        {
            this.exchange.releaseSlot(this.id);
            this.closed = true;
        }
    }
}
