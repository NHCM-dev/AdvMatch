#include "advmatch/observe/TradingObserver.h"
#include "advmatch/observe/ObservationCache.h"
#include "advmatch/observe/ObservedConsumer.h"
#include "advmatch/observe/ObservationEventSink.h"
#include "advmatch/persistence/CommandExecutor.h"
#include "advmatch/persistence/PersistentTradingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <latch>

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
    using namespace advmatch::observe;

    struct Fixture
    {
        TradingState state;
        TradeBuffer trades;
        ObservationConfig config;

        explicit Fixture(OrderBookImplementation implementation = OrderBookImplementation::Radix)
        {
            CHECK(state.assets.createAsset("Bitcoin", "BTC", 0));
            CHECK(state.assets.createAsset("Dollar", "USD", 0));
            for (std::size_t index = 0; index < 3; ++index)
            {
                const auto id = state.users.createUser();
                auto* user = state.users.getUser(id);
                CHECK(user->getSpotAccount().updateBalance(1, 1000));
                CHECK(user->getSpotAccount().updateBalance(2, 100'000));
                CHECK(user->getContractAccount().updateBalance(2, 10'000));
            }

            CHECK(state.symbols.createSymbol("SPOT", SymbolType::Spot, 1, 2));
            CHECK(state.matching.addSpotBook(1, implementation));
            config.indexUserOpenOrders = true;
            config.tradeHistoryCapacity = 2;
            config.contractEventCapacity = 2;
        }

        PlaceOrderResult order(UserId userId, OrderSide side, Price price, Quantity quantity, Timestamp time)
        {
            return state.matching.placeOrder(userId, 1, side, OrderType::Limit, TimeInForce::GTC, price, quantity, time, trades);
        }
    };

    template <typename Row>
    std::vector<Row> all(TradingObserver& observer, ObservationQuery query)
    {
        std::vector<Row> output;
        for (std::size_t iteration = 0; iteration < 1000; ++iteration)
        {
            const auto result = observer.read(query);
            CHECK(result.page.state == ReadState::Success);
            CHECK(result.page.examined <= query.scanBudget);
            const auto rows = result.rows<Row>();
            output.insert(output.end(), rows.begin(), rows.end());
            if (!result.page.hasMore)
            {
                return output;
            }

            CHECK(result.page.next != query.cursor);
            query.cursor = result.page.next;
        }

        CHECK(false);
        return output;
    }

    void bookAndIndex(OrderBookImplementation implementation)
    {
        Fixture fixture(implementation);
        const auto first = fixture.order(1, OrderSide::Ask, 100, 5, 1);
        TradingObserver observer(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
        CHECK(observer.overview().restingOrders == 1);
        const auto second = fixture.order(2, OrderSide::Ask, 100, 7, 2);
        const auto third = fixture.order(1, OrderSide::Ask, 101, 3, 3);
        const auto bid = fixture.order(3, OrderSide::Bid, 98, 4, 4);
        CHECK(first && second && third && bid);
        observer.notifyChanged();

        ObservationQuery query;
        query.kind = ReadKind::Depth;
        query.symbolId = 1;
        query.side = OrderSide::Ask;
        query.limit = 1;
        query.scanBudget = 1;
        const auto asks = all<DepthView>(observer, query);
        CHECK(asks.size() == 2 && asks[0].price == 100 && asks[0].quantity == 12 && asks[0].orderCount == 2 && asks[1].price == 101);
        CHECK(observer.level(1, OrderSide::Bid, 98)->quantity == 4);
        ObservationQuery exact;
        exact.kind = ReadKind::Level;
        exact.symbolId = 1;
        exact.side = OrderSide::Bid;
        exact.price = 98;
        CHECK(observer.read(exact).rows<DepthView>()[0].quantity == 4);
        CHECK(observer.book(1)->askLevels == 2);
        std::array<DepthView, 1> buffer;
        const auto page = observer.readDepth(query, buffer);
        CHECK(page.count == 1 && buffer[0].price == 100 && page.hasMore);

        query.kind = ReadKind::BookOrders;
        const auto orders = all<OrderView>(observer, query);
        CHECK(orders.size() == 3 && orders[0].orderId == first.orderId && orders[1].orderId == second.orderId && orders[2].orderId == third.orderId);
        query.kind = ReadKind::UserOpenOrders;
        query.userId = 1;
        CHECK(all<OrderView>(observer, query).size() == 2);
        CHECK(observer.overview().restingOrders == 4);

        auto immutable = observer.read(query);
        CHECK(fixture.state.matching.modifyQuantity(1, first.orderId, 1, 5));
        observer.notifyChanged();
        query.kind = ReadKind::BookOrders;
        query.userId = 0;
        const auto changed = all<OrderView>(observer, query);
        CHECK(changed[0].orderId == second.orderId && changed[1].orderId == first.orderId);
        CHECK(immutable.rows<OrderView>()[0].filling.unfilled == 5 || immutable.rows<OrderView>()[0].orderId == third.orderId);
        query.cursor = page.next;
        CHECK(observer.read(query).page.state == ReadState::CursorExpired);
        query.cursor = {};

        CHECK(fixture.state.matching.modifyPrice(1, first.orderId, 102, 6, fixture.trades));
        CHECK(fixture.state.matching.cancelOrder(1, third.orderId, 7));
        observer.notifyChanged();
        query.kind = ReadKind::UserOpenOrders;
        query.userId = 1;
        CHECK(all<OrderView>(observer, query).size() == 1);
        CHECK(observer.order(first.orderId)->price == 102);
        CHECK(observer.order(third.orderId)->filling.status == OrderStatus::Cancelled);

        const auto taker = fixture.order(3, OrderSide::Bid, 103, 13, 8);
        CHECK(taker);
        observer.notifyChanged();
        observer.recordBatch(1, {fixture.trades.data(), fixture.trades.size()}, {});
        CHECK(observer.overview().restingOrders == 1);
        CHECK(all<OrderView>(observer, query).empty());
        CHECK(observer.order(second.orderId)->filling.status == OrderStatus::Filled);
        query.kind = ReadKind::Trades;
        query.symbolId = 0;
        query.userId = 3;
        CHECK(all<TradeView>(observer, query).size() == 2);
        CHECK(observer.balance(5, 1)->active == 1013);
        std::array<OrderId, 3> ids{first.orderId, second.orderId, 100000};
        std::array<std::optional<OrderView>, 3> output;
        CHECK(observer.readOrders(ids, output) == 3 && output[0] && output[1] && !output[2]);

        const auto oldTradePage = observer.read(query);
        CHECK(fixture.order(1, OrderSide::Ask, 105, 1, 9));
        CHECK(fixture.order(3, OrderSide::Bid, 105, 1, 10));
        observer.recordBatch(2, {fixture.trades.data(), fixture.trades.size()}, {});
        query.cursor = oldTradePage.page.next;
        CHECK(observer.read(query).page.state == ReadState::Success);
        CHECK(fixture.order(1, OrderSide::Ask, 106, 1, 11));
        CHECK(fixture.order(3, OrderSide::Bid, 106, 1, 12));
        observer.recordBatch(3, {fixture.trades.data(), fixture.trades.size()}, {});
        query.cursor = oldTradePage.page.next;
        CHECK(observer.read(query).page.state == ReadState::CursorExpired);
        observer.recordBatch(3, {fixture.trades.data(), fixture.trades.size()}, {});
        CHECK(observer.overview().newestTrade == 4);
    }

    void managersAndBuffers()
    {
        Fixture fixture;
        TradingObserver observer(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching);
        ObservationQuery query;
        query.limit = 1;
        query.scanBudget = 1;
        query.kind = ReadKind::Assets;
        CHECK(all<AssetView>(observer, query).size() == 2);
        query.kind = ReadKind::Users;
        CHECK(all<UserView>(observer, query).size() == 3);
        query.kind = ReadKind::Accounts;
        CHECK(all<AccountView>(observer, query).size() == 6);
        query.userId = 2;
        CHECK(all<AccountView>(observer, query).size() == 2);
        query.userId = 0;
        query.kind = ReadKind::Balances;
        CHECK(all<BalanceView>(observer, query).size() == 9);
        query.userId = 1;
        CHECK(all<BalanceView>(observer, query).size() == 3);
        query.userId = 0;
        query.kind = ReadKind::Symbols;
        CHECK(all<SymbolView>(observer, query).size() == 1);
        query.kind = ReadKind::Books;
        CHECK(all<BookView>(observer, query).size() == 1);
        query.kind = ReadKind::UserOpenOrders;
        query.userId = 1;
        CHECK(observer.read(query).page.state == ReadState::IndexUnavailable);

        auto* account = &fixture.state.users.getUser(1)->getSpotAccount();
        for (AssetId assetId = 3; assetId < 20; ++assetId)
        {
            CHECK(account->updateBalance(assetId, assetId));
        }

        observer.notifyChanged();
        query.kind = ReadKind::Balances;
        query.accountId = 1;
        query.userId = 0;
        CHECK(all<BalanceView>(observer, query).size() == 19);
        CHECK(observer.balance(1, 19)->total == 19);
        CHECK(fixture.state.users.removeUser(2));
        observer.notifyChanged();
        query.kind = ReadKind::Users;
        query.accountId = 0;
        CHECK(all<UserView>(observer, query).size() == 2);
        query.includeInactive = true;
        CHECK(all<UserView>(observer, query).size() == 3);
        CHECK(!observer.user(2)->active && !observer.account(3)->active);

        query.kind = ReadKind::Assets;
        const auto page = observer.read(query);
        query.cursor = page.page.next;
        query.assetId = 2;
        CHECK(observer.read(query).page.state == ReadState::CursorExpired);
        query.cursor = {};
        query.limit = 100000;
        CHECK(observer.read(query).page.state == ReadState::InvalidQuery);
        query.limit = 1;
        query.cursor.anchor = 1;
        CHECK(observer.read(query).page.state == ReadState::InvalidQuery);

        ObservationConfig disabledHistory;
        disabledHistory.tradeHistoryCapacity = 0;
        disabledHistory.contractEventCapacity = 0;
        TradingObserver noHistory(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, disabledHistory);
        query = {};
        query.kind = ReadKind::Trades;
        CHECK(noHistory.read(query).page.state == ReadState::HistoryUnavailable);
        query.kind = ReadKind::ContractEvents;
        CHECK(noHistory.read(query).page.state == ReadState::HistoryUnavailable);

        const std::array<ObservationQuery, 17> tooMany{};
        CHECK(observer.readBatch(tooMany).state == ReadState::InvalidQuery);
        std::array<ObservationQuery, 5> tooMuch{};
        for (auto& value : tooMuch)
        {
            value.scanBudget = 16'384;
        }

        CHECK(observer.readBatch(tooMuch).state == ReadState::InvalidQuery);
    }

    void newestHistory()
    {
        Fixture fixture;
        CHECK(fixture.order(1, OrderSide::Bid, 80, 1, 3));
        CHECK(fixture.order(1, OrderSide::Bid, 81, 1, 1));
        CHECK(fixture.order(1, OrderSide::Bid, 82, 1, 2));
        TradingObserver observer(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
        ObservationQuery query;
        query.kind = ReadKind::OrdersNewest;
        query.userId = 1;
        query.limit = 1;
        query.scanBudget = 1;
        const auto orders = all<OrderView>(observer, query);
        CHECK(orders.size() == 3 && orders[0].orderId == 1 && orders[1].orderId == 3 && orders[2].orderId == 2);
        auto first = observer.read(query);
        CHECK(fixture.order(1, OrderSide::Bid, 83, 1, 4));
        observer.notifyChanged();
        query.cursor = first.page.next;
        const auto next = observer.read(query);
        CHECK(next.page.state == ReadState::Success && next.rows<OrderView>()[0].orderId == 3);
        query.cursor = {};
        CHECK(observer.read(query).rows<OrderView>()[0].orderId == 4);

        CHECK(fixture.state.symbols.createSymbol("PERP", SymbolType::Perpetual, 1, 2));
        ContractSpec spec;
        spec.makerFeeRate = 0;
        spec.takerFeeRate = 0;
        spec.liquidationFeeRate = 0;
        spec.firstFundingTimestamp = 1000;
        CHECK(fixture.state.matching.addContractBook(2, spec, 100, 0));
        ContractOrderOptions options;
        options.marginMode = MarginMode::Cross;
        const auto ask = fixture.state.matching.placeContractOrder(2, 2, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 2, 5, options, fixture.trades);
        const auto bid = fixture.state.matching.placeContractOrder(1, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 2, 5, options, fixture.trades);
        CHECK(ask && bid);
        observer.recordBatch(1, fixture.trades, {});
        const auto empty = fixture.state.matching.placeContractOrder(1, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 80, 1, 5, options, fixture.trades);
        CHECK(empty && fixture.state.matching.cancelOrder(1, empty.orderId, 5));
        options.reduceOnly = true;
        options.positionId = ask.positionId;
        CHECK(fixture.state.matching.placeContractOrder(2, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 2, 5, options, fixture.trades));
        options.positionId = bid.positionId;
        options.priceLimit = 100;
        CHECK(fixture.state.matching.placeContractOrder(1, 2, OrderSide::Ask, OrderType::Market, TimeInForce::IOC, 0, 2, 5, options, fixture.trades));
        observer.recordBatch(2, fixture.trades, {});
        query = {};
        query.kind = ReadKind::PositionsNewest;
        query.userId = 1;
        query.positionFilter = PositionFilter::ClosedExecuted;
        query.limit = 1;
        query.scanBudget = 1;
        const auto closed = all<PositionView>(observer, query);
        CHECK(closed.size() == 1 && closed[0].positionId == bid.positionId && closed[0].closed);
        CHECK(closed[0].paidFees == 0 && closed[0].realizedPnL == 0 && closed[0].updatedTimestamp == closed[0].createdTimestamp);
        observer.rebuildOpenOrderIndex();
        CHECK(all<PositionView>(observer, query).size() == 1);
        // Rebuilding from retained order fills must work without the recent trade tape.
        ObservationConfig detachedConfig;
        TradingObserver restored(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, detachedConfig);
        CHECK(all<PositionView>(restored, query).size() == 1);
        query = {};
        query.kind = ReadKind::OrdersNewest;
        query.userId = 1;
        CHECK(all<OrderView>(observer, query)[0].reduceOnly);
    }

    void contractsAndBatches()
    {
        Fixture fixture;
        CHECK(fixture.state.symbols.createSymbol("PERP", SymbolType::Perpetual, 1, 2));
        ContractSpec spec;
        spec.makerFeeRate = 0;
        spec.takerFeeRate = 0;
        spec.liquidationFeeRate = 0;
        spec.firstFundingTimestamp = 1000;
        CHECK(fixture.state.matching.addContractBook(2, spec, 100, 0));
        TradingObserver observer(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
        ContractOrderOptions options;
        options.marginMode = MarginMode::Cross;
        const auto ask = fixture.state.matching.placeContractOrder(2, 2, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 10, 1, options, fixture.trades);
        const auto bid = fixture.state.matching.placeContractOrder(1, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 10, 2, options, fixture.trades);
        CHECK(ask && bid);
        observer.notifyChanged();
        CHECK(observer.position(bid.positionId)->size == 10 && observer.position(bid.positionId)->accountId == 2);
        CHECK(observer.order(bid.orderId)->marginMode == MarginMode::Cross);
        CHECK(observer.account(2)->openPositionCount == 1);
        CHECK(observer.crossMargin(2, 2)->initialMargin == 100);
        ObservationQuery query;
        query.kind = ReadKind::Positions;
        query.limit = 1;
        query.scanBudget = 1;
        CHECK(all<PositionView>(observer, query).size() == 2);
        query.userId = 1;
        CHECK(all<PositionView>(observer, query).size() == 1);
        query.kind = ReadKind::AccountPositions;
        query.accountId = 2;
        query.positionFilter = PositionFilter::Open;
        CHECK(all<PositionView>(observer, query).size() == 1);
        query.kind = ReadKind::ManagedPositions;
        CHECK(all<PositionView>(observer, query).size() == 1);
        query.kind = ReadKind::CrossMargins;
        CHECK(all<CrossMarginView>(observer, query).size() == 1);
        query = {};
        query.kind = ReadKind::Markets;
        CHECK(all<MarketView>(observer, query).size() == 1);

        query.kind = ReadKind::MarketPositions;
        query.symbolId = 2;
        query.positionFilter = PositionFilter::Open;
        query.limit = 1;
        query.scanBudget = 1;
        CHECK(all<PositionView>(observer, query).size() == 2);
        const auto pendingOrder = fixture.state.matching.placeContractOrder(3, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 80, 1, 2, options, fixture.trades);
        CHECK(pendingOrder);
        // An existing open participant can also have an order. Trading is a
        // union, so it must not appear twice in a paginated market response.
        auto existing = options;
        existing.positionId = bid.positionId;
        const auto additionalOrder = fixture.state.matching.placeContractOrder(1, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 80, 1, 2, existing, fixture.trades);
        CHECK(additionalOrder);
        observer.notifyChanged();
        query.positionFilter = PositionFilter::Trading;
        CHECK(all<PositionView>(observer, query).size() == 3);
        CHECK(fixture.state.matching.cancelOrder(3, pendingOrder.orderId, 2));
        CHECK(fixture.state.matching.cancelOrder(1, additionalOrder.orderId, 2));
        observer.notifyChanged();
        CHECK(all<PositionView>(observer, query).size() == 2);

        auto& importedAccount = fixture.state.users.getUser(3)->getContractAccount();
        Position imported(100'000, 3, 2, PositionSide::Long, MarginMode::Isolated, 1, 1, 2);
        imported.positionSize = 3;
        imported.closed = false;
        CHECK(importedAccount.recordPosition(imported));
        observer.notifyChanged();
        query = {};
        query.kind = ReadKind::AccountPositions;
        query.accountId = importedAccount.getAccountId();
        query.positionFilter = PositionFilter::Open;
        CHECK(all<PositionView>(observer, query).size() == 1);
        CHECK(observer.position(100'000, query.accountId)->size == 3);
        CHECK(!observer.position(100'000));
        query.kind = ReadKind::ManagedPositions;
        CHECK(all<PositionView>(observer, query).empty());
        query = {};

        const std::array<ContractEvent, 1> funding{{{ContractEventType::Funding, 2, bid.positionId, 1, 100, 10, 0, -7, 0, 0, 2}}};
        observer.recordBatch(1, {}, funding);
        query.kind = ReadKind::ContractEvents;
        query.accountId = 2;
        const auto events = all<ContractEventView>(observer, query);
        CHECK(events.size() == 1 && events[0].accountId == 2 && events[0].event.fundingChange == -7);
        query.accountId = 4;
        CHECK(all<ContractEventView>(observer, query).empty());
        query = {};

        auto& contracts = fixture.state.matching.getContractEngine();
        contracts.setAutomaticLiquidationBudget(0);
        CHECK(fixture.state.users.getUser(1)->getContractAccount().updateBalance(2, -9900));
        const auto marked = contracts.updateMarkPrice(2, 90, 3);
        CHECK(marked.state == ContractState::PendingLiquidation);
        observer.notifyChanged();
        query.kind = ReadKind::PendingLiquidations;
        query.symbolId = 2;
        const auto pending = all<PositionView>(observer, query);
        CHECK(pending.size() == 1 && pending[0].positionId == bid.positionId && pending[0].liquidationQueued);
        CHECK(observer.market(2)->pendingLiquidations == 1);
        std::array<ObservationQuery, 3> batch;
        batch[0].kind = ReadKind::User;
        batch[0].userId = 1;
        batch[1].kind = ReadKind::Balances;
        batch[1].userId = 1;
        batch[2].kind = ReadKind::Positions;
        batch[2].userId = 1;
        const auto response = observer.readBatch(batch);
        CHECK(response.state == ReadState::Success && response.results.size() == 3);
        for (const auto& result : response.results)
        {
            CHECK(result.page.stamp.revision == response.stamp.revision && result.page.stamp.session == response.stamp.session);
        }
    }

    void cacheConcurrency()
    {
        Fixture fixture;
        CHECK(fixture.order(1, OrderSide::Ask, 100, 1, 1));
        TradingObserver observer(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching);
        ObservationCache cache(1);
        ObservationQuery query;
        query.kind = ReadKind::Depth;
        query.symbolId = 1;
        query.side = OrderSide::Ask;
        const auto slot = cache.create(query);
        CHECK(slot->load() == nullptr && cache.size() == 1);
        const auto initial = slot->refresh(observer);
        std::atomic<bool> done{false};
        std::atomic<std::size_t> reads{0};
        std::latch ready(4);
        std::array<std::thread, 4> threads;
        for (auto& thread : threads)
        {
            thread = std::thread([&]
            {
                CHECK(slot->load()->rows<DepthView>().front().quantity == 1);
                reads.fetch_add(1, std::memory_order_relaxed);
                ready.count_down();
                while (!done.load(std::memory_order_acquire))
                {
                    const auto image = slot->load();
                    CHECK(image != nullptr && image->page.count == image->rows<DepthView>().size());
                    CHECK(image->rows<DepthView>().front().quantity > 0);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        ready.wait();

        for (Timestamp time = 2; time < 102; ++time)
        {
            CHECK(fixture.state.matching.modifyQuantity(1, 1, 1, time));
            observer.notifyChanged();
            slot->refresh(observer);
        }

        done.store(true, std::memory_order_release);
        for (auto& thread : threads)
        {
            thread.join();
        }

        CHECK(reads.load() > 0);
        CHECK(initial->rows<DepthView>().front().quantity == 1);
        CHECK(slot->load()->rows<DepthView>().front().quantity == 101);
        CHECK(!slot->publish(initial));
        auto wrong = std::make_shared<ObservationResult>(*initial);
        wrong->query.symbolId = 2;
        CHECK(!slot->publish(wrong));
        auto durable = std::make_shared<ObservationResult>(*slot->load());
        durable->page.stamp.durableThrough = 10;
        CHECK(slot->publish(durable));
        CHECK(!slot->publish(slot->refresh(observer)));
        bool exceeded = false;
        try
        {
            cache.create(query);
        }
        catch (const std::length_error&)
        {
            exceeded = true;
        }

        CHECK(exceeded && cache.size() == 1);
        ObservationCache released(1);
        released.create(query).reset();
        CHECK(released.size() == 0 && released.create(query) != nullptr);

        auto faulted = std::make_shared<ObservationResult>(*slot->load());
        faulted->page.stamp.sourceFaulted = true;
        faulted->page.state = ReadState::SourceFaulted;
        CHECK(slot->publish(faulted));
        CHECK(!slot->publish(durable));
    }

    struct Worker : ObservedConsumer<TradingCommand, CommandResult>
    {
        TradingState state;
        TradingObserver observer;
        TradeBuffer trades;

        Worker()
            : observer(state.assets, state.users, state.symbols, state.matching)
        {
            this->bindObserver(this->observer);
        }

        ~Worker() override
        {
            this->stopBeforeDerivedDestruction();
        }

        CommandResult consumeCommand(TradingCommand&& command) override
        {
            return executeTradingCommand(this->state, command, this->trades);
        }
    };

    void consumerIntegration()
    {
        Worker worker;
        worker.start(128);
        consuming::ResponseSlot<ObservedResponse<CommandResult>> slot;
        auto first = worker.post(TradingCommand(commands::CreateUser{}), slot).get();
        CHECK(std::get<0>(first).entityId == 1);
        ObservationCache cache;
        ObservationQuery query;
        query.kind = ReadKind::Users;
        auto handle = cache.create(query);
        auto reply = worker.post(ObservationRequest{query, handle, {}}, slot).get();
        CHECK(std::get<1>(reply)->rows<UserView>().size() == 1);
        CHECK(handle->load()->rows<UserView>().size() == 1);
        ObservationRequest batch;
        batch.batch = {query, ObservationQuery{}};
        auto joined = worker.post(std::move(batch), slot).get();
        CHECK(std::get<2>(joined)->results.size() == 2);
        worker.stop();
    }

    void persistenceIntegration()
    {
        PersistenceConfig config;
        config.enabled = false;
        PersistentTradingEngine engine(config);
        TradingObserver observer(engine);
        ObservationEventSink events(observer);
        engine.setEventSink(&events);
        CHECK(engine.execute(commands::CreateUser{}).result);
        ObservationQuery query;
        query.kind = ReadKind::Users;
        const auto reply = observer.read(query);
        CHECK(reply.page.stamp.sequence == 1 && reply.page.stamp.durableThrough == 0 && reply.rows<UserView>().size() == 1);
        engine.setEventSink(nullptr);
        engine.close();
    }

    void lifecycleAndFailures()
    {
        Fixture fixture;
        {
            TradingObserver indexed(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
            CHECK(fixture.state.assets.createAsset("Euro", "EUR", 0));
            CHECK(fixture.state.symbols.createSymbol("SPOT2", SymbolType::Spot, 1, 3));
            CHECK(fixture.state.matching.addSpotBook(2));
            CHECK(fixture.state.matching.placeOrder(1, 2, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 3, 1, fixture.trades));
            indexed.notifyChanged();
            CHECK(indexed.overview().restingOrders == 1);
            bool conflicting = false;
            try
            {
                TradingObserver another(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
            }
            catch (const std::logic_error&)
            {
                conflicting = true;
            }

            CHECK(conflicting);
        }

        // Destruction detached listeners, including books created after attach.
        CHECK(fixture.state.matching.cancelOrder(1, 1, 2));
        TradingObserver replacement(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, fixture.config);
        CHECK(replacement.overview().restingOrders == 0);

        struct Archive : HistorySink
        {
            bool archiveOrder(const BasicOrder&) noexcept override
            {
                return true;
            }

            bool archivePosition(const Position&) noexcept override
            {
                return true;
            }
        } archive;
        const auto detached = replacement.order(1);
        CHECK(detached && detached->quantity == 3);
        CHECK(fixture.state.matching.archiveHistory(archive, 100).archivedOrders == 1);
        replacement.notifyChanged();
        CHECK(!replacement.order(1) && detached->quantity == 3);

        TradingObserver faulted(fixture.state.assets, fixture.state.users, fixture.state.symbols, fixture.state.matching, {}, []
        {
            ReadStamp value;
            value.sourceFaulted = true;
            return value;
        });
        CHECK(faulted.read({}).page.state == ReadState::SourceFaulted);
        ObservationQuery depth;
        depth.kind = ReadKind::Depth;
        depth.symbolId = 1;
        std::array<DepthView, 1> output{{{1, OrderSide::Bid, 7, 9, 1}}};
        CHECK(faulted.readDepth(depth, output).state == ReadState::SourceFaulted && output[0].price == 7);
    }
}

int main()
{
    bookAndIndex(OrderBookImplementation::Radix);
    bookAndIndex(OrderBookImplementation::IndexedTree);
    managersAndBuffers();
    newestHistory();
    contractsAndBatches();
    cacheConcurrency();
    consumerIntegration();
    persistenceIntegration();
    lifecycleAndFailures();
    std::puts("Observation tests passed");
}
