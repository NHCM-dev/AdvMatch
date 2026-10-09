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

    template <typename Function, typename State>
    void rejected(Function function, State expected)
    {
        try
        {
            function();
        }
        catch (const sdk::BusinessError& error)
        {
            check(error.receipt().result.state == static_cast<std::uint8_t>(expected), "wrong rejection state");
            return;
        }

        throw std::runtime_error("expected business rejection");
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
            for (const auto& user : {this->buyer, this->seller})
            {
                user.spotAccount().get().updateBalance(this->base, 1000).get();
                user.spotAccount().get().updateBalance(this->quote, 1'000'000).get();
                user.contractAccount().get().updateBalance(this->quote, 1'000'000).get();
            }
        }

        sdk::Symbol contract(sdk::SymbolType type = sdk::SymbolType::Perpetual)
        {
            auto symbol = this->exchange.symbols().create("CONTRACT", type, this->base, this->quote).get();
            sdk::ContractSpec spec;
            spec.firstFundingTimestamp = type == sdk::SymbolType::Perpetual ? 100 : 0;
            spec.expiryTimestamp = type == sdk::SymbolType::Future ? 50 : 0;
            this->exchange.books().addContract(symbol, spec, 100, 0).get();
            return symbol;
        }

        sdk::Position open(const sdk::Symbol& symbol)
        {
            auto book = symbol.book();
            book.place(this->seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1, sdk::ContractOrderOptions{}).get();
            auto order = book.place(this->buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 10, 2, sdk::ContractOrderOptions{}).get();
            return *order.position().get();
        }

        void audit()
        {
            const auto report = this->exchange.audit().report().get();
            for (const auto& issue : report.issues)
            {
                std::cerr << issue.message << '\n';
            }

            check(report.conserved && report.consistent, "halt preserves balances and reservations");
        }
    };

    void admissionAndScopes()
    {
        Fixture fixture;
        auto order = fixture.book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1).get();
        fixture.exchange.trading().halt(fixture.spot, "maintenance", 2).get();
        auto status = fixture.exchange.trading().status(fixture.spot).get();
        check(status.halted && status.symbolHalted && !status.exchangeHalted && status.symbolReason == "maintenance", "symbol halt status");
        rejected([&] { fixture.book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 10, 3).get(); }, sdk::PlaceOrderState::TradingDisabled);
        rejected([&] { order.modifyPrice(101, 3).get(); }, sdk::ModifyOrderState::TradingDisabled);
        rejected([&] { order.modifyQuantity(-1, 3).get(); }, sdk::ModifyOrderState::TradingDisabled);
        check(order.snapshot().get().resting && order.snapshot().get().reservedBalance == 10, "halt does not cancel or release resting orders");
        sdk::ConditionalOrderRequest request;
        request.userId = fixture.seller.id();
        request.symbolId = fixture.spot.id();
        request.quantity = 1;
        request.triggerPrice = 90;
        request.timestamp = 3;
        rejected([&] { fixture.exchange.conditionalOrders().place(request).get(); }, sdk::ConditionalOrderState::TradingDisabled);
        order.cancel(4).get();
        check(fixture.seller.spotAccount().get().balance(fixture.base).get().reserved == 0, "cancellation remains available");
        fixture.exchange.trading().halt("global maintenance", 5).get();
        fixture.exchange.trading().resume(fixture.spot, 6).get();
        status = fixture.exchange.trading().status(fixture.spot).get();
        check(status.halted && status.exchangeHalted && !status.symbolHalted, "symbol resume cannot override exchange halt");
        fixture.exchange.trading().resume(7).get();
        fixture.exchange.trading().halt(fixture.spot, "local", 8).get();
        fixture.exchange.trading().halt("global", 9).get();
        fixture.exchange.trading().resume(10).get();
        check(fixture.exchange.trading().status(fixture.spot).get().halted, "exchange resume preserves local halt");
        auto otherBase = fixture.exchange.assets().create("OTHER", "OTHER", 0).get();
        auto other = fixture.exchange.symbols().create("OTHER", sdk::SymbolType::Spot, otherBase, fixture.quote).get();
        auto otherBook = fixture.exchange.books().addSpot(other).get();
        auto unaffected = otherBook.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 90, 1, 11).get();
        check(unaffected.snapshot().get().resting, "other symbol remains tradable");
        rejected([&] { fixture.exchange.trading().resume(fixture.spot, 7).get(); }, sdk::TradingControlState::InvalidTimestamp);
        rejected([&] { fixture.exchange.trading().halt(std::string(257, 'x'), 11).get(); }, sdk::TradingControlState::InvalidReason);
        const auto invalid = fixture.exchange.submit(advmatch::commands::SetTradingHalt{9999, true, 12, "invalid"}).get();
        check(!invalid.result.accepted && invalid.result.state == static_cast<std::uint8_t>(sdk::TradingControlState::InvalidSymbol), "unknown symbol cannot be halted");
        fixture.exchange.trading().resume(fixture.spot, 12).get();
        check(!fixture.exchange.trading().status(fixture.spot).get().halted, "local resume restores admission");
        fixture.audit();
    }

    void liquidationAndDelivery()
    {
        Fixture fixture;
        auto symbol = fixture.contract();
        auto position = fixture.open(symbol);
        fixture.exchange.trading().halt(symbol, "market outage", 3).get();
        fixture.exchange.contracts().updateMark(symbol, 80, 4).get();
        rejected([&] { fixture.exchange.contracts().processLiquidations(symbol, 4).get(); }, sdk::ContractState::TradingHalted);
        auto result = fixture.exchange.submit(advmatch::commands::Liquidate{symbol.id(), position.id(), 4}).get();
        check(result.result.state == static_cast<std::uint8_t>(sdk::ContractState::TradingHalted), "explicit liquidation cannot bypass halt");
        check(position.snapshot().get().size == 10, "mark updates do not fill during halt");
        fixture.exchange.contracts().fundInsurance(symbol, fixture.seller.contractAccount().get(), 1000).get();
        fixture.exchange.trading().resume(symbol, 5).get();
        symbol.book().place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 6, sdk::ContractOrderOptions{}).get();
        check(position.snapshot().get().closed, "queued liquidation continues when liquidity arrives after resume");
        fixture.audit();

        Fixture future;
        symbol = future.contract(sdk::SymbolType::Future);
        position = future.open(symbol);
        future.exchange.trading().halt(symbol, "delivery preparation", 3).get();
        future.exchange.contracts().deliver(symbol, 110, 50).get();
        check(position.snapshot().get().closed, "cash delivery remains available during halt");
        future.exchange.trading().resume(symbol, 51).get();
        rejected([&] { symbol.book().place(future.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 1, 52, sdk::ContractOrderOptions{}).get(); }, sdk::PlaceOrderState::ContractExpired);
        future.audit();
    }

    void crossMarketLiquidation()
    {
        Fixture fixture;
        const auto first = fixture.contract();
        const auto second = fixture.exchange.symbols().create("OTHER_CONTRACT", sdk::SymbolType::Future, fixture.base, fixture.quote).get();
        sdk::ContractSpec spec;
        spec.expiryTimestamp = 1000;
        fixture.exchange.books().addContract(second, spec, 100, 0).get();
        sdk::ContractOrderOptions options;
        options.marginMode = sdk::MarginMode::Cross;
        std::vector<sdk::Position> positions;
        for (const auto& symbol : {first, second})
        {
            symbol.book().place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1, options).get();
            const auto order = symbol.book().place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 10, 2, options).get();
            positions.push_back(*order.position().get());
        }

        const auto account = fixture.buyer.contractAccount().get();
        account.updateBalance(fixture.quote, 300 - account.balance(fixture.quote).get().active).get();
        fixture.exchange.contracts().setAutomaticLiquidationBudget(0).get();
        fixture.exchange.trading().halt(second, "paused collateral market", 3).get();
        const auto mark = fixture.exchange.submit(advmatch::commands::UpdateMarkPrice{first.id(), 71, 4}).get();
        check(mark.result.state == static_cast<std::uint8_t>(sdk::ContractState::PendingLiquidation), "cross collateral below maintenance");
        first.book().place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 71, 10, 5, sdk::ContractOrderOptions{}).get();
        const auto result = fixture.exchange.submit(advmatch::commands::ProcessLiquidations{first.id(), 6, true}).get();
        check(result.result.state == static_cast<std::uint8_t>(sdk::ContractState::PendingLiquidation), "cross liquidation defers halted symbol");
        check(positions[0].snapshot().get().closed, "unhalted position closes");
        const auto deferred = positions[1].snapshot().get();
        check(deferred.size == 10 && deferred.liquidating && deferred.liquidationQueued, "halted position stays queued even if remaining equity recovers");
        fixture.audit();
        fixture.exchange.trading().resume(second, 7).get();
        second.book().place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 8, sdk::ContractOrderOptions{}).get();
        fixture.exchange.contracts().processLiquidations(second, 9).get();
        check(positions[1].snapshot().get().closed, "deferred cross liquidation completes after resume");
        fixture.audit();
    }

    void conditionalRecovery()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-halt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::SymbolId symbolId = 0;
        sdk::PositionId positionId = 0;
        sdk::ConditionalOrderId stopId = 0;
        std::uint64_t hash = 0;
        {
            Fixture fixture(config);
            auto symbol = fixture.contract();
            symbolId = symbol.id();
            auto position = fixture.open(symbol);
            positionId = position.id();
            position.addMargin(1000).get();
            sdk::ConditionalOrderRequest request;
            request.userId = fixture.buyer.id();
            request.symbolId = symbol.id();
            request.source = sdk::TriggerPriceSource::Mark;
            request.quantity = 10;
            request.triggerPrice = 95;
            request.timestamp = 3;
            sdk::ContractOrderOptions close;
            close.positionId = position.id();
            close.reduceOnly = true;
            close.priceLimit = 50;
            request.contractOptions = close;
            stopId = fixture.exchange.conditionalOrders().place(request).get().id();
            symbol.book().place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 90, 10, 4, sdk::ContractOrderOptions{}).get();
            fixture.exchange.trading().halt(symbol, "price interruption", 5, 66).get();
            fixture.exchange.trading().halt("operator maintenance", 7).get();
            fixture.exchange.contracts().updateMark(symbol, 90, 8).get();
            fixture.exchange.contracts().settleFunding(symbol, 1000, 100).get();
            check(fixture.exchange.conditionalOrders().get(stopId).snapshot().get().status == sdk::ConditionalOrderStatus::Waiting, "halted trigger stays waiting while prices and funding update");
            fixture.exchange.persistence().checkpoint().get();
            fixture.audit();
            hash = fixture.exchange.audit().report().get().stateHash;
        }

        {
            sdk::MatchingExchange exchange(config);
            auto symbol = exchange.symbols().get(symbolId);
            auto status = exchange.trading().status(symbol).get();
            check(status.exchangeHalted && status.symbolHalted && status.symbolReason == "price interruption", "both halt scopes survive checkpoint replay");
            check(exchange.audit().report().get().stateHash == hash, "halt metadata participates in recovery fingerprint");
            exchange.trading().resume(symbol, 101).get();
            check(exchange.conditionalOrders().get(stopId).snapshot().get().status == sdk::ConditionalOrderStatus::Waiting, "global halt prevents local trigger reactivation");
            exchange.trading().resume(102).get();
            check(exchange.conditionalOrders().get(stopId).snapshot().get().status == sdk::ConditionalOrderStatus::Activated, "resume evaluates latest mark with current command timestamp");
            check(exchange.positions().get(positionId).snapshot().get().closed, "resumed protective order settles once");
            exchange.trading().halt(symbol, "price interruption", 5, 66).get();
            check(!exchange.trading().status(symbol).get().halted, "duplicate halt does not halt again after recovery");
            auto report = exchange.audit().report().get();
            check(report.conserved && report.consistent, "funding and resumed fills conserve assets");
            hash = report.stateHash;
        }

        {
            sdk::MatchingExchange exchange(config);
            check(exchange.audit().report().get().stateHash == hash && !exchange.trading().status(exchange.symbols().get(symbolId)).get().halted, "resume and child execution survive WAL replay");
        }

        std::filesystem::remove_all(directory);
    }
}

int main()
{
    try
    {
        admissionAndScopes();
        liquidationAndDelivery();
        crossMarketLiquidation();
        conditionalRecovery();
        std::cout << "Trading halt: admission, independent scopes, cancellations, triggers, liquidation, funding, delivery and recovery passed\n";
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
