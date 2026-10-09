#include <advmatch/sdk/AdvMatch.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>

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

    template <typename Function>
    void rejected(Function action, sdk::ContractState expected)
    {
        try
        {
            action();
        }
        catch (const sdk::BusinessError& error)
        {
            check(error.receipt().result.state == static_cast<std::uint8_t>(expected), "unexpected contract rejection");
            return;
        }

        throw std::runtime_error("expected contract rejection");
    }

    sdk::ExchangeConfig memory()
    {
        sdk::ExchangeConfig config;
        config.persistence.enabled = false;
        return config;
    }

    struct Fixture
    {
        sdk::MatchingExchange exchange{memory()};
        sdk::Asset base = this->exchange.assets().create("BTC", "BTC", 0).get();
        sdk::Asset quote = this->exchange.assets().create("USD", "USD", 0).get();
        sdk::User buyer = this->exchange.users().create().get();
        sdk::User seller = this->exchange.users().create().get();
        sdk::Symbol spot = this->exchange.symbols().create("BTCUSD", sdk::SymbolType::Spot, this->base, this->quote).get();
        sdk::OrderBook spotBook = this->exchange.books().addSpot(this->spot).get();
        std::optional<sdk::Order> spotBid;
        std::optional<sdk::Order> spotAsk;

        Fixture()
        {
            this->buyer.spotAccount().get().updateBalance(this->quote, 100'000'000).get();
            this->seller.spotAccount().get().updateBalance(this->base, 100).get();
            this->buyer.contractAccount().get().updateBalance(this->quote, 100'000'000).get();
            this->seller.contractAccount().get().updateBalance(this->quote, 100'000'000).get();
            this->spotBid = this->spotBook.place(this->buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 99'900, 10, 1).get();
            this->spotAsk = this->spotBook.place(this->seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100'100, 10, 1).get();
        }

        sdk::Symbol contract(sdk::SymbolType type)
        {
            const auto symbol = this->exchange.symbols().create("CONTRACT", type, this->base, this->quote).get();
            sdk::ContractSpec spec;
            spec.takerFeeRate = 0;
            spec.liquidationFeeRate = 0;
            spec.fundingInterval = 100;
            spec.firstFundingTimestamp = type == sdk::SymbolType::Perpetual ? 100 : 0;
            spec.expiryTimestamp = type == sdk::SymbolType::Future ? 500 : 0;
            this->exchange.books().addContract(symbol, spec, 100'000, 0).get();
            return symbol;
        }

        std::pair<sdk::Position, sdk::Position> open(const sdk::Symbol& symbol)
        {
            const auto book = symbol.book();
            const auto ask = book.place(this->seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100'000, 10, 2, sdk::ContractOrderOptions{}).get();
            const auto bid = book.place(this->buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100'000, 10, 3, sdk::ContractOrderOptions{}).get();
            return {*bid.position().get(), *ask.position().get()};
        }
    };

    class CustomAlgorithm final : public sdk::DefaultReferencePriceAlgorithm
    {
    public:
        sdk::Price mark(sdk::Price index, sdk::Price, const sdk::ReferencePriceConfig&) const override
        {
            return index;
        }

        std::int32_t funding(std::int32_t, const sdk::ReferencePriceConfig&, const sdk::ContractSpec&) const override
        {
            return -200;
        }

        sdk::Price delivery(sdk::Price index, sdk::Price) const override
        {
            return index + 100;
        }
    };

    void algorithms()
    {
        sdk::DefaultReferencePriceAlgorithm algorithm;
        sdk::ReferencePriceConfig config;
        config.sources = {{sdk::PriceSourceType::External, 1}, {sdk::PriceSourceType::External, 2}, {sdk::PriceSourceType::External, 3}};
        config.minimumSources = 2;
        sdk::ContractSpec spec;
        const std::array<sdk::WeightedPrice, 3> prices = {{{100'000, 1, 1}, {100'200, 1, 3}, {200'000, 1, 1}}};
        const auto value = algorithm.calculate(prices, 104'000, config, spec, 10);
        check(value && value->indexPrice == 100'150 && value->sourceCount == 2, "weighted index rejects outlier");
        check(value->markPrice == 102'153 && value->fundingRate == 10'000, "bounded mark and funding");
        check(algorithm.funding(-2000, config, spec) == -1500, "negative premium funding");
        check(!algorithm.calculate(prices, 0, config, spec, 30'002), "stale data rejected");
        check(!algorithm.calculate(prices, 0, config, spec, 0), "future data rejected");
        config.sources[1] = config.sources[0];
        check(!sdk::ReferencePriceAlgorithm::valid(config), "duplicate source rejected");
        config.sources[1].sourceId = 2;
        config.sources[1].denominator = 0;
        check(!sdk::ReferencePriceAlgorithm::valid(config), "invalid conversion rejected");
        config = {};
        const std::array<sdk::WeightedPrice, 1> maximum = {{{static_cast<sdk::Price>(std::numeric_limits<sdk::Balance>::max()), 1, std::numeric_limits<std::uint32_t>::max()}}};
        check(algorithm.calculate(maximum, 0, config, spec, 1)->indexPrice == maximum[0].price, "wide arithmetic preserves maximum price");
    }

    void perpetual()
    {
        Fixture fixture;
        const auto symbol = fixture.contract(sdk::SymbolType::Perpetual);
        const auto [longPosition, shortPosition] = fixture.open(symbol);
        const auto manager = fixture.exchange.contracts();
        symbol.book().place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 103'000, 1, 4, sdk::ContractOrderOptions{}).get();
        symbol.book().place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 105'000, 1, 4, sdk::ContractOrderOptions{}).get();
        manager.refresh(symbol, 10).get();
        const auto preview = manager.market(symbol).get();
        check(preview.referencePrices.indexPrice == 100'000 && preview.markPrice == 102'000, "default spot binding");
        check(preview.referencePrices.premiumRate == 40'000 && preview.referencePrices.fundingRate == 10'000, "premium preview");
        manager.refresh(symbol, 100).get();
        check(longPosition.snapshot().get().fundingPnL == -10'200 && shortPosition.snapshot().get().fundingPnL == 10'200, "funding moves money at computed mark");
        check(manager.market(symbol).get().nextFundingTimestamp == 200, "funding advances deadline");
        manager.refresh(symbol, 100).get();
        check(longPosition.snapshot().get().fundingPnL == -10'200, "same deadline does not charge twice");

        sdk::ContractSchedule schedule;
        schedule.fundingInterval = 200;
        schedule.nextFundingTimestamp = 450;
        schedule.maxFundingRate = 2000;
        manager.configureSchedule(symbol, schedule, 101).get();
        check(manager.market(symbol).get().spec.fundingInterval == 200, "adjust funding interval");
        schedule.nextFundingTimestamp = 50;
        rejected([&] { manager.configureSchedule(symbol, schedule, 101).get(); }, sdk::ContractState::InvalidConfiguration);
        check(manager.market(symbol).get().nextFundingTimestamp == 450, "rejected schedule changes nothing");

        sdk::ReferencePriceConfig config;
        config.sources = {{sdk::PriceSourceType::Spot, fixture.spot.id()}, {sdk::PriceSourceType::External, 700, 3}, {sdk::PriceSourceType::External, 701}};
        config.minimumSources = 2;
        manager.configureReference(symbol, config).get();
        manager.publishPrice(symbol, 700, 100'200, 109).get();
        manager.publishPrice(symbol, 701, 200'000, 109).get();
        manager.refresh(symbol, 110).get();
        check(manager.market(symbol).get().referencePrices.indexPrice == 100'150, "mixed source weighted index");
        manager.publishPrice(symbol, 700, 100'200, 109).get();
        auto delayedConfig = config;
        delayedConfig.sources.push_back({sdk::PriceSourceType::External, 702});
        manager.configureReference(symbol, delayedConfig).get();
        manager.publishPrice(symbol, 702, 100'200, 105).get();
        check(manager.market(symbol).get().externalPrices.back().timestamp == 105, "delayed fresh feed accepted after reference publication");
        manager.configureReference(symbol, config).get();
        rejected([&] { manager.publishPrice(symbol, 700, 100'200, 108).get(); }, sdk::ContractState::InvalidTimestamp);
        rejected([&] { manager.publishPrice(symbol, 700, 100'300, 109).get(); }, sdk::ContractState::InvalidTimestamp);
        rejected([&] { manager.publishPrice(symbol, 999, 100'000, 110).get(); }, sdk::ContractState::InvalidConfiguration);
        manager.publishPrice(symbol, 700, 100'200, 200).get();
        rejected([&] { manager.refresh(symbol, 130).get(); }, sdk::ContractState::PriceUnavailable);
        check(manager.market(symbol).get().referenceTimestamp == 110, "missing fresh quorum preserves prices");

        CustomAlgorithm custom;
        const std::array<sdk::WeightedPrice, 1> inputs = {{{100'000, 140, 1}}};
        sdk::ReferencePriceConfig customConfig;
        const auto calculated = custom.calculate(inputs, 0, customConfig, manager.market(symbol).get().spec, 140);
        check(calculated && calculated->deliveryPrice == 100'100, "custom virtual policy");
        manager.updateReference(symbol, *calculated, 140).get();
        check(manager.market(symbol).get().referencePrices.fundingRate == -200, "custom result submitted");
        manager.refresh(symbol, 450).get();
        check(manager.market(symbol).get().lastFundingRate == 2000 && manager.market(symbol).get().nextFundingTimestamp == 650, "configured limit and automatic funding");
        manager.refresh(symbol, 651).get();
        check(manager.market(symbol).get().lastFundingTimestamp == 650 && manager.market(symbol).get().nextFundingTimestamp == 850, "late scheduler settles the previous published rate");
        rejected([&] { manager.refresh(symbol, 40'000).get(); }, sdk::ContractState::PriceUnavailable);
        check(manager.market(symbol).get().referenceTimestamp == 651, "stale sources preserve latest prices");
        config.automaticFunding = false;
        manager.configureReference(symbol, config).get();
        rejected([&] { manager.refresh(symbol, 850).get(); }, sdk::ContractState::FundingRequired);
        const auto before = longPosition.snapshot().get().fundingPnL;
        manager.settleFunding(symbol, -2000, 850).get();
        check(longPosition.snapshot().get().fundingPnL > before, "manual negative rate receives funding");
        manager.refresh(symbol, 850).get();
        rejected([&] { manager.refresh(symbol, 849).get(); }, sdk::ContractState::InvalidTimestamp);
    }

    void delivery(bool automatic)
    {
        Fixture fixture;
        const auto symbol = fixture.contract(sdk::SymbolType::Future);
        const auto [longPosition, shortPosition] = fixture.open(symbol);
        const auto manager = fixture.exchange.contracts();
        sdk::ContractSchedule schedule;
        schedule.expiryTimestamp = 600;
        manager.configureSchedule(symbol, schedule, 100).get();
        sdk::ReferencePriceConfig config;
        config.automaticDelivery = automatic;
        manager.configureReference(symbol, config).get();
        fixture.spotAsk->modifyPrice(100'300, 598).get();
        fixture.spotBid->modifyPrice(100'100, 598).get();
        manager.refresh(symbol, 599).get();
        check(manager.market(symbol).get().status == sdk::ContractStatus::Trading, "new expiry retains trading");
        manager.refresh(symbol, 600).get();
        if (automatic)
        {
            check(manager.market(symbol).get().deliveryPrice == 100'200, "index drives delivery");
        }
        else
        {
            check(manager.market(symbol).get().status == sdk::ContractStatus::Expired && !longPosition.snapshot().get().closed, "manual delivery mode expires first");
            config.sources = {{sdk::PriceSourceType::External, 77}};
            manager.configureReference(symbol, config).get();
            manager.publishPrice(symbol, 77, 100'123, 599).get();
            manager.refresh(symbol, 600).get();
            check(manager.market(symbol).get().referencePrices.indexPrice == 100'123, "expired contract accepts replacement settlement feed");
            manager.deliver(symbol, 100'123, 601).get();
        }

        check(manager.market(symbol).get().status == sdk::ContractStatus::Settled && longPosition.snapshot().get().closed && shortPosition.snapshot().get().closed, "delivery closes positions");
        check(longPosition.snapshot().get().realizedPnL + shortPosition.snapshot().get().realizedPnL == 0, "delivery conserves pnl");
        rejected([&] { manager.refresh(symbol, 602).get(); }, sdk::ContractState::AlreadySettled);
    }

    void liquidation()
    {
        Fixture fixture;
        const auto symbol = fixture.contract(sdk::SymbolType::Perpetual);
        const auto victim = fixture.exchange.users().create().get();
        victim.contractAccount().get().updateBalance(fixture.quote, 11'000).get();
        symbol.book().place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100'000, 1, 2, sdk::ContractOrderOptions{}).get();
        const auto order = symbol.book().place(victim, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100'000, 1, 3, sdk::ContractOrderOptions{}).get();
        const auto position = *order.position().get();
        fixture.spotBid->modifyPrice(89'900, 9).get();
        fixture.spotAsk->modifyPrice(90'100, 9).get();
        rejected([&] { fixture.exchange.contracts().refresh(symbol, 10).get(); }, sdk::ContractState::PendingLiquidation);
        check(position.snapshot().get().liquidating && fixture.exchange.contracts().market(symbol).get().referencePrices.indexPrice == 90'000, "reference mark triggers liquidation");
        symbol.book().place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 90'000, 1, 11, sdk::ContractOrderOptions{}).get();
        check(position.snapshot().get().closed, "pending liquidation completes with liquidity");
    }

    void recovery()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-reference-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::SymbolId symbolId = 0;
        {
            sdk::MatchingExchange exchange(config);
            const auto base = exchange.assets().create("BTC", "BTC", 0).get();
            const auto quote = exchange.assets().create("USD", "USD", 0).get();
            const auto symbol = exchange.symbols().create("PERP", sdk::SymbolType::Perpetual, base, quote).get();
            symbolId = symbol.id();
            sdk::ContractSpec spec;
            spec.firstFundingTimestamp = 100;
            spec.fundingInterval = 100;
            exchange.books().addContract(symbol, spec, 100'000, 0).get();
            sdk::ReferencePriceConfig reference;
            reference.sources = {{sdk::PriceSourceType::External, 42}};
            exchange.contracts().configureReference(symbol, reference).get();
            exchange.contracts().publishPrice(symbol, 42, 100'000, 1).get();
            exchange.contracts().refresh(symbol, 100).get();
            sdk::ContractSchedule schedule;
            schedule.fundingInterval = 200;
            schedule.nextFundingTimestamp = 300;
            exchange.contracts().configureSchedule(symbol, schedule, 101).get();
            exchange.contracts().updateReference(symbol, sdk::ReferencePrices{100'100, 100'100, 0, 100, 100'100, 1}, 101).get();
            exchange.persistence().checkpoint().get();
        }

        {
            sdk::MatchingExchange exchange(config);
            const auto symbol = exchange.symbols().get(symbolId);
            const auto restored = exchange.contracts().market(symbol).get();
            check(restored.referencePrices.indexPrice == 100'100 && restored.referenceTimestamp == 101 && restored.externalPrices.front().sourceId == 42, "reference state recovered");
            check(restored.lastFundingTimestamp == 100 && restored.nextFundingTimestamp == 300 && restored.spec.fundingInterval == 200, "schedule recovered");
            exchange.contracts().publishPrice(symbol, 42, 102'000, 102).get();
            exchange.contracts().refresh(symbol, 102).get();
            check(exchange.contracts().market(symbol).get().referencePrices.indexPrice == 102'000, "restored source remains usable");
        }

        std::filesystem::remove_all(directory);
    }
}

int main()
{
    try
    {
        algorithms();
        perpetual();
        delivery(true);
        delivery(false);
        liquidation();
        recovery();
        std::cout << "Reference prices: algorithms, feeds, funding, schedules, delivery, custom policies and recovery passed\n";
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
