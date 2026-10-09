package nhcm.advmatch.internal;

public final class BenchmarkNatives
{
    private BenchmarkNatives()
    {
    }

    public static long[] allocator(long objects)
    {
        NativeBindings.abiVersion();
        return measureAllocator(objects);
    }

    private static native long[] measureAllocator(long objects);
}
