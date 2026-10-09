package nhcm.advmatch.benchmark.support;

import nhcm.advmatch.MatchingExchange;
import nhcm.advmatch.ObservationSlot;
import nhcm.advmatch.TransportOptions;
import nhcm.advmatch.command.Command;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.account.UpdateBalance;
import nhcm.advmatch.command.read.Read;
import nhcm.advmatch.command.read.ReadUser;
import nhcm.advmatch.command.read.ReadPosition;
import nhcm.advmatch.model.*;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import java.util.concurrent.CompletableFuture;

public final class BenchmarkContext implements AutoCloseable
{
    public final MatchingExchange exchange;
    private final BackpressureSubmitter submitter;

    public BenchmarkContext(int ringSize, int producers, int batchSize, boolean indexed)
    {
        var outstanding = Math.max(4096L, Math.multiplyExact((long) producers, batchSize) * 2 + 16);
        require(outstanding <= 1_000_000, "too many outstanding requests");
        var observation = new ObservationConfig().withIndexUserOpenOrders(indexed).withTradeHistoryCapacity(0).withContractEventCapacity(0).withMaxScan(65536);
        var config = new ExchangeConfig().withRingBufferSize(ringSize).withMaxPendingRequests(outstanding).withObservation(observation).withPersistence(new PersistenceConfig().withEnabled(false));
        this.exchange = new MatchingExchange(config, java.util.concurrent.ForkJoinPool.commonPool(), new TransportOptions(1024, 64, 1000, (int)outstanding));
        this.submitter = new BackpressureSubmitter(ringSize);
    }

    public <T> CompletableFuture<T> submit(Command<T> command)
    {
        return this.submitter.submit(() -> this.exchange.submit(command));
    }

    public <T> List<CompletableFuture<T>> submitBatch(List<? extends Command<T>> commands)
    {
        var owned = List.copyOf(commands);
        return this.submitter.submitBatch(owned.size(), () -> this.exchange.submitBatch(owned), index -> this.exchange.submit(owned.get(index)));
    }

    public CompletableFuture<ObservationSlot> createObservationSlot(ObservationQuery query)
    {
        return this.submitter.submit(() -> this.exchange.createObservationSlot(query));
    }

    public CompletableFuture<ObservationResult> refresh(ObservationSlot slot)
    {
        return this.submitter.submit(slot::refresh);
    }

    public PositionView position(long id)
    {
        return this.submit(new ReadPosition(new ObservationQuery().withPositionId(id))).join().values().getFirst();
    }

    public static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new IllegalStateException(message);
        }
    }

    public static CommandReceipt accepted(Receipt receipt)
    {
        require(receipt.result().accepted(), "rejected " + receipt.result().type() + " state=" + receipt.result().state());
        return receipt.receipt();
    }

    public <T extends Receipt> T execute(Command<T> command)
    {
        var result = this.submit(command).join();
        accepted(result);
        return result;
    }

    public long asset(String name)
    {
        return this.execute(new CreateAsset(name, name, 0)).id();
    }

    // Account IDs come from the actual UserView, never userId * 2 assumptions.
    public long account(long userId, boolean contract)
    {
        var value = this.submit(new ReadUser(new ObservationQuery().withUserId(userId))).join().values().getFirst();
        return contract ? value.contractAccountId() : value.spotAccountId();
    }

    public void credit(long accountId, long assetId, long amount)
    {
        this.execute(new UpdateBalance(accountId, assetId, amount));
    }

    public <T> void rows(ObservationQuery query, Class<T> type, Consumer<T> consume)
    {
        var next = query.withLimit(1024).withScanBudget(65536);
        while (true)
        {
            var response = this.submit(new Read(next)).join().typed(type);
            require(response.page().state() == ReadState.Success, "read rejected " + response.page().state());
            response.values().forEach(consume);
            if (!response.page().hasMore())
            {
                return;
            }

            next = next.withCursor(response.page().next());
        }
    }

    public <T> List<T> rows(ObservationQuery query, Class<T> type)
    {
        var result = new ArrayList<T>();
        this.rows(query, type, result::add);
        return List.copyOf(result);
    }

    public static void await(List<CompletableFuture<CommandReceipt>> pending, Consumer<CommandReceipt> consume)
    {
        for (var future : pending)
        {
            var result = future.join();
            accepted(result);
            consume.accept(result);
        }

        pending.clear();
    }

    @Override
    public void close()
    {
        this.submitter.close();
        try
        {
            this.exchange.close();
        }
        finally
        {
            this.submitter.report();
        }
    }
}
