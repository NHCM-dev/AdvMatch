#include "advmatch/matching/MatchingEngine.h"

#include <cstdlib>
#include <vector>

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
    CHECK(users.getUser(seller)->updateBalance(1, 1000));
    CHECK(users.getUser(seller)->updateBalance(3, 1000));
    CHECK(users.getUser(seller)->updateBalance(4, 1000));
    CHECK(users.getUser(buyer)->updateBalance(2, 100000));
    MatchingEngine engine(users, symbols);
    CHECK(engine.addSpotBook(1));
    CHECK(!engine.addSpotBook(1));

    std::vector<Trade> trades;
    const auto ask1 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 10, 1, trades);
    const auto ask2 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 5, 2, trades);
    const auto ask3 = engine.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 101, 4, 3, trades);
    CHECK(ask1 && ask2 && ask3);
    CHECK(engine.bestAsk(1)->price == 100);
    CHECK(engine.askAt(1, 100)->quantity == 15);
    CHECK(engine.askAt(1, 100)->orderCount == 2);

    const auto bid = engine.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 12, 4, trades);
    CHECK(bid && bid.state.status == OrderStatus::Filled);
    CHECK(trades.size() == 2);
    CHECK(trades[0].makerOrderId == ask1.orderId && trades[0].quantity == 10);
    CHECK(trades[1].makerOrderId == ask2.orderId && trades[1].quantity == 2);
    CHECK(engine.askAt(1, 100)->quantity == 3);
    CHECK(engine.queryOrder(ask1.orderId)->getStatus() == OrderStatus::Filled);

    CHECK(engine.cancelOrder(seller, ask2.orderId, 5));
    CHECK(engine.cancelOrder(seller, ask2.orderId, 6).state == CancelOrderState::OrderCancelled);
    CHECK(engine.bestAsk(1)->price == 101);
    CHECK(engine.modifyPrice(seller, ask3.orderId, 99, 7, trades));
    CHECK(engine.bestAsk(1)->price == 99);
    CHECK(engine.modifyQuantity(seller, ask3.orderId, 2, 8));
    CHECK(engine.bestAsk(1)->quantity == 6);

    const auto fok = engine.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::FOK, 99, 7, 9, trades);
    CHECK(!fok && fok.rejectReason == PlaceOrderRejectReason::InsufficientLiquidity);
    CHECK(trades.empty() && engine.bestAsk(1)->quantity == 6);

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
    CHECK(priority.bestBid(2)->price == 96);
    CHECK(priority.bestBid(2)->quantity == 1);
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
    CHECK(cases.queryOrder(lowBid.orderId)->getStatus() == OrderStatus::Filled);

    const auto cancelled = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 90, 2, 23, trades);
    CHECK(cases.modifyQuantity(buyer, cancelled.orderId, -2, 24));
    CHECK(cases.queryOrder(cancelled.orderId)->getStatus() == OrderStatus::Cancelled);
    CHECK(!cases.bestBid(3));
    CHECK(cases.cancelOrder(buyer, cancelled.orderId, 25).state == CancelOrderState::OrderCancelled);
    const auto invalid = cases.placeOrder(buyer, 3, OrderSide::Bid, OrderType::Market, TimeInForce::GTC, 0, 1, 26, trades);
    CHECK(!invalid && invalid.rejectReason == PlaceOrderRejectReason::InvalidTimeInForce);
}
