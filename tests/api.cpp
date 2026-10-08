#include "advmatch/Exchange.h"
#include "advmatch/api/command/Commands.h"
#include "advmatch/api/async/impl/ThreadPoolExecutor.h"
#include "advmatch/observe/cache/ObservationCache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <latch>
#include <thread>

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
    using namespace advmatch::api::async;
    namespace command = advmatch::api::command;
    using namespace std::chrono_literals;

    class ManualExecutor final : public Executor
    {
        std::deque<std::function<void()>> jobs;

    public:
        void execute(std::function<void()> job) override
        {
            this->jobs.push_back(std::move(job));
        }

        void drain()
        {
            while (!this->jobs.empty())
            {
                auto job = std::move(this->jobs.front());
                this->jobs.pop_front();
                job();
            }
        }
    };

    template <typename Error, typename T>
    void fails(const CompletableFuture<T>& future)
    {
        bool caught = false;
        try
        {
            future.getFor(5s);
        }
        catch (const Error&)
        {
            caught = true;
        }

        CHECK(caught);
    }

    void futures()
    {
        auto executor = std::make_shared<ManualExecutor>();
        CompletionSource<int> source(executor);
        auto value = source.future();
        const auto applied = value.thenApply([](int input)
        {
            return input * 2;
        });
        CHECK(!value.isDone() && source.complete(21) && !source.complete(22));
        CHECK(value.get() == 21 && !applied.isDone());
        executor->drain();
        CHECK(applied.get() == 42);
        CHECK(value.get() == 21);
        auto moved = std::move(value);
        CHECK(value.get() == 21 && moved.get() == 21);

        const auto composed = value.thenCompose([](int input)
        {
            return CompletableFuture<int>::completedFuture(input + 1);
        });
        executor->drain();
        CHECK(composed.get() == 22);
        const auto combined = value.thenCombine(applied, [](int first, int second)
        {
            return first + second;
        });
        executor->drain();
        CHECK(combined.get() == 63);

        CompletionSource<std::unique_ptr<int>> unique(executor);
        const auto combinedUnique = unique.future().thenCombine(applied, [](const std::unique_ptr<int>& first, int second)
        {
            return *first + second;
        });
        unique.complete(std::make_unique<int>(10));
        executor->drain();
        CHECK(combinedUnique.get() == 52);

        int consumed = 0;
        auto accepted = value.thenAccept([&](int input)
        {
            consumed = input;
        });
        auto ran = value.thenRun([&]
        {
            ++consumed;
        });
        executor->drain();
        accepted.get();
        ran.get();
        CHECK(consumed == 22);

        CompletionSource<void> unit(executor);
        const auto fromVoid = unit.future().thenApply([]
        {
            return 9;
        });
        unit.complete();
        executor->drain();
        CHECK(fromVoid.get() == 9);
        const auto voidComposed = unit.future().thenCompose([]
        {
            return CompletableFuture<void>::completedFuture();
        });
        executor->drain();
        voidComposed.get();

        const auto error = std::make_exception_ptr(std::runtime_error("original"));
        const auto failed = CompletableFuture<int>::failedFuture(error);
        const auto recovered = failed.exceptionally([](std::exception_ptr)
        {
            return 7;
        }, executor);
        const auto handled = failed.handle([](const FutureOutcome<int>& outcome)
        {
            return outcome.error != nullptr ? -1 : *outcome.value;
        }, executor);
        const auto throwing = value.thenApply([](int) -> int
        {
            throw std::invalid_argument("callback failed");
        });
        bool called = false;
        const auto observed = failed.whenComplete([&](const FutureOutcome<int>& outcome)
        {
            CHECK(outcome.error != nullptr);
            called = true;
            throw std::logic_error("must preserve original failure");
        }, executor);
        executor->drain();
        CHECK(recovered.get() == 7 && handled.get() == -1 && called);
        fails<std::invalid_argument>(throwing);
        try
        {
            observed.get();
            CHECK(false);
        }
        catch (const std::runtime_error& result)
        {
            CHECK(std::string(result.what()) == "original");
        }

        CompletionSource<int> pending(executor);
        bool timedOut = false;
        try
        {
            pending.future().getFor(1ms);
        }
        catch (const FutureTimeout&)
        {
            timedOut = true;
        }

        CHECK(timedOut && !pending.future().isDone());
        CHECK(pending.future().cancel() && pending.future().isCancelled() && !pending.complete(3));
        fails<FutureCancelled>(pending.future());
        executor->drain();

        int callbacks = 0;
        auto cancelledCallback = value.thenRun([&]
        {
            ++callbacks;
        });
        CHECK(cancelledCallback.cancel());
        executor->drain();
        CHECK(callbacks == 0);
        CompletionSource<int> running;
        CHECK(running.tryStart() && !running.future().cancel());
        running.complete(1);

        CompletionSource<int> late;
        std::array inputs{failed, late.future()};
        const auto together = CompletableFuture<int>::allOf(inputs);
        CHECK(!together.isDone());
        CHECK(CompletableFuture<int>::anyOf(inputs).isDone());
        late.complete(2);
        fails<std::runtime_error>(together);
        CompletableFuture<int>::allOf({}).get();
        fails<std::invalid_argument>(CompletableFuture<int>::anyOf({}));
        auto closedExecutor = std::make_shared<ThreadPoolExecutor>(1);
        closedExecutor->shutdown();
        fails<std::runtime_error>(value.thenApply([](int input)
        {
            return input;
        }, closedExecutor));
        CHECK(CompletableFuture<std::unique_ptr<int>>::completedFuture(std::make_unique<int>(17)).getShared()->get() != nullptr);
    }

    ExchangeConfig memoryConfig(int ringSize = 128)
    {
        ExchangeConfig config;
        config.persistence.enabled = false;
        config.ringBufferSize = ringSize;
        config.observation.indexUserOpenOrders = true;
        return config;
    }

    observe::ObservationResult read(Exchange& exchange, observe::ReadKind kind, UserId userId = 0, AccountId accountId = 0, SymbolId symbolId = 0, OrderId orderId = 0, PositionId positionId = 0)
    {
        observe::ObservationQuery query;
        query.kind = kind;
        query.userId = userId;
        query.accountId = accountId;
        query.symbolId = symbolId;
        query.orderId = orderId;
        query.positionId = positionId;
        return exchange.submit(command::Read(query)).getFor(5s);
    }

    void spotAndReads()
    {
        Exchange exchange(memoryConfig());
        api::ExchangeApi& api = exchange;
        CHECK(api.submit(command::CreateAsset({"Bitcoin", "BTC", 0}, 1)).get().result.entityId == 1);
        const auto duplicate = api.submit(command::CreateAsset({"Bitcoin", "BTC", 0}, 1)).get();
        CHECK(duplicate.duplicate && duplicate.sequence == 1);
        CHECK(api.submit(command::CreateAsset({"Dollar", "USD", 0})).get().result.entityId == 2);
        CHECK(api.submit(command::CreateUser({})).get().result.entityId == 1);
        CHECK(api.submit(command::CreateUser({})).get().result.entityId == 2);
        CHECK(api.submit(command::CreateSymbol({"BTCUSD", SymbolType::Spot, 1, 2})).get().result);
        CHECK(api.submit(command::AddSpotBook({1})).get().result);
        CHECK(api.submit(command::UpdateBalance({1, 2, 10000})).get().result);
        CHECK(api.submit(command::UpdateBalance({3, 1, 20})).get().result);
        CHECK(api.submit(command::Transfer({1, 2, 2, 100})).get().result);
        CHECK(api.submit(command::Transfer({2, 1, 2, 50})).get().result);

        const auto ask = api.submit(command::PlaceOrder({2, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 10, 1})).get();
        const auto bid = api.submit(command::PlaceOrder({1, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 3, 2})).get();
        CHECK(ask.result && bid.result && bid.result.filled == 3);
        CHECK(read(exchange, observe::ReadKind::Order, 0, 0, 0, ask.result.entityId).rows<observe::OrderView>()[0].filling.unfilled == 7);
        CHECK(read(exchange, observe::ReadKind::Trades).rows<observe::TradeView>().size() == 1);
        CHECK(api.submit(command::ModifyQuantity({2, ask.result.entityId, 1, 3})).get().result);
        CHECK(api.submit(command::ModifyPrice({2, ask.result.entityId, 102, 4})).get().result);
        CHECK(api.submit(command::CancelOrder({2, ask.result.entityId, 5})).get().result);
        CHECK(read(exchange, observe::ReadKind::UserOpenOrders, 2).rows<observe::OrderView>().empty());
        CHECK(read(exchange, observe::ReadKind::Balances, 1, 1).rows<observe::BalanceView>().size() == 2);

        const auto resting = api.submit(command::PlaceOrder({2, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 105, 5, 6})).get();
        observe::ObservationQuery depth;
        depth.kind = observe::ReadKind::Depth;
        depth.symbolId = 1;
        depth.side = OrderSide::Ask;
        const auto slot = api.submit(command::CreateObservationSlot(depth)).get();
        const auto old = api.submit(command::RefreshObservation(slot)).get();
        CHECK(old->rows<observe::DepthView>()[0].quantity == 5);
        const auto typedDepth = api.submit(command::ReadDepth(depth)).get();
        CHECK(typedDepth.rows().size() == 1 && typedDepth.values.front().quantity == 5);
        observe::ObservationQuery missing;
        missing.userId = 999;
        const auto missingUser = api.submit(command::ReadUser(missing)).get();
        CHECK(missingUser.page.state == observe::ReadState::NotFound && missingUser.values.empty());
        CHECK(api.submit(command::ModifyQuantity({2, resting.result.entityId, 2, 7})).get().result);
        api.submit(command::RefreshObservation(slot)).get();
        CHECK(old->rows<observe::DepthView>()[0].quantity == 5 && slot->load()->rows<observe::DepthView>()[0].quantity == 7);
        api.submit(command::RebuildOrderIndex()).get();

        std::vector<observe::ObservationQuery> queries(3);
        queries[0].kind = observe::ReadKind::User;
        queries[0].userId = 1;
        queries[1].kind = observe::ReadKind::Balances;
        queries[1].userId = 1;
        queries[2].kind = observe::ReadKind::Book;
        queries[2].symbolId = 1;
        const auto batch = api.submit(command::ReadBatch(queries)).get();
        CHECK(batch.state == observe::ReadState::Success && batch.results.size() == 3);
        CHECK(batch.results[0].page.stamp.revision == batch.results[2].page.stamp.revision);
        const auto detached = read(exchange, observe::ReadKind::Order, 0, 0, 0, ask.result.entityId);
        CHECK(api.submit(command::ArchiveHistory({100, {ask.result.entityId}, {}})).get().result.archivedOrders == 1);
        CHECK(read(exchange, observe::ReadKind::Order, 0, 0, 0, ask.result.entityId).page.state == observe::ReadState::NotFound);
        CHECK(detached.rows<observe::OrderView>()[0].filling.status == OrderStatus::Cancelled);
        CHECK(!api.submit(command::RemoveAsset({1})).get().result);
        CHECK(!api.submit(command::RemoveSymbol({1})).get().result);
        CHECK(!api.submit(command::RemoveUser({999})).get().result);
        auto sharedCommand = std::make_shared<const command::ReadOverview>();
        CHECK(api.submit(sharedCommand).get().page.state == observe::ReadState::Success);
        std::shared_ptr<const command::Command<observe::ObservationResult>> nullCommand;
        fails<api::ApiError>(api.submit(nullCommand));
        api.close().get();
        api.close().get();
        CHECK(slot->load()->rows<observe::DepthView>()[0].quantity == 7);
        fails<api::ApiError>(api.submit(command::ReadOverview()));
    }

    struct Gate
    {
        std::latch entered{1};
        std::latch released{1};
    };

    class WaitCommand final : public command::Command<std::thread::id>
    {
        std::shared_ptr<Gate> gate;

    public:
        explicit WaitCommand(std::shared_ptr<Gate> gate)
            : gate(std::move(gate))
        {
        }

        std::thread::id execute(command::CommandContext&) const override
        {
            this->gate->entered.count_down();
            this->gate->released.wait();
            return std::this_thread::get_id();
        }
    };

    class CountCommand final : public command::Command<void>
    {
        std::atomic<int>& count;

    public:
        explicit CountCommand(std::atomic<int>& count)
            : count(count)
        {
        }

        void execute(command::CommandContext&) const override
        {
            ++this->count;
        }
    };

    class AwaitCommand final : public command::Command<int>
    {
        CompletableFuture<int> pending;

    public:
        explicit AwaitCommand(CompletableFuture<int> pending)
            : pending(std::move(pending))
        {
        }

        int execute(command::CommandContext&) const override
        {
            return this->pending.get();
        }
    };

    void ownerWaitGuard()
    {
        Exchange exchange(memoryConfig());
        CompletionSource<int> pending;
        fails<std::logic_error>(exchange.submit(AwaitCommand(pending.future())));
        CHECK(exchange.submit(command::CreateUser({})).getFor(5s).result);
        pending.complete(7);
        CHECK(exchange.submit(AwaitCommand(pending.future())).getFor(5s) == 7);
        exchange.close().get();
    }

    void admissionAndCancellation()
    {
        Exchange exchange(memoryConfig(2));
        auto gate = std::make_shared<Gate>();
        const auto active = exchange.submit(WaitCommand(gate));
        gate->entered.wait();
        CHECK(!active.cancel());
        std::atomic<int> count{0};
        auto pending = exchange.submit(CountCommand(count));
        CHECK(pending.cancel());
        const auto rejected = exchange.submit(command::CreateUser({}));
        try
        {
            rejected.getFor(1s);
            CHECK(false);
        }
        catch (const api::ApiError& error)
        {
            CHECK(error.reason() == api::ApiFailure::QueueFull);
        }

        const auto closing = exchange.close();
        CHECK(!closing.isDone() && !closing.cancel());
        fails<api::ApiError>(exchange.submit(command::ReadOverview()));
        gate->released.count_down();
        active.getFor(5s);
        fails<FutureCancelled>(pending);
        closing.getFor(5s);
        CHECK(count == 0);
    }

    void callbacksAndProducers()
    {
        Exchange exchange(memoryConfig(1024));
        auto callbackGate = std::make_shared<Gate>();
        auto gate = std::make_shared<Gate>();
        auto thread = exchange.submit(WaitCommand(gate));
        gate->entered.wait();
        gate->released.count_down();
        const auto ownerThread = thread.getFor(5s);
        const auto callback = exchange.submit(command::CreateUser({})).thenApply([callbackGate, ownerThread](const CommandReceipt& receipt)
        {
            CHECK(std::this_thread::get_id() != ownerThread);
            callbackGate->entered.count_down();
            callbackGate->released.wait();
            return receipt.result.entityId;
        });
        callbackGate->entered.wait();
        CHECK(exchange.submit(command::CreateUser({})).getFor(5s).result.entityId == 2);
        callbackGate->released.count_down();
        CHECK(callback.getFor(5s) == 1);
        std::array<std::thread, 4> producers;
        for (auto& producer : producers)
        {
            producer = std::thread([&]
            {
                for (int index = 0; index < 100; ++index)
                {
                    CHECK(exchange.submit(command::CreateUser({})).getFor(5s).result);
                }
            });
        }

        for (auto& producer : producers)
        {
            producer.join();
        }

        const auto overview = exchange.submit(command::ReadOverview()).get();
        CHECK(overview.value->activeUsers == 402);
        exchange.close().get();
    }

    void contracts()
    {
        Exchange exchange(memoryConfig());
        CHECK(exchange.submit(command::CreateAsset({"Bitcoin", "BTC", 0})).get().result);
        CHECK(exchange.submit(command::CreateAsset({"Dollar", "USD", 0})).get().result);
        for (int index = 0; index < 2; ++index)
        {
            CHECK(exchange.submit(command::CreateUser({})).get().result);
        }

        CHECK(exchange.submit(command::UpdateBalance({2, 2, 10000})).get().result);
        CHECK(exchange.submit(command::UpdateBalance({4, 2, 10000})).get().result);
        CHECK(exchange.submit(command::CreateSymbol({"PERP", SymbolType::Perpetual, 1, 2})).get().result);
        ContractSpec spec;
        spec.takerFeeRate = 0;
        spec.liquidationFeeRate = 0;
        spec.firstFundingTimestamp = 100;
        spec.fundingInterval = 100;
        CHECK(exchange.submit(command::AddContractBook({1, spec, 100, 0})).get().result);
        ContractOrderOptions options;
        const auto ask = exchange.submit(command::PlaceOrder({2, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 10, 1, options})).get();
        const auto bid = exchange.submit(command::PlaceOrder({1, 1, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 10, 2, options})).get();
        CHECK(ask.result && bid.result && bid.result.positionId != 0);
        CHECK(exchange.submit(command::SetLeverage({bid.result.positionId, 1, 1, 5, 3})).get().result);
        CHECK(exchange.submit(command::AddMargin({bid.result.positionId, 1, 1, 100})).get().result);
        CHECK(exchange.submit(command::FundInsurance({1, 4, 1000})).get().result);
        CHECK(exchange.submit(command::SettleFunding({1, 10000, 100})).get().result);
        CHECK(read(exchange, observe::ReadKind::Position, 0, 0, 0, 0, bid.result.positionId).rows<observe::PositionView>()[0].fundingPnL == -10);
        CHECK(read(exchange, observe::ReadKind::ContractEvents).rows<observe::ContractEventView>().size() >= 4);
        CHECK(exchange.submit(command::SetAutomaticLiquidationBudget({0})).get().result);
        const auto marked = exchange.submit(command::UpdateMarkPrice({1, 50, 101})).get();
        CHECK(marked.result.state == static_cast<std::uint8_t>(ContractState::PendingLiquidation));
        CHECK(read(exchange, observe::ReadKind::PendingLiquidations, 0, 0, 1).rows<observe::PositionView>().size() == 1);
        // No liquidity: liquidation stays pending and is exposed through API.
        const auto liquidated = exchange.submit(command::Liquidate({1, bid.result.positionId, 102})).get();
        CHECK(liquidated.result.type == CommandResultType::Contract);
        CHECK(exchange.submit(command::ProcessLiquidations({1, 103, true, 1})).get().result.type == CommandResultType::Contract);

        CHECK(exchange.submit(command::CreateSymbol({"FUTURE", SymbolType::Future, 1, 2})).get().result);
        spec.firstFundingTimestamp = 0;
        spec.expiryTimestamp = 1000;
        CHECK(exchange.submit(command::AddContractBook({2, spec, 100, 0})).get().result);
        CHECK(exchange.submit(command::PlaceOrder({2, 2, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100, 1, 1, options})).get().result);
        CHECK(exchange.submit(command::PlaceOrder({1, 2, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 1, 2, options})).get().result);
        CHECK(exchange.submit(command::Expire({2, 1000})).get().result);
        CHECK(exchange.submit(command::Deliver({2, 105, 1000})).get().result);
        CHECK(read(exchange, observe::ReadKind::Market, 0, 0, 2).rows<observe::MarketView>()[0].status == ContractStatus::Settled);
        exchange.close().get();
    }

    void persistenceAndRecovery()
    {
        const auto directory = std::filesystem::current_path() / ("exchange-api-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(std::filesystem::create_directory(directory));
        ExchangeConfig config;
        config.persistence.directory = directory;
        config.persistence.flushPolicy = FlushPolicy::Manual;
        {
            Exchange exchange(config);
            CHECK(exchange.submit(command::CreateUser({}, 1)).get().durableThrough == 0);
            CHECK(exchange.submit(command::CreateUser({}, 2)).get().result.entityId == 2);
            CHECK(exchange.submit(command::Tick()).get().sequence == 2);
            CHECK(exchange.submit(command::Flush()).get().durableThrough == 2);
            CHECK(exchange.submit(command::Checkpoint()).get().durableThrough == 2);
            const auto first = exchange.submit(command::ReadCommitted({0, 1})).get();
            CHECK(first.records.size() == 1 && first.nextSequence == 1 && first.hasMore);
            const auto second = exchange.submit(command::ReadCommitted({first.nextSequence, 1})).get();
            CHECK(second.records.size() == 1 && second.nextSequence == 2 && !second.hasMore);
            exchange.close().get();
            // Explicit close releases the directory lock even while the facade
            // still exists, so another Exchange can recover this same journal.
            Exchange recovered(config);
            CHECK(read(recovered, observe::ReadKind::Users).rows<observe::UserView>().size() == 2);
            CHECK(recovered.submit(command::CreateUser({}, 2)).get().duplicate);
            CHECK(recovered.submit(command::ReadPersistenceStatus()).get().sequence == 2);
            recovered.close().get();
        }

        CHECK(directory.parent_path() == std::filesystem::current_path() && directory.filename().string().starts_with("exchange-api-tests-"));
        std::filesystem::remove_all(directory);
    }

    class FailingStore final : public PersistenceStore
    {
        std::atomic<bool>& released;

    public:
        explicit FailingStore(std::atomic<bool>& released)
            : released(released)
        {
        }

        ~FailingStore() override
        {
            this->released = true;
        }

        std::optional<StoredSnapshot> loadSnapshot() override
        {
            return std::nullopt;
        }

        void replay(const std::function<void(const JournalRecord&)>&) override
        {
        }

        void append(const JournalRecord&) override
        {
            throw std::runtime_error("simulated disk failure");
        }

        void sync() override
        {
        }

        void publishSnapshot(CommandSequence, std::span<const std::byte>) override
        {
        }

        void resetJournalThrough(CommandSequence) override
        {
        }
    };

    void faultedSession()
    {
        std::atomic<bool> released{false};
        Exchange exchange({}, std::make_unique<FailingStore>(released));
        fails<std::runtime_error>(exchange.submit(command::CreateUser({})));
        CHECK(exchange.submit(command::ReadPersistenceStatus()).get().faulted);
        CHECK(exchange.submit(command::ReadUsers()).get().page.state == observe::ReadState::SourceFaulted);
        fails<PersistenceError>(exchange.close());
        CHECK(released.load());
    }
}

int main()
{
    futures();
    spotAndReads();
    admissionAndCancellation();
    ownerWaitGuard();
    callbacksAndProducers();
    contracts();
    persistenceAndRecovery();
    faultedSession();
    std::puts("Exchange API tests passed");
}
