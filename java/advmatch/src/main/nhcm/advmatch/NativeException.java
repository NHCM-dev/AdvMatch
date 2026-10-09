package nhcm.advmatch;

public final class NativeException extends RuntimeException
{
    public static final int QUEUE_FULL = -3;
    private final int status;

    public NativeException(String message)
    {
        this(-1, message);
    }

    public NativeException(int status, String message)
    {
        super(message);
        this.status = status;
    }

    public int status()
    {
        return this.status;
    }
}
