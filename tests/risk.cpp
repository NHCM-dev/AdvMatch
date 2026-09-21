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
    SymbolManager symbols;
    const UserId buyerId = users.createUser();
    const UserId sellerId = users.createUser();
    User* buyer = users.getUser(buyerId);
    User* seller = users.getUser(sellerId);
    CHECK(buyer->updateBalance(2, 1000));
    CHECK(seller->updateBalance(1, 20));

    MatchingEngine engine(users, symbols);
    CHECK(!engine.addSpotBook(1));
    const Symbol* symbol = symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2);
    CHECK(symbol && symbol->symbolId == 1);
    CHECK(!symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2));
    CHECK(!symbols.createSymbol("INVALID", SymbolType::Spot, 1, 1));
    CHECK(engine.addSpotBook(symbol->symbolId));

    TradeBuffer trades;
    const auto insufficientBuy = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 20, 1, trades);
    CHECK(insufficientBuy.rejectReason == PlaceOrderRejectReason::InsufficientBalance);
    const auto insufficientSell = engine.placeOrder(sellerId, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 8, 21, 2, trades);
    CHECK(insufficientSell.rejectReason == PlaceOrderRejectReason::InsufficientBalance);

    const auto ask = engine.placeOrder(sellerId, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 8, 5, 3, trades);
    CHECK(ask && seller->queryActiveBalance(1) == 15 && seller->queryLockedBalance(1) == 5);

    const auto bid = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 10, 3, 4, trades);
    CHECK(bid && trades.size() == 1 && trades[0].price == 8);
    CHECK(buyer->queryActiveBalance(2) == 976 && buyer->queryLockedBalance(2).value_or(0) == 0);
    CHECK(buyer->queryActiveBalance(1) == 3);
    CHECK(seller->queryActiveBalance(1) == 15 && seller->queryLockedBalance(1) == 2);
    CHECK(seller->queryActiveBalance(2) == 24);

    const auto restingBid = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 5, 2, 5, trades);
    CHECK(restingBid && buyer->queryActiveBalance(2) == 966 && buyer->queryLockedBalance(2) == 10);
    CHECK(engine.modifyPrice(buyerId, restingBid.orderId, 7, 6, trades));
    CHECK(buyer->queryActiveBalance(2) == 962 && buyer->queryLockedBalance(2) == 14);
    CHECK(engine.modifyQuantity(buyerId, restingBid.orderId, -1, 7));
    CHECK(buyer->queryActiveBalance(2) == 969 && buyer->queryLockedBalance(2) == 7);
    CHECK(engine.cancelOrder(buyerId, restingBid.orderId, 8));
    CHECK(buyer->queryActiveBalance(2) == 976 && buyer->queryLockedBalance(2).value_or(0) == 0);

    CHECK(engine.modifyQuantity(sellerId, ask.orderId, 3, 9));
    CHECK(seller->queryActiveBalance(1) == 12 && seller->queryLockedBalance(1) == 5);
    CHECK(engine.cancelOrder(sellerId, ask.orderId, 10));
    CHECK(seller->queryActiveBalance(1) == 17 && seller->queryLockedBalance(1).value_or(0) == 0);

    const auto askTwo = engine.placeOrder(sellerId, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 8, 2, 11, trades);
    CHECK(askTwo);
    const auto marketBuy = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Market, TimeInForce::IOC, 0, 3, 12, trades);
    CHECK(marketBuy && marketBuy.state.status == OrderStatus::Cancelled && marketBuy.state.filled == 2);
    CHECK(buyer->queryActiveBalance(2) == 960 && buyer->queryLockedBalance(2).value_or(0) == 0);
    CHECK(buyer->queryActiveBalance(1) == 5);
    CHECK(seller->queryActiveBalance(1) == 15 && seller->queryLockedBalance(1).value_or(0) == 0);
    CHECK(seller->queryActiveBalance(2) == 40);

    const auto retained = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 5, 1, 13, trades);
    CHECK(retained && buyer->queryLockedBalance(2) == 5);
    CHECK(users.removeUser(buyerId));
    CHECK(symbols.removeSymbol(1));
    CHECK(!users.getUser(buyerId));
    CHECK(!symbols.getSymbol(1));
    CHECK(!users.containsUser(buyerId) && users.getUserCount() == 1);
    CHECK(!symbols.containsSymbol(1) && symbols.getSymbolCount() == 0);
    CHECK(engine.cancelOrder(buyerId, retained.orderId, 14));
    CHECK(buyer->queryLockedBalance(2).value_or(0) == 0);

    const auto inactiveUser = engine.placeOrder(buyerId, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 5, 1, 15, trades);
    CHECK(!inactiveUser && inactiveUser.rejectReason == PlaceOrderRejectReason::UserNotFound);
    const auto inactiveSymbol = engine.placeOrder(sellerId, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 5, 1, 16, trades);
    CHECK(!inactiveSymbol && inactiveSymbol.rejectReason == PlaceOrderRejectReason::InvalidSymbol);
}
