package nhcm.advmatch.object;

import nhcm.advmatch.Exchange;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;

// An owner + ID reference, not a C++ pointer. Reads return immutable snapshots.
// Removing/archiving an entity makes later operations fail in the native engine.
public abstract class Entity<T>
{
    protected final Exchange exchange;
    private final long id;

    protected Entity(Exchange exchange, long id)
    {
        this.exchange = Objects.requireNonNull(exchange);
        if (id == 0)
        {
            throw new IllegalArgumentException("Entity ID must be nonzero");
        }

        this.id = id;
    }

    public final long id()
    {
        return this.id;
    }

    public final long handle()
    {
        return this.id;
    }

    public final void requireOwner(Exchange owner)
    {
        if (this.exchange != owner)
        {
            throw new IllegalArgumentException("Handle belongs to another Exchange");
        }
    }

    public abstract CompletableFuture<T> snapshot();

    @Override
    public final boolean equals(Object value)
    {
        return value != null && value.getClass() == this.getClass() && ((Entity<?>) value).exchange == this.exchange && ((Entity<?>) value).id == this.id;
    }

    @Override
    public final int hashCode()
    {
        return Objects.hash(System.identityHashCode(this.exchange), this.getClass(), this.id);
    }
}
