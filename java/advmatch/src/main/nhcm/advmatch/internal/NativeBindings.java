package nhcm.advmatch.internal;

public final class NativeBindings
{
    static
    {
        NativeLoader.load();
        if (abiVersion() != 4)
        {
            throw new UnsatisfiedLinkError("AdvMatch requires native ABI 4");
        }
    }

    private NativeBindings()
    {
    }

    // Bound by JNI_OnLoad/RegisterNatives, not Java_* symbol names.
    public static native int abiVersion();
    public static native long create(byte[] config);
    public static native long submit(long exchange, byte[] command);
    public static native long[] submitBatch(long exchange, byte[] commands, int count);
    public static native byte[] drain(long exchange, int limit);
    public static native boolean awaitResponses(long exchange, int timeoutMillis);
    public static native byte[] poll(long exchange, long request);
    public static native boolean cancel(long exchange, long request);
    public static native long close(long exchange);
    public static native void release(long exchange, long request);
    public static native void destroy(long exchange);
    public static native byte[] loadSlot(long exchange, long slot);
    public static native void releaseSlot(long exchange, long slot);
}
