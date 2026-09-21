#include "advmatch/matching/QueuedMatchingEngine.h"

#include <algorithm>
#include <cstdlib>
#include <thread>
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
    const auto buyer = users.createUser();
    const auto seller = users.createUser();
    SymbolManager symbols;
    const Symbol* symbol = symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2);
    CHECK(symbol);
    CHECK(users.getUser(buyer)->updateBalance(2, 100000));
    CHECK(users.getUser(seller)->updateBalance(1, 1000));
    QueuedMatchingEngine engine(users, symbols);
    CHECK(engine.addSpotBook(symbol->symbolId));
    engine.start(64);

    constexpr int producers = 4;
    constexpr int perProducer = 100;
    std::vector<std::thread> threads;
    std::vector<std::vector<OrderId>> ids(producers);
    for (int producer = 0; producer < producers; ++producer)
    {
        threads.emplace_back(
            [&, producer]
            {
                for (int i = 0; i < perProducer; ++i)
                {
                    auto outcome = engine.placeOrder(buyer, symbol->symbolId, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 90, 1,
                                                     static_cast<Timestamp>(producer * perProducer + i + 1));
                    CHECK(outcome.result && outcome.trades.empty());
                    ids[producer].push_back(outcome.result.orderId);
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    std::vector<OrderId> all;
    for (const auto& group : ids)
    {
        all.insert(all.end(), group.begin(), group.end());
    }
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    CHECK(engine.bestBid(symbol->symbolId)->quantity == producers * perProducer);
    CHECK(engine.bidAt(symbol->symbolId, 90)->orderCount == producers * perProducer);

    auto fill = engine.placeOrder(seller, symbol->symbolId, OrderSide::Ask, OrderType::Market, TimeInForce::IOC, 0, 150, 500);
    CHECK(fill.result && fill.result.state.status == OrderStatus::Filled);
    CHECK(fill.trades.size() == 150);
    for (std::size_t i = 0; i < fill.trades.size(); ++i)
    {
        CHECK(fill.trades[i].makerOrderId == all[i]);
        CHECK(fill.trades[i].price == 90);
    }
    CHECK(engine.bestBid(symbol->symbolId)->quantity == 250);
    CHECK(engine.queryOrder(all.front())->getStatus() == OrderStatus::Filled);
    engine.stop();
    CHECK(users.getUser(buyer)->queryActiveBalance(2) == 64000);
    CHECK(users.getUser(buyer)->queryLockedBalance(2) == 22500);
    CHECK(users.getUser(buyer)->queryActiveBalance(1) == 150);
    CHECK(users.getUser(seller)->queryActiveBalance(1) == 850);
    CHECK(users.getUser(seller)->queryActiveBalance(2) == 13500);
}
