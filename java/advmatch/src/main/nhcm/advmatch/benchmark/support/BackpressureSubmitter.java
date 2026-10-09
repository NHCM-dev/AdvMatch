package nhcm.advmatch.benchmark.support;

import nhcm.advmatch.NativeException;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.LongAdder;
import java.util.function.Supplier;
import java.util.List;
import java.util.ArrayList;
import java.util.function.IntFunction;

// Benchmark-only blocking admission. The ordinary SDK remains fail-fast.
// Retry only typed QueueFull: the native command has not been executed.
public final class BackpressureSubmitter implements AutoCloseable
{
    private static final long WAIT_NANOS = TimeUnit.SECONDS.toNanos(30);
    private final Semaphore capacity;
    private final ScheduledExecutorService retries;
    private final AtomicBoolean closed = new AtomicBoolean();
    private final LongAdder retryCount = new LongAdder();
    private final LongAdder submitted = new LongAdder();
    private final LongAdder waitNanos = new LongAdder();

    public BackpressureSubmitter(int capacity)
    {
        if (capacity <= 0)
        {
            throw new IllegalArgumentException("capacity must be positive");
        }

        this.capacity = new Semaphore(capacity);
        this.retries = Executors.newSingleThreadScheduledExecutor(task ->
        {
            var thread = new Thread(task, "advmatch-benchmark-backpressure");
            thread.setDaemon(true);
            return thread;
        });
    }

    public <T> CompletableFuture<T> submit(Supplier<CompletableFuture<T>> action)
    {
        var started = System.nanoTime();
        try
        {
            if (!this.capacity.tryAcquire(30, TimeUnit.SECONDS))
            {
                return CompletableFuture.failedFuture(new TimeoutException("Benchmark admission did not progress for 30 seconds"));
            }
        }
        catch (InterruptedException error)
        {
            Thread.currentThread().interrupt();
            return CompletableFuture.failedFuture(error);
        }

        this.waitNanos.add(System.nanoTime() - started);
        this.submitted.increment();
        var response = new CompletableFuture<T>();
        // All attempts share one permit; completed failures never retain tickets.
        response.whenComplete((value, error) -> this.capacity.release());
        this.attempt(action, response, System.nanoTime());
        return response;
    }

    private <T> void attempt(Supplier<CompletableFuture<T>> action, CompletableFuture<T> response, long started)
    {
        if (this.closed.get())
        {
            response.completeExceptionally(new IllegalStateException("Benchmark submitter is closed"));
            return;
        }

        try
        {
            action.get().whenComplete((value, error) ->
            {
                if (error == null)
                {
                    response.complete(value);
                    return;
                }

                var cause = unwrap(error);
                if (!(cause instanceof NativeException nativeError) || nativeError.status() != NativeException.QUEUE_FULL)
                {
                    response.completeExceptionally(cause);
                    return;
                }

                if (System.nanoTime() - started >= WAIT_NANOS)
                {
                    response.completeExceptionally(new TimeoutException("Native queue did not admit this command for 30 seconds"));
                    return;
                }

                this.retryCount.increment();
                try
                {
                    // Deferred retry also prevents recursive completion chains.
                    this.retries.schedule(() -> this.attempt(action, response, started), 50, TimeUnit.MICROSECONDS);
                }
                catch (RuntimeException failure)
                {
                    response.completeExceptionally(failure);
                }
            });
        }
        catch (Throwable error)
        {
            response.completeExceptionally(error);
        }
    }

    public <T> List<CompletableFuture<T>> submitBatch(int count, Supplier<List<CompletableFuture<T>>> action, IntFunction<CompletableFuture<T>> retry)
    {
        var started = System.nanoTime();
        var responses = new ArrayList<CompletableFuture<T>>(count);
        for (int index = 0; index < count; ++index)
        {
            responses.add(new CompletableFuture<>());
        }

        try
        {
            if (!this.capacity.tryAcquire(30, TimeUnit.SECONDS))
            {
                throw new TimeoutException("Benchmark batch admission did not progress for 30 seconds");
            }
        }
        catch (Exception error)
        {
            if (error instanceof InterruptedException)
            {
                Thread.currentThread().interrupt();
            }

            responses.forEach(future -> future.completeExceptionally(error));
            return responses;
        }

        this.waitNanos.add(System.nanoTime() - started);
        this.submitted.add(count);
        CompletableFuture.allOf(responses.toArray(CompletableFuture[]::new)).whenComplete((value, error) -> this.capacity.release());
        try
        {
            if (this.closed.get())
            {
                throw new IllegalStateException("Benchmark submitter is closed");
            }

            var submitted = action.get();
            for (int index = 0; index < count; ++index)
            {
                var response = responses.get(index);
                var offset = index;
                submitted.get(index).whenComplete((value, error) ->
                {
                    var cause = error == null ? null : unwrap(error);
                    if (cause instanceof NativeException nativeError && nativeError.status() == NativeException.QUEUE_FULL)
                    {
                        // Only retry this rejected command, never successful
                        // siblings: batches are ordered but not transactional.
                        this.retryCount.increment();
                        try
                        {
                            this.retries.execute(() -> this.attempt(() -> retry.apply(offset), response, started));
                        }
                        catch (RuntimeException failure)
                        {
                            response.completeExceptionally(failure);
                        }
                    }
                    else if (cause != null)
                    {
                        response.completeExceptionally(cause);
                    }
                    else
                    {
                        response.complete(value);
                    }
                });
            }
        }
        catch (Throwable error)
        {
            responses.forEach(future -> future.completeExceptionally(error));
        }

        return responses;
    }

    private static Throwable unwrap(Throwable error)
    {
        while ((error instanceof CompletionException || error instanceof ExecutionException) && error.getCause() != null)
        {
            error = error.getCause();
        }

        return error;
    }

    public void report()
    {
        System.out.printf("logical_submissions=%d queue_full_retries=%d producer_admission_wait_ms=%.3f%n", this.submitted.sum(), this.retryCount.sum(), this.waitNanos.sum() / 1e6);
    }

    @Override
    public void close()
    {
        this.closed.set(true);
        // Existing delayed tasks still run once, see closed, and finish Futures.
        // shutdownNow would abandon those Futures and leak their permits.
        this.retries.shutdown();
    }
}
