#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"

#include <cstdlib>
#include <future>
#include <stdexcept>
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

namespace
{
    class CounterConsumer final : public advmatch::consuming::SingleThreadConsumer<int, int>
    {
        int count = 0;
        std::thread::id workerId;

    protected:
        int consume(int&& value) override
        {
            if (value < 0)
            {
                throw std::invalid_argument("negative command");
            }

            if (this->count == 0)
            {
                this->workerId = std::this_thread::get_id();
            }

            CHECK(this->workerId == std::this_thread::get_id());
            ++this->count;
            return value * 2;
        }

    public:
        ~CounterConsumer() override
        {
            stop();
        }

        int getCount() const noexcept
        {
            return this->count;
        }
    };

    class VoidConsumer final : public advmatch::consuming::SingleThreadConsumer<int, void>
    {
        int total = 0;

    protected:
        void consume(int&& value) override
        {
            this->total += value;
        }

    public:
        ~VoidConsumer() override
        {
            stop();
        }

        int getTotal() const noexcept
        {
            return this->total;
        }
    };

    struct PlaceCommand
    {
        advmatch::UserId userId;
        advmatch::SymbolId symbolId;
        advmatch::OrderSide side;
        advmatch::OrderType type;
        advmatch::TimeInForce timeInForce;
        advmatch::Price price;
        advmatch::Quantity quantity;
        advmatch::Timestamp timestamp;
    };

    struct PlaceResponse
    {
        advmatch::PlaceOrderResult result;
        std::vector<advmatch::Trade> trades;
    };

    class MatchingConsumer final : public advmatch::consuming::SingleThreadConsumer<PlaceCommand, PlaceResponse>
    {
        advmatch::MatchingEngine engine;

    protected:
        PlaceResponse consume(PlaceCommand&& command) override
        {
            std::vector<advmatch::Trade> trades;
            auto result = this->engine.placeOrder(command.userId, command.symbolId, command.side, command.type,
                command.timeInForce, command.price, command.quantity, command.timestamp, trades);
            return {std::move(result), std::move(trades)};
        }

    public:
        MatchingConsumer(advmatch::UserManager& users, advmatch::SymbolManager& symbols)
            : engine(users, symbols)
        {}

        ~MatchingConsumer() override
        {
            stop();
        }

        bool addSpotBook(advmatch::SymbolId symbolId)
        {
            return this->engine.addSpotBook(symbolId);
        }
    };
}

int main()
{
    CounterConsumer counter;
    bool rejectedBeforeStart = false;
    try
    {
        counter.post(1);
    }
    catch (const std::logic_error&)
    {
        rejectedBeforeStart = true;
    }
    CHECK(rejectedBeforeStart);

    counter.start(64);
    constexpr int producers = 4;
    constexpr int perProducer = 100;
    std::vector<std::thread> threads;
    std::vector<std::vector<std::future<int>>> responses(producers);

    for (int producer = 0; producer < producers; ++producer)
    {
        threads.emplace_back([&, producer]
        {
            for (int i = 0; i < perProducer; ++i)
            {
                responses[producer].push_back(counter.post(producer * perProducer + i));
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    for (int producer = 0; producer < producers; ++producer)
    {
        for (int i = 0; i < perProducer; ++i)
        {
            CHECK(responses[producer][i].get() == 2 * (producer * perProducer + i));
        }
    }

    auto failure = counter.post(-1);
    bool propagated = false;
    try
    {
        failure.get();
    }
    catch (const std::invalid_argument&)
    {
        propagated = true;
    }
    CHECK(propagated);
    CHECK(counter.post(10).get() == 20);
    auto pending = counter.post(11);
    counter.stop();
    CHECK(pending.get() == 22);
    CHECK(counter.getCount() == producers * perProducer + 2);

    bool rejectedAfterStop = false;
    try
    {
        counter.post(1);
    }
    catch (const std::logic_error&)
    {
        rejectedAfterStop = true;
    }
    CHECK(rejectedAfterStop);

    VoidConsumer voidConsumer;
    voidConsumer.start(8);
    auto completed = voidConsumer.post(5);
    voidConsumer.stop();
    completed.get();
    CHECK(voidConsumer.getTotal() == 5);

    using namespace advmatch;
    UserManager users;
    SymbolManager symbols;
    const UserId buyer = users.createUser();
    const UserId seller = users.createUser();
    const Symbol* symbol = symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2);
    CHECK(symbol);
    CHECK(users.getUser(buyer)->updateBalance(2, 1000));
    CHECK(users.getUser(seller)->updateBalance(1, 10));

    MatchingConsumer matching(users, symbols);
    CHECK(matching.addSpotBook(symbol->symbolId));
    matching.start(64);

    auto ask = matching.post({seller, symbol->symbolId, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 10, 5, 1});
    CHECK(ask.get().result);
    auto bid = matching.post({buyer, symbol->symbolId, OrderSide::Bid, OrderType::Limit, TimeInForce::IOC, 11, 3, 2});
    auto outcome = bid.get();
    CHECK(outcome.result && outcome.trades.size() == 1 && outcome.trades[0].price == 10);

    matching.stop();
    CHECK(users.getUser(buyer)->queryActiveBalance(2) == 970);
    CHECK(users.getUser(buyer)->queryActiveBalance(1) == 3);
    CHECK(users.getUser(seller)->queryLockedBalance(1) == 2);
    CHECK(users.getUser(seller)->queryActiveBalance(2) == 30);
}
