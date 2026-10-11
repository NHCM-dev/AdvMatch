package nhcm.advmatch.remote;

/** A lost response is explicitly different from a rejected command. */
public class RemoteFailure extends RuntimeException
{
    private final boolean mayHaveExecuted;

    public RemoteFailure(String message, boolean mayHaveExecuted, Throwable cause)
    {
        super(message, cause);
        this.mayHaveExecuted = mayHaveExecuted;
    }

    public boolean mayHaveExecuted()
    {
        return mayHaveExecuted;
    }
}
