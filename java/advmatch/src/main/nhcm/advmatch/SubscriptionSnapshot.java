package nhcm.advmatch;

import nhcm.advmatch.model.ObservationResult;

/** A stale view remains inspectable, but is never advertised as live data. */
public record SubscriptionSnapshot(State state, ObservationResult result, Throwable error, long receivedAt,
                                   byte[] wire)
{
    public enum State { STARTING, LIVE, STALE, CLOSED }

    public SubscriptionSnapshot
    {
        wire = wire == null ? null : wire.clone();
    }

    @Override
    public byte[] wire()
    {
        return wire == null ? null : wire.clone();
    }
}
