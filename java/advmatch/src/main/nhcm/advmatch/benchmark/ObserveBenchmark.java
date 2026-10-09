package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.order.*;
import nhcm.advmatch.command.read.*;
import nhcm.advmatch.model.*;

import java.util.ArrayList;
import java.util.Set;
import java.util.concurrent.CompletableFuture;

public final class ObserveBenchmark
{
    public static void main(String[] arguments)
    {
        // Retain the C++ executable's two positional arguments as well.
        if (arguments.length > 0 && !arguments[0].startsWith("--"))
        {
            BenchmarkContext.require(arguments.length <= 2, "expected orders queries");
            arguments = new String[]{"--orders", arguments[0], "--queries", arguments.length == 2 ? arguments[1] : "1000000"};
        }

        var options = new Options(arguments, Set.of("--orders", "--queries", "--ring-size"));
        var orders = options.number("--orders", 50_000, 1, 100_000_000);
        var queries = options.number("--queries", 1_000_000, 1, 1_000_000_000);
        run(orders, queries, options.ringSize(1024), false);
        run(orders, queries, options.ringSize(1024), true);
    }

    private static void run(long orders, long queries, int ringSize, boolean indexed)
    {
        try (var context = new BenchmarkContext(ringSize, 1, 256, indexed))
        {
            var base = context.asset("BASE");
            var quote = context.asset("QUOTE");
            var symbol = context.execute(new CreateSymbol("TEST", SymbolType.Spot, base, quote)).id();
            context.execute(new AddSpotBook(symbol));
            for (int index = 0; index < 1000; ++index)
            {
                var user = context.execute(new CreateUser()).id();
                context.credit(context.account(user, false), base, Long.MAX_VALUE / 2);
            }

            var started = System.nanoTime();
            var pending = new ArrayList<CompletableFuture<CommandReceipt>>(256);
            for (long index = 0; index < orders; ++index)
            {
                pending.add(context.submit(new PlaceOrder(index % 1000 + 1, symbol, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100 + index % 1000, 1, index + 1)));
                if (pending.size() == 256 || index + 1 == orders)
                {
                    BenchmarkContext.await(pending, value -> {});
                }
            }

            var writeSeconds = Metrics.seconds(started);
            long checksum = 0;
            started = System.nanoTime();
            for (long index = 0; index < queries; ++index)
            {
                var response = context.submit(new ReadOrder(new ObservationQuery().withOrderId(index * 7919 % orders + 1))).join();
                BenchmarkContext.require(response.page().state() == ReadState.Success, "order read failed");
                var value = response.values().getFirst();
                checksum += value.price() + value.filling().unfilled();
            }

            var orderSeconds = Metrics.seconds(started);
            var depth = new ObservationQuery().withSymbolId(symbol).withSide(OrderSide.Ask).withLimit(20).withKind(ReadKind.Depth);
            var depthQueries = Math.max(1, queries / 20);
            started = System.nanoTime();
            for (long index = 0; index < depthQueries; ++index)
            {
                var response = context.submit(new ReadDepth(depth)).join();
                BenchmarkContext.require(response.page().state() == ReadState.Success && !response.values().isEmpty(), "depth read failed");
                checksum += response.values().size() + response.values().getFirst().quantity();
            }

            var depthSeconds = Metrics.seconds(started);
            try (var slot = context.createObservationSlot(depth).join())
            {
                started = System.nanoTime();
                var old = context.refresh(slot).join();
                var refreshMicroseconds = Metrics.seconds(started) * 1_000_000;
                started = System.nanoTime();
                for (long index = 0; index < queries; ++index)
                {
                    checksum += slot.load().typed(DepthView.class).values().getFirst().quantity();
                }

                var cacheSeconds = Metrics.seconds(started);
                BenchmarkContext.require(old.typed(DepthView.class).values().getFirst().quantity() > 0, "snapshot mutated");
                System.out.printf("scenario=observe measurement=java_api_end_to_end index=%s orders=%d queries=%d write_tps=%.0f order_reads_s=%.0f depth20_reads_s=%.0f native_slot_loads_s=%.0f refresh_us=%.3f checksum=%d validation=passed%n", indexed ? "on" : "off", orders, queries, orders / writeSeconds, queries / orderSeconds, depthQueries / depthSeconds, queries / cacheSeconds, refreshMicroseconds, checksum);
            }

            Metrics.memory();
        }
    }
}
