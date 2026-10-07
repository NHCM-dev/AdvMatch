#include "advmatch/matching/MatchingEngine.h"

#include <cstdlib>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::abort(); \
        } \
    } while (false)

int main()
{
    using namespace advmatch;
    UserManager users;
    const UserId seller = users.createUser();
    const UserId buyer = users.createUser();
    SymbolManager symbols;
    CHECK(symbols.createSymbol("BASE1/QUOTE", SymbolType::Spot, 1, 2));
    CHECK(symbols.createSymbol("BASE2/QUOTE", SymbolType::Spot, 3, 2));
    CHECK(symbols.createSymbol("BASE3/QUOTE", SymbolType::Spot, 4, 2));
    CHECK(users.getUser(seller)->getSpotAccount().updateBalance(1, 1000));
    CHECK(users.getUser(seller)->getSpotAccount().updateBalance(3, 1000));
    CHECK(users.getUser(seller)->getSpotAccount().updateBalance(4, 1000));
    CHECK(users.getUser(buyer)->getSpotAccount().updateBalance(2, 100000));
    MatchingEngine engine(users, symbols);
    CHECK(engine.addSpotBook(1));
    CHECK(!engine.addSpotBook(1));

    TradeBuffer trades;
    const auto ask1 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 10, 1, trades);
    const auto ask2 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 5, 2, trades);
    const auto ask3 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 101, 4, 3, trades);
    CHECK(ask1 && ask2 && ask3);
    std::size_t visitedBooks = 0;
    engine.forEachOrderBook([&](const OrderBook& book)
    {
        ++visitedBooks;
        Price previousPrice = 0;
        std::size_t visitedLevels = 0;
        book.forEachLevel(OrderSide::Ask, [&](const Quote& level)
        {
            CHECK(level.price > previousPrice);
            previousPrice = level.price;
            ++visitedLevels;
        });
        CHECK(visitedLevels == 2);

        std::size_t visitedOrders = 0;
        const OrderId expectedIds[] = {ask1.orderId, ask2.orderId, ask3.orderId};
        book.forEachOrder(OrderSide::Ask, [&](const BasicOrder& order)
        {
            CHECK(visitedOrders < 3);
            CHECK(order.orderId == expectedIds[visitedOrders]);
            ++visitedOrders;
        });
        CHECK(visitedOrders == 3);
    });
    CHECK(visitedBooks == 1);

    std::size_t totalOrders = 0;
    engine.forEachOrder([&](const BasicOrder&)
    {
        ++totalOrders;
    });
    CHECK(totalOrders == 3);

    const auto initialBestAsk = engine.bestAsk(1);
    const auto initialAskLevel = engine.askAt(1, 100);
    CHECK(initialBestAsk);
    CHECK(initialAskLevel);
    CHECK(initialBestAsk->price == 100);
    CHECK(initialAskLevel->quantity == 15);
    CHECK(initialAskLevel->orderCount == 2);

    const auto bid = engine.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 12, 4, trades);
    CHECK(bid && bid.state.status == OrderStatus::Filled);
    CHECK(trades.size() == 2);
    CHECK(trades[0].makerOrderId == ask1.orderId && trades[0].quantity == 10);
    CHECK(trades[1].makerOrderId == ask2.orderId && trades[1].quantity == 2);
    const auto partiallyFilledAskLevel = engine.askAt(1, 100);
    const auto filledAsk = engine.queryOrder(ask1.orderId);
    CHECK(partiallyFilledAskLevel);
    CHECK(filledAsk);
    CHECK(partiallyFilledAskLevel->quantity == 3);
    CHECK(filledAsk->getStatus() == OrderStatus::Filled);

    CHECK(engine.cancelOrder(seller, ask2.orderId, 5));
    CHECK(engine.cancelOrder(seller, ask2.orderId, 6).state == CancelOrderState::OrderCancelled);
    const auto bestAskAfterCancel = engine.bestAsk(1);
    CHECK(bestAskAfterCancel);
    CHECK(bestAskAfterCancel->price == 101);
    CHECK(engine.modifyPrice(seller, ask3.orderId, 99, 7, trades));
    const auto bestAskAfterPriceChange = engine.bestAsk(1);
    CHECK(bestAskAfterPriceChange);
    CHECK(bestAskAfterPriceChange->price == 99);
    CHECK(engine.modifyQuantity(seller, ask3.orderId, 2, 8));
    const auto bestAskAfterQuantityChange = engine.bestAsk(1);
    CHECK(bestAskAfterQuantityChange);
    CHECK(bestAskAfterQuantityChange->quantity == 6);

    const auto fok = engine.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::FOK, 99, 7, 9, trades);
    CHECK(!fok && fok.rejectReason == PlaceOrderState::InsufficientLiquidity);
    const auto bestAskAfterRejectedFok = engine.bestAsk(1);
    CHECK(bestAskAfterRejectedFok);
    CHECK(trades.empty() && bestAskAfterRejectedFok->quantity == 6);

    const auto market = engine.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Market, TimeInForce::IOC, 0, 8, 10, trades);
    CHECK(market && market.state.status == OrderStatus::Cancelled);
    CHECK(market.state.filled == 6 && market.state.unfilled == 2);
    CHECK(trades.size() == 1 && trades[0].price == 99);
    CHECK(!engine.bestAsk(1));

    MatchingEngine priority(users, symbols);
    CHECK(priority.addSpotBook(2));
    const auto bid95 = priority.placeOrder(buyer, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 95, 1, 11, trades);
    const auto bid96a = priority.placeOrder(buyer, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 96, 1, 12, trades);
    const auto bid96b = priority.placeOrder(buyer, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 96, 1, 13, trades);
    CHECK(priority.modifyQuantity(buyer, bid96a.orderId, 1, 14));
    const auto sell = priority.placeOrder(seller, 2, OrderSide::Ask, OrderType::Market, TimeInForce::IOC, 0, 2, 15, trades);
    CHECK(sell && trades.size() == 2);
    CHECK(trades[0].makerOrderId == bid96b.orderId);
    CHECK(trades[1].makerOrderId == bid96a.orderId);
    const auto remainingBestBid = priority.bestBid(2);
    CHECK(remainingBestBid);
    CHECK(remainingBestBid->price == 96);
    CHECK(remainingBestBid->quantity == 1);
    CHECK(priority.cancelOrder(seller, bid95.orderId, 16).state == CancelOrderState::WrongUser);

    MatchingEngine cases(users, symbols);
    CHECK(cases.addSpotBook(3));
    const auto ask110 = cases.placeOrder(seller, 3, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 110, 3, 17, trades);
    const auto ask111 = cases.placeOrder(seller, 3, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 111, 2, 18, trades);
    const auto full = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Limit, TimeInForce::FOK, 111, 5, 19, trades);
    CHECK(full && full.state.status == OrderStatus::Filled);
    CHECK(trades.size() == 2);
    CHECK(trades[0].makerOrderId == ask110.orderId && trades[0].price == 110);
    CHECK(trades[1].makerOrderId == ask111.orderId && trades[1].price == 111);
    CHECK(!cases.bestAsk(3));

    const auto highAsk = cases.placeOrder(seller, 3, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 120, 1, 20, trades);
    const auto lowBid = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 1, 21, trades);
    CHECK(cases.modifyPrice(buyer, lowBid.orderId, 120, 22, trades));
    CHECK(trades.size() == 1 && trades[0].makerOrderId == highAsk.orderId);
    CHECK(!cases.bestBid(3) && !cases.bestAsk(3));
    const auto filledBid = cases.queryOrder(lowBid.orderId);
    CHECK(filledBid);
    CHECK(filledBid->getStatus() == OrderStatus::Filled);

    const auto cancelled = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 90, 2, 23, trades);
    CHECK(cases.modifyQuantity(buyer, cancelled.orderId, -2, 24));
    const auto cancelledOrder = cases.queryOrder(cancelled.orderId);
    CHECK(cancelledOrder);
    CHECK(cancelledOrder->getStatus() == OrderStatus::Cancelled);
    CHECK(!cases.bestBid(3));
    CHECK(cases.cancelOrder(buyer, cancelled.orderId, 25).state == CancelOrderState::OrderCancelled);
    const auto invalid = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Market, TimeInForce::GTC, 0, 1, 26, trades);
    CHECK(!invalid && invalid.rejectReason == PlaceOrderState::InvalidTimeInForce);

    CHECK(symbols.createSymbol("BASE4/QUOTE", SymbolType::Spot, 5, 2));
    CHECK(users.getUser(seller)->getSpotAccount().updateBalance(5, 10));
    MatchingEngine spill(users, symbols);
    CHECK(spill.addSpotBook(4));
    for (Timestamp timestamp = 27; timestamp < 32; ++timestamp)
    {
        CHECK(spill.placeOrder(seller, 4, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 1, timestamp, trades));
    }

    const auto fiveFills = spill.placeOrder(buyer, 4, OrderSide::Bid, OrderType::Limit, TimeInForce::FOK, 100, 5, 32, trades);
    CHECK(fiveFills && trades.size() == 5);
    const std::size_t expandedTradeCapacity = trades.capacity();
    CHECK(expandedTradeCapacity >= 5);

    const auto afterExpansion = spill.placeOrder(seller, 4, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 101, 1, 33, trades);
    CHECK(afterExpansion && trades.empty());
    CHECK(trades.capacity() == expandedTradeCapacity);
}
