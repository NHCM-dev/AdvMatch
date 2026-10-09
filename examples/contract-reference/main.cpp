#include <advmatch/sdk/AdvMatch.h>
#include <iostream>

int main()
{
    namespace sdk = advmatch::sdk;
    sdk::ExchangeConfig config;
    config.persistence.enabled = false;
    sdk::MatchingExchange exchange(config);
    const auto btc = exchange.assets().create("Bitcoin", "BTC", 0).get();
    const auto usd = exchange.assets().create("Dollar", "USD", 0).get();
    const auto buyer = exchange.users().create().get();
    const auto seller = exchange.users().create().get();
    buyer.spotAccount().get().updateBalance(usd, 100'000'000).get();
    seller.spotAccount().get().updateBalance(btc, 100).get();
    buyer.contractAccount().get().updateBalance(usd, 100'000'000).get();
    seller.contractAccount().get().updateBalance(usd, 100'000'000).get();

    const auto spot = exchange.symbols().create("BTCUSD", sdk::SymbolType::Spot, btc, usd).get();
    const auto spotBook = exchange.books().addSpot(spot).get();
    spotBook.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 99'900, 1, 1).get();
    spotBook.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100'100, 1, 1).get();

    const auto perpetual = exchange.symbols().create("BTC-PERP", sdk::SymbolType::Perpetual, btc, usd).get();
    sdk::ContractSpec spec;
    spec.takerFeeRate = 0;
    spec.firstFundingTimestamp = 100;
    spec.fundingInterval = 100;
    const auto book = exchange.books().addContract(perpetual, spec, 100'000, 0).get();
    book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100'000, 10, 2, sdk::ContractOrderOptions{}).get();
    const auto order = book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100'000, 10, 3, sdk::ContractOrderOptions{}).get();

    const auto contracts = exchange.contracts();
    contracts.bindSpot(perpetual, spot).get();
    contracts.refresh(perpetual, 100).get();
    const auto market = contracts.market(perpetual).get();
    std::cout << "index=" << market.referencePrices.indexPrice << " mark=" << market.markPrice << " fundingRate=" << market.lastFundingRate << " fundingPnL=" << order.position().get()->snapshot().get().fundingPnL << '\n';

    sdk::ReferencePriceConfig reference;
    reference.sources = {{sdk::PriceSourceType::Spot, spot.id()}, {sdk::PriceSourceType::External, 42, 3}};
    contracts.configureReference(perpetual, reference).get();
    contracts.publishPrice(perpetual, 42, 100'200, 101).get();
    contracts.refresh(perpetual, 101).get();

    sdk::ContractSchedule schedule;
    schedule.fundingInterval = 200;
    schedule.nextFundingTimestamp = 400;
    contracts.configureSchedule(perpetual, schedule, 102).get();
    exchange.close();
}
