package nhcm.advmatch.manager;

import nhcm.advmatch.Exchange;
import java.util.Objects;

public abstract class Manager
{
    protected final Exchange exchange;

    protected Manager(Exchange exchange)
    {
        this.exchange = Objects.requireNonNull(exchange);
    }
}
