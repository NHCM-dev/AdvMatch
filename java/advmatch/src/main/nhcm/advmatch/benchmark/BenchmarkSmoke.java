package nhcm.advmatch.benchmark;

public final class BenchmarkSmoke
{
    public static void main(String[] arguments)
    {
        TpsBenchmark.main(new String[]{"--orders", "200", "--producers", "2", "--ring-size", "32", "--batch-size", "16"});
        // Logical batches larger than the shared ring must apply backpressure.
        TpsBenchmark.main(new String[]{"--orders", "1000", "--producers", "4", "--ring-size", "1", "--batch-size", "256"});
        ScaleBenchmark.main(new String[]{"--users", "32", "--accounts", "128", "--symbols", "4", "--pending-orders", "64", "--cycles", "50", "--rounds", "2", "--producers", "2", "--batch-size", "8"});
        PerpetualBenchmark.main(new String[]{"--orders", "100", "--warmup", "20", "--users", "16", "--symbols", "2", "--producers", "2", "--batch-size", "16", "--latency-sample", "4"});
        PerpetualBenchmark.main(new String[]{"--orders", "100", "--warmup", "0", "--users", "16", "--symbols", "2", "--producers", "2", "--batch-size", "16", "--margin-mode", "cross"});
        LiquidationBenchmark.main(new String[]{"--users", "24", "--positions-per-user", "2", "--symbols", "4", "--waves", "4", "--levels", "2"});
        LiquidationBenchmark.main(new String[]{"--users", "24", "--positions-per-user", "2", "--symbols", "4", "--waves", "4", "--levels", "2", "--trades-per-wave", "32"});
        LiquidationBenchmark.main(new String[]{"--users", "24", "--positions-per-user", "2", "--symbols", "4", "--waves", "4", "--levels", "2", "--direction", "up", "--book", "tree"});
        LiquidationBenchmark.main(new String[]{"--users", "24", "--positions-per-user", "2", "--symbols", "4", "--waves", "3", "--depth-percent", "0", "--recover", "0", "--candidate-budget", "2"});
        ObserveBenchmark.main(new String[]{"--orders", "100", "--queries", "50"});
        BigAllocsBenchmark.main(new String[]{"--objects", "1000", "--warmup", "100", "--rounds", "2"});
        System.out.println("All six Java benchmark categories passed");
    }
}
