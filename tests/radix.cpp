#include "advmatch/util/RadixIndex.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <vector>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #condition); \
            std::abort(); \
        } \
    } while (false)

namespace
{
    using namespace advmatch;

    void indexAgainstOrderedMap()
    {
        RadixIndex<std::uint64_t> index;
        std::map<std::uint64_t, std::uint64_t> reference;
        index.reserve(10'000);
        std::mt19937_64 random(721);
        const auto verify = [&]
        {
            CHECK(index.size() == reference.size());
            const auto* entry = index.first();
            const auto* previous = static_cast<const RadixIndex<std::uint64_t>::Entry*>(nullptr);
            for (const auto& [key, value] : reference)
            {
                CHECK(entry && entry->key == key && entry->value == value && entry->previous == previous);
                CHECK(index.find(key) == entry);
                previous = entry;
                entry = entry->next;
            }

            CHECK(!entry && index.last() == previous);
            entry = index.last();
            for (auto it = reference.rbegin(); it != reference.rend(); ++it)
            {
                CHECK(entry && entry->key == it->first);
                entry = entry->previous;
            }

            CHECK(!entry);
        };

        auto* stable = &index.getOrCreate(0, 42);
        reference.emplace(0, 42);
        for (int bit = 0; bit < 64; ++bit)
        {
            const auto key = std::uint64_t{1} << bit;
            CHECK(index.getOrCreate(key, key).value == key);
            reference.emplace(key, key);
        }

        index.getOrCreate(std::numeric_limits<std::uint64_t>::max(), 99);
        reference.emplace(std::numeric_limits<std::uint64_t>::max(), 99);
        for (int iteration = 0; iteration < 30'000; ++iteration)
        {
            const auto key = iteration % 2 == 0 ? random() : random() % 1024 + 1;
            if (iteration % 3 == 0)
            {
                CHECK(index.erase(key) == (reference.erase(key) != 0));
            }
            else
            {
                const auto [it, inserted] = reference.try_emplace(key, key ^ 123);
                CHECK(index.getOrCreate(key, key ^ 123).value == it->second);
            }

            CHECK(index.find(0) == stable);
            if (iteration % 1000 == 0)
            {
                verify();
            }
        }

        verify();
        while (auto* first = index.first())
        {
            index.erase(*first);
        }

        CHECK(index.empty() && !index.first() && !index.last());
        CHECK(index.getOrCreate(100, 5).value == 5);
        CHECK(index.erase(100) && !index.erase(100));
    }

    struct Fixture
    {
        UserManager users;
        SymbolManager symbols;
        MatchingEngine matching{this->users, this->symbols};
        TradeBuffer trades;

        explicit Fixture(OrderBookImplementation implementation)
        {
            CHECK(this->symbols.createSymbol("PAIR", SymbolType::Spot, 1, 2));
            CHECK(this->matching.addSpotBook(1, implementation));
            CHECK(this->matching.reservePriceLevels(1, 64, 64));
            for (int index = 0; index < 4; ++index)
            {
                const UserId user = this->users.createUser();
                CHECK(this->users.getUser(user)->getSpotAccount().updateBalance(1, 1'000'000));
                CHECK(this->users.getUser(user)->getSpotAccount().updateBalance(2, 1'000'000'000));
            }
        }
    };

    void equivalentMatching()
    {
        Fixture radix(OrderBookImplementation::Radix);
        Fixture tree(OrderBookImplementation::IndexedTree);
        std::mt19937_64 random(938);
        std::vector<std::pair<UserId, OrderId>> orders;
        for (int iteration = 0; iteration < 12'000; ++iteration)
        {
            radix.trades.clear();
            tree.trades.clear();
            const auto action = random() % 5;
            const UserId user = random() % 4 + 1;
            const Price price = random() % 21 + 90;
            if (action < 2 || orders.empty())
            {
                const auto side = random() % 2 == 0 ? OrderSide::Ask : OrderSide::Bid;
                const auto tif = static_cast<TimeInForce>(random() % 3);
                const Quantity quantity = random() % 10 + 1;
                const auto left = radix.matching.placeOrder(user, 1, side, OrderType::Limit, tif, price, quantity, 1, radix.trades);
                const auto right = tree.matching.placeOrder(user, 1, side, OrderType::Limit, tif, price, quantity, 1, tree.trades);
                CHECK(left.orderId == right.orderId && left.rejectReason == right.rejectReason && left.state.status == right.state.status);
                if (left)
                {
                    orders.emplace_back(user, left.orderId);
                }
            }
            else
            {
                const auto [owner, id] = orders[random() % orders.size()];
                if (action == 2)
                {
                    CHECK(radix.matching.cancelOrder(owner, id, 1).state == tree.matching.cancelOrder(owner, id, 1).state);
                }
                else if (action == 3)
                {
                    CHECK(radix.matching.modifyPrice(owner, id, price, 1, radix.trades).state == tree.matching.modifyPrice(owner, id, price, 1, tree.trades).state);
                }
                else
                {
                    const QuantityChange change = static_cast<QuantityChange>(random() % 9) - 4;
                    CHECK(radix.matching.modifyQuantity(owner, id, change, 1).state == tree.matching.modifyQuantity(owner, id, change, 1).state);
                }
            }

            CHECK(radix.trades.size() == tree.trades.size());
            for (std::size_t index = 0; index < radix.trades.size(); ++index)
            {
                const auto& left = radix.trades[index];
                const auto& right = tree.trades[index];
                CHECK(left.makerOrderId == right.makerOrderId && left.takerOrderId == right.takerOrderId && left.quantity == right.quantity && left.price == right.price);
            }

            const auto compareQuote = [&](const auto& left, const auto& right)
            {
                CHECK(left.has_value() == right.has_value());
                if (left)
                {
                    CHECK(left->price == right->price && left->quantity == right->quantity && left->orderCount == right->orderCount);
                }
            };
            compareQuote(radix.matching.bestBid(1), tree.matching.bestBid(1));
            compareQuote(radix.matching.bestAsk(1), tree.matching.bestAsk(1));
            for (Price value = 90; value <= 110; ++value)
            {
                compareQuote(radix.matching.bidAt(1, value), tree.matching.bidAt(1, value));
                compareQuote(radix.matching.askAt(1, value), tree.matching.askAt(1, value));
            }

            for (UserId owner = 1; owner <= 4; ++owner)
            {
                for (AssetId asset : {1u, 2u})
                {
                    const auto& left = radix.users.getUser(owner)->getSpotAccount();
                    const auto& right = tree.users.getUser(owner)->getSpotAccount();
                    CHECK(left.queryActiveBalance(asset) == right.queryActiveBalance(asset));
                    CHECK(left.queryReservedBalance(asset) == right.queryReservedBalance(asset));
                }
            }
        }
    }

    void intrusiveQueueFences()
    {
        LiquidationQueue queue;
        std::array<ContractParticipant, 5> participants;
        for (auto& participant : participants)
        {
            queue.enqueue(participant, participant.liquidationLink);
            queue.enqueue(participant, participant.liquidationLink);
        }

        CHECK(queue.size() == 5);
        {
            LiquidationQueue::Pass pass(queue);
            auto* first = pass.next();
            CHECK(first == &participants[0]);
            queue.enqueue(*first, first->liquidationLink);
            queue.remove(participants[2].liquidationLink);
            CHECK(pass.next() == &participants[1]);
            CHECK(pass.next() == &participants[3]);
            CHECK(pass.next() == &participants[4]);
            CHECK(!pass.next() && pass.exhausted() && queue.size() == 1);
        }

        {
            LiquidationQueue::Pass pass(queue);
            CHECK(pass.next() == &participants[0] && !pass.next());
        }

        CHECK(queue.empty());
        queue.enqueue(participants[1], participants[1].liquidationLink);
        queue.clear();
        CHECK(queue.empty() && !participants[1].liquidationLink.linked);
    }

    void budgetedLiquidations()
    {
        UserManager users;
        SymbolManager symbols;
        MatchingEngine matching(users, symbols);
        CHECK(symbols.createSymbol("BUDGET", SymbolType::Perpetual, 1, 2));
        ContractSpec spec;
        spec.firstFundingTimestamp = 1000;
        CHECK(matching.addContractBook(1, spec, 100, 0));
        for (int index = 0; index < 3; ++index)
        {
            const auto id = users.createUser();
            CHECK(users.getUser(id)->getContractAccount().updateBalance(2, 10'000));
        }

        TradeBuffer trades;
        ContractOrderOptions options;
        CHECK(matching.placeContractOrder(1, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 5, 1, options, trades));
        for (int index = 0; index < 5; ++index)
        {
            CHECK(matching.placeContractOrder(2, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::IOC, 100, 1, 1, options, trades));
        }

        auto& contracts = matching.getContractEngine();
        contracts.setAutomaticLiquidationBudget(0);
        CHECK(contracts.updateMarkPrice(1, 80, 2).state == ContractState::PendingLiquidation);
        CHECK(contracts.findMarket(1)->pendingLiquidations.size() == 5);
        const auto zero = contracts.processLiquidations(1, 2, true, 0);
        CHECK(zero.affectedPositions == 0 && contracts.findMarket(1)->pendingLiquidations.size() == 5);
        const auto retained = matching.retainedOrderCount();
        for (int index = 0; index < 10; ++index)
        {
            CHECK(contracts.processLiquidations(1, 2, true, 1).state == ContractState::PendingLiquidation);
        }

        CHECK(matching.retainedOrderCount() == retained);
        CHECK(contracts.fundInsurance(1, users.getUser(3)->getContractAccount(), 100));
        CHECK(matching.placeContractOrder(3, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 80, 5, 3, options, trades));
        CHECK(contracts.findMarket(1)->pendingLiquidations.size() == 5);
        const auto one = contracts.processLiquidations(1, 3, true, 1);
        CHECK(one.affectedPositions == 1 && contracts.findMarket(1)->pendingLiquidations.size() == 4);
        const auto two = contracts.processLiquidations(1, 3, true, 2);
        CHECK(two.affectedPositions == 2 && contracts.findMarket(1)->pendingLiquidations.size() == 2);
        CHECK(contracts.processLiquidations(1, 3));
        CHECK(contracts.findMarket(1)->pendingLiquidations.empty());
    }
}

int main()
{
    indexAgainstOrderedMap();
    equivalentMatching();
    intrusiveQueueFences();
    budgetedLiquidations();
    std::puts("Radix and intrusive liquidation tests passed");
}
