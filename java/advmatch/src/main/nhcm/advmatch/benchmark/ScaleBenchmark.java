package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.order.*;
import nhcm.advmatch.command.read.ReadOverview;
import nhcm.advmatch.model.*;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Set;
import java.util.concurrent.CompletableFuture;

public final class ScaleBenchmark
{
    public static void main(String[] arguments)
    {
        var options = new Options(arguments, Set.of("--profile", "--users", "--accounts", "--symbols", "--pending-orders", "--cycles", "--rounds", "--producers", "--batch-size", "--ring-size", "--force"));
        var profile = options.text("--profile", "smoke");
        BenchmarkContext.require(profile.equals("smoke") || profile.equals("target"), "profile must be smoke or target");
        var target = profile.equals("target");
        var users = options.integer("--users", target ? 3_000_000 : 3000, 2, 10_000_000);
        var records = options.number("--accounts", target ? 10_000_000 : 10000, users, 100_000_000);
        var symbols = options.integer("--symbols", target ? 100_000 : 100, 1, users - 1);
        var pendingOrders = options.number("--pending-orders", target ? 4_000_000 : 4000, symbols, 100_000_000);
        var cycles = options.number("--cycles", target ? 100_000 : 1000, 1, 10_000_000);
        var rounds = options.integer("--rounds", target ? 3 : 2, 1, 1000);
        var producers = options.integer("--producers", 4, 1, 256);
        var batchSize = options.integer("--batch-size", target ? 256 : 64, 1, 65536);
        var ringSize = options.ringSize(1024);
        var estimated = Math.addExact(Math.multiplyExact((long) users, 1600), Math.addExact(Math.multiplyExact(records, 80), Math.multiplyExact(pendingOrders + cycles * rounds * 2, 600)));
        var os = (com.sun.management.OperatingSystemMXBean) java.lang.management.ManagementFactory.getOperatingSystemMXBean();
        BenchmarkContext.require(options.flag("--force") || estimated < os.getFreeMemorySize(), "estimated native+JVM state exceeds free physical memory; choose smaller options or explicitly use --force");
        System.out.printf("scenario=large_state_mixed_spot measurement=java_api_end_to_end users=%d asset_balance_records=%d symbols=%d pending_orders=%d estimated_state_mb=%.1f%n", users, records, symbols, pendingOrders, estimated / 1048576.0);
        try (var context = new BenchmarkContext(ringSize, producers, batchSize * 2, false))
        {
            var setup = System.nanoTime();
            var quote = context.asset("QUOTE");
            var bases = new long[symbols];
            for (int index = 0; index < symbols; ++index)
            {
                bases[index] = context.asset("BASE" + index);
                var symbol = context.execute(new CreateSymbol("SPOT" + index, SymbolType.Spot, bases[index], quote)).id();
                context.execute(new AddSpotBook(symbol));
            }

            var accounts = new long[users];
            var totalCycles = Math.multiplyExact(cycles, rounds);
            var perSymbol = (totalCycles + symbols - 1) / symbols;
            for (int index = 0; index < users; ++index)
            {
                var user = context.execute(new CreateUser()).id();
                accounts[index] = context.account(user, false);
                context.credit(accounts[index], quote, Math.multiplyExact(perSymbol + 1, 100));
            }

            for (long index = 0; index < pendingOrders; ++index)
            {
                var owner = (int) (index % users);
                var symbol = (int) ((owner % symbols + index / users) % symbols);
                context.credit(accounts[owner], bases[symbol], 2);
            }

            for (int symbol = 0; symbol < symbols; ++symbol)
            {
                context.credit(accounts[(symbol + 1) % users], bases[symbol], 1);
                context.credit(accounts[symbol], bases[symbol], perSymbol);
            }

            var actual = balanceRecords(context);
            BenchmarkContext.require(actual <= records, "--accounts is smaller than the scenario's required balance records");
            var deficit = records - actual;
            long fillerAsset = 0;
            for (long index = 0; index < deficit; ++index)
            {
                if (index % users == 0)
                {
                    fillerAsset = context.asset("FILLER" + index / users);
                }

                context.credit(accounts[(int) (index % users)], fillerAsset, 1);
            }

            var pending = new ArrayList<CompletableFuture<CommandReceipt>>(batchSize);
            for (long index = 0; index < pendingOrders; ++index)
            {
                var owner = index % users;
                var symbol = (owner % symbols + index / users) % symbols;
                pending.add(context.submit(new PlaceOrder(owner + 1, symbol + 1, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, 1)));
                if (pending.size() == batchSize || index + 1 == pendingOrders)
                {
                    BenchmarkContext.await(pending, value -> BenchmarkContext.require(value.result().orderStatus() == OrderStatus.NotFilled, "preload unexpected fill"));
                }
            }

            BenchmarkContext.require(restingOrders(context) == pendingOrders && balanceRecords(context) == records, "setup scale mismatch");
            System.out.printf("setup_seconds=%.6f%n", Metrics.seconds(setup));
            Metrics.memory();
            for (int round = 1; round <= rounds; ++round)
            {
                var latencies = Collections.synchronizedList(new ArrayList<Long>());
                var elapsed = Metrics.concurrent(cycles, producers, (first, count) ->
                {
                    var responses = new ArrayList<CompletableFuture<CommandReceipt>>(batchSize * 2);
                    for (long index = 0; index < count; ++index)
                    {
                        var symbol = (first + index) % symbols;
                        var buyer = (symbol + 1) % users + 1;
                        submit(context, responses, latencies, new PlaceOrder(buyer, symbol + 1, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, 1), true);
                        submit(context, responses, latencies, new PlaceOrder(symbol + 1, symbol + 1, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, 1), false);
                        if (responses.size() == batchSize * 2 || index + 1 == count)
                        {
                            BenchmarkContext.await(responses, value -> {});
                        }
                    }
                });

                System.out.printf("round=%d trades=%d commands=%d seconds=%.6f trade_tps=%.0f command_tps=%.0f%n", round, cycles, cycles * 2, elapsed, cycles / elapsed, cycles * 2 / elapsed);
                Metrics.latency(new ArrayList<>(latencies));
                Metrics.memory();
            }

            var overview = context.submit(new ReadOverview()).join().value();
            BenchmarkContext.require(overview.activeUsers() == users && overview.books() == symbols && restingOrders(context) == pendingOrders && balanceRecords(context) == records, "final scale mismatch");
            System.out.printf("verified_users=%d verified_asset_balance_records=%d actual_account_entities=%d verified_books=%d verified_pending=%d retained_orders=%d validation=passed%n", users, records, overview.activeAccounts(), overview.books(), pendingOrders, overview.retainedOrders());
        }
    }

    private static void submit(BenchmarkContext context, ArrayList<CompletableFuture<CommandReceipt>> responses, java.util.List<Long> latencies, PlaceOrder command, boolean fill)
    {
        var started = System.nanoTime();
        responses.add(context.submit(command).thenApply(value ->
        {
            BenchmarkContext.accepted(value);
            BenchmarkContext.require(value.result().orderStatus() == (fill ? OrderStatus.Filled : OrderStatus.NotFilled), "mixed workload mismatch");
            latencies.add(System.nanoTime() - started);
            return value;
        }));
    }

    private static long balanceRecords(BenchmarkContext context)
    {
        var count = new long[1];
        context.rows(new ObservationQuery().withKind(ReadKind.Accounts), AccountView.class, value -> count[0] += value.balanceCount());
        return count[0];
    }

    private static long restingOrders(BenchmarkContext context)
    {
        var count = new long[1];
        context.rows(new ObservationQuery().withKind(ReadKind.Orders).withIncludeTerminal(false), OrderView.class, value ->
        {
            if (value.resting())
            {
                ++count[0];
            }
        });
        return count[0];
    }
}
