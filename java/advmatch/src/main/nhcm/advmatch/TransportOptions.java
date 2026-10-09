package nhcm.advmatch;

// Java transport tuning is separate from trading/persistence configuration.
// maxOutstanding bounds native requests plus responses awaiting callbacks.
public record TransportOptions(int responseBatchSize, int spinCount, long activeParkNanos, int maxOutstanding)
{
    public TransportOptions()
    {
        this(256, 64, 1_000, 4096);
    }

    public TransportOptions
    {
        if (responseBatchSize < 1 || responseBatchSize > 1024 || spinCount < 0 || activeParkNanos < 1 || maxOutstanding < 1)
        {
            throw new IllegalArgumentException("Invalid transport limits");
        }
    }
}
