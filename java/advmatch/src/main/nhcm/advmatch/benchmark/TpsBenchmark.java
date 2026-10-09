package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.order.*;
import nhcm.advmatch.command.read.*;
import nhcm.advmatch.model.*;

import java.util.ArrayList;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.LongAdder;

public final class TpsBenchmark
{
    public static void main(String[] arguments)
    {
        var options = new Options(arguments, Set.of("--orders", "--producers", "--batch-size", "--ring-size", "--publish-batch"));
        var orders = options.number("--orders", 1_000_000, 1, Long.MAX_VALUE / 100);
        var producers = options.integer("--producers", 4, 1, 256);
        var batchSize = options.integer("--batch-size", 256, 1, 65536);
        var ringSize = options.ringSize(32);
        var publishBatch = options.integer("--publish-batch", Math.min(batchSize, 256), 1, Math.min(batchSize, 1024));
        try (var context = new BenchmarkContext(ringSize, producers, batchSize, false))
        {
            var base = context.asset("BASE");
            var quote = context.asset("QUOTE");
            var seller = context.execute(new CreateUser()).id();
            var buyer = context.execute(new CreateUser()).id();
            var sellerAccount = context.account(seller, false);
            var buyerAccount = context.account(buyer, false);
            context.credit(sellerAccount, base, orders);
            context.credit(buyerAccount, quote, Math.multiplyExact(orders, 100));
            var symbol = context.execute(new CreateSymbol("BASE/QUOTE", SymbolType.Spot, base, quote)).id();
            context.execute(new AddSpotBook(symbol));
            var preload = System.nanoTime();
            var pending = new ArrayList<CompletableFuture<CommandReceipt>>(batchSize);
            var commands = new ArrayList<PlaceOrder>(publishBatch);
            for (long index = 0; index < orders; ++index)
            {
                commands.add(new PlaceOrder(seller, symbol, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, index + 1));
                if (commands.size() == publishBatch || pending.size() + commands.size() == batchSize || index + 1 == orders)
                {
                    pending.addAll(context.submitBatch(commands));
                    commands.clear();
                }
                if (pending.size() == batchSize || index + 1 == orders)
                {
                    BenchmarkContext.await(pending, value -> BenchmarkContext.require(value.result().orderStatus() == OrderStatus.NotFilled, "preload filled unexpectedly"));
                }
            }

            var preloadSeconds = Metrics.seconds(preload);
            var filled = new LongAdder();
            var elapsed = Metrics.concurrent(orders, producers, (first, count) ->
            {
                var responses = new ArrayList<CompletableFuture<CommandReceipt>>(batchSize);
                var batch = new ArrayList<PlaceOrder>(publishBatch);
                for (long index = 0; index < count; ++index)
                {
                    batch.add(new PlaceOrder(buyer, symbol, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, first + index + 1));
                    if (batch.size() == publishBatch || responses.size() + batch.size() == batchSize || index + 1 == count)
                    {
                        responses.addAll(context.submitBatch(batch));
                        batch.clear();
                    }
                    if (responses.size() == batchSize || index + 1 == count)
                    {
                        BenchmarkContext.await(responses, value ->
                        {
                            BenchmarkContext.require(value.result().orderStatus() == OrderStatus.Filled && value.result().filled() == 1, "taker did not fill");
                            filled.increment();
                        });
                    }
                }
            });

            BenchmarkContext.require(filled.sum() == orders, "fill count mismatch");
            BenchmarkContext.require(context.submit(new ReadBook(new ObservationQuery().withSymbolId(symbol))).join().values().getFirst().bestAsk() == null, "asks remain");
            verify(context, sellerAccount, base, 0);
            verify(context, sellerAccount, quote, orders * 100);
            verify(context, buyerAccount, base, orders);
            verify(context, buyerAccount, quote, 0);
            System.out.printf("scenario=preloaded_asks_then_limit_bids measurement=java_api_end_to_end orders=%d producers=%d batch_size=%d ring_size=%d native_publish_batch=%d%n", orders, producers, batchSize, ringSize, publishBatch);
            System.out.printf("preload_seconds=%.6f matched=%d elapsed_seconds=%.6f trade_tps=%.0f validation=passed%n", preloadSeconds, filled.sum(), elapsed, filled.sum() / elapsed);
            Metrics.memory();
        }
    }

    private static void verify(BenchmarkContext context, long account, long asset, long expected)
    {
        var result = context.submit(new ReadBalance(new ObservationQuery().withAccountId(account).withAssetId(asset))).join();
        BenchmarkContext.require(result.page().state() == ReadState.Success && result.values().getFirst().active() == expected && result.values().getFirst().reserved() == 0, "balance mismatch");
    }
}
