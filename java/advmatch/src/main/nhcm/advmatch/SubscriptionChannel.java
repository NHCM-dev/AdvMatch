package nhcm.advmatch;

import nhcm.advmatch.model.ObservationQuery;

import java.time.Duration;
import java.util.function.Consumer;

public interface SubscriptionChannel
{
    AutoCloseable listen(ObservationQuery query, Duration interval, Consumer<SubscriptionSnapshot> receiver);
}
