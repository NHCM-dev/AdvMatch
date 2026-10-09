package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.contract.*;
import nhcm.advmatch.command.order.PlaceOrder;
import nhcm.advmatch.model.*;

import java.math.BigInteger;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.LongAdder;

public final class PerpetualBenchmark
{
    private record Configuration(long orders, int users, int symbols, int producers, int batchSize, int publishBatch, int ringSize, long latencySample, MarginMode marginMode, int leverage, long makerFee, long takerFee)
    {
    }

    public static void main(String[] arguments)
    {
        var options = new Options(arguments, Set.of("--orders", "--warmup", "--users", "--symbols", "--producers", "--batch-size", "--ring-size", "--publish-batch", "--latency-sample", "--margin-mode", "--leverage", "--maker-fee-rate", "--taker-fee-rate"));
        var users = options.integer("--users", 1024, 2, 10_000_000);
        BenchmarkContext.require(users % 2 == 0, "users must be even");
        var mode = options.text("--margin-mode", "isolated");
        BenchmarkContext.require(mode.equals("isolated") || mode.equals("cross"), "margin-mode must be isolated or cross");
        var batchSize = options.integer("--batch-size", 256, 1, 65536);
        var publishBatch = options.integer("--publish-batch", Math.min(batchSize, 256), 1, Math.min(batchSize, 1024));
        var config = new Configuration(options.number("--orders", 100_000, 1, 10_000_000), users, options.integer("--symbols", 1, 1, 100_000), options.integer("--producers", 4, 1, 256), batchSize, publishBatch, options.ringSize(32), options.number("--latency-sample", 1024, 0, Long.MAX_VALUE), mode.equals("cross") ? MarginMode.Cross : MarginMode.Isolated, options.integer("--leverage", 10, 1, 100), options.number("--maker-fee-rate", 100, 0, 1_000_000), options.number("--taker-fee-rate", 500, 0, 1_000_000));
        var warmup = options.number("--warmup", 1000, 0, 10_000_000);
        if (warmup > 0)
        {
            run(new Configuration(warmup, config.users(), config.symbols(), config.producers(), config.batchSize(), config.publishBatch(), config.ringSize(), 0, config.marginMode(), config.leverage(), config.makerFee(), config.takerFee()), false);
        }

        run(config, true);
    }

    private static void run(Configuration config, boolean report)
    {
        try (var context = new BenchmarkContext(config.ringSize(), config.producers(), config.batchSize(), false))
        {
            var setup = System.nanoTime();
            context.asset("UNUSED");
            var quote = context.asset("USD");
            var deposit = Math.multiplyExact((config.orders() / (config.users() / 2) + 1) * 3 + 2, 100_000);
            var accounts = new long[config.users()];
            for (int index = 0; index < config.users(); ++index)
            {
                var user = context.execute(new CreateUser()).id();
                BenchmarkContext.require(user == index + 1, "unexpected user ID");
                accounts[index] = context.account(user, true);
                context.credit(accounts[index], quote, deposit);
            }

            var spec = new ContractSpec().withFirstFundingTimestamp(1000).withDefaultLeverage(config.leverage()).withMakerFeeRate(config.makerFee()).withTakerFeeRate(config.takerFee());
            for (int index = 0; index < config.symbols(); ++index)
            {
                var base = context.asset("BASE" + index);
                var symbol = context.execute(new CreateSymbol("PERP" + index, SymbolType.Perpetual, base, quote)).id();
                context.execute(new AddContractBook(symbol, spec, 100_000, 0));
            }

            var setupSeconds = Metrics.seconds(setup);
            var latencies = Collections.synchronizedList(new ArrayList<Long>());
            var preload = System.nanoTime();
            orders(context, config, 0, config.orders(), true, latencies);
            var preloadSeconds = Metrics.seconds(preload);
            var elapsed = Metrics.concurrent(config.orders(), config.producers(), (first, count) -> orders(context, config, first, count, false, latencies));
            var markStart = System.nanoTime();
            for (int symbol = 1; symbol <= config.symbols(); ++symbol)
            {
                context.execute(new UpdateMarkPrice(symbol, 100_000, 2));
            }

            var markSeconds = Metrics.seconds(markStart);
            var fundingStart = System.nanoTime();
            long funded = 0;
            for (int symbol = 1; symbol <= config.symbols(); ++symbol)
            {
                funded += context.execute(new SettleFunding(symbol, 1000, 1000)).result().affected();
            }

            var fundingSeconds = Metrics.seconds(fundingStart);
            BenchmarkContext.require(funded == config.orders() * 2, "funding position count mismatch");
            audit(context, config, deposit);
            if (report)
            {
                System.out.printf("scenario=perpetual_preloaded_asks_then_limit_bids measurement=java_api_end_to_end orders=%d users=%d symbols=%d producers=%d ring_size=%d batch_size=%d native_publish_batch=%d margin_mode=%s leverage=%d maker_fee_rate=%d taker_fee_rate=%d%n", config.orders(), config.users(), config.symbols(), config.producers(), config.ringSize(), config.batchSize(), config.publishBatch(), config.marginMode(), config.leverage(), config.makerFee(), config.takerFee());
                System.out.printf("setup_seconds=%.6f preload_seconds=%.6f elapsed_seconds=%.6f trade_tps=%.0f mark_seconds=%.6f funding_seconds=%.6f funded_positions=%d funding_positions_s=%.0f validation=passed%n", setupSeconds, preloadSeconds, elapsed, config.orders() / elapsed, markSeconds, fundingSeconds, funded, funded / fundingSeconds);
                Metrics.latency(new ArrayList<>(latencies));
                Metrics.memory();
            }
        }
    }

    private static void orders(BenchmarkContext context, Configuration config, long first, long count, boolean maker, List<Long> latencies)
    {
        var pending = new ArrayList<CompletableFuture<CommandReceipt>>(config.batchSize());
        var commands = new ArrayList<PlaceOrder>(config.publishBatch());
        var timestamps = new ArrayList<Long>(config.publishBatch());
        var orderOptions = new ContractOrderOptions().withLeverage(config.leverage()).withMarginMode(config.marginMode());
        for (long offset = 0; offset < count; ++offset)
        {
            var index = first + offset;
            var user = index % (config.users() / 2) * 2 + (maker ? 1 : 2);
            var sampled = !maker && config.latencySample() > 0 && index % config.latencySample() == 0;
            timestamps.add(sampled ? System.nanoTime() : 0);
            commands.add(new PlaceOrder(user, index % config.symbols() + 1, maker ? OrderSide.Ask : OrderSide.Bid, OrderType.Limit, maker ? TimeInForce.GTC : TimeInForce.IOC, 100_000, 1, 1, orderOptions));
            if (commands.size() == config.publishBatch() || pending.size() + commands.size() == config.batchSize() || offset + 1 == count)
            {
                var futures = context.submitBatch(commands);
                for (int entry = 0; entry < futures.size(); ++entry)
                {
                    var future = futures.get(entry);
                    var started = timestamps.get(entry);
                    if (started != 0)
                    {
                        future = future.thenApply(value ->
                        {
                            latencies.add(System.nanoTime() - started);
                            return value;
                        });
                    }

                    pending.add(future);
                }

                commands.clear();
                timestamps.clear();
            }

            if (pending.size() == config.batchSize() || offset + 1 == count)
            {
                BenchmarkContext.await(pending, value -> BenchmarkContext.require(value.result().orderStatus() == (maker ? OrderStatus.NotFilled : OrderStatus.Filled), "unexpected order status"));
            }
        }
    }

    private static void audit(BenchmarkContext context, Configuration config, long deposit)
    {
        var cash = new BigInteger[]{BigInteger.ZERO};
        context.rows(new ObservationQuery().withKind(ReadKind.Balances), BalanceView.class, value ->
        {
            BenchmarkContext.require(value.active() >= 0 && value.reserved() >= 0, "negative balance");
            cash[0] = cash[0].add(BigInteger.valueOf(value.active())).add(BigInteger.valueOf(value.reserved()));
        });
        var longs = new LongAdder();
        var shorts = new LongAdder();
        var funding = new BigInteger[]{BigInteger.ZERO};
        var positions = new LongAdder();
        context.rows(new ObservationQuery().withKind(ReadKind.Positions), PositionView.class, value ->
        {
            BenchmarkContext.require(!value.closed() && !value.liquidating() && value.size() == 1 && value.leverage() == config.leverage() && value.marginMode() == config.marginMode(), "invalid position");
            (value.side() == PositionSide.Long ? longs : shorts).add(value.size());
            funding[0] = funding[0].add(BigInteger.valueOf(value.fundingPnL()));
            positions.increment();
        });
        context.rows(new ObservationQuery().withKind(ReadKind.Markets), MarketView.class, value ->
        {
            cash[0] = cash[0].add(BigInteger.valueOf(value.insuranceBalance())).add(BigInteger.valueOf(value.clearingBalance())).add(BigInteger.valueOf(value.feeBalance())).add(BigInteger.valueOf(value.fundingRoundingBalance()));
            BenchmarkContext.require(value.pendingLiquidations() == 0, "unexpected liquidation");
        });
        BenchmarkContext.require(longs.sum() == config.orders() && shorts.sum() == config.orders() && positions.sum() == config.orders() * 2 && funding[0].signum() == 0, "position/funding conservation failed");
        BenchmarkContext.require(cash[0].equals(BigInteger.valueOf(deposit).multiply(BigInteger.valueOf(config.users()))), "cash conservation failed");
    }
}
