#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "Optimization regression failed at %d: %s\n", __LINE__, #condition); \
            std::abort(); \
        } \
    } while (false)

namespace
{
    using namespace advmatch;

    struct Archive final : HistorySink
    {
        bool accepting = false;
        std::size_t orders = 0;
        std::size_t positions = 0;
        std::optional<BasicOrder> firstOrder;
        std::optional<Position> firstPosition;

        bool archiveOrder(const BasicOrder& order) noexcept override
        {
            if (!this->accepting)
            {
                return false;
            }

            ++this->orders;
            if (!this->firstOrder)
            {
                this->firstOrder.emplace(order);
            }

            return true;
        }

        bool archivePosition(const Position& position) noexcept override
        {
            if (!this->accepting)
            {
                return false;
            }

            ++this->positions;
            if (!this->firstPosition)
            {
                this->firstPosition.emplace(position);
            }

            return true;
        }
    };

    struct Fixture
    {
        UserManager users;
        SymbolManager symbols;
        MatchingEngine matching{this->users, this->symbols};
        TradeBuffer trades;
        UserId buyer = this->users.createUser();
        UserId seller = this->users.createUser();

        Fixture()
        {
            CHECK(this->users.getUser(this->buyer)->getContractAccount().updateBalance(2, 1'000'000'000));
            CHECK(this->users.getUser(this->seller)->getContractAccount().updateBalance(2, 1'000'000'000));
            CHECK(this->symbols.createSymbol("PERP", SymbolType::Perpetual, 1, 2));
            ContractSpec spec;
            spec.firstFundingTimestamp = 1'000'000;
            spec.makerFeeRate = 0;
            spec.takerFeeRate = 0;
            CHECK(this->matching.addContractBook(1, spec, 100, 0));
        }

        PlaceOrderResult order(UserId user, OrderSide side, Quantity quantity = 1, PositionId positionId = 0,
            TimeInForce timeInForce = TimeInForce::GTC)
        {
            ContractOrderOptions options;
            options.positionId = positionId;
            return this->matching.placeContractOrder(user, 1, side, OrderType::Limit, timeInForce, 100, quantity, 1, options, this->trades);
        }

        void verifyCash() const
        {
            WideBalance cash = 0;
            this->users.forEachUser([&](UserId, const User& user)
            {
                cash += user.getContractAccount().queryActiveBalance(2).value_or(0);
                cash += user.getContractAccount().queryReservedBalance(2).value_or(0);
            });
            const auto* market = this->matching.getContractEngine().findMarket(1);
            cash += static_cast<WideBalance>(market->feeBalance) + market->clearingBalance + market->insuranceBalance + market->fundingRoundingBalance;
            CHECK(cash == 2'000'000'000);
        }
    };

    void poolGrowthAndHistory()
    {
        Fixture fixture;
        std::vector<std::pair<PositionId, PositionId>> pairs;
        const Position* firstPosition = nullptr;
        for (int index = 0; index < 6'000; ++index)
        {
            const auto ask = fixture.order(fixture.seller, OrderSide::Ask);
            const auto bid = fixture.order(fixture.buyer, OrderSide::Bid, 1, 0, TimeInForce::IOC);
            CHECK(ask && bid && fixture.trades.size() == 1);
            pairs.emplace_back(bid.positionId, ask.positionId);
            if (!firstPosition)
            {
                firstPosition = fixture.matching.getContractEngine().queryPosition(bid.positionId);
            }
        }

        CHECK(fixture.matching.getContractEngine().queryPosition(pairs.front().first) == firstPosition);
        CHECK(firstPosition->positionSize == 1);
        for (const auto& [longId, shortId] : pairs)
        {
            CHECK(fixture.order(fixture.buyer, OrderSide::Ask, 1, longId));
            CHECK(fixture.order(fixture.seller, OrderSide::Bid, 1, shortId, TimeInForce::IOC));
        }

        CHECK(firstPosition->closed);
        const auto active = fixture.order(fixture.seller, OrderSide::Ask);
        CHECK(active);
        Archive archive;
        const auto untouched = fixture.matching.archiveHistory(archive, 0);
        CHECK(untouched.scannedOrders == 0 && untouched.scannedPositions == 0);
        for (int index = 0; index < 10; ++index)
        {
            const auto result = fixture.matching.archiveHistory(archive, 31);
            CHECK(result.archivedOrders == 0 && result.archivedPositions == 0);
        }

        archive.accepting = true;
        for (int index = 0; index < 4'000; ++index)
        {
            const auto result = fixture.matching.archiveHistory(archive, 31);
            CHECK(result.scannedOrders + result.scannedPositions <= 31);
        }

        CHECK(archive.orders == 24'000 && archive.positions == 12'000);
        CHECK(fixture.matching.retainedOrderCount() == 1);
        CHECK(!fixture.matching.queryOrder(1));
        CHECK(!fixture.matching.getContractEngine().queryPosition(pairs.front().first));
        CHECK(fixture.matching.queryOrder(active.orderId));
        CHECK(fixture.matching.getContractEngine().queryPosition(active.positionId));
        CHECK(!fixture.users.removeUser(fixture.seller));
        fixture.verifyCash();

        CHECK(fixture.matching.cancelOrder(fixture.seller, active.orderId, 1));
        // A one-item budget alternates order/position work without starvation.
        for (int index = 0; index < 60'000; ++index)
        {
            fixture.matching.archiveHistory(archive, 1);
        }

        CHECK(archive.orders == 24'001 && archive.positions == 12'001);
        CHECK(fixture.matching.retainedOrderCount() == 0);
        const auto reused = fixture.order(fixture.seller, OrderSide::Ask);
        CHECK(reused && reused.orderId > active.orderId && reused.positionId > active.positionId);
        CHECK(!fixture.matching.queryOrder(1));
        fixture.verifyCash();
    }

    void balanceTokensAndSpotPromotion()
    {
        UserManager users;
        const auto buyer = users.createUser();
        const auto seller = users.createUser();
        auto& account = users.getUser(buyer)->getSpotAccount();
        CHECK(account.updateBalance(2, 1'000));
        const auto stale = account.prepareBalanceChange(account.captureBalance(2), -50, 50);
        CHECK(stale);
        for (AssetId id = 3; id <= 12; ++id)
        {
            CHECK(account.updateBalance(id, 1));
        }

        CHECK(account.commitBalanceChange(*stale).state == BalanceOpResult::StaleUpdate);
        CHECK(account.queryActiveBalance(2) == 1'000);
        const auto update = account.prepareBalanceChange(account.captureBalance(2), -50, 50);
        CHECK(update && account.commitBalanceChange(*update));
        CHECK(!account.commitBalanceChange(*update));
        CHECK(account.queryActiveBalance(2) == 950 && account.queryReservedBalance(2) == 50);
        CHECK(account.releaseReservedBalance(2, 50));
        const auto changed = account.prepareBalanceChange(account.captureBalance(2), 1, 0);
        CHECK(account.updateBalance(2, -1));
        CHECK(account.commitBalanceChange(*changed).state == BalanceOpResult::StaleUpdate);
        CHECK(!users.getUser(seller)->getSpotAccount().commitBalanceChange(*changed));

        SymbolManager symbols;
        MatchingEngine matching(users, symbols);
        CHECK(symbols.createSymbol("SPOT", SymbolType::Spot, 1, 2));
        CHECK(matching.addSpotBook(1));
        auto& sellerAccount = users.getUser(seller)->getSpotAccount();
        CHECK(sellerAccount.updateBalance(1, 10));
        for (AssetId id = 3; id <= 11; ++id)
        {
            CHECK(sellerAccount.updateBalance(id, 1));
        }

        TradeBuffer trades;
        CHECK(matching.placeOrder(seller, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 1, 1, trades));
        CHECK(matching.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::IOC, 100, 1, 1, trades));
        CHECK(account.queryActiveBalance(1) == 1 && account.queryActiveBalance(2) == 899);
        CHECK(sellerAccount.queryActiveBalance(1) == 9 && sellerAccount.queryActiveBalance(2) == 100);
        // Existing spot behavior permits self trades; merge both asset changes.
        CHECK(matching.placeOrder(buyer, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 1, 1, trades));
        CHECK(matching.placeOrder(buyer, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::IOC, 100, 1, 1, trades));
        CHECK(account.queryActiveBalance(1) == 1 && account.queryActiveBalance(2) == 899);
        CHECK(account.queryReservedBalance(1).value_or(0) == 0 && account.queryReservedBalance(2).value_or(0) == 0);
    }

    void reusableFokAndFunding()
    {
        Fixture fixture;
        Archive archive;
        archive.accepting = true;
        for (int index = 0; index < 100; ++index)
        {
            const auto ask = fixture.order(fixture.seller, OrderSide::Ask, 2);
            const auto rejected = fixture.order(fixture.buyer, OrderSide::Bid, 3, 0, TimeInForce::FOK);
            CHECK(ask && !rejected && fixture.matching.askAt(1, 100)->quantity == 2);
            const auto bid = fixture.order(fixture.buyer, OrderSide::Bid, 2, 0, TimeInForce::FOK);
            CHECK(bid && fixture.trades.size() == 1);
            CHECK(fixture.order(fixture.buyer, OrderSide::Ask, 2, bid.positionId));
            CHECK(fixture.order(fixture.seller, OrderSide::Bid, 2, ask.positionId, TimeInForce::FOK));
            fixture.verifyCash();
            fixture.matching.archiveHistory(archive, 2'000);
        }

        const auto ask = fixture.order(fixture.seller, OrderSide::Ask, 10);
        const auto bid = fixture.order(fixture.buyer, OrderSide::Bid, 10);
        CHECK(ask && bid);
        auto& contracts = fixture.matching.getContractEngine();
        const Timestamp interval = contracts.findMarket(1)->spec.fundingInterval;
        for (int period = 0; period < 8; ++period)
        {
            const auto* market = contracts.findMarket(1);
            const Timestamp due = market->nextFundingTimestamp;
            CHECK(contracts.updateMarkPrice(1, period % 2 == 0 ? 101 : 100, due - 1));
            const auto rate = period % 2 == 0 ? 1'000 : -1'000;
            CHECK(contracts.settleFunding(1, rate, due));
            CHECK(contracts.findMarket(1)->nextFundingTimestamp == due + interval);
            CHECK(contracts.settleFunding(1, rate, due).state == ContractState::AlreadySettled);
            fixture.verifyCash();
        }
    }

    class Queue final : public consuming::SingleThreadConsumer<int, int>
    {
        int consume(int&& value) override
        {
            if (value == -1)
            {
                throw std::runtime_error("expected command failure");
            }

            return value;
        }

    public:
        ~Queue() override
        {
            this->stopBeforeDerivedDestruction();
        }
    };

    class VoidQueue final : public consuming::SingleThreadConsumer<int, void>
    {
        void consume(int&& value) override
        {
            if (value == -1)
            {
                throw std::runtime_error("expected void failure");
            }
        }

    public:
        ~VoidQueue() override
        {
            this->stopBeforeDerivedDestruction();
        }
    };

    void responseWakeupsAndProgress()
    {
        for (int ringSize : {1, 2, 32, 128})
        {
            Queue queue;
            queue.start(ringSize, ringSize);
            std::array<std::jthread, 2> producers;
            for (auto& producer : producers)
            {
                producer = std::jthread([&]
                {
                    consuming::ResponseSlot<int> slot;
                    for (int value = 0; value < 500; ++value)
                    {
                        CHECK(queue.post(value, slot).get() == value);
                    }

                    auto failed = queue.post(-1, slot);
                    bool caught = false;
                    try
                    {
                        static_cast<void>(failed.get());
                    }
                    catch (const std::runtime_error&)
                    {
                        caught = true;
                    }

                    CHECK(caught && queue.post(42, slot).get() == 42);
                });
            }

            for (auto& producer : producers)
            {
                producer.join();
            }

            queue.stop();
        }

        VoidQueue queue;
        queue.start(1);
        consuming::ResponseSlot<void> slot;
        queue.post(1, slot).get();
        bool caught = false;
        try
        {
            queue.post(-1, slot).get();
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }

        CHECK(caught);
        queue.post(2, slot).get();
        queue.stop();
    }
}

int main()
{
    poolGrowthAndHistory();
    balanceTokensAndSpotPromotion();
    reusableFokAndFunding();
    responseWakeupsAndProgress();
    std::puts("Optimization regressions passed");
}
