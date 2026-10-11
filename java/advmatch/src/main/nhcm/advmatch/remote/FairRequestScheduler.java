package nhcm.advmatch.remote;

import nhcm.advmatch.NativeException;

import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Supplier;

/** Bounded gateway admission. Trading commands keep their order within each gateway. */
class FairRequestScheduler implements AutoCloseable
{
    private static class Gateway
    {
        final ArrayDeque<Job> trading = new ArrayDeque<>();
        final ArrayDeque<Job> reads = new ArrayDeque<>();
        int running;
    }

    private record Job(boolean read, Supplier<CompletableFuture<byte[]>> work, CompletableFuture<byte[]> result) {}
    record Metrics(int queued, int running, int runningReads, long admitted, long rejected) {}

    private final Map<String, Gateway> gateways = new LinkedHashMap<>();
    private final ExecutorService dispatcher = Executors.newSingleThreadExecutor(
            Thread.ofPlatform().daemon().name("advmatch-admission").factory());
    private int queued, running, runningReads;
    private long admitted, rejected;
    private String last = "";
    private boolean scheduled, closed;

    synchronized CompletableFuture<byte[]> submit(String gateway, boolean read, Supplier<CompletableFuture<byte[]>> work)
    {
        var state = gateways.computeIfAbsent(gateway, ignored -> new Gateway());
        if (closed || queued >= 4096 || state.trading.size() + state.reads.size() >= 256)
        {
            rejected++;
            if (state.running == 0 && state.trading.isEmpty() && state.reads.isEmpty()) gateways.remove(gateway);
            return CompletableFuture.failedFuture(new NativeException(NativeException.QUEUE_FULL, "Gateway admission limit reached"));
        }
        var result = new CompletableFuture<byte[]>();
        (read ? state.reads : state.trading).addLast(new Job(read, work, result));
        queued++;
        wake();
        return result;
    }

    synchronized Metrics metrics()
    {
        return new Metrics(queued, running, runningReads, admitted, rejected);
    }

    private void wake()
    {
        if (!scheduled && !closed)
        {
            scheduled = true;
            dispatcher.execute(this::drain);
        }
    }

    private void drain()
    {
        while (true)
        {
            Job job = null;
            Gateway selected = null;
            String selectedKey = null;
            synchronized (this)
            {
                if (!closed && running < 128)
                {
                    var entries = new ArrayList<>(gateways.entrySet());
                    int start = 0;
                    for (int index = 0; index < entries.size(); index++)
                    {
                        if (entries.get(index).getKey().equals(last)) start = index + 1;
                    }
                    for (int index = 0; index < entries.size(); index++)
                    {
                        var entry = entries.get((start + index) % entries.size());
                        var state = entry.getValue();
                        if (state.running >= 8) continue;
                        if (!state.trading.isEmpty()) job = state.trading.removeFirst();
                        else if (runningReads < 32 && !state.reads.isEmpty()) job = state.reads.removeFirst();
                        if (job == null) continue;
                        selected = state;
                        selectedKey = entry.getKey();
                        last = selectedKey;
                        queued--;
                        running++;
                        state.running++;
                        if (job.read()) runningReads++;
                        admitted++;
                        break;
                    }
                }
                if (job == null)
                {
                    scheduled = false;
                    return;
                }
            }
            execute(selectedKey, selected, job);
        }
    }

    private void execute(String key, Gateway state, Job job)
    {
        CompletableFuture<byte[]> work;
        try
        {
            work = job.work().get();
        }
        catch (Throwable failure)
        {
            work = CompletableFuture.failedFuture(failure);
        }
        work.whenComplete((value, failure) -> {
            synchronized (this)
            {
                running--;
                state.running--;
                if (job.read()) runningReads--;
                if (state.running == 0 && state.trading.isEmpty() && state.reads.isEmpty()) gateways.remove(key, state);
                wake();
            }
            if (failure == null) job.result().complete(value);
            else job.result().completeExceptionally(failure);
        });
    }

    @Override
    public synchronized void close()
    {
        if (closed) return;
        closed = true;
        var error = new IllegalStateException("Exchange endpoint closed before command admission");
        for (var state : gateways.values())
        {
            state.trading.forEach(job -> job.result().completeExceptionally(error));
            state.reads.forEach(job -> job.result().completeExceptionally(error));
            state.trading.clear();
            state.reads.clear();
        }
        queued = 0;
        dispatcher.shutdown();
    }
}
