package nhcm.advmatch.benchmark;

import nhcm.advmatch.benchmark.support.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.contract.*;
import nhcm.advmatch.command.order.*;
import nhcm.advmatch.model.*;

import java.math.BigInteger;
import java.util.ArrayList;
import java.util.Set;

public final class LiquidationBenchmark
{
    private static final long ENTRY = 100_000;

    private record Configuration(int users, int positionsPerUser, int symbols, int waves, int crossPercent, int shockPercent, int impactPercent, int depthPercent, int levels, int slippageBps, int insurancePercent, int restingPerUser, long candidateBudget, int ringSize, boolean up, boolean recover, OrderBookImplementation book, int tradesPerWave)
    {
    }

    private record State(long closedVictims, long pending)
    {
    }

    public static void main(String[] arguments)
    {
        var options = new Options(arguments, Set.of("--users", "--positions-per-user", "--symbols", "--waves", "--cross-percent", "--shock-percent", "--impact-percent", "--depth-percent", "--levels", "--slippage-bps", "--insurance-percent", "--resting-per-user", "--candidate-budget", "--ring-size", "--direction", "--recover", "--book", "--trades-per-wave"));
        var users = options.integer("--users", 20_000, 1, 10_000_000);
        var positions = options.integer("--positions-per-user", 4, 1, 20_000_000 / users);
        var direction = options.text("--direction", "down");
        var book = options.text("--book", "radix");
        BenchmarkContext.require(direction.equals("down") || direction.equals("up"), "direction must be down or up");
        BenchmarkContext.require(book.equals("radix") || book.equals("tree"), "book must be radix or tree");
        var config = new Configuration(users, positions, options.integer("--symbols", 8, 1, Math.min(100000, users * positions)), options.integer("--waves", 8, 1, 1000), options.integer("--cross-percent", 50, 0, 100), options.integer("--shock-percent", 35, 0, 90), options.integer("--impact-percent", 30, 0, 90), options.integer("--depth-percent", 10, 0, 100), options.integer("--levels", 8, 1, 1000), options.integer("--slippage-bps", 100, 0, 2000), options.integer("--insurance-percent", 10, 0, 200), options.integer("--resting-per-user", 1, 0, 100), options.number("--candidate-budget", Long.MAX_VALUE, 0, Long.MAX_VALUE), options.ringSize(32), direction.equals("up"), options.integer("--recover", 1, 0, 1) == 1, book.equals("radix") ? OrderBookImplementation.Radix : OrderBookImplementation.IndexedTree, options.integer("--trades-per-wave", 0, 0, 10_000_000 / options.integer("--waves", 8, 1, 1000)));
        BenchmarkContext.require(config.shockPercent() + config.impactPercent() <= 95, "combined shock/impact must not exceed 95%");
        run(config);
    }

    private static void run(Configuration config)
    {
        var victims = (long) config.users() * config.positionsPerUser();
        try (var context = new BenchmarkContext(config.ringSize(), 1, 256, false))
        {
            System.out.printf("scenario=synthetic_liquidation_cascade measurement=java_api_end_to_end direction=%s book=%s users=%d victim_positions=%d symbols=%d cross_percent=%d candidate_budget=%d%n", config.up() ? "up" : "down", config.book(), config.users(), victims, config.symbols(), config.crossPercent(), config.candidateBudget());
            var setupStart = System.nanoTime();
            context.asset("UNUSED");
            var quote = context.asset("COLLATERAL");
            var maker = context.execute(new CreateUser()).id();
            var insurer = context.execute(new CreateUser()).id();
            var makerAccount = context.account(maker, true);
            var insurerAccount = context.account(insurer, true);
            var fixtureCredit = Math.multiplyExact(victims, ENTRY * 3);
            context.credit(makerAccount, quote, fixtureCredit);
            context.credit(insurerAccount, quote, fixtureCredit);
            var initialCash = BigInteger.valueOf(fixtureCredit).multiply(BigInteger.TWO);
            context.execute(new SetAutomaticLiquidationBudget(config.candidateBudget()));
            var makerPositions = new long[config.symbols()];
            var seeded = new long[config.symbols()];
            var spec = new ContractSpec().withMakerFeeRate(0).withTakerFeeRate(0).withLiquidationFeeRate(0).withPriceBandRate(300_000).withFirstFundingTimestamp(1_000_000_000);
            var makerOptions = new ContractOrderOptions().withLeverage(1);
            for (int slot = 0; slot < config.symbols(); ++slot)
            {
                var base = context.asset("BASE" + slot);
                var symbol = context.execute(new CreateSymbol("CASCADE" + slot, SymbolType.Perpetual, base, quote)).id();
                context.execute(new AddContractBook(symbol, spec, ENTRY, 0, config.book()));
                var quantity = victims / config.symbols() + (slot < victims % config.symbols() ? 1 : 0);
                seeded[slot] = quantity;
                var order = context.execute(new PlaceOrder(maker, symbol, config.up() ? OrderSide.Bid : OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, ENTRY, quantity, 1, makerOptions));
                makerPositions[slot] = order.result().positionId();
                context.execute(new FundInsurance(symbol, insurerAccount, quantity * ENTRY * config.insurancePercent() / 100));
            }

            var leverages = new int[]{5, 10, 20, 40, 80};
            for (int userIndex = 0; userIndex < config.users(); ++userIndex)
            {
                var user = context.execute(new CreateUser()).id();
                var leverage = leverages[userIndex % leverages.length];
                var victim = new ContractOrderOptions().withLeverage(leverage).withMarginMode(userIndex % 100 < config.crossPercent() ? MarginMode.Cross : MarginMode.Isolated);
                var deposit = (ENTRY * 13 / 10 + leverage - 1) / leverage * config.positionsPerUser();
                context.credit(context.account(user, true), quote, deposit);
                initialCash = initialCash.add(BigInteger.valueOf(deposit));
                for (int index = 0; index < config.positionsPerUser(); ++index)
                {
                    var symbol = ((long) userIndex * config.positionsPerUser() + index) % config.symbols() + 1;
                    var order = context.execute(new PlaceOrder(user, symbol, config.up() ? OrderSide.Ask : OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, ENTRY, 1, 1, victim));
                    BenchmarkContext.require(order.result().orderStatus() == OrderStatus.Filled, "victim setup did not fill");
                    if (index == 0)
                    {
                        var closing = victim.withPositionId(order.result().positionId()).withReduceOnly(true);
                        for (int resting = 0; resting < config.restingPerUser(); ++resting)
                        {
                            var price = config.up() ? ENTRY * 9 / 10 - resting : ENTRY * 11 / 10 + resting;
                            context.execute(new PlaceOrder(user, symbol, config.up() ? OrderSide.Bid : OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, price, 1, 1, closing));
                        }
                    }
                }
            }

            for (int slot = 0; slot < config.symbols(); ++slot)
            {
                context.execute(new AddMargin(makerPositions[slot], maker, slot + 1, seeded[slot] * ENTRY));
            }

            long trafficBuyer = 0;
            long trafficSeller = 0;
            long trafficSymbol = 0;
            if (config.tradesPerWave() != 0)
            {
                trafficBuyer = context.execute(new CreateUser()).id();
                trafficSeller = context.execute(new CreateUser()).id();
                var credit = (long)config.tradesPerWave() * config.waves() * ENTRY * 2;
                context.credit(context.account(trafficBuyer, true), quote, credit);
                context.credit(context.account(trafficSeller, true), quote, credit);
                initialCash = initialCash.add(BigInteger.valueOf(credit).multiply(BigInteger.TWO));
                trafficSymbol = context.execute(new CreateSymbol("CASCADE-TRAFFIC", SymbolType.Perpetual, context.asset("TRAFFIC"), quote)).id();
                context.execute(new AddContractBook(trafficSymbol, spec, ENTRY, 0, config.book()));
            }

            var setupSeconds = Metrics.seconds(setupStart);
            var liquidity = new ArrayList<Long>();
            var stressStart = System.nanoTime();
            long peakPending = 0;
            long markNanos = 0;
            long maintenanceNanos = 0;
            long ordinaryTrades = 0;
            long timestamp = 2;
            var waveTimes = new ArrayList<Long>();
            var state = state(context, makerPositions);
            for (int wave = 1; wave <= config.waves(); ++wave, ++timestamp)
            {
                var started = System.nanoTime();
                var before = state.closedVictims();
                var external = (long) config.shockPercent() * 10000 * wave / config.waves();
                var feedback = (long) config.impactPercent() * 10000 * before / victims;
                var movement = external + feedback;
                var price = ENTRY * (config.up() ? 1_000_000 + movement : 1_000_000 - movement) / 1_000_000;
                var markStart = System.nanoTime();
                for (int symbol = 1; symbol <= config.symbols(); ++symbol)
                {
                    shock(context, new UpdateMarkPrice(symbol, price, timestamp));
                }

                markNanos += System.nanoTime() - markStart;
                var beforeRefill = state(context, makerPositions).pending();
                peakPending = Math.max(peakPending, beforeRefill);
                var refillStart = System.nanoTime();
                for (int symbol = 1; symbol <= config.symbols(); ++symbol)
                {
                    refill(context, config, maker, makerPositions[symbol - 1], symbol, price, (seeded[symbol - 1] * config.depthPercent() + 99) / 100, timestamp, liquidity);
                }

                maintenanceNanos += System.nanoTime() - refillStart;
                ordinaryTrades += tradeBatch(context, trafficBuyer, trafficSeller, trafficSymbol, config.tradesPerWave(), timestamp);
                state = state(context, makerPositions);
                var waveNanos = System.nanoTime() - started;
                waveTimes.add(waveNanos);
                System.out.printf("wave=%d mark=%d external_move_ppm=%d feedback_move_ppm=%d liquidated=%d pending_before_refill=%d pending_after_refill=%d wave_ms=%.3f%n", wave, price, external, feedback, state.closedVictims() - before, beforeRefill, state.pending(), waveNanos / 1e6);
            }

            var stressSeconds = Metrics.seconds(stressStart);
            var stressLiquidated = state.closedVictims();
            var pendingBeforeRecovery = state.pending();
            var recoveryStart = System.nanoTime();
            if (config.recover())
            {
                for (int symbol = 1; symbol <= config.symbols(); ++symbol)
                {
                    context.execute(new FundInsurance(symbol, insurerAccount, seeded[symbol - 1] * ENTRY));
                }

                for (int pass = 0; pass < 3; ++pass, ++timestamp)
                {
                    for (int symbol = 1; symbol <= config.symbols(); ++symbol)
                    {
                        var market = context.rows(new ObservationQuery().withKind(ReadKind.Market).withSymbolId(symbol), MarketView.class).getFirst();
                        refill(context, config, maker, makerPositions[symbol - 1], symbol, market.markPrice(), seeded[symbol - 1], timestamp, liquidity);
                        shock(context, new ProcessLiquidations(symbol, timestamp));
                    }
                }
            }

            var recoverySeconds = Metrics.seconds(recoveryStart);
            state = state(context, makerPositions);
            var auditStart = System.nanoTime();
            audit(context, config, initialCash, state);
            BenchmarkContext.require(ordinaryTrades == (long)config.tradesPerWave() * config.waves(), "mixed traffic count mismatch");
            System.out.printf("setup_seconds=%.6f stress_seconds=%.6f recovery_seconds=%.6f audit_seconds=%.6f stress_liquidated_positions=%d total_liquidated_positions=%d peak_pending=%d pending_before_recovery=%d pending_final=%d%n", setupSeconds, stressSeconds, recoverySeconds, Metrics.seconds(auditStart), stressLiquidated, state.closedVictims(), peakPending, pendingBeforeRecovery, state.pending());
            System.out.printf("stress_liquidation_tps=%.0f total_liquidation_tps=%.0f mark_api_ms=%.3f maintenance_api_ms=%.3f validation=passed%n", stressLiquidated / stressSeconds, state.closedVictims() / (stressSeconds + recoverySeconds), markNanos / 1e6, maintenanceNanos / 1e6);
            System.out.printf("ordinary_trades=%d ordinary_order_commands=%d ordinary_trade_tps_under_stress=%.0f trades_per_wave=%d%n", ordinaryTrades, ordinaryTrades * 2, ordinaryTrades / stressSeconds, config.tradesPerWave());
            Metrics.latency(waveTimes);
            Metrics.memory();
        }
    }

    private static long tradeBatch(BenchmarkContext context, long buyer, long seller, long symbol, int count, long timestamp)
    {
        var opening = new ContractOrderOptions().withLeverage(1);
        for (int offset = 0; offset < count;)
        {
            var commands = new ArrayList<nhcm.advmatch.command.Command<CommandReceipt>>(1024);
            var size = Math.min(512, count - offset);
            for (int index = 0; index < size; ++index)
            {
                commands.add(new PlaceOrder(seller, symbol, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, ENTRY, 1, timestamp, opening));
                commands.add(new PlaceOrder(buyer, symbol, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, ENTRY, 1, timestamp, opening));
            }

            var futures = context.submitBatch(commands);
            for (int index = 0; index < futures.size(); ++index)
            {
                var result = BenchmarkContext.accepted(futures.get(index).join()).result();
                BenchmarkContext.require(result.orderStatus() == (index % 2 == 0 ? OrderStatus.NotFilled : OrderStatus.Filled), "mixed traffic order failed");
                BenchmarkContext.require(result.filled() == index % 2, "mixed traffic quantity failed");
            }

            offset += size;
        }

        return count;
    }

    private static void shock(BenchmarkContext context, nhcm.advmatch.command.Command<CommandReceipt> command)
    {
        var result = context.submit(command).join().result();
        BenchmarkContext.require(result.state() == ContractState.Success.code() || result.state() == ContractState.PendingLiquidation.code(), "shock rejected state=" + result.state());
    }

    private static void refill(BenchmarkContext context, Configuration config, long maker, long positionId, int symbol, long price, long quantity, long timestamp, ArrayList<Long> liquidity)
    {
        // Matches the C++ stress fixture: replace the outstanding liquidity set.
        for (var order : liquidity)
        {
            context.submit(new CancelOrder(maker, order, timestamp)).join();
        }

        liquidity.clear();
        var position = context.position(positionId);
        var remaining = Math.min(quantity, position.size());
        var closing = new ContractOrderOptions().withPositionId(positionId).withReduceOnly(true).withLeverage(position.leverage());
        for (int level = 0; level < config.levels() && remaining > 0; ++level)
        {
            var size = (remaining + config.levels() - level - 1) / (config.levels() - level);
            var slip = (long) config.slippageBps() * (level + 1) / config.levels();
            var executionPrice = price * (config.up() ? 10000 + slip : 10000 - slip) / 10000;
            var result = context.execute(new PlaceOrder(maker, symbol, config.up() ? OrderSide.Ask : OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, executionPrice, size, timestamp, closing));
            liquidity.add(result.id());
            remaining -= size;
        }
    }

    private static State state(BenchmarkContext context, long[] makerPositions)
    {
        long closed = 0;
        long pending = 0;
        for (var market : context.rows(new ObservationQuery().withKind(ReadKind.Markets), MarketView.class))
        {
            closed += market.retainedPositions() - market.openPositions();
            pending += market.pendingLiquidations();
        }

        for (var id : makerPositions)
        {
            if (context.position(id).closed())
            {
                --closed;
            }
        }

        return new State(closed, pending);
    }

    private static void audit(BenchmarkContext context, Configuration config, BigInteger initialCash, State state)
    {
        var cash = new BigInteger[]{BigInteger.ZERO};
        var net = new long[config.symbols() + (config.tradesPerWave() != 0 ? 1 : 0)];
        var closed = new long[1];
        context.rows(new ObservationQuery().withKind(ReadKind.Balances), BalanceView.class, value ->
        {
            BenchmarkContext.require(value.active() >= 0 && value.reserved() >= 0, "negative balance");
            cash[0] = cash[0].add(BigInteger.valueOf(value.active())).add(BigInteger.valueOf(value.reserved()));
        });
        context.rows(new ObservationQuery().withKind(ReadKind.Positions), PositionView.class, value ->
        {
            BenchmarkContext.require(value.closed() == (value.size() == 0) && value.initialMargin() >= 0, "invalid position state");
            net[(int) value.symbolId() - 1] += value.side() == PositionSide.Long ? value.size() : -value.size();
            if (value.userId() > 2 && value.closed())
            {
                ++closed[0];
            }
        });
        context.rows(new ObservationQuery().withKind(ReadKind.Markets), MarketView.class, value ->
        {
            cash[0] = cash[0].add(BigInteger.valueOf(value.insuranceBalance())).add(BigInteger.valueOf(value.clearingBalance())).add(BigInteger.valueOf(value.feeBalance())).add(BigInteger.valueOf(value.fundingRoundingBalance()));
            BenchmarkContext.require(value.insuranceBalance() >= 0 && net[(int) value.symbolId() - 1] == 0, "position conservation failed");
        });
        BenchmarkContext.require(cash[0].equals(initialCash) && closed[0] == state.closedVictims() && (!config.recover() || state.pending() == 0), "cash/closed-position/recovery audit failed");
    }
}
