#include <advmatch/sdk/AdvMatch.h>

#include <iostream>
#include <array>

int main()
{
    namespace sdk = advmatch::sdk;

    sdk::DefaultReferencePriceAlgorithm algorithm;
    const std::array<sdk::WeightedPrice, 1> samples = {{{100, 1, 1}}};
    if (!algorithm.calculate(samples, 0, sdk::ReferencePriceConfig{}, sdk::ContractSpec{}, 1))
    {
        throw std::runtime_error("SDK reference algorithm verification failed");
    }

    sdk::ExchangeConfig config;
    config.persistence.enabled = false;
    config.observation.indexUserOpenOrders = true;
    config.callbackExecutor = std::make_shared<sdk::ThreadPoolExecutor>(2);
    sdk::MatchingExchange exchange(config);

    const auto btc = exchange.assets().create("Bitcoin", "BTC", 0).get();
    const auto usd = exchange.assets().create("Dollar", "USD", 0).get();
    const auto buyer = exchange.users().create().get();
    const auto seller = exchange.users().create().get();
    const auto buyerAccount = buyer.spotAccount().get();
    const auto sellerAccount = seller.spotAccount().get();
    buyerAccount.updateBalance(usd, 10'000).get();
    sellerAccount.updateBalance(btc, 10).get();

    const auto symbol = exchange.symbols().create("BTCUSD", sdk::SymbolType::Spot, btc, usd).get();
    const auto book = exchange.books().addSpot(symbol).get();
    const auto ask = book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 2, 1).get();
    const auto bid = book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 2, 2).get();

    const auto order = bid.snapshot().get();
    const auto balance = buyerAccount.balance(btc).get();
    if (order.filling.status != sdk::OrderStatus::Filled || balance.active != 2)
    {
        throw std::runtime_error("SDK trade verification failed");
    }

    sdk::ObservationQuery query;
    query.userId = buyer.id();
    const auto user = exchange.submit(sdk::command::ReadUser(query)).get();
    std::cout << "C++ SDK passed: buyer=" << user.values.front().userId << " ask=" << ask.id() << " bid=" << bid.id() << " acquired=" << balance.active << '\n';
    exchange.close();
}
