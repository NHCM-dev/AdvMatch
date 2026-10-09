#include <advmatch/sdk/AdvMatch.h>

#include <filesystem>
#include <iostream>
#include <thread>

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

    template <typename Error, typename Function>
    void expectError(Function action)
    {
        try
        {
            action();
        }
        catch (const Error&)
        {
            return;
        }

        throw std::runtime_error("expected SDK error was not raised");
    }

    sdk::ExchangeConfig memory()
    {
        sdk::ExchangeConfig config;
        config.persistence.enabled = false;
        config.observation.indexUserOpenOrders = true;
        return config;
    }

    template <typename Function>
    void expectPendingLiquidation(Function action)
    {
        try
        {
            action();
        }
        catch (const sdk::BusinessError& error)
        {
            check(error.receipt().result.type == sdk::CommandResultType::Contract, "contract result type");
            check(error.receipt().result.state == static_cast<std::uint32_t>(sdk::ContractState::PendingLiquidation), "pending liquidation result");
            return;
        }

        throw std::runtime_error("expected pending liquidation was not raised");
    }

    class ReadCreatedUser final : public sdk::command::Command<sdk::UserView>
    {
    public:
        sdk::CompletableFuture<sdk::UserView> execute(sdk::ExchangeApi& api) const override
        {
            // ExchangeApi::submit retains its owner for this composed command.
            return api.execute(advmatch::commands::CreateUser{}).thenCompose([&api](const sdk::CommandReceipt& result)
            {
                sdk::ObservationQuery query;
                query.kind = sdk::ReadKind::User;
                query.userId = result.result.entityId;
                return api.readRows<sdk::UserView>(query).thenApply([](const sdk::ReadResponse<sdk::UserView>& value)
                {
                    return value.values.front();
                });
            });
        }
    };

    void objectApi()
    {
        sdk::MatchingExchange exchange(memory());
        sdk::MatchingExchange other(memory());
        const auto created = exchange.submit(sdk::command::CreateAsset({"Base", "BASE", 0})).get();
        check(created.id() != 0 && created.id() == created.handle(), "creation ID");
        const auto base = exchange.assets().get(static_cast<sdk::AssetId>(created.id()));
        const auto quote = exchange.assets().create("USD", "USD", 0).get();
        const auto buyer = exchange.users().create().get();
        const auto seller = exchange.users().create().get();
        const auto account = buyer.spotAccount().get();
        const auto contracts = buyer.contractAccount().get();
        account.updateBalance(quote, 10'000).get();
        seller.spotAccount().get().updateBalance(base, 100).get();
        account.transferTo(contracts, quote, 1000).get();
        check(contracts.balance(quote).get().active == 1000, "separate contract balance");
        check(account.balance(quote).get().active == 9000, "spot transfer debit");
        check(exchange.accounts().find(contracts.id()).get()->snapshot().get().type == sdk::AccountType::Contract, "account type dispatch");
        expectError<std::invalid_argument>([&] { exchange.accounts().spot(contracts.id()).snapshot().get(); });

        const auto symbol = exchange.symbols().create("PAIR", sdk::SymbolType::Spot, base, quote).get();
        const auto book = exchange.books().addSpot(symbol).get();
        const auto ask = book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 10, 1).get();
        ask.modifyQuantity(1, 2).get();
        ask.modifyPrice(101, 3).get();
        check(book.depth(sdk::OrderSide::Ask).get().values.front().quantity == 11, "modify price and quantity");
        check(book.level(sdk::OrderSide::Ask, 101).get().values.front().quantity == 11, "single price level");
        const auto bid = exchange.matching().place(buyer, symbol, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 101, 2, 4).get();
        check(bid.snapshot().get().filling.status == sdk::OrderStatus::Filled, "spot matching");
        ask.cancel(5).get();
        check(ask.snapshot().get().filling.status == sdk::OrderStatus::Cancelled, "cancel");
        check(!bid.position().get(), "spot has no position");
        check(account.balance(base).get().active == 2, "settled asset balance");
        check(exchange.orders().trades().get().values.size() == 1, "trade history");
        check(exchange.books().find(symbol.id()).get().id() == book.id(), "book find");
        check(exchange.orders().get(ask.id()) == ask, "stable entity equality");
        check(exchange.users().find(buyer.id()).get().id() == buyer.id(), "user find");
        expectError<sdk::BusinessError>([&] { ask.cancel(6).get(); });
        const auto unknown = exchange.users().find(9999);
        expectError<sdk::ReadError>([&] { unknown.get(); });
        const auto foreign = other.users().create().get().spotAccount().get();
        const auto invalidTransfer = account.transferTo(foreign, quote, 1);
        expectError<std::invalid_argument>([&] { invalidTransfer.get(); });
        expectError<std::invalid_argument>([&] { exchange.symbols().create("WRONG", sdk::SymbolType::Spot, other.assets().create("X", "X", 0).get(), quote).get(); });

        const auto rejected = exchange.submit(sdk::command::CreateSymbol({"BAD", sdk::SymbolType::Spot, 9999, 9998})).get();
        check(!rejected.receipt.result.accepted && rejected.id() == 0, "raw command business rejection");
        sdk::ObservationQuery query;
        query.userId = buyer.id();
        check(exchange.submit(sdk::command::ReadUser(query)).get().values.front().userId == buyer.id(), "typed read");
        query.kind = sdk::ReadKind::User;
        const auto slot = exchange.observations().createSlot(query).get();
        check(slot->load() == nullptr, "unpublished slot");
        slot->refresh().get();
        const auto cached = slot->load();
        check(cached && cached->rows<sdk::UserView>().front().userId == buyer.id(), "cache publication");
        expectError<std::invalid_argument>([&] { other.submit(sdk::command::RefreshObservation(slot)).get(); });
        check(exchange.observations().readBatch({query}).get().results.size() == 1, "batch read");
        exchange.observations().rebuildOrderIndex().get();
        check(exchange.observations().overview().get().value->retainedOrders == 2, "overview");

        // Instantiate every public typed read command, including default
        // selections whose missing IDs deliberately return a value status.
#define READ_COMMAND(name) check(exchange.submit(sdk::command::name{}).get().page.state != sdk::ReadState::SourceFaulted, #name)
        READ_COMMAND(ReadOverview);
        READ_COMMAND(ReadAsset);
        READ_COMMAND(ReadAssets);
        READ_COMMAND(ReadUser);
        READ_COMMAND(ReadUsers);
        READ_COMMAND(ReadAccount);
        READ_COMMAND(ReadAccounts);
        READ_COMMAND(ReadBalance);
        READ_COMMAND(ReadBalances);
        READ_COMMAND(ReadCrossMargin);
        READ_COMMAND(ReadCrossMargins);
        READ_COMMAND(ReadSymbol);
        READ_COMMAND(ReadSymbols);
        READ_COMMAND(ReadBook);
        READ_COMMAND(ReadBooks);
        READ_COMMAND(ReadLevel);
        READ_COMMAND(ReadDepth);
        READ_COMMAND(ReadBookOrders);
        READ_COMMAND(ReadOrder);
        READ_COMMAND(ReadOrders);
        READ_COMMAND(ReadUserOpenOrders);
        READ_COMMAND(ReadPosition);
        READ_COMMAND(ReadPositions);
        READ_COMMAND(ReadAccountPositions);
        READ_COMMAND(ReadMarket);
        READ_COMMAND(ReadMarkets);
        READ_COMMAND(ReadPendingLiquidations);
        READ_COMMAND(ReadTrades);
        READ_COMMAND(ReadContractEvents);
        READ_COMMAND(ReadManagedPositions);
        READ_COMMAND(ReadMarketPositions);
#undef READ_COMMAND

        exchange.persistence().archiveHistory(16, {bid.id()}, {}).get();
        expectError<sdk::ReadError>([&] { bid.snapshot().get(); });
        check(exchange.submit(ReadCreatedUser{}).get().userId != 0, "custom composed command");
        std::shared_ptr<sdk::command::Command<sdk::UserView>> abstractCommand = std::make_shared<ReadCreatedUser>();
        check(exchange.submit(abstractCommand).get().userId != 0, "abstract command submission");
        abstractCommand.reset();
        expectError<std::invalid_argument>([&] { exchange.submit(abstractCommand).get(); });

        sdk::ObservationQuery page;
        page.limit = 1;
        const auto first = exchange.users().list(page).get();
        check(first.page.hasMore, "paged users");
        exchange.users().create().get();
        page.cursor = first.page.next;
        check(exchange.users().list(page).get().page.state == sdk::ReadState::CursorExpired, "stale cursor retained");

        const auto disposable = exchange.users().create().get();
        disposable.remove().get();
        expectError<sdk::ReadError>([&] { disposable.snapshot().get(); });
        exchange.close();
        check(slot->load() == cached, "cached snapshot survives close");
        expectError<sdk::ApiError>([&] { slot->refresh().get(); });
        expectError<sdk::ApiError>([&] { buyer.snapshot().get(); });
    }

    void contracts()
    {
        sdk::MatchingExchange exchange(memory());
        const auto base = exchange.assets().create("BTC", "BTC", 0).get();
        const auto quote = exchange.assets().create("USD", "USD", 0).get();
        const auto buyer = exchange.users().create().get();
        const auto seller = exchange.users().create().get();
        buyer.contractAccount().get().updateBalance(quote, 10'000).get();
        seller.contractAccount().get().updateBalance(quote, 10'000).get();
        const auto symbol = exchange.symbols().create("PERP", sdk::SymbolType::Perpetual, base, quote).get();
        sdk::ContractSpec spec;
        spec.takerFeeRate = 0;
        spec.firstFundingTimestamp = 100;
        const auto book = exchange.books().addContract(symbol, spec, 100, 0).get();
        sdk::ContractOrderOptions options;
        options.marginMode = sdk::MarginMode::Cross;
        book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 1, 1, options).get();
        const auto order = book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 1, 2, options).get();
        const auto position = *order.position().get();
        position.setLeverage(5, 3).get();
        exchange.contracts().settleFunding(symbol, 1000, 100).get();
        check(position.snapshot().get().leverage == 5, "leverage");
        check(position.snapshot().get().marginMode == sdk::MarginMode::Cross, "cross position");
        check(exchange.contracts().market(symbol).get().openPositions == 2, "perpetual positions");
        check(exchange.contracts().positions(symbol).get().values.size() == 2, "market positions");
        check(!buyer.contractAccount().get().crossMargins().get().values.empty(), "cross margin exposure");
        exchange.contracts().setAutomaticLiquidationBudget(0).get();
        exchange.contracts().updateMark(symbol, 99, 101).get();
        exchange.contracts().processLiquidations(symbol, 102).get();
        check(!exchange.contracts().events().get().values.empty(), "contract event history");

        const auto victim = exchange.users().create().get();
        victim.contractAccount().get().updateBalance(quote, 11).get();
        options.marginMode = sdk::MarginMode::Isolated;
        book.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 99, 1, 103, options).get();
        const auto victimOrder = book.place(victim, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 99, 1, 103, options).get();
        const auto victimPosition = *victimOrder.position().get();
        expectPendingLiquidation([&] { exchange.contracts().updateMark(symbol, 89, 105).get(); });
        expectPendingLiquidation([&] { victimPosition.liquidate(106).get(); });
        check(victimPosition.snapshot().get().liquidating, "liquidity shortage leaves pending liquidation");
        exchange.contracts().fundInsurance(symbol, buyer.contractAccount().get(), 100).get();
        book.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 96, 1, 107, options).get();
        victimPosition.liquidate(108).get();
        check(victimPosition.snapshot().get().closed, "liquidation settles through the book");

        const auto future = exchange.symbols().create("FUTURE", sdk::SymbolType::Future, base, quote).get();
        spec.expiryTimestamp = 200;
        spec.firstFundingTimestamp = 0;
        const auto futureBook = exchange.books().addContract(future, spec, 100, 0).get();
        options.marginMode = sdk::MarginMode::Isolated;
        futureBook.place(seller, sdk::OrderSide::Ask, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 1, 1, options).get();
        const auto futureOrder = futureBook.place(buyer, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::IOC, 100, 1, 2, options).get();
        const auto futurePosition = *futureOrder.position().get();
        futurePosition.addMargin(10).get();
        exchange.contracts().fundInsurance(future, buyer.contractAccount().get(), 100).get();
        exchange.contracts().expire(future, 200).get();
        exchange.contracts().deliver(future, 101, 200).get();
        check(futurePosition.snapshot().get().closed, "cash delivery");
        check(exchange.contracts().market(future).get().status == sdk::ContractStatus::Settled, "future settled");
    }

    void lifecycleAndConcurrency()
    {
        std::optional<sdk::User> retained;
        {
            sdk::MatchingExchange exchange(memory());
            retained = exchange.users().create().get();
            std::vector<std::thread> producers;
            for (int index = 0; index < 4; ++index)
            {
                producers.emplace_back([manager = exchange.users()]
                {
                    for (int request = 0; request < 20; ++request)
                    {
                        manager.create().get();
                    }
                });
            }

            for (auto& producer : producers)
            {
                producer.join();
            }

            check(exchange.observations().overview().get().value->activeUsers == 81, "multi producer API");
        }

        expectError<sdk::ApiError>([&] { retained->snapshot().get(); });
        sdk::CompletableFuture<sdk::UserView> delayed;
        {
            sdk::MatchingExchange exchange(memory());
            // The temporary User can be gone before the snapshot completes.
            delayed = exchange.users().create().get().snapshot();
            exchange.close();
        }

        check(delayed.get().active, "future owns result after facade destruction");
    }

    void persistence()
    {
        const auto directory = std::filesystem::temp_directory_path() / ("advmatch-sdk-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        sdk::ExchangeConfig config;
        config.persistence.directory = directory;
        sdk::UserId userId = 0;
        {
            sdk::MatchingExchange exchange(config);
            userId = exchange.users().create(1).get().id();
            check(exchange.persistence().flush().get().durableThrough == 1, "durable commit");
            check(exchange.persistence().tick().get().sequence == 1, "persistence tick");
            check(exchange.persistence().checkpoint().get().enabled, "checkpoint");
            check(exchange.persistence().readCommitted().get().records.size() == 1, "committed history");
        }

        {
            sdk::MatchingExchange exchange(config);
            check(exchange.users().find(userId).get().id() == userId, "SDK recovery");
            check(exchange.persistence().status().get().sequence == 1, "recovered WAL sequence");
        }

        std::filesystem::remove_all(directory);
    }
}

int main()
{
    try
    {
        objectApi();
        contracts();
        lifecycleAndConcurrency();
        persistence();
        std::cout << "C++ SDK: entities, spot, contracts, reads, lifecycle, concurrency and recovery passed\n";
    }
    catch (const sdk::BusinessError& error)
    {
        std::cerr << error.what() << " type=" << static_cast<int>(error.receipt().result.type) << " state=" << static_cast<int>(error.receipt().result.state) << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
