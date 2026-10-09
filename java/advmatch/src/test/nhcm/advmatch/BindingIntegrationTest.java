package nhcm.advmatch;

import nhcm.advmatch.command.Command;
import nhcm.advmatch.command.account.*;
import nhcm.advmatch.command.contract.*;
import nhcm.advmatch.command.management.*;
import nhcm.advmatch.command.order.*;
import nhcm.advmatch.command.read.*;
import nhcm.advmatch.command.persistence.*;
import nhcm.advmatch.model.*;
import nhcm.advmatch.benchmark.support.BackpressureSubmitter;
import nhcm.advmatch.internal.NativeBindings;
import nhcm.advmatch.internal.Wire;
import nhcm.advmatch.internal.WireWriter;
import nhcm.advmatch.internal.WireReader;

import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class BindingIntegrationTest
{
    private static void directBatchEncoding()
    {
        var writer = new WireWriter(1);
        writer.u8(255);
        writer.u16(65535);
        writer.u32(0xffffffffL);
        writer.i32(Integer.MIN_VALUE);
        writer.u64(-1L);
        writer.i64(Long.MIN_VALUE);
        var reader = new WireReader(writer.bytes());
        check(reader.u8() == 255 && reader.u16() == 65535 && reader.u32() == 0xffffffffL);
        check(reader.i32() == Integer.MIN_VALUE && reader.u64() == -1L && reader.i64() == Long.MIN_VALUE);
        reader.finish();

        List<Command<?>> commands = List.of(new CreateUser(), new CreateAsset("比特币", "BTC", 8),
            new PlaceOrder(1, 1, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, 1),
            new PlaceOrder(1, 1, OrderSide.Ask, OrderType.Market, TimeInForce.IOC, 0, 2, 3, new ContractOrderOptions()),
            new ReadBatch(List.of(new ObservationQuery(), new ObservationQuery().withKind(ReadKind.Positions))),
            new Checkpoint());
        writer = new WireWriter(1);
        writer.u32(commands.size());
        for (var command : commands)
        {
            var frame = writer.beginFrame();
            command.encodeTo(writer);
            writer.endFrame(frame);
        }

        var bytes = writer.bytes();
        reader = new WireReader(bytes);
        check(reader.u32() == commands.size());
        for (var command : commands)
        {
            var size = Math.toIntExact(reader.u32());
            check(Arrays.equals(command.encode(), Arrays.copyOfRange(bytes, reader.offset(), reader.offset() + size)));
            reader.skip(size);
        }

        reader.finish();
        // Existing custom Command implementations need no encodeTo override.
        Command<byte[]> custom = new Command<>()
        {
            public byte[] encode()
            {
                return new byte[]{1, 2, 3};
            }

            public byte[] decode(byte[] response)
            {
                return response;
            }
        };
        writer = new WireWriter(1);
        custom.encodeTo(writer);
        check(Arrays.equals(custom.encode(), writer.bytes()));
    }

    private static void check(boolean condition)
    {
        if (!condition)
        {
            throw new AssertionError("Binding check failed");
        }
    }

    private static ExchangeConfig memory()
    {
        return new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false)).withObservation(new ObservationConfig().withIndexUserOpenOrders(true));
    }

    private static void accepted(CompletableFuture<? extends Receipt> future)
    {
        check(future.orTimeout(10, TimeUnit.SECONDS).join().result().accepted());
    }

    private static void spotAndConcurrency() throws Exception
    {
        try (var exchange = new Exchange(memory()))
        {
            accepted(exchange.submit(new CreateAsset("比特币", "BTC", 0, 1)));
            check(exchange.submit(new CreateAsset("比特币", "BTC", 0, 1)).join().duplicate());
            accepted(exchange.submit(new CreateAsset("美元", "USD", 0)));
            accepted(exchange.submit(new CreateUser()));
            accepted(exchange.submit(new CreateUser()));
            accepted(exchange.submit(new CreateSymbol("BTCUSD", SymbolType.Spot, 1, 2)));
            accepted(exchange.submit(new AddSpotBook(1)));
            accepted(exchange.submit(new UpdateBalance(1, 2, 10000)));
            accepted(exchange.submit(new UpdateBalance(3, 1, 20)));
            accepted(exchange.submit(new Transfer(1, 2, 2, 100)));
            accepted(exchange.submit(new Transfer(2, 1, 2, 100)));

            var ask = exchange.submit(new PlaceOrder(2, 1, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 10, 1)).join();
            var bid = exchange.submit(new PlaceOrder(1, 1, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 3, 2)).join();
            check(bid.result().accepted() && bid.result().filled() == 3);
            var query = new ObservationQuery().withSymbolId(1).withSide(OrderSide.Ask).withKind(ReadKind.Depth);
            try (var slot = exchange.createObservationSlot(query).join())
            {
                var old = slot.refresh().join().typed(DepthView.class);
                check(old.values().getFirst().quantity() == 7);
                accepted(exchange.submit(new ModifyQuantity(2, ask.result().entityId(), 1, 3)));
                slot.refresh().join();
                check(slot.load().typed(DepthView.class).values().getFirst().quantity() == 8);
                check(old.values().getFirst().quantity() == 7);
            }

            accepted(exchange.submit(new ModifyPrice(2, ask.result().entityId(), 102, 4)));
            accepted(exchange.submit(new CancelOrder(2, ask.result().entityId(), 5)));
            var asset = exchange.submit(new ReadAsset(new ObservationQuery().withAssetId(1))).join().values().getFirst();
            check(asset.displayName().equals("比特币"));
            check(exchange.submit(new ReadTrades()).join().values().size() == 1);
            check(exchange.submit(new ReadBalances(new ObservationQuery().withAccountId(1))).join().values().getFirst().accountId() == 1);
            exchange.submit(new RebuildOrderIndex()).join();
            var batch = exchange.submit(new ReadBatch(List.of(new ObservationQuery().withKind(ReadKind.Users), new ObservationQuery().withKind(ReadKind.Books)))).join();
            check(batch.results().size() == 2 && batch.results().getFirst().page().stamp().revision() == batch.results().getLast().page().stamp().revision());
            for (var kind : ReadKind.values())
            {
                // Every payload/error shape must decode without ABI mismatch.
                var result = exchange.submit(new Read(query.withKind(kind).withUserId(1).withAccountId(1).withAssetId(1).withOrderId(ask.result().entityId()))).join();
                check(result.page() != null);
            }

            var gate = new CountDownLatch(1);
            var entered = new CountDownLatch(1);
            // A non-Async continuation may run inline on the attaching caller
            // if the request completed first. Explicit dispatch avoids that race.
            var callback = exchange.submit(new CreateUser()).thenRunAsync(() ->
            {
                entered.countDown();
                try
                {
                    check(gate.await(10, TimeUnit.SECONDS));
                }
                catch (InterruptedException error)
                {
                    throw new RuntimeException(error);
                }
            });
            check(entered.await(10, TimeUnit.SECONDS));
            accepted(exchange.submit(new CreateUser()));
            gate.countDown();
            callback.join();

            var producers = new ArrayList<CompletableFuture<Void>>();
            for (int index = 0; index < 4; ++index)
            {
                producers.add(CompletableFuture.runAsync(() ->
                {
                    for (int count = 0; count < 25; ++count)
                    {
                        accepted(exchange.submit(new CreateUser()));
                    }
                }));
            }

            CompletableFuture.allOf(producers.toArray(CompletableFuture[]::new)).orTimeout(20, TimeUnit.SECONDS).join();
            check(exchange.submit(new ReadOverview()).join().value().activeUsers() == 104);
            Command<Void> malformed = new Command<>()
            {
                public byte[] encode()
                {
                    return new byte[]{1, 0, 127};
                }

                public Void decode(byte[] bytes)
                {
                    return null;
                }
            };
            check(exchange.submit(malformed).handle((value, error) -> error != null).join());
            accepted(exchange.submit(new CreateUser()));
            var queued = exchange.submit(new CreateUser());
            var closing = exchange.closeAsync();
            check(!closing.cancel(false));
            closing.orTimeout(10, TimeUnit.SECONDS).join();
            accepted(queued);
            check(!queued.cancel(false));
            check(exchange.submit(new ReadOverview()).handle((value, error) -> error != null).join());
        }
    }

    private static void contracts()
    {
        try (var exchange = new Exchange(memory()))
        {
            accepted(exchange.submit(new CreateAsset("BTC", "BTC", 0)));
            accepted(exchange.submit(new CreateAsset("USD", "USD", 0)));
            accepted(exchange.submit(new CreateUser()));
            accepted(exchange.submit(new CreateUser()));
            accepted(exchange.submit(new UpdateBalance(2, 2, 10000)));
            accepted(exchange.submit(new UpdateBalance(4, 2, 10000)));
            accepted(exchange.submit(new CreateSymbol("PERP", SymbolType.Perpetual, 1, 2)));
            var spec = new ContractSpec().withTakerFeeRate(0).withLiquidationFeeRate(0).withFirstFundingTimestamp(100).withFundingInterval(100);
            accepted(exchange.submit(new AddContractBook(1, spec, 100, 0)));
            accepted(exchange.submit(new PlaceOrder(2, 1, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 10, 1, new ContractOrderOptions())));
            var bid = exchange.submit(new PlaceOrder(1, 1, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 10, 2, new ContractOrderOptions())).join();
            check(bid.result().accepted());
            long positionId = bid.result().positionId();
            accepted(exchange.submit(new SetLeverage(positionId, 1, 1, 5, 3)));
            accepted(exchange.submit(new AddMargin(positionId, 1, 1, 100)));
            accepted(exchange.submit(new FundInsurance(1, 4, 1000)));
            accepted(exchange.submit(new SettleFunding(1, 10000, 100)));
            check(exchange.submit(new ReadPosition(new ObservationQuery().withPositionId(positionId))).join().values().getFirst().fundingPnL() == -10);
            check(!exchange.submit(new ReadMarket(new ObservationQuery().withSymbolId(1))).join().values().isEmpty());
            check(!exchange.submit(new ReadContractEvents()).join().values().isEmpty());
            accepted(exchange.submit(new SetAutomaticLiquidationBudget(0)));
            exchange.submit(new UpdateMarkPrice(1, 50, 101)).join();
            check(exchange.submit(new ReadPendingLiquidations(new ObservationQuery().withSymbolId(1))).join().values().size() == 1);
            exchange.submit(new Liquidate(1, positionId, 102)).join();
            exchange.submit(new ProcessLiquidations(1, 103)).join();

            accepted(exchange.submit(new CreateSymbol("FUTURE", SymbolType.Future, 1, 2)));
            spec = spec.withFirstFundingTimestamp(0).withExpiryTimestamp(1000);
            accepted(exchange.submit(new AddContractBook(2, spec, 100, 0)));
            accepted(exchange.submit(new PlaceOrder(2, 2, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, 1, new ContractOrderOptions())));
            accepted(exchange.submit(new PlaceOrder(1, 2, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 1, 2, new ContractOrderOptions())));
            accepted(exchange.submit(new Expire(2, 1000)));
            accepted(exchange.submit(new Deliver(2, 105, 1000)));
            check(exchange.submit(new ReadMarket(new ObservationQuery().withSymbolId(2))).join().values().getFirst().status() == ContractStatus.Settled);
        }
    }

    private static void persistence() throws Exception
    {
        var directory = Files.createTempDirectory("advmatch-java-持久化-");
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withDirectory(directory.toString()).withFlushPolicy(FlushPolicy.Manual));
        try
        {
            try (var exchange = new Exchange(config))
            {
                accepted(exchange.submit(new CreateUser(1)));
                check(exchange.submit(new ReadPersistenceStatus()).join().durableThrough() == 0);
                exchange.submit(new Tick()).join();
                check(exchange.submit(new Flush()).join().durableThrough() == 1);
                exchange.submit(new Checkpoint()).join();
                check(exchange.submit(new ReadCommitted(new CommittedQuery())).join().records().size() == 1);
            }

            try (var recovered = new Exchange(config))
            {
                check(recovered.submit(new ReadUsers()).join().values().size() == 1);
                check(recovered.submit(new CreateUser(1)).join().duplicate());
            }
        }
        finally
        {
            try (var files = Files.walk(directory))
            {
                for (var file : files.sorted(java.util.Comparator.reverseOrder()).toList())
                {
                    Files.delete(file);
                }
            }
        }
    }

    private static void batchInfrastructure() throws Exception
    {
        var blockedCallbacks = new java.util.concurrent.ConcurrentLinkedQueue<Runnable>();
        var config = memory().withRingBufferSize(1).withMaxPendingRequests(8);
        var exchange = new Exchange(config, blockedCallbacks::add, new TransportOptions(4, 16, 1000, 8));
        var commands = new ArrayList<CreateUser>();
        for (int index = 0; index < 8; ++index)
        {
            commands.add(new CreateUser());
        }

        var results = exchange.submitBatch(commands);
        var deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (exchange.pendingRequests() != 0 && System.nanoTime() < deadline)
        {
            Thread.sleep(1);
        }

        check(exchange.pendingRequests() == 0 && exchange.outstandingRequests() == 8);
        check(exchange.submit(new CreateUser()).handle((value, error) -> error instanceof NativeException nativeError && nativeError.status() == -3).join());
        while (!blockedCallbacks.isEmpty())
        {
            blockedCallbacks.remove().run();
        }

        for (int index = 0; index < 8; ++index)
        {
            check(results.get(index).join().id() == index + 1);
        }

        check(exchange.outstandingRequests() == 0);
        var malformed = new Command<CreationResult>()
        {
            public byte[] encode()
            {
                return new byte[]{1, 0, (byte)255};
            }

            public CreationResult decode(byte[] bytes)
            {
                throw new AssertionError("Malformed command executed");
            }
        };
        var invalid = exchange.submitBatch(List.of(new CreateUser(), malformed));
        check(invalid.stream().allMatch(CompletableFuture::isCompletedExceptionally));
        var following = exchange.submit(new CreateUser());
        deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (exchange.pendingRequests() != 0 && System.nanoTime() < deadline)
        {
            Thread.sleep(1);
        }

        while (!blockedCallbacks.isEmpty())
        {
            blockedCallbacks.remove().run();
        }

        check(following.join().id() == 9);
        var closing = exchange.closeAsync();
        deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (!closing.isDone() && System.nanoTime() < deadline)
        {
            var action = blockedCallbacks.poll();
            if (action != null)
            {
                action.run();
            }
            else
            {
                Thread.sleep(1);
            }
        }

        check(closing.isDone());
        closing.join();
        check(exchange.submitBatch(List.of(new CreateUser())).getFirst().isCompletedExceptionally());
        check(exchange.submitBatch(List.<CreateUser>of()).isEmpty());
    }

    public static void main(String[] arguments) throws Exception
    {
        directBatchEncoding();
        batchInfrastructure();
        backpressure();
        objectApi();
        spotAndConcurrency();
        contracts();
        referencePrices();
        conditionalOrders();
        tradingRules();
        persistence();
        System.out.println("Java bindings: spot, futures, perpetual, reads, callbacks, concurrency, close and recovery passed");
    }

    private static void backpressure() throws Exception
    {
        var config = memory().withMaxPendingRequests(1);
        var writer = new WireWriter();
        writer.u16(1);
        Wire.writeExchangeConfig(writer, config);
        var session = NativeBindings.create(writer.bytes());
        try
        {
            // Retain the first native ticket even if execution already finished.
            NativeBindings.submit(session, new CreateUser().encode());
            try
            {
                NativeBindings.submit(session, new CreateUser().encode());
                throw new AssertionError("Pending limit was not enforced");
            }
            catch (NativeException error)
            {
                check(error.status() == NativeException.QUEUE_FULL);
            }
        }
        finally
        {
            NativeBindings.destroy(session);
        }

        try (var submitter = new BackpressureSubmitter(1))
        {
            var attempts = new java.util.concurrent.atomic.AtomicInteger();
            var result = submitter.submit(() -> attempts.incrementAndGet() <= 3 ? CompletableFuture.failedFuture(new NativeException(-3, "full")) : CompletableFuture.completedFuture(42));
            check(result.get(10, TimeUnit.SECONDS) == 42 && attempts.get() == 4);
            // Similar text is not sufficient to retry a different failure code.
            attempts.set(0);
            var failed = submitter.submit(() ->
            {
                attempts.incrementAndGet();
                return CompletableFuture.failedFuture(new NativeException(-1, "exchange command queue is full"));
            });
            check(failed.handle((value, error) -> error instanceof NativeException).join() && attempts.get() == 1);
            check(submitter.submit(() -> CompletableFuture.completedFuture(7)).join() == 7);

            var held = new CompletableFuture<Integer>();
            var first = submitter.submit(() -> held);
            var entered = new CountDownLatch(1);
            var second = CompletableFuture.supplyAsync(() ->
            {
                entered.countDown();
                return submitter.submit(() -> CompletableFuture.completedFuture(9)).join();
            });
            check(entered.await(10, TimeUnit.SECONDS) && !second.isDone());
            held.complete(8);
            check(first.get(10, TimeUnit.SECONDS) == 8 && second.get(10, TimeUnit.SECONDS) == 9);
        }
    }

    private static void conditionalOrders()
    {
        try (var exchange = new MatchingExchange(memory()))
        {
            var base = exchange.assets().create("BASE", "BASE", 0).join();
            var quote = exchange.assets().create("QUOTE", "QUOTE", 0).join();
            var buyer = exchange.users().create().join();
            var seller = exchange.users().create().join();
            buyer.spotAccount().join().updateBalance(quote, 1000000).join();
            seller.spotAccount().join().updateBalance(base, 1000).join();
            var symbol = exchange.symbols().create("SPOT", SymbolType.Spot, base, quote).join();
            var book = exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
            book.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, 1).join();
            book.place(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, 2).join();
            var request = new ConditionalOrderRequest().withUserId(seller.id()).withSymbolId(symbol.id()).withQuantity(3).withTimestamp(3).withTriggerPrice(90);
            var stop = exchange.conditionalOrders().place(request).join();
            stop.modify(85, 0, 3, 4).join();
            var profit = exchange.conditionalOrders().place(request.withTimestamp(4).withKind(ConditionalOrderKind.TakeProfit).withTriggerPrice(110).withOcoOrderId(stop.id())).join();
            var disposable = exchange.conditionalOrders().place(request.withTimestamp(4).withTriggerPrice(50)).join();
            disposable.cancel(4).join();
            check(disposable.snapshot().join().status() == ConditionalOrderStatus.Cancelled);
            book.place(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 80, 10, 5).join();
            book.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 85, 1, 6).join();
            book.place(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 85, 1, 6).join();
            var view = stop.snapshot().join();
            check(view.status() == ConditionalOrderStatus.Activated && view.triggeredPrice() == 85);
            check(stop.order().join().snapshot().join().filling().filled() == 3);
            check(profit.snapshot().join().status() == ConditionalOrderStatus.Cancelled);
            check(exchange.conditionalOrders().list(new ObservationQuery().withIncludeTerminal(false)).join().values().isEmpty());
            check(exchange.submit(new ReadConditionalOrders(new ObservationQuery().withUserId(seller.id()))).join().values().size() == 3);

            buyer.contractAccount().join().updateBalance(quote, 1000000).join();
            seller.contractAccount().join().updateBalance(quote, 1000000).join();
            var perpetual = exchange.symbols().create("PERP", SymbolType.Perpetual, base, quote).join();
            var spec = new ContractSpec().withTakerFeeRate(0).withPriceBandRate(200000).withFirstFundingTimestamp(1000);
            var contractBook = exchange.books().addContract(perpetual, spec, 100, 0, OrderBookImplementation.Radix).join();
            contractBook.placeContract(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 10, 1, new ContractOrderOptions()).join();
            var opening = contractBook.placeContract(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 10, 2, new ContractOrderOptions()).join();
            var position = opening.position().join();
            position.addMargin(1000).join();
            var closing = new ContractOrderOptions().withPositionId(position.id()).withReduceOnly(true).withPriceLimit(50);
            var protective = exchange.conditionalOrders().place(request.withUserId(buyer.id()).withSymbolId(perpetual.id()).withTimestamp(3).withQuantity(10).withSource(TriggerPriceSource.Mark).withContractOptions(closing)).join();
            contractBook.placeContract(seller, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 10, 4, new ContractOrderOptions().withLeverage(1)).join();
            exchange.contracts().updateMark(perpetual, 90, 5).join();
            check(protective.snapshot().join().status() == ConditionalOrderStatus.Activated && position.snapshot().join().closed());
        }
    }

    private static void tradingRules() throws Exception
    {
        var directory = Files.createTempDirectory("advmatch-java-rules-");
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withDirectory(directory.toString()));
        long stateHash;
        long symbolId;
        long quoteId;
        try (var exchange = new MatchingExchange(config))
        {
            var base = exchange.assets().create("BASE", "BASE", 0).join();
            var quote = exchange.assets().create("QUOTE", "QUOTE", 0).join();
            quoteId = quote.id();
            var buyer = exchange.users().create().join();
            var seller = exchange.users().create().join();
            buyer.spotAccount().join().updateBalance(quote, 1000000).join();
            seller.spotAccount().join().updateBalance(base, 1000).join();
            var symbol = exchange.symbols().create("SPOT", SymbolType.Spot, base, quote).join();
            symbolId = symbol.id();
            var book = exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
            var rules = new TradingRules().withPriceTick(5).withQuantityStep(10).withMinQuantity(10)
                .withFees(new FeeSchedule(10000, 20000)).withRisk(new RiskLimits().withMaxActiveOrders(2).withMaxAccountNotional(50000))
                .withSelfTradePrevention(SelfTradePrevention.CancelBoth);
            exchange.rules().configure(symbol, rules).join();
            check(symbol.snapshot().join().tradingRules().equals(rules));
            var invalid = exchange.submit(new PlaceOrder(buyer.id(), symbol.id(), OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 101, 100, 1)).join();
            check(!invalid.result().accepted() && invalid.result().state() == PlaceOrderState.InvalidPriceTick.code());
            var maker = book.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 100, 2).join();
            check(exchange.rules().userRisk(symbol, seller).join().activeOrders() == 1);
            var taker = book.place(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.FOK, 100, 100, 3).join();
            check(maker.snapshot().join().paidFees() == 100 && taker.snapshot().join().paidFees() == 2);
            check(exchange.rules().spotFees(base).join().balance() == 2 && exchange.rules().spotFees(quote).join().balance() == 100);
            exchange.rules().withdrawSpotFees(buyer.spotAccount().join(), quote, 50).join();
            check(exchange.rules().spotFees(quote).join().balance() == 50);
            var own = book.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.PostOnly, 100, 10, 4).join();
            seller.spotAccount().join().updateBalance(quote, 1000).join();
            var prevented = book.place(seller, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 10, 5).join();
            check(own.snapshot().join().filling().status() == OrderStatus.Cancelled && prevented.snapshot().join().filling().filled() == 0);
            check(exchange.rules().userRisk(symbol, seller).join().activeOrders() == 0);
            var report = exchange.audit().report().join();
            check(report.consistent() && report.conserved() && report.issues().isEmpty());
            exchange.submit(new Checkpoint()).join();
            stateHash = report.stateHash();
        }

        try (var exchange = new MatchingExchange(config))
        {
            var report = exchange.audit().report().join();
            check(report.conserved() && report.consistent() && report.stateHash() == stateHash);
            check(exchange.symbols().get(symbolId).snapshot().join().tradingRules().priceTick() == 5);
            check(exchange.rules().spotFees(exchange.assets().get(quoteId)).join().balance() == 50);
        }

        try (var paths = Files.walk(directory))
        {
            for (var path : paths.sorted(java.util.Comparator.reverseOrder()).toList())
            {
                Files.delete(path);
            }
        }
    }

    private static void referencePrices()
    {
        try (var exchange = new MatchingExchange(memory()))
        {
            var base = exchange.assets().create("BTC", "BTC", 0).join();
            var quote = exchange.assets().create("USD", "USD", 0).join();
            var buyer = exchange.users().create().join();
            var seller = exchange.users().create().join();
            buyer.spotAccount().join().updateBalance(quote, 100000000).join();
            seller.spotAccount().join().updateBalance(base, 100).join();
            buyer.contractAccount().join().updateBalance(quote, 100000000).join();
            seller.contractAccount().join().updateBalance(quote, 100000000).join();
            var spot = exchange.symbols().create("SPOT", SymbolType.Spot, base, quote).join();
            var spotBook = exchange.books().addSpot(spot, OrderBookImplementation.Radix).join();
            spotBook.place(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 99900, 1, 1).join();
            spotBook.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100100, 1, 1).join();
            var perpetual = exchange.symbols().create("PERP", SymbolType.Perpetual, base, quote).join();
            var spec = new ContractSpec().withTakerFeeRate(0).withLiquidationFeeRate(0).withFundingInterval(100).withFirstFundingTimestamp(100);
            var book = exchange.books().addContract(perpetual, spec, 100000, 0, OrderBookImplementation.Radix).join();
            book.placeContract(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100000, 10, 2, new ContractOrderOptions()).join();
            var order = book.placeContract(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100000, 10, 3, new ContractOrderOptions()).join();
            exchange.contracts().bindSpot(perpetual, spot).join();
            exchange.contracts().refresh(perpetual, 100).join();
            var market = exchange.contracts().market(perpetual).join();
            check(market.referencePrices().indexPrice() == 100000 && market.lastFundingRate() == 100 && market.nextFundingTimestamp() == 200);
            check(order.position().join().snapshot().join().fundingPnL() == -100);
            var config = new ReferencePriceConfig().withSources(List.of(new PriceSource().withType(PriceSourceType.External).withSourceId(7)));
            exchange.contracts().configureReference(perpetual, config).join();
            exchange.contracts().publishPrice(perpetual, 7, 101000, 101).join();
            exchange.contracts().refresh(perpetual, 101).join();
            market = exchange.contracts().market(perpetual).join();
            check(market.referencePrices().indexPrice() == 101000 && market.externalPrices().getFirst().sourceId() == 7);
            exchange.contracts().configureSchedule(perpetual, new ContractSchedule().withFundingInterval(200).withNextFundingTimestamp(400), 102).join();
            exchange.contracts().updateReference(perpetual, new ReferencePrices(100000, 100000, 0, -500, 100000, 1), 102).join();
            check(exchange.contracts().market(perpetual).join().referencePrices().fundingRate() == -500);
            var future = exchange.symbols().create("FUTURE", SymbolType.Future, base, quote).join();
            exchange.books().addContract(future, new ContractSpec().withTakerFeeRate(0).withExpiryTimestamp(500), 100000, 0, OrderBookImplementation.Radix).join();
            exchange.contracts().configureSchedule(future, new ContractSchedule().withExpiryTimestamp(600), 100).join();
            exchange.contracts().refresh(future, 600).join();
            check(exchange.contracts().market(future).join().status() == ContractStatus.Settled);
        }
    }

    private static void objectApi()
    {
        try (var exchange = new MatchingExchange(memory()); var other = new MatchingExchange(memory()))
        {
            var created = exchange.submit(new CreateAsset("BASE", "BASE", 0)).join();
            check(created.id() != 0 && created.id() == created.handle());
            var base = exchange.assets().get(created.id());
            var quote = exchange.assets().create("USD", "USD", 0).join();
            var buyer = exchange.users().create().join();
            var seller = exchange.users().create().join();
            var buyerAccount = buyer.spotAccount().join();
            var sellerAccount = seller.spotAccount().join();
            buyerAccount.updateBalance(quote, 10000).join();
            sellerAccount.updateBalance(base, 100).join();
            var contractAccount = buyer.contractAccount().join();
            buyerAccount.transferTo(contractAccount, quote, 1000).join();
            check(contractAccount.snapshot().join().type() == AccountType.Contract);
            var symbol = exchange.symbols().create("PAIR", SymbolType.Spot, base, quote).join();
            var book = exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
            var order = book.place(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 10, 1).join();
            check(order.handle() != 0 && order.equals(exchange.orders().get(order.id())));
            order.modifyQuantity(1, 2).join();
            order.modifyPrice(101, 3).join();
            check(book.depth(OrderSide.Ask, 10).join().values().getFirst().quantity() == 11);
            order.cancel(4).join();
            check(order.snapshot().join().filling().status() == OrderStatus.Cancelled);
            check(order.position().join() == null);
            check(exchange.users().list(new ObservationQuery()).join().values().size() == 2);
            seller.contractAccount().join().updateBalance(quote, 2000).join();
            var perpetual = exchange.symbols().create("PERP", SymbolType.Perpetual, base, quote).join();
            var spec = new ContractSpec().withTakerFeeRate(0).withFirstFundingTimestamp(100);
            var contractBook = exchange.books().addContract(perpetual, spec, 100, 0, OrderBookImplementation.Radix).join();
            contractBook.placeContract(seller, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 1, 1, new ContractOrderOptions()).join();
            var contractOrder = contractBook.placeContract(buyer, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, 2, new ContractOrderOptions()).join();
            var position = contractOrder.position().join();
            position.setLeverage(5, 3).join();
            position.addMargin(10).join();
            exchange.contracts().settleFunding(perpetual, 1000, 100).join();
            check(position.snapshot().join().leverage() == 5);
            check(exchange.contracts().market(perpetual).join().openPositions() == 2);
            var disposable = exchange.users().create().join();
            disposable.remove().join();
            check(disposable.snapshot().handle((value, failure) -> failure != null && failure.getCause() instanceof BusinessException).join());
            var failed = exchange.submit(new CreateSymbol("BAD", SymbolType.Spot, 9999, 9998)).join();
            check(!failed.result().accepted() && failed.id() == 0);
            try
            {
                buyerAccount.transferTo(other.users().create().join().spotAccount().join(), quote, 1);
                throw new AssertionError("Cross-exchange handle accepted");
            }
            catch (IllegalArgumentException expected)
            {
                check(expected.getMessage().contains("another Exchange"));
            }
        }
    }
}
