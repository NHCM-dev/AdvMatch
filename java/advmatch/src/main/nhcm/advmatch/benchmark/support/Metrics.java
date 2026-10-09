package nhcm.advmatch.benchmark.support;

import java.lang.management.ManagementFactory;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.function.BiConsumer;

public final class Metrics
{
    private Metrics()
    {
    }

    public static double seconds(long start)
    {
        return (System.nanoTime() - start) / 1_000_000_000.0;
    }

    public static double concurrent(long count, int producers, BiConsumer<Long, Long> worker)
    {
        try (var threads = Executors.newFixedThreadPool(producers))
        {
            var ready = new CountDownLatch(producers);
            var start = new CountDownLatch(1);
            var finished = new ArrayList<CompletableFuture<Void>>();
            long offset = 0;
            for (int producer = 0; producer < producers; ++producer)
            {
                var first = offset;
                var length = count / producers + (producer < count % producers ? 1 : 0);
                offset += length;
                finished.add(CompletableFuture.runAsync(() ->
                {
                    ready.countDown();
                    try
                    {
                        start.await();
                    }
                    catch (InterruptedException error)
                    {
                        Thread.currentThread().interrupt();
                        throw new IllegalStateException(error);
                    }

                    worker.accept(first, length);
                }, threads));
            }

            ready.await();
            var started = System.nanoTime();
            start.countDown();
            CompletableFuture.allOf(finished.toArray(CompletableFuture[]::new)).join();
            return seconds(started);
        }
        catch (InterruptedException error)
        {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(error);
        }
    }

    public static void latency(List<Long> values)
    {
        Collections.sort(values);
        System.out.printf("response_latency_samples=%d p50_us=%.3f p95_us=%.3f p99_us=%.3f p99_9_us=%.3f max_us=%.3f%n", values.size(), percentile(values, 0.50), percentile(values, 0.95), percentile(values, 0.99), percentile(values, 0.999), values.isEmpty() ? 0 : values.getLast() / 1000.0);
    }

    public static double percentile(List<Long> sorted, double fraction)
    {
        return sorted.isEmpty() ? 0 : sorted.get(Math.max(0, (int) Math.ceil(sorted.size() * fraction) - 1)) / 1000.0;
    }

    public static void memory()
    {
        var runtime = Runtime.getRuntime();
        var gc = ManagementFactory.getGarbageCollectorMXBeans();
        System.out.printf("java_heap_used_mb=%.3f java_heap_committed_mb=%.3f gc_count=%d gc_ms=%d%n", (runtime.totalMemory() - runtime.freeMemory()) / 1048576.0, runtime.totalMemory() / 1048576.0, gc.stream().mapToLong(value -> Math.max(0, value.getCollectionCount())).sum(), gc.stream().mapToLong(value -> Math.max(0, value.getCollectionTime())).sum());
        // JVM heap does not include C++ orders/accounts/books or JNI allocations.
    }
}
