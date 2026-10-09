package nhcm.advmatch.example;

import nhcm.advmatch.MatchingExchange;
import nhcm.advmatch.model.*;

public final class SpotExample
{
    public static void main(String[] arguments)
    {
        var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false));
        try (var exchange = new MatchingExchange(config))
        {
            var bitcoin = exchange.assets().create("Bitcoin", "BTC", 0).join();
            var dollar = exchange.assets().create("Dollar", "USD", 0).join();
            var user = exchange.users().create().join();
            var account = user.spotAccount().join();
            account.updateBalance(dollar, 10000).join();
            var symbol = exchange.symbols().create("BTCUSD", SymbolType.Spot, bitcoin, dollar).join();
            var book = exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
            var order = book.place(user, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 10, 1).join();
            var view = order.snapshot().join();
            System.out.println("Order=" + order.id() + " unfilled=" + view.filling().unfilled());
            System.out.println("Bid=" + book.depth(OrderSide.Bid, 20).join().values().getFirst().price());
        }
    }
}
