package nhcm.advmatch;

import nhcm.advmatch.model.ExchangeConfig;
import java.util.concurrent.Executor;

// Exchange lifecycle/commands with its object managers; one native owner thread.
public final class MatchingExchange extends Exchange
{
    public MatchingExchange()
    {
        super();
    }

    public MatchingExchange(ExchangeConfig config)
    {
        super(config);
    }

    public MatchingExchange(ExchangeConfig config, Executor callbacks)
    {
        super(config, callbacks);
    }

    public MatchingExchange(ExchangeConfig config, Executor callbacks, TransportOptions transport)
    {
        super(config, callbacks, transport);
    }
}
