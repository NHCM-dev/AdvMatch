#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/util/ContractMath.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

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

    struct Fixture
    {
        UserManager users;
        SymbolManager symbols;
        MatchingEngine matching;
        TradeBuffer trades;
        UserId buyer;
        UserId seller;
        UserId third;
        ContractSpec spec;

        explicit Fixture(SymbolType type = SymbolType::Perpetual, bool fees = false, Timestamp firstFunding = 100, Balance maxPositionNotional = 1'000'000'000'000'000)
            : matching(this->users, this->symbols)
        {
            this->buyer = this->users.createUser();
            this->seller = this->users.createUser();
            this->third = this->users.createUser();
            for (UserId id : {this->buyer, this->seller, this->third})
            {
                CHECK(this->users.getUser(id)->getContractAccount().updateBalance(2, 10'000));
            }

            this->spec.maxPositionNotional = maxPositionNotional;
            this->spec.maintenanceMarginRate = 10'000;
            this->spec.maxLeverage = 20;
            this->spec.makerFeeRate = fees ? 10'000 : 0;
            this->spec.takerFeeRate = fees ? 20'000 : 0;
            this->spec.liquidationFeeRate = 0;
            this->spec.fundingInterval = 100;
            this->spec.firstFundingTimestamp = type == SymbolType::Perpetual ? firstFunding : 0;
            this->spec.expiryTimestamp = type == SymbolType::Future ? 1000 : 0;
            CHECK(this->symbols.createSymbol("TEST", type, 1, 2));
            CHECK(this->matching.addContractBook(1, this->spec, 100, 0));
        }

        ContractEngine& engine()
        {
            return this->matching.getContractEngine();
        }

        ContractAccount& account(UserId id)
        {
            return this->users.getUser(id)->getContractAccount();
        }

        PlaceOrderResult order(UserId user, OrderSide side, Price price, Quantity quantity, Timestamp time,
            PositionId position = 0, MarginMode mode = MarginMode::Isolated, TimeInForce tif = TimeInForce::GTC)
        {
            ContractOrderOptions options;
            options.positionId = position;
            options.marginMode = mode;
            return this->matching.placeContractOrder(user, 1, side, OrderType::Limit, tif, price, quantity, time, options, this->trades);
        }

        std::pair<PositionId, PositionId> open(Quantity quantity = 10, MarginMode mode = MarginMode::Isolated)
        {
            const auto ask = this->order(this->seller, OrderSide::Ask, 100, quantity, 1, 0, mode);
            const auto bid = this->order(this->buyer, OrderSide::Bid, 100, quantity, 2, 0, mode);
            CHECK(ask && bid && this->trades.size() == 1);
            CHECK(bid.positionId != ask.positionId);
            return {bid.positionId, ask.positionId};
        }

        Balance cash() const
        {
            WideBalance total = 0;
            this->users.forEachUser([&](UserId, const User& user)
            {
                user.getContractAccount().forEachBalance([&](AssetId, const AssetBalance& balance)
                {
                    total += static_cast<WideBalance>(balance.active) + balance.reserved;
                });
            });
            const auto* market = this->matching.getContractEngine().findMarket(1);
            total += static_cast<WideBalance>(market->insuranceBalance) + market->clearingBalance + market->feeBalance + market->fundingRoundingBalance;
            return static_cast<Balance>(total);
        }
    };

    void isolatedAndHedge()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open();
        const auto* longPosition = fixture.engine().queryPosition(longId);
        CHECK(longPosition->positionSize == 10 && longPosition->entryNotional == 1000);
        CHECK(longPosition->initialMargin == 100 && longPosition->leverage == 10);
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2) == 100);
        CHECK(fixture.cash() == 30'000);
        CHECK(!fixture.users.removeUser(fixture.buyer));
        CHECK(!fixture.account(fixture.buyer).removePosition(longId));

        // The buyer can hold an independent short; it is not netted away.
        const auto otherBid = fixture.order(fixture.third, OrderSide::Bid, 100, 3, 3);
        const auto hedge = fixture.order(fixture.buyer, OrderSide::Ask, 100, 3, 4);
        CHECK(otherBid && hedge);
        CHECK(fixture.engine().queryPosition(longId)->positionSize == 10);
        CHECK(fixture.engine().queryPosition(hedge.positionId)->positionSide == PositionSide::Short);
        CHECK(fixture.engine().queryPosition(hedge.positionId)->positionSize == 3);
        CHECK(fixture.cash() == 30'000);

        CHECK(fixture.engine().updateMarkPrice(1, 105, 5));
        CHECK(fixture.engine().queryPosition(longId)->getUnrealizedPnL(105) == 50);
        CHECK(fixture.engine().queryPosition(shortId)->getUnrealizedPnL(105) == -50);
        const auto bid = fixture.order(fixture.third, OrderSide::Bid, 105, 4, 6);
        CHECK(bid);
        const auto close = fixture.order(fixture.buyer, OrderSide::Ask, 105, 4, 7, longId);
        CHECK(close && fixture.trades.size() == 1);
        CHECK(fixture.engine().queryPosition(longId)->positionSize == 6);
        CHECK(fixture.engine().queryPosition(longId)->realizedPnL == 20);
        CHECK(fixture.engine().queryPosition(longId)->entryNotional == 600);
        CHECK(fixture.cash() == 30'000);
    }

    void reservationsAndValidation()
    {
        Fixture fixture;
        const auto pending = fixture.order(fixture.buyer, OrderSide::Bid, 95, 10, 1);
        CHECK(pending);
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2) == 110);
        CHECK(fixture.matching.modifyQuantity(fixture.buyer, pending.orderId, -5, 2));
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2) == 55);
        CHECK(fixture.matching.modifyPrice(fixture.buyer, pending.orderId, 96, 3, fixture.trades));
        CHECK(fixture.matching.cancelOrder(fixture.buyer, pending.orderId, 4));
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2).value_or(0) == 0);
        CHECK(fixture.account(fixture.buyer).queryActiveBalance(2) == 10'000);

        ContractOrderOptions options;
        options.leverage = 21;
        const auto invalid = fixture.matching.placeContractOrder(fixture.buyer, 1, OrderSide::Bid, OrderType::Limit,
            TimeInForce::GTC, 100, 1, 5, options, fixture.trades);
        CHECK(invalid.rejectReason == PlaceOrderState::InvalidLeverage);
        options.leverage = 10;
        options.reduceOnly = true;
        const auto reduce = fixture.matching.placeContractOrder(fixture.buyer, 1, OrderSide::Ask, OrderType::Limit,
            TimeInForce::GTC, 100, 1, 6, options, fixture.trades);
        CHECK(reduce.rejectReason == PlaceOrderState::ReduceOnlyViolation);
        CHECK(fixture.order(fixture.buyer, OrderSide::Bid, 100, 1, 7, 0, MarginMode::Isolated, TimeInForce::FOK).rejectReason == PlaceOrderState::InsufficientLiquidity);
        CHECK(fixture.cash() == 30'000);
    }

    void leverageAndMargin()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open();
        CHECK(fixture.engine().setLeverage(longId, fixture.buyer, 1, 5, 3));
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 200);
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2) == 200);
        CHECK(fixture.engine().addMargin(longId, fixture.buyer, 1, 50));
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 250);
        CHECK(fixture.engine().addMargin(longId, fixture.buyer, 1, -50));
        CHECK(!fixture.engine().addMargin(longId, fixture.buyer, 1, -1));
        CHECK(fixture.engine().setLeverage(longId, fixture.buyer, 1, 10, 4));
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2) == 100);
        CHECK(fixture.cash() == 30'000);
        const auto pending = fixture.order(fixture.buyer, OrderSide::Bid, 99, 1, 5, longId);
        CHECK(pending);
        CHECK(fixture.engine().setLeverage(longId, fixture.buyer, 1, 5, 6).state == ContractState::OpenOrdersExist);
    }

    void funding()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open();
        CHECK(fixture.engine().settleFunding(1, 10'000, 100));
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 90);
        CHECK(fixture.engine().queryPosition(shortId)->initialMargin == 110);
        CHECK(fixture.engine().queryPosition(longId)->fundingPnL == -10);
        CHECK(fixture.engine().queryPosition(shortId)->fundingPnL == 10);
        CHECK(fixture.engine().settleFunding(1, 10'000, 100).state == ContractState::AlreadySettled);
        CHECK(fixture.engine().settleFunding(1, -10'000, 200));
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 100);
        CHECK(fixture.engine().queryPosition(shortId)->initialMargin == 100);
        CHECK(fixture.engine().settleFunding(1, 10'001, 300).state == ContractState::InvalidConfiguration);
        CHECK(fixture.engine().findMarket(1)->lastFundingTimestamp == 200);
        CHECK(fixture.cash() == 30'000);
    }

    void crossMargin()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open(10, MarginMode::Cross);
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2).value_or(0) == 0);
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->initialMargin == 100);
        CHECK(!fixture.account(fixture.buyer).updateBalance(2, -9901));
        CHECK(fixture.account(fixture.buyer).updateBalance(2, -9800));
        CHECK(fixture.engine().updateMarkPrice(1, 95, 3));
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->unrealizedPnL == -50);
        CHECK(!fixture.account(fixture.buyer).updateBalance(2, -51));
        CHECK(fixture.engine().settleFunding(1, 10'000, 100));
        CHECK(fixture.account(fixture.buyer).queryActiveBalance(2) == 191);
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 100);
        CHECK(fixture.engine().setLeverage(longId, fixture.buyer, 1, 5, 101).state == ContractState::InsufficientBalance);
        CHECK(fixture.engine().updateMarkPrice(1, 80, 102).state == ContractState::PendingLiquidation);
        CHECK(fixture.engine().queryPosition(longId)->liquidating);
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->liquidating);
    }

    void liquidationAndInsurance()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open();
        const auto bid = fixture.order(fixture.third, OrderSide::Bid, 90, 4, 3);
        CHECK(bid);
        CHECK(fixture.engine().updateMarkPrice(1, 90, 4).state == ContractState::PendingLiquidation);
        CHECK(fixture.engine().queryPosition(longId)->positionSize == 6);
        CHECK(fixture.engine().queryPosition(longId)->liquidating);
        CHECK(fixture.cash() == 30'000);
        CHECK(fixture.engine().fundInsurance(1, fixture.account(fixture.third), 100));
        CHECK(fixture.engine().updateMarkPrice(1, 80, 5).state == ContractState::PendingLiquidation);
        CHECK(fixture.order(fixture.third, OrderSide::Bid, 80, 6, 6));
        CHECK(fixture.engine().queryPosition(longId)->closed);
        CHECK(fixture.engine().findMarket(1)->insuranceBalance == 40);
        CHECK(fixture.engine().queryPosition(longId)->realizedPnL == -160);
        CHECK(fixture.cash() == 30'000);
        CHECK(fixture.engine().liquidate(1, longId, 7, fixture.trades).state == ContractState::LiquidationNotRequired);
    }

    void expiryAndDelivery()
    {
        Fixture fixture(SymbolType::Future);
        const auto [longId, shortId] = fixture.open();
        CHECK(fixture.order(fixture.third, OrderSide::Bid, 95, 1, 3));
        CHECK(fixture.engine().deliver(1, 105, 999).state == ContractState::NotDue);
        CHECK(fixture.engine().deliver(1, 105, 1000));
        CHECK(fixture.engine().queryPosition(longId)->closed);
        CHECK(fixture.engine().queryPosition(shortId)->closed);
        CHECK(fixture.account(fixture.buyer).queryActiveBalance(2) == 10050);
        CHECK(fixture.account(fixture.seller).queryActiveBalance(2) == 9950);
        CHECK(!fixture.matching.bestBid(1));
        CHECK(fixture.engine().deliver(1, 105, 1000).state == ContractState::AlreadySettled);
        CHECK(fixture.order(fixture.buyer, OrderSide::Bid, 100, 1, 1001).rejectReason == PlaceOrderState::ContractExpired);
        CHECK(fixture.cash() == 30'000);
    }

    void liquidationRetryDependencies()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open();
        auto& engine = fixture.engine();
        CHECK(engine.updateMarkPrice(1, 80, 3).state == ContractState::PendingLiquidation);
        const auto maker = fixture.order(fixture.third, OrderSide::Bid, 80, 10, 4);
        CHECK(maker);
        CHECK(engine.queryPosition(longId)->positionSize == 10);
        CHECK(engine.processLiquidations(1, 5).state == ContractState::PendingLiquidation);
        auto& settlement = engine.getSettleEngine();
        CHECK(settlement.liquidationRetryCacheHits() > 0);
        auto hits = settlement.liquidationRetryCacheHits();

        // Promotion changes balance addresses. Value changes must invalidate
        // the financial key even when the top maker/price stays unchanged.
        for (AssetId asset = 3; asset < 20; ++asset)
        {
            CHECK(fixture.account(fixture.third).updateBalance(asset, 1));
        }

        CHECK(fixture.account(fixture.third).updateBalance(2, 1));
        CHECK(engine.processLiquidations(1, 6).state == ContractState::PendingLiquidation);
        CHECK(settlement.liquidationRetryCacheHits() == hits);
        CHECK(engine.processLiquidations(1, 7).state == ContractState::PendingLiquidation);
        CHECK(settlement.liquidationRetryCacheHits() == ++hits);

        // A now-invalid maker must be cancelled before considering the taker.
        auto* market = engine.findMarket(1);
        market->spec.maxPositionNotional = 1;
        CHECK(engine.processLiquidations(1, 8).state == ContractState::PendingLiquidation);
        CHECK(fixture.matching.queryOrder(maker.orderId)->getStatus() == OrderStatus::Cancelled);
        CHECK(!fixture.matching.bestBid(1));
        CHECK(settlement.liquidationRetryCacheHits() == hits);
        market->spec.maxPositionNotional = fixture.spec.maxPositionNotional;
        CHECK(fixture.order(fixture.third, OrderSide::Bid, 80, 10, 9));
        CHECK(engine.fundInsurance(1, fixture.account(fixture.third), 100));
        CHECK(engine.processLiquidations(1, 10));
        CHECK(engine.queryPosition(longId)->closed);
        CHECK(engine.queryPosition(shortId)->positionSize == 10);
        CHECK(!engine.findParticipant(longId)->liquidationRetry);
    }

    void deliveryDefaultAtomicity()
    {
        Fixture fixture(SymbolType::Future);
        const auto [longId, shortId] = fixture.open();
        const auto failed = fixture.engine().deliver(1, 70, 1000);
        CHECK(failed.state == ContractState::InsufficientInsurance);
        CHECK(!fixture.engine().queryPosition(longId)->closed);
        CHECK(fixture.engine().queryPosition(longId)->realizedPnL == 0);
        CHECK(fixture.engine().queryPosition(shortId)->realizedPnL == 0);
        CHECK(fixture.cash() == 30'000);
        CHECK(fixture.engine().fundInsurance(1, fixture.account(fixture.third), 200));
        CHECK(fixture.engine().deliver(1, 70, 1001));
        CHECK(fixture.engine().findMarket(1)->insuranceBalance == 0);
        CHECK(fixture.account(fixture.buyer).queryActiveBalance(2) == 9900);
        CHECK(fixture.account(fixture.seller).queryActiveBalance(2) == 10300);
        CHECK(fixture.cash() == 30'000);
    }

    void fokAndMarketProtection()
    {
        Fixture fixture;
        const auto ask1 = fixture.order(fixture.seller, OrderSide::Ask, 100, 2, 1);
        const auto ask2 = fixture.order(fixture.third, OrderSide::Ask, 101, 3, 2);
        CHECK(ask1 && ask2);
        const auto failed = fixture.order(fixture.buyer, OrderSide::Bid, 101, 6, 3, 0, MarginMode::Isolated, TimeInForce::FOK);
        CHECK(failed.rejectReason == PlaceOrderState::InsufficientLiquidity);
        CHECK(fixture.matching.askAt(1, 100)->quantity == 2);
        CHECK(fixture.matching.askAt(1, 101)->quantity == 3);
        CHECK(fixture.account(fixture.buyer).queryReservedBalance(2).value_or(0) == 0);
        const auto success = fixture.order(fixture.buyer, OrderSide::Bid, 101, 5, 4, 0, MarginMode::Isolated, TimeInForce::FOK);
        CHECK(success && success.state.status == OrderStatus::Filled && fixture.trades.size() == 2);
        CHECK(fixture.engine().queryPosition(success.positionId)->entryNotional == 503);
        CHECK(fixture.cash() == 30'000);

        const auto ownAsk = fixture.order(fixture.buyer, OrderSide::Ask, 100, 1, 5);
        const auto selfFok = fixture.order(fixture.buyer, OrderSide::Bid, 100, 1, 6, 0, MarginMode::Isolated, TimeInForce::FOK);
        CHECK(selfFok.rejectReason == PlaceOrderState::InsufficientLiquidity);
        CHECK(fixture.matching.queryOrder(ownAsk.orderId)->getUnfilledQuantity() == 1);
        CHECK(fixture.matching.cancelOrder(fixture.buyer, ownAsk.orderId, 7));

        CHECK(fixture.order(fixture.seller, OrderSide::Ask, 105, 1, 8));
        ContractOrderOptions options;
        options.priceLimit = 104;
        const auto protectedMarket = fixture.matching.placeContractOrder(fixture.buyer, 1, OrderSide::Bid,
            OrderType::Market, TimeInForce::IOC, 0, 1, 9, options, fixture.trades);
        CHECK(protectedMarket && protectedMarket.state.status == OrderStatus::Cancelled);
        CHECK(fixture.trades.empty());
        CHECK(fixture.matching.bestAsk(1)->price == 105);
        CHECK(fixture.cash() == 30'000);
    }

    void staleReduceOnly()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open(2);
        const auto first = fixture.order(fixture.buyer, OrderSide::Ask, 100, 2, 3, longId);
        const auto second = fixture.order(fixture.buyer, OrderSide::Ask, 100, 2, 4, longId);
        CHECK(first && second);
        const auto taker = fixture.order(fixture.third, OrderSide::Bid, 100, 4, 5, 0, MarginMode::Isolated, TimeInForce::IOC);
        CHECK(taker && taker.state.filled == 2);
        CHECK(fixture.engine().queryPosition(longId)->closed);
        CHECK(fixture.matching.queryOrder(second.orderId)->getStatus() == OrderStatus::Cancelled);
        CHECK(fixture.cash() == 30'000);
    }

    void crossOrderCollateral()
    {
        Fixture fixture;
        const auto [longId, shortId] = fixture.open(10, MarginMode::Cross);
        CHECK(fixture.account(fixture.buyer).updateBalance(2, -9800));
        const auto pending = fixture.order(fixture.buyer, OrderSide::Bid, 80, 1, 3, 0, MarginMode::Cross);
        CHECK(pending);
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->orderReservation == 11);
        CHECK(fixture.engine().updateMarkPrice(1, 81, 4));
        CHECK(!fixture.engine().queryPosition(longId)->liquidating);
        CHECK(fixture.matching.cancelOrder(fixture.buyer, pending.orderId, 5));
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->orderReservation == 0);
    }

    void multiSymbolCrossLiquidation()
    {
        Fixture fixture;
        CHECK(fixture.symbols.createSymbol("TEST2", SymbolType::Perpetual, 3, 2));
        CHECK(fixture.matching.addContractBook(2, fixture.spec, 100, 0));
        const auto [longId, shortId] = fixture.open(10, MarginMode::Cross);
        ContractOrderOptions options;
        options.marginMode = MarginMode::Cross;
        const auto ask = fixture.matching.placeContractOrder(fixture.seller, 2, OrderSide::Ask, OrderType::Limit,
            TimeInForce::GTC, 100, 10, 3, options, fixture.trades);
        const auto bid = fixture.matching.placeContractOrder(fixture.buyer, 2, OrderSide::Bid, OrderType::Limit,
            TimeInForce::GTC, 100, 10, 4, options, fixture.trades);
        CHECK(ask && bid);
        CHECK(fixture.account(fixture.buyer).findCrossMargin(2)->initialMargin == 200);
        CHECK(fixture.account(fixture.buyer).updateBalance(2, -9780));
        CHECK(fixture.engine().updateMarkPrice(1, 90, 5));
        CHECK(!fixture.engine().queryPosition(longId)->liquidating);
        CHECK(fixture.engine().updateMarkPrice(2, 89, 6).state == ContractState::PendingLiquidation);
        CHECK(fixture.engine().queryPosition(longId)->liquidating);
        CHECK(fixture.engine().queryPosition(bid.positionId)->liquidating);
        CHECK(fixture.engine().findMarket(1)->hasPendingLiquidation(longId));
        CHECK(fixture.engine().findMarket(2)->hasPendingLiquidation(bid.positionId));
    }

    void fundingOverflowAtomicity()
    {
        Fixture fixture;
        CHECK(fixture.account(fixture.seller).updateBalance(2, std::numeric_limits<Balance>::max() - 10'000));
        const auto [longId, shortId] = fixture.open();
        CHECK(fixture.account(fixture.seller).updateBalance(2, 1).state == BalanceOpResult::Overflow);
        CHECK(fixture.engine().settleFunding(1, 10'000, 100).state == ContractState::Overflow);
        CHECK(fixture.engine().queryPosition(longId)->initialMargin == 100);
        CHECK(fixture.engine().queryPosition(shortId)->initialMargin == 100);
        CHECK(fixture.engine().queryPosition(longId)->fundingPnL == 0);
        CHECK(fixture.engine().findMarket(1)->nextFundingTimestamp == 100);
    }

    struct EventCollector final : ContractEventSink
    {
        std::size_t fills = 0;
        std::size_t funding = 0;
        std::size_t delivery = 0;
        std::size_t liquidations = 0;

        void onEvent(const ContractEvent& event) noexcept override
        {
            if (event.type == ContractEventType::Fill)
            {
                ++this->fills;
            }
            else if (event.type == ContractEventType::Funding)
            {
                ++this->funding;
            }
            else if (event.type == ContractEventType::Delivery)
            {
                ++this->delivery;
            }
            else
            {
                ++this->liquidations;
            }
        }
    };

    void eventsAndRepeatedTrading()
    {
        EventCollector collector;
        Fixture fixture(SymbolType::Perpetual, false, 5000);
        fixture.engine().setEventSink(&collector);
        Timestamp timestamp = 1;
        const Position* firstPosition = nullptr;
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            const Price price = 99 + iteration % 3;
            CHECK(fixture.order(fixture.seller, OrderSide::Ask, price, 1, timestamp++));
            const auto buy = fixture.order(fixture.buyer, OrderSide::Bid, price, 1, timestamp++);
            CHECK(buy && buy.state.status == OrderStatus::Filled);
            if (!firstPosition)
            {
                firstPosition = fixture.engine().queryPosition(buy.positionId);
            }

            CHECK(fixture.order(fixture.third, OrderSide::Bid, 100, 1, timestamp++));
            CHECK(fixture.order(fixture.buyer, OrderSide::Ask, 100, 1, timestamp++, buy.positionId));
            CHECK(fixture.engine().queryPosition(buy.positionId)->closed);
            CHECK(fixture.cash() == 30'000);
            WideBalance quantity = 0;
            fixture.users.forEachUser([&](UserId, const User& user)
            {
                user.getContractAccount().forEachPosition([&](PositionId, const Position& position)
                {
                    quantity += position.positionSide == PositionSide::Long ? static_cast<WideBalance>(position.positionSize) : -static_cast<WideBalance>(position.positionSize);
                });
            });
            CHECK(quantity == 0);
        }

        CHECK(firstPosition->closed);
        CHECK(fixture.engine().queryPosition(firstPosition->positionId) == firstPosition);
        CHECK(collector.fills == 2000);
    }

    void feeAccountingAndFundingClock()
    {
        EventCollector collector;
        Fixture fixture(SymbolType::Perpetual, true);
        fixture.engine().setEventSink(&collector);
        const auto [longId, shortId] = fixture.open();
        CHECK(fixture.account(fixture.buyer).queryActiveBalance(2) == 9880);
        CHECK(fixture.account(fixture.seller).queryActiveBalance(2) == 9890);
        CHECK(fixture.engine().findMarket(1)->feeBalance == 30);
        CHECK(fixture.cash() == 30'000);
        CHECK(collector.fills == 2);
        const auto late = fixture.order(fixture.third, OrderSide::Bid, 100, 1, 101);
        CHECK(late.rejectReason == PlaceOrderState::FundingRequired);
        CHECK(fixture.engine().updateMarkPrice(1, 101, 101).state == ContractState::FundingRequired);
        CHECK(fixture.engine().settleFunding(1, 10'000, 100));
        CHECK(collector.funding == 2);
        CHECK(fixture.order(fixture.third, OrderSide::Bid, 100, 1, 101));
        const auto close = fixture.order(fixture.buyer, OrderSide::Ask, 100, 1, 102, longId);
        CHECK(close && fixture.trades.size() == 1);
        CHECK(fixture.engine().findMarket(1)->feeBalance == 33);
        CHECK(fixture.cash() == 30'000);
    }

    void asymmetricFundingRounding()
    {
        Fixture fixture;
        const auto ask = fixture.order(fixture.seller, OrderSide::Ask, 100, 2, 1);
        const auto long1 = fixture.order(fixture.buyer, OrderSide::Bid, 100, 1, 2);
        const auto long2 = fixture.order(fixture.third, OrderSide::Bid, 100, 1, 3);
        CHECK(ask && long1 && long2);
        CHECK(fixture.engine().settleFunding(1, 5'000, 100));
        // Each long owes floor(0.5) = 0; the short cannot receive an unfunded 1.
        CHECK(fixture.engine().queryPosition(ask.positionId)->fundingPnL == 0);
        CHECK(fixture.engine().queryPosition(long1.positionId)->fundingPnL == 0);
        CHECK(fixture.engine().queryPosition(long2.positionId)->fundingPnL == 0);
        CHECK(fixture.engine().findMarket(1)->fundingRoundingBalance >= 0);
        CHECK(fixture.cash() == 30'000);
    }

    void closeAboveNotionalLimit()
    {
        Fixture fixture(SymbolType::Perpetual, false, 100, 1000);
        const auto [longId, shortId] = fixture.open(9);
        CHECK(fixture.engine().updateMarkPrice(1, 130, 3).state == ContractState::PendingLiquidation);
        CHECK(fixture.order(fixture.third, OrderSide::Bid, 130, 1, 4));
        CHECK(fixture.order(fixture.buyer, OrderSide::Ask, 130, 1, 5, longId));
        CHECK(fixture.engine().queryPosition(longId)->positionSize == 8);
        CHECK(fixture.cash() == 30'000);
    }

    void mathBoundaries()
    {
        CHECK(!ContractMath::notional(std::numeric_limits<Price>::max(), 2));
        CHECK(!ContractMath::notional(100, std::numeric_limits<Quantity>::max()));
        CHECK(!ContractMath::notional(100, 10, std::numeric_limits<Quantity>::max()));
        CHECK(ContractMath::notional(100, 10, 2) == 2000);
        CHECK(ContractMath::margin(101, 10) == 11);
        CHECK(ContractMath::rate(1, 1, true) == 1);
        CHECK(ContractMath::proportional(101, 3, 3) == 101);
    }
}

int main()
{
    reservationsAndValidation();
    leverageAndMargin();
    funding();
    crossMargin();
    liquidationAndInsurance();
    liquidationRetryDependencies();
    expiryAndDelivery();
    deliveryDefaultAtomicity();
    mathBoundaries();
    closeAboveNotionalLimit();
    asymmetricFundingRounding();
    feeAccountingAndFundingClock();
    fokAndMarketProtection();
    staleReduceOnly();
    crossOrderCollateral();
    multiSymbolCrossLiquidation();
    fundingOverflowAtomicity();
    eventsAndRepeatedTrading();
    // Separate fixture intentionally moves a short beyond maintenance.
    isolatedAndHedge();
    std::puts("Contract lifecycle tests passed");
}
