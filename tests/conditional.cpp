#include <advmatch/sdk/AdvMatch.h>

#include <chrono>
#include <filesystem>
#include <iostream>

namespace sdk = advmatch::sdk;

namespace
{
    void check(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    sdk::ExchangeConfig memory()
    {
        sdk::ExchangeConfig config;
        config.persistence.enabled = false;
        return config;
    }

    struct Fixture
    {
        sdk::MatchingExchange exchange;
        sdk::Asset base;
        sdk::Asset quote;
        sdk::User buyer;
        sdk::User seller;
        sdk::Symbol spot;
        sdk::OrderBook book;

        Fixture()
            : Fixture(memory())
        {
        }

        explicit Fixture(sdk::ExchangeConfig config)
            : exchange(std::move(config)), base(exchange.assets().create("BASE", "BASE", 0).get()), quote(exchange.assets().create("QUOTE", "QUOTE", 0).get()), buyer(exchange.users().create().get()), seller(exchange.users().create().get()), spot(exchange.symbols().create("SPOT", sdk::SymbolType::Spot, base, quote).get()), book(exchange.books().addSpot(spot).get())
        {
            this->buyer.spotAccount().get().updateBalance(this->quote, 1'000'000).get();
            this->seller.spotAccount().get().updateBalance(this->base, 1000).get();
            this->buyer.contractAccount().get().updateBalance(this->quote, 1'000'000).get();
            this->seller.contractAccount().get().updateBalance(this->quote, 1'000'000).get();
        }

        void trade(sdk::Price price, sdk::Timestamp timestamp)
        {
            this->book.place(this->seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, price, 1, timestamp).get();
            this->book.place(this->buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, price, 1, timestamp).get();
        }

        sdk::ConditionalOrderRequest request(sdk::Price trigger = 90) const
        {
            sdk::ConditionalOrderRequest request;
            request.userId = this->seller.id();
            request.symbolId = this->spot.id();
            request.quantity = 3;
            request.timestamp = 3;
            request.triggerPrice = trigger;
            return request;
        }

        sdk::Symbol contract(sdk::SymbolType type)
        {
            const auto symbol = this->exchange.symbols().create("CONTRACT", type, this->base, this->quote).get();
            sdk::ContractSpec spec;
            spec.takerFeeRate = 0;
            spec.liquidationFeeRate = 0;
            spec.priceBandRate = 200'000;
            spec.firstFundingTimestamp = 1000;
            spec.expiryTimestamp = type == sdk::SymbolType::Future ? 50 : 0;
            this->exchange.books().addContract(symbol, spec, 100, 0).get();
            return symbol;
        }
    };

    void directions()
    {
        for (auto kind : {sdk::ConditionalOrderKind::StopLoss, sdk::ConditionalOrderKind::TakeProfit})
        {
            for (auto side : {sdk::OrderSide::Bid, sdk::OrderSide::Ask})
            {
                Fixture fixture;
                fixture.trade(100, 2);
                const bool rising = (kind == sdk::ConditionalOrderKind::StopLoss) == (side == sdk::OrderSide::Bid);
                auto request = fixture.request(rising ? 120 : 80);
                request.userId = side == sdk::OrderSide::Bid ? fixture.buyer.id() : fixture.seller.id();
                request.side = side;
                request.kind = kind;
                request.type = sdk::OrderType::Limit;
                request.timeInForce = sdk::TimeInForce::GTC;
                request.price = side == sdk::OrderSide::Bid ? 1 : 200;
                const auto order = fixture.exchange.conditionalOrders().place(request).get();
                fixture.trade(rising ? 119 : 81, 4);
                check(order.snapshot().get().status == sdk::ConditionalOrderStatus::Waiting, "direction waits before threshold");
                fixture.trade(rising ? 120 : 80, 5);
                const auto view = order.snapshot().get();
                check(view.status == sdk::ConditionalOrderStatus::Activated && view.triggeredPrice == request.triggerPrice, "inclusive buy/sell TP/SL threshold");
                check(order.order().get()->snapshot().get().resting, "stop limit becomes an ordinary resting order");
            }
        }
    }

    void spotOco()
    {
        Fixture fixture;
        fixture.trade(100, 2);
        const auto manager = fixture.exchange.conditionalOrders();
        const auto stop = manager.place(fixture.request()).get();
        auto profitRequest = fixture.request(110);
        profitRequest.kind = sdk::ConditionalOrderKind::TakeProfit;
        profitRequest.ocoOrderId = stop.id();
        const auto profit = manager.place(profitRequest).get();
        check(fixture.seller.spotAccount().get().balance(fixture.base).get().reserved == 0, "waiting orders do not reserve funds");
        fixture.book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 4).get();
        fixture.trade(90, 5);
        const auto view = stop.snapshot().get();
        check(view.status == sdk::ConditionalOrderStatus::Activated && view.orderId != 0 && view.updatedTimestamp == 5, "stop market activated once");
        check(stop.order().get()->snapshot().get().filling.filled == 3, "stop market settles the child fill");
        check(profit.snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled, "OCO sibling cancelled on acceptance");
        check(fixture.exchange.orders().trades().get().values.size() == 3, "conditional trades included in command observation tape");
        sdk::ObservationQuery query;
        query.userId = fixture.seller.id();
        query.limit = 1;
        auto page = manager.list(query).get();
        check(page.values.size() == 1 && page.page.hasMore, "conditionals paginated");
        query.cursor = page.page.next;
        check(manager.list(query).get().values.size() == 1, "conditional cursor resumes");
        query.cursor = {};
        query.includeTerminal = false;
        check(manager.list(query).get().values.empty(), "waiting-only filter excludes completed OCO");
    }

    void modificationAndRejection()
    {
        Fixture fixture;
        fixture.trade(100, 2);
        auto manager = fixture.exchange.conditionalOrders();
        auto stop = manager.place(fixture.request()).get();
        auto result = fixture.exchange.submit(advmatch::commands::CancelConditionalOrder{fixture.buyer.id(), stop.id(), 3}).get();
        check(!result.result.accepted && result.result.state == static_cast<std::uint8_t>(sdk::ConditionalOrderState::WrongUser), "ownership checked");
        stop.modify(85, 0, 2, 4).get();
        check(stop.snapshot().get().request.triggerPrice == 85, "trigger modification reindexes");
        fixture.trade(90, 5);
        check(stop.snapshot().get().status == sdk::ConditionalOrderStatus::Waiting, "old trigger removed");
        stop.cancel(6).get();
        fixture.trade(80, 7);
        check(stop.snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled && !stop.order().get(), "cancel never activates");

        auto bad = fixture.request();
        bad.source = sdk::TriggerPriceSource::Mark;
        result = fixture.exchange.submit(advmatch::commands::PlaceConditionalOrder{bad}).get();
        check(!result.result.accepted, "spot cannot use mark triggers");
        bad = fixture.request();
        bad.timestamp = 1;
        result = fixture.exchange.submit(advmatch::commands::PlaceConditionalOrder{bad}).get();
        check(result.result.state == static_cast<std::uint8_t>(sdk::ConditionalOrderState::InvalidTimestamp), "stale registration rejected");

        const auto poor = fixture.exchange.users().create().get();
        auto failed = fixture.request(75);
        failed.userId = poor.id();
        failed.timestamp = 8;
        const auto rejected = manager.place(failed).get();
        auto survivor = failed;
        survivor.kind = sdk::ConditionalOrderKind::TakeProfit;
        survivor.triggerPrice = 110;
        survivor.ocoOrderId = rejected.id();
        const auto sibling = manager.place(survivor).get();
        fixture.trade(75, 9);
        const auto rejectedView = rejected.snapshot().get();
        check(rejectedView.status == sdk::ConditionalOrderStatus::Rejected && rejectedView.rejectReason == sdk::PlaceOrderState::InsufficientBalance, "activation uses current risk and stores rejection");
        check(sibling.snapshot().get().status == sdk::ConditionalOrderStatus::Waiting && sibling.snapshot().get().request.ocoOrderId == 0, "rejection preserves the other protection");
        sibling.cancel(10).get();

        auto immediate = fixture.request(80);
        immediate.timestamp = 11;
        const auto activated = manager.place(immediate).get();
        check(activated.snapshot().get().status == sdk::ConditionalOrderStatus::Activated, "already satisfied trigger activates immediately");
    }

    void contracts(sdk::TriggerPriceSource source)
    {
        Fixture fixture;
        const auto symbol = fixture.contract(sdk::SymbolType::Perpetual);
        const auto book = symbol.book();
        book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1, sdk::ContractOrderOptions{}).get();
        const auto longOrder = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 10, 2, sdk::ContractOrderOptions{}).get();
        const auto position = *longOrder.position().get();
        position.addMargin(1000).get();
        if (source == sdk::TriggerPriceSource::Index)
        {
            fixture.exchange.contracts().updateReference(symbol, sdk::ReferencePrices{100, 100, 0, 100, 100, 1}, 2).get();
        }

        auto request = fixture.request(90);
        request.userId = fixture.buyer.id();
        request.symbolId = symbol.id();
        request.source = source;
        request.quantity = 10;
        request.contractOptions = sdk::ContractOrderOptions{0, position.id(), sdk::MarginMode::Isolated, true, 50};
        const auto stop = fixture.exchange.conditionalOrders().place(request).get();
        auto profitRequest = request;
        profitRequest.kind = sdk::ConditionalOrderKind::TakeProfit;
        profitRequest.triggerPrice = 110;
        profitRequest.ocoOrderId = stop.id();
        const auto profit = fixture.exchange.conditionalOrders().place(profitRequest).get();
        book.place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 4, sdk::ContractOrderOptions{1}).get();
        if (source == sdk::TriggerPriceSource::Mark)
        {
            fixture.exchange.contracts().updateMark(symbol, 90, 5).get();
        }
        else
        {
            fixture.exchange.contracts().updateReference(symbol, sdk::ReferencePrices{90, 100, 0, 100, 90, 1}, 5).get();
        }

        check(stop.snapshot().get().status == sdk::ConditionalOrderStatus::Activated, "contract reference triggers reduce-only child");
        check(stop.snapshot().get().positionId == position.id() && position.snapshot().get().closed, "conditional closes existing position without opening opposite exposure");
        check(profit.snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled, "contract OCO cannot close twice");
        check(fixture.exchange.orders().trades().get().values.size() == 2, "mark/index triggered trade observable");
    }

    void executionBoundaries()
    {
        Fixture fixture;
        fixture.trade(100, 2);
        auto request = fixture.request(90);
        request.timeInForce = sdk::TimeInForce::FOK;
        request.quantity = 20;
        auto fok = fixture.exchange.conditionalOrders().place(fixture.seller, fixture.spot, request).get();
        const auto liquidity = fixture.book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 4).get();
        fixture.trade(90, 5);
        check(fok.snapshot().get().status == sdk::ConditionalOrderStatus::Rejected && fok.snapshot().get().rejectReason == sdk::PlaceOrderState::InsufficientLiquidity, "FOK trigger rejects without a partial fill");
        check(fixture.book.snapshot().get().bestBid->quantity == 10, "failed FOK leaves liquidity intact");
        liquidity.cancel(6).get();

        request = fixture.request(70);
        request.timestamp = 10;
        auto waiting = fixture.exchange.conditionalOrders().place(request).get();
        fixture.trade(60, 9);
        check(waiting.snapshot().get().status == sdk::ConditionalOrderStatus::Waiting, "old price cannot execute a newer registration");
        fixture.trade(60, 11);
        check(waiting.snapshot().get().status == sdk::ConditionalOrderStatus::Activated, "a later matching price activates");

        Fixture cascade;
        cascade.trade(100, 2);
        auto firstRequest = cascade.request(90);
        auto first = cascade.exchange.conditionalOrders().place(firstRequest).get();
        auto secondRequest = cascade.request(80);
        secondRequest.quantity = 2;
        auto second = cascade.exchange.conditionalOrders().place(secondRequest).get();
        cascade.book.place(cascade.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 4).get();
        cascade.trade(90, 5);
        check(first.order().get()->snapshot().get().filling.filled == 3 && second.order().get()->snapshot().get().filling.filled == 2, "child trades trigger an iterative cascade");
        check(cascade.exchange.orders().trades().get().values.size() == 4, "cascade retains every fill in the triggering command");
    }

    void expiry()
    {
        Fixture fixture;
        const auto symbol = fixture.contract(sdk::SymbolType::Future);
        auto request = fixture.request(80);
        request.symbolId = symbol.id();
        request.source = sdk::TriggerPriceSource::Mark;
        request.contractOptions = sdk::ContractOrderOptions{0, 0, sdk::MarginMode::Isolated, false, 50};
        auto pending = fixture.exchange.conditionalOrders().place(request).get();
        fixture.exchange.contracts().deliver(symbol, 100, 50).get();
        check(pending.snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled, "expiry cancels dormant conditions");
    }

    void recovery()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-conditional-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::ConditionalOrderId stopId = 0;
        sdk::ConditionalOrderId profitId = 0;
        sdk::ConditionalOrderId cancelledId = 0;
        sdk::UserId buyerId = 0;
        sdk::UserId sellerId = 0;
        sdk::SymbolId symbolId = 0;
        sdk::OrderId childId = 0;
        {
            Fixture fixture(config);
            fixture.trade(100, 2);
            buyerId = fixture.buyer.id();
            sellerId = fixture.seller.id();
            symbolId = fixture.spot.id();
            auto manager = fixture.exchange.conditionalOrders();
            auto stop = manager.place(fixture.request()).get();
            stopId = stop.id();
            stop.modify(85, 0, 3, 4).get();
            auto request = fixture.request(110);
            request.timestamp = 4;
            request.kind = sdk::ConditionalOrderKind::TakeProfit;
            request.ocoOrderId = stopId;
            profitId = manager.place(request).get().id();
            request.ocoOrderId = 0;
            auto cancelled = manager.place(request).get();
            cancelledId = cancelled.id();
            cancelled.cancel(5).get();
            fixture.exchange.persistence().checkpoint().get();
        }

        {
            sdk::MatchingExchange exchange(config);
            const auto manager = exchange.conditionalOrders();
            check(manager.get(stopId).snapshot().get().request.triggerPrice == 85, "checkpoint restores changed trigger and index");
            check(manager.get(profitId).snapshot().get().request.ocoOrderId == stopId, "checkpoint restores mutual OCO");
            check(manager.get(cancelledId).snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled, "cancellation survives replay");
            auto book = exchange.books().get(symbolId);
            auto buyer = exchange.users().get(buyerId);
            auto seller = exchange.users().get(sellerId);
            book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 6).get();
            book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 85, 1, 7).get();
            book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 85, 1, 7).get();
            childId = manager.get(stopId).snapshot().get().orderId;
            check(childId != 0 && manager.get(profitId).snapshot().get().status == sdk::ConditionalOrderStatus::Cancelled, "restored waiting order activates");
        }

        {
            sdk::MatchingExchange exchange(config);
            const auto value = exchange.conditionalOrders().get(stopId).snapshot().get();
            check(value.status == sdk::ConditionalOrderStatus::Activated && value.orderId == childId, "WAL replay preserves child ID without duplicate activation");
        }

        std::filesystem::remove_all(directory);
    }
}

int main()
{
    try
    {
        directions();
        spotOco();
        modificationAndRejection();
        contracts(sdk::TriggerPriceSource::Mark);
        contracts(sdk::TriggerPriceSource::Index);
        executionBoundaries();
        expiry();
        recovery();
        std::cout << "Conditional orders: TP/SL directions, market/limit, OCO, risk, reads, expiry and recovery passed\n";
    }
    catch (const sdk::BusinessError& error)
    {
        std::cerr << error.what() << " type=" << static_cast<unsigned>(error.receipt().result.type) << " state=" << static_cast<unsigned>(error.receipt().result.state) << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
