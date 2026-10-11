package nhcm.advmatch;

import java.util.concurrent.CompletableFuture;

public interface ObservationSubscription extends AutoCloseable
{
    SubscriptionSnapshot latest();

    CompletableFuture<SubscriptionSnapshot> first();

    @Override
    void close();
}
