package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.internal.BenchmarkNatives;
import java.util.Set;

public final class BigAllocsBenchmark
{
    public static void main(String[] arguments)
    {
        var options = new Options(arguments, Set.of("--objects", "--warmup", "--rounds"));
        var objects = options.number("--objects", 1_000_000, 1, 100_000_000);
        var warmup = options.number("--warmup", 10000, 0, 100_000_000);
        var rounds = options.integer("--rounds", 3, 1, 1000);
        if (warmup > 0)
        {
            BenchmarkNatives.allocator(warmup);
        }

        for (int round = 1; round <= rounds; ++round)
        {
            var started = System.nanoTime();
            var values = BenchmarkNatives.allocator(objects);
            var elapsed = Metrics.seconds(started);
            BenchmarkContext.require(values[2] == values[3] && values[2] == 9 * objects * (objects - 1), "checksum mismatch");
            System.out.printf("scenario=big_allocs measurement=native_allocator_via_jni objects=%d round=%d pool_seconds=%.6f new_delete_seconds=%.6f pool_ops_s=%.0f new_delete_ops_s=%.0f speedup=%.3f java_wall_seconds=%.6f checksum=%s validation=passed%n", objects, round, values[0] / 1e9, values[1] / 1e9, objects * 2e9 / values[0], objects * 2e9 / values[1], (double) values[1] / values[0], elapsed, Long.toUnsignedString(values[2]));
        }
    }
}
