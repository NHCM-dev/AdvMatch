package nhcm.advmatch;

import nhcm.advmatch.command.account.UpdateBalance;
import nhcm.advmatch.command.read.ReadChanges;
import nhcm.advmatch.model.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class SubscriptionIntegrationTest
{
    private static void require(boolean condition, String message)
    {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception
    {
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false))
                                         .withObservation(new ObservationConfig().withIndexUserOpenOrders(true));
        try (var exchange = new Exchange(config))
        {
            var base = exchange.assets().create("base", "Base", 0).join();
            var quote = exchange.assets().create("quote", "Quote", 0).join();
            var maker = exchange.users().create().join();
            var buyer = exchange.users().create().join();
            var quiet = exchange.users().create().join();
            var makerAccount = maker.spotAccount().join();
            var buyerAccount = buyer.spotAccount().join();
            exchange.submit(new UpdateBalance(makerAccount.id(), base.id(), 100)).join();
            exchange.submit(new UpdateBalance(buyerAccount.id(), quote.id(), 10000)).join();
            var symbol = exchange.symbols().create("BASE/QUOTE", SymbolType.Spot, base, quote).join();
            exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
            var queries = List.of(new ObservationQuery().withKind(ReadKind.UserOpenOrders).withUserId(maker.id()),
                                  new ObservationQuery().withKind(ReadKind.UserOpenOrders).withUserId(quiet.id()),
                                  new ObservationQuery().withKind(ReadKind.Balances).withAccountId(buyerAccount.id()));
            var empty = new ReadStamp();
            var first = exchange.submit(new ReadChanges(queries, List.of(empty, empty, empty))).join();
            require(first.results().size() == 3, "initial images");
            var known = List.of(first.stamp(), first.stamp(), first.stamp());
            require(exchange.submit(new ReadChanges(queries, known)).join().results().isEmpty(), "idle queries must not build images");
            var order = exchange.matching().place(maker, symbol, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 10, 5, 1, null).join();
            var changed = exchange.submit(new ReadChanges(queries, known)).join();
            require(changed.results().size() == 1 && changed.results().getFirst().query().userId() == maker.id(), "quiet account must stay unchanged");
            var afterPlace = List.of(changed.stamp(), changed.stamp(), changed.stamp());
            exchange.matching().place(buyer, symbol, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 10, 2, 2, null).join();
            var filled = exchange.submit(new ReadChanges(queries, afterPlace)).join();
            require(filled.results().size() == 2, "both sides of a trade must invalidate");
            require(filled.results().stream().filter(row -> row.query().kind() == ReadKind.UserOpenOrders)
                          .findFirst().orElseThrow().typed(OrderView.class).values().getFirst().filling().unfilled() == 3, "partial fill snapshot");
            var afterFill = List.of(filled.stamp(), filled.stamp(), filled.stamp());
            order.cancel(3).join();
            require(exchange.submit(new ReadChanges(queries, afterFill)).join().results().getFirst().typed(OrderView.class).values().isEmpty(), "cancel must publish an empty image");
            var query = queries.get(1);
            try (var a = exchange.subscriptions().subscribe(query, Duration.ofMillis(100), ignored -> {});
                 var b = exchange.subscriptions().subscribe(query, Duration.ofMillis(100), ignored -> {}))
            {
                a.first().get(5, TimeUnit.SECONDS);
                b.first().get(5, TimeUnit.SECONDS);
                require(exchange.subscriptions().activeQueries() == 1, "identical queries share a feed");
                long images = exchange.subscriptions().metrics().changedSnapshots();
                Thread.sleep(350);
                require(exchange.subscriptions().metrics().changedSnapshots() == images, "idle subscriptions must not construct snapshots");
                require(exchange.subscriptions().metrics().conditionalChecks() > images, "conditional checks must run");
            }
            require(exchange.subscriptions().activeQueries() == 0, "feed lifetime");
            var conditional = new ConditionalOrderRequest().withUserId(quiet.id()).withSymbolId(symbol.id()).withQuantity(1)
                                                           .withKind(ConditionalOrderKind.TakeProfit).withTriggerPrice(11).withTimestamp(4);
            exchange.conditionalOrders().place(conditional).join();
            var conditionalQuery = new ObservationQuery().withKind(ReadKind.ConditionalOrders).withUserId(quiet.id());
            var beforeTrigger = exchange.submit(new ReadChanges(List.of(conditionalQuery), List.of(empty))).join();
            exchange.matching().place(maker, symbol, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 11, 1, 5, null).join();
            exchange.matching().place(buyer, symbol, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 11, 1, 6, null).join();
            var triggered = exchange.submit(new ReadChanges(List.of(conditionalQuery), List.of(beforeTrigger.stamp()))).join();
            require(triggered.results().size() == 1, "third-party conditional activation must invalidate even if execution is rejected");
            require(triggered.results().getFirst().typed(ConditionalOrderView.class).values().getFirst().status() != ConditionalOrderStatus.Waiting,
                    "conditional rejection must be visible");
            var waitingQuery = conditionalQuery.withIncludeTerminal(false);
            require(exchange.submit(new ReadChanges(List.of(waitingQuery), List.of(empty))).join().results().getFirst()
                            .typed(ConditionalOrderView.class).values().isEmpty(), "activated conditionals leave the waiting index");
            for (int index = 0; index < 1000; index++)
            {
                exchange.conditionalOrders().place(conditional.withUserId(buyer.id()).withTriggerPrice(1000).withTimestamp(6)).join();
            }
            exchange.conditionalOrders().place(conditional.withUserId(maker.id()).withTriggerPrice(1000).withTimestamp(6)).join();
            var scopedConditional = waitingQuery.withUserId(maker.id()).withScanBudget(1);
            var indexed = exchange.submit(new ReadChanges(List.of(scopedConditional), List.of(empty))).join().results().getFirst();
            require(indexed.page().examined() == 1 && indexed.typed(ConditionalOrderView.class).values().size() == 1,
                    "private conditionals must not scan another account's history");
            var contract = exchange.symbols().create("BASE/PERP", SymbolType.Perpetual, base, quote).join();
            exchange.books().addContract(contract, new ContractSpec().withFirstFundingTimestamp(1000), 100, 0, OrderBookImplementation.Radix).join();
            maker.contractAccount().join().updateBalance(quote, 10000).join();
            buyer.contractAccount().join().updateBalance(quote, 10000).join();
            var positionQueries = List.of(new ObservationQuery().withKind(ReadKind.Positions).withUserId(maker.id()),
                                          new ObservationQuery().withKind(ReadKind.Positions).withUserId(buyer.id()),
                                          new ObservationQuery().withKind(ReadKind.Positions).withUserId(quiet.id()));
            var beforeContract = exchange.submit(new ReadChanges(positionQueries, List.of(empty, empty, empty))).join();
            exchange.matching().place(maker, contract, OrderSide.Ask, OrderType.Limit, TimeInForce.GTC, 100, 2, 7, new ContractOrderOptions()).join();
            exchange.matching().place(buyer, contract, OrderSide.Bid, OrderType.Limit, TimeInForce.IOC, 100, 1, 8, new ContractOrderOptions()).join();
            var positions = exchange.submit(new ReadChanges(positionQueries, List.of(beforeContract.stamp(), beforeContract.stamp(), beforeContract.stamp()))).join();
            require(positions.results().size() == 2, "contract fills update counterpart positions without refreshing an unrelated account");
            require(exchange.audit().report(32).join().conserved(), "financial conservation");
        }
        System.out.println("Conditional subscriptions: idle reads, scope isolation, counterpart fills, empty publication and lifetime passed");
    }
}
