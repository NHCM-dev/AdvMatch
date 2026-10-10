#include <advmatch/sdk/AdvMatch.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>

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

    sdk::AuditReport audit(sdk::MatchingExchange& exchange)
    {
        auto report = exchange.audit().report().get();
        for (const auto& issue : report.issues)
        {
            std::cerr << "audit entity=" << issue.entityId << " asset=" << issue.assetId << " " << issue.message << '\n';
        }

        check(report.conserved && report.consistent && report.issueCount == 0, "asset conservation and structural consistency");
        return report;
    }

    template <typename Function, typename State>
    void rejected(Function action, State expected)
    {
        try
        {
            action();
        }
        catch (const sdk::BusinessError& error)
        {
            check(error.receipt().result.state == static_cast<std::uint8_t>(expected), "unexpected business rejection");
            return;
        }

        throw std::runtime_error("expected rejection");
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
                user.spotAccount().get().updateBalance(this->base, 100'000).get();
                user.spotAccount().get().updateBalance(this->quote, 1'000'000).get();
                user.contractAccount().get().updateBalance(this->quote, 1'000'000).get();
            }
        }

        sdk::Order place(const sdk::User& user, sdk::OrderSide side, sdk::Price price, sdk::Quantity quantity, sdk::Timestamp timestamp, sdk::TimeInForce tif = sdk::TimeInForce::GTC)
        {
            return this->book.place(user, side, sdk::OrderType::Limit, tif, price, quantity, timestamp).get();
        }
    };

    void fees()
    {
        Fixture fixture;
        sdk::TradingRules rules;
        rules.fees = {10'000, 20'000};
        fixture.exchange.rules().configure(fixture.spot, rules).get();
        auto maker = fixture.place(fixture.seller, sdk::OrderSide::Ask, 100, 100, 1);
        auto taker = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 100, 2, sdk::TimeInForce::FOK);
        check(fixture.buyer.spotAccount().get().balance(fixture.base).get().active == 100'098, "buyer receives base net of taker fee");
        check(fixture.seller.spotAccount().get().balance(fixture.quote).get().active == 1'009'900, "seller receives quote net of maker fee");
        check(maker.snapshot().get().paidFees == 100 && taker.snapshot().get().paidFees == 2, "order fee observations");
        auto trade = fixture.exchange.orders().trades().get().values.back();
        check(trade.makerFeeAssetId == fixture.quote.id() && trade.makerFee == 100 && trade.takerFeeAssetId == fixture.base.id() && trade.takerFee == 2, "trade fee assets and amounts");
        check(fixture.exchange.rules().spotFees(fixture.quote).get().balance == 100, "quote treasury");
        fixture.exchange.rules().withdrawSpotFees(fixture.buyer.spotAccount().get(), fixture.quote, 60).get();
        check(fixture.exchange.rules().spotFees(fixture.quote).get().balance == 40, "fee withdrawal debits treasury");
        rejected([&] { fixture.exchange.rules().withdrawSpotFees(fixture.buyer.spotAccount().get(), fixture.quote, 41).get(); }, sdk::TradingRuleState::InsufficientFees);
        audit(fixture.exchange);

        fixture.place(fixture.buyer, sdk::OrderSide::Ask, 100, 100, 3);
        fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 100, 4, sdk::TimeInForce::FOK);
        audit(fixture.exchange);
        check(fixture.exchange.rules().spotFees(fixture.base).get().balance == 4 && fixture.exchange.rules().spotFees(fixture.quote).get().balance == 140, "allowed self trade charges fees once");
    }

    void prevention()
    {
        for (auto mode : {sdk::SelfTradePrevention::Allow, sdk::SelfTradePrevention::CancelTaker, sdk::SelfTradePrevention::CancelMaker, sdk::SelfTradePrevention::CancelBoth})
        {
            Fixture fixture;
            sdk::TradingRules rules;
            rules.selfTradePrevention = mode;
            fixture.exchange.rules().configure(fixture.spot, rules).get();
            const auto maker = fixture.place(fixture.buyer, sdk::OrderSide::Ask, 100, 10, 1);
            const auto taker = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 2, sdk::TimeInForce::IOC);
            check(taker.snapshot().get().filling.filled == (mode == sdk::SelfTradePrevention::Allow ? 10 : 0), "STP fill rule");
            check(maker.snapshot().get().resting == (mode == sdk::SelfTradePrevention::CancelTaker), "STP maker cancellation");
            audit(fixture.exchange);
        }

        Fixture fixture;
        sdk::TradingRules rules;
        rules.selfTradePrevention = sdk::SelfTradePrevention::CancelMaker;
        fixture.exchange.rules().configure(fixture.spot, rules).get();
        auto own = fixture.place(fixture.buyer, sdk::OrderSide::Ask, 100, 10, 1);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 2, sdk::TimeInForce::FOK); }, sdk::PlaceOrderState::InsufficientLiquidity);
        check(own.snapshot().get().resting, "failed FOK does not cancel own maker");
        fixture.place(fixture.seller, sdk::OrderSide::Ask, 100, 10, 3);
        auto result = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 4, sdk::TimeInForce::FOK);
        check(result.snapshot().get().filling.filled == 10 && !own.snapshot().get().resting, "FOK skips own maker and fills external liquidity");
        audit(fixture.exchange);
    }

    void precisionAndLimits()
    {
        Fixture fixture;
        sdk::TradingRules rules;
        rules.priceTick = 5;
        rules.quantityStep = 10;
        rules.minQuantity = 10;
        rules.minOrderNotional = 1000;
        rules.risk.maxOrderQuantity = 100;
        rules.risk.maxOrderNotional = 10'000;
        rules.risk.maxAccountNotional = 5000;
        rules.risk.maxActiveOrders = 2;
        fixture.exchange.rules().configure(fixture.spot, rules).get();
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 101, 10, 1); }, sdk::PlaceOrderState::InvalidPriceTick);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 11, 1); }, sdk::PlaceOrderState::InvalidQuantityStep);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 95, 10, 1); }, sdk::PlaceOrderState::MinimumNotional);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 110, 1); }, sdk::PlaceOrderState::RiskLimitExceeded);
        auto first = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 20, 2);
        auto second = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 20, 3);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 4); }, sdk::PlaceOrderState::RiskLimitExceeded);
        rejected([&] { first.modifyQuantity(20, 4).get(); }, sdk::ModifyOrderState::RiskLimitExceeded);
        rejected([&] { first.modifyQuantity(1, 4).get(); }, sdk::ModifyOrderState::InvalidQuantityStep);
        rejected([&] { first.modifyPrice(101, 4).get(); }, sdk::ModifyOrderState::InvalidPriceTick);
        check(first.snapshot().get().quantity == 20, "invalid amend leaves quantity unchanged");
        rejected([&] { fixture.exchange.rules().configure(fixture.spot, rules).get(); }, sdk::TradingRuleState::OpenExposure);
        auto risk = fixture.exchange.rules().userRisk(fixture.spot, fixture.buyer).get();
        check(risk.activeOrders == 2 && risk.pendingNotional == 4000, "O(1) cached risk view");
        first.cancel(5).get();
        second.modifyQuantity(-10, 6).get();
        risk = fixture.exchange.rules().userRisk(fixture.spot, fixture.buyer).get();
        check(risk.activeOrders == 1 && risk.pendingNotional == 1000, "cancel and quantity decrease release risk");
        fixture.place(fixture.seller, sdk::OrderSide::Ask, 100, 10, 7, sdk::TimeInForce::IOC);
        check(fixture.exchange.rules().userRisk(fixture.spot, fixture.buyer).get().activeOrders == 0, "fill releases risk");
        auto ask = fixture.place(fixture.seller, sdk::OrderSide::Ask, 110, 10, 8, sdk::TimeInForce::PostOnly);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 110, 10, 9, sdk::TimeInForce::PostOnly); }, sdk::PlaceOrderState::PostOnlyWouldTrade);
        auto bid = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 10, sdk::TimeInForce::PostOnly);
        rejected([&] { bid.modifyPrice(110, 11).get(); }, sdk::ModifyOrderState::PostOnlyWouldTrade);
        check(bid.snapshot().get().resting && bid.snapshot().get().price == 100 && ask.snapshot().get().resting, "post-only amend is atomic");
        audit(fixture.exchange);

        sdk::ConditionalOrderRequest request;
        request.userId = fixture.buyer.id();
        request.symbolId = fixture.spot.id();
        request.side = sdk::OrderSide::Bid;
        request.triggerPrice = 101;
        request.quantity = 10;
        request.timestamp = 12;
        check(!fixture.exchange.submit(advmatch::commands::PlaceConditionalOrder{request}).get().result.accepted, "conditional trigger obeys tick");
    }

    void overflow()
    {
        Fixture fixture;
        const auto balance = fixture.buyer.spotAccount().get().balance(fixture.base).get();
        fixture.buyer.spotAccount().get().updateBalance(fixture.base, std::numeric_limits<sdk::Balance>::max() - balance.active).get();
        auto maker = fixture.place(fixture.seller, sdk::OrderSide::Ask, 100, 10, 1);
        rejected([&] { fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 2, sdk::TimeInForce::FOK); }, sdk::PlaceOrderState::InsufficientLiquidity);
        check(maker.snapshot().get().resting && maker.snapshot().get().filling.filled == 0, "credit overflow has no FOK prefix fills");
        auto taker = fixture.place(fixture.buyer, sdk::OrderSide::Bid, 100, 10, 3, sdk::TimeInForce::IOC);
        check(taker.snapshot().get().filling.filled == 0 && maker.snapshot().get().resting, "credit overflow cancels taker before settlement");
        auto report = audit(fixture.exchange);
        check(report.assets.front().externalFlow == "9223372036854875807", "audit totals use wide arithmetic");
    }

    void contracts(sdk::MarginMode mode)
    {
        Fixture fixture;
        auto symbol = fixture.exchange.symbols().create("PERP", sdk::SymbolType::Perpetual, fixture.base, fixture.quote).get();
        sdk::ContractSpec spec;
        spec.makerFeeRate = 1000;
        spec.takerFeeRate = 2000;
        spec.firstFundingTimestamp = 100;
        spec.fundingInterval = 100;
        fixture.exchange.books().addContract(symbol, spec, 100, 0).get();
        sdk::TradingRules rules;
        rules.fees = {1000, 2000};
        rules.risk.maxPositionNotional = 1500;
        rules.risk.maxAccountNotional = 2500;
        rules.risk.maxLeverage = 20;
        rules.risk.maxActiveOrders = 3;
        rules.selfTradePrevention = sdk::SelfTradePrevention::CancelTaker;
        fixture.exchange.rules().configure(symbol, rules).get();
        auto book = symbol.book();
        sdk::ContractOrderOptions options;
        options.marginMode = mode;
        options.leverage = 10;
        auto maker = book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1, options).get();
        auto taker = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::FOK, 100, 10, 2, options).get();
        auto position = *taker.position().get();
        check(fixture.exchange.rules().userRisk(symbol, fixture.buyer).get().positionNotional == 1000, "gross contract position risk");
        check(taker.snapshot().get().paidFees == 2 && maker.snapshot().get().paidFees == 1, "contract fee observations");
        auto high = options;
        high.leverage = 21;
        rejected([&] { book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 90, 1, 3, high).get(); }, sdk::PlaceOrderState::RiskLimitExceeded);
        rejected([&] { position.setLeverage(21, 3).get(); }, sdk::ContractState::RiskLimitExceeded);
        high = options;
        high.positionId = position.id();
        rejected([&] { book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 5, 3, high).get(); }, sdk::PlaceOrderState::RiskLimitExceeded);
        rejected([&] { book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 14, 3, options).get(); }, sdk::PlaceOrderState::RiskLimitExceeded);
        audit(fixture.exchange);
        fixture.exchange.contracts().updateMark(symbol, 101, 4).get();
        fixture.exchange.contracts().settleFunding(symbol, 1000, 100).get();
        audit(fixture.exchange);
        auto close = options;
        close.positionId = position.id();
        book.place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 101, options).get();
        book.place(fixture.buyer, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::FOK, 100, 10, 102, close).get();
        check(position.snapshot().get().closed && fixture.exchange.rules().userRisk(symbol, fixture.buyer).get().positionNotional == 0, "reduce-only close releases gross exposure");
        audit(fixture.exchange);
    }

    void contractProtection()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-protection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::SymbolId symbolId = 0;
        {
            Fixture fixture(config);
            auto symbol = fixture.exchange.symbols().create("PROTECTION", sdk::SymbolType::Perpetual, fixture.base, fixture.quote).get();
            symbolId = symbol.id();
            sdk::ContractSpec spec;
            spec.firstFundingTimestamp = 100;
            auto book = fixture.exchange.books().addContract(symbol, spec, 100, 0).get();
            auto maker = book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 150, 10, 1, sdk::ContractOrderOptions{}).get();
            sdk::ContractOrderOptions marketOptions;
            marketOptions.priceLimit = 200;
            marketOptions.leverage = 1;
            auto blocked = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Market, sdk::TimeInForce::IOC, 0, 10, 2, marketOptions).get();
            check(blocked.snapshot().get().filling.filled == 0, "enabled band blocks far-price execution");
            if (maker.snapshot().get().resting)
                maker.cancel(3).get();

            fixture.exchange.contracts().configureProtection(symbol, 0).get();
            check(fixture.exchange.contracts().market(symbol).get().spec.priceBandRate == 0, "zero disables protection");
            maker = book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 150, 10, 4, sdk::ContractOrderOptions{}).get();
            marketOptions.priceLimit = 140;
            auto capped = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Market, sdk::TimeInForce::IOC, 0, 10, 5, marketOptions).get();
            check(capped.snapshot().get().filling.filled == 0 && maker.snapshot().get().resting, "explicit market cap survives disabled exchange band");
            marketOptions.priceLimit = 200;
            auto filled = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Market, sdk::TimeInForce::IOC, 0, 10, 6, marketOptions).get();
            check(filled.snapshot().get().filling.filled == 10, "disabled band allows funded far-price trade");
            rejected([&] { fixture.exchange.contracts().configureProtection(symbol, 1'000'000).get(); }, sdk::ContractState::InvalidConfiguration);
            check(fixture.exchange.contracts().market(symbol).get().spec.priceBandRate == 0, "invalid rate preserves setting");
            auto empty = fixture.exchange.users().create().get();
            rejected([&] { book.place(empty, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 150, 1, 7, sdk::ContractOrderOptions{}).get(); }, sdk::PlaceOrderState::InsufficientBalance);
            fixture.exchange.persistence().checkpoint().get();
            fixture.exchange.contracts().configureProtection(symbol, 250000).get();
            fixture.exchange.contracts().configureProtection(symbol, 0).get();
            audit(fixture.exchange);
        }
        {
            sdk::MatchingExchange exchange(config);
            auto symbol = exchange.symbols().get(symbolId);
            check(exchange.contracts().market(symbol).get().spec.priceBandRate == 0, "protection survives checkpoint and WAL recovery");
            exchange.contracts().configureProtection(symbol, 250000).get();
            check(exchange.contracts().market(symbol).get().spec.priceBandRate == 250000, "protection can be re-enabled at a new rate");
            audit(exchange);
        }
        std::filesystem::remove_all(directory);
    }

    void contractLifecycleRecovery()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-risk-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::PositionId victimId = 0;
        std::uint64_t hash = 0;
        {
            Fixture fixture(config);
            sdk::TradingRules rules;
            rules.fees = {1000, 2000};
            rules.risk.maxAccountNotional = 10'000;
            rules.risk.maxPositionNotional = 5000;
            rules.risk.maxActiveOrders = 10;
            rules.selfTradePrevention = sdk::SelfTradePrevention::CancelMaker;
            for (auto type : {sdk::SymbolType::Future, sdk::SymbolType::Perpetual})
            {
                auto symbol = fixture.exchange.symbols().create(type == sdk::SymbolType::Future ? "FUTURE" : "PERP", type, fixture.base, fixture.quote).get();
                sdk::ContractSpec spec;
                spec.makerFeeRate = 1000;
                spec.takerFeeRate = 2000;
                spec.expiryTimestamp = type == sdk::SymbolType::Future ? 50 : 0;
                spec.firstFundingTimestamp = type == sdk::SymbolType::Perpetual ? 100 : 0;
                auto book = fixture.exchange.books().addContract(symbol, spec, 100, 0).get();
                fixture.exchange.rules().configure(symbol, rules).get();
                book.place(fixture.seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1, sdk::ContractOrderOptions{}).get();
                auto taker = book.place(fixture.buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::FOK, 100, 10, 2, sdk::ContractOrderOptions{}).get();
                if (type == sdk::SymbolType::Future)
                {
                    fixture.exchange.contracts().deliver(symbol, 110, 50).get();
                    check(taker.position().get()->snapshot().get().closed, "future delivery closes positions");
                    check(fixture.exchange.rules().userRisk(symbol, fixture.buyer).get().positionNotional == 0, "delivery releases cached exposure");
                }
                else
                {
                    victimId = taker.position().get()->id();
                    fixture.exchange.contracts().setAutomaticLiquidationBudget(0).get();
                    fixture.exchange.contracts().fundInsurance(symbol, fixture.seller.contractAccount().get(), 1000).get();
                    auto result = fixture.exchange.submit(advmatch::commands::UpdateMarkPrice{symbol.id(), 80, 3}).get();
                    check(result.result.state == static_cast<std::uint8_t>(sdk::ContractState::PendingLiquidation), "mark queues underwater position");
                    book.place(fixture.seller, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 80, 10, 4, sdk::ContractOrderOptions{}).get();
                    fixture.exchange.contracts().processLiquidations(symbol, 5).get();
                    check(taker.position().get()->snapshot().get().closed, "liquidation consumes insurance and closes position");
                    check(fixture.exchange.rules().userRisk(symbol, fixture.buyer).get().positionNotional == 0, "liquidation releases exposure");
                }

                audit(fixture.exchange);
            }

            fixture.exchange.persistence().checkpoint().get();
            hash = audit(fixture.exchange).stateHash;
        }

        {
            sdk::MatchingExchange exchange(config);
            check(audit(exchange).stateHash == hash && exchange.positions().get(victimId).snapshot().get().closed, "fee/risk/insurance/delivery state survives recovery");
        }

        std::filesystem::remove_all(directory);
    }

    void recovery()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-rules-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        std::uint64_t hash = 0;
        sdk::SymbolId symbolId = 0;
        sdk::UserId userId = 0;
        sdk::AssetId quoteId = 0;
        {
            Fixture fixture(config);
            symbolId = fixture.spot.id();
            userId = fixture.buyer.id();
            quoteId = fixture.quote.id();
            sdk::TradingRules rules;
            rules.fees = {10'000, 20'000};
            rules.risk.maxActiveOrders = 1000;
            rules.risk.maxAccountNotional = 1'000'000;
            rules.selfTradePrevention = sdk::SelfTradePrevention::CancelMaker;
            fixture.exchange.rules().configure(fixture.spot, rules).get();
            std::mt19937 random(719);
            std::vector<sdk::Order> orders;
            for (sdk::Timestamp timestamp = 1; timestamp <= 200; ++timestamp)
            {
                if (timestamp == 100)
                {
                    fixture.exchange.persistence().checkpoint().get();
                }

                if (!orders.empty() && random() % 4 == 0)
                {
                    auto order = orders[random() % orders.size()];
                    if (order.snapshot().get().resting)
                    {
                        if (random() % 2 == 0)
                        {
                            order.cancel(timestamp).get();
                        }
                        else
                        {
                            order.modifyPrice(95 + random() % 11, timestamp).get();
                        }
                    }
                }
                else
                {
                    const auto user = random() % 2 == 0 ? fixture.buyer : fixture.seller;
                    const auto side = random() % 2 == 0 ? sdk::OrderSide::Bid : sdk::OrderSide::Ask;
                    orders.push_back(fixture.place(user, side, 95 + random() % 11, 100, timestamp));
                }

                audit(fixture.exchange);
            }

            fixture.exchange.rules().withdrawSpotFees(fixture.buyer.spotAccount().get(), fixture.quote, 1, 987).get();
            hash = audit(fixture.exchange).stateHash;
        }

        {
            sdk::MatchingExchange exchange(config);
            check(audit(exchange).stateHash == hash, "checkpoint plus WAL recovery reproduces economic state");
            auto symbol = exchange.symbols().get(symbolId);
            check(symbol.snapshot().get().tradingRules.selfTradePrevention == sdk::SelfTradePrevention::CancelMaker, "rules recovered");
            const auto fees = exchange.rules().spotFees(exchange.assets().get(quoteId)).get().balance;
            exchange.rules().withdrawSpotFees(exchange.users().get(userId).spotAccount().get(), exchange.assets().get(quoteId), 1, 987).get();
            check(exchange.rules().spotFees(exchange.assets().get(quoteId)).get().balance == fees && audit(exchange).stateHash == hash, "deduplication does not charge or transfer twice after recovery");
        }

        std::filesystem::remove_all(directory);
    }
}

int main()
{
    try
    {
        fees();
        prevention();
        precisionAndLimits();
        overflow();
        contracts(sdk::MarginMode::Isolated);
        contracts(sdk::MarginMode::Cross);
        contractProtection();
        contractLifecycleRecovery();
        recovery();
        std::cout << "Trading rules: fees, quotas, STP, precision, post-only, FOK, conservation and recovery passed\n";
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
