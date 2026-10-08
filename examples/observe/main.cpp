#include "advmatch/observe/ObservedConsumer.h"
#include "advmatch/observe/ObservationEventSink.h"

#include <cstdio>

namespace
{
    using namespace advmatch;
    using namespace advmatch::observe;

    struct Work
    {
        TradingCommand command = commands::CreateUser{};
        bool close = false;
    };

    class TradingWorker final : public ObservedConsumer<Work, CommandReceipt>
    {
        PersistentTradingEngine trading;
        TradingObserver observer;
        ObservationEventSink events;
        bool eventsInstalled = false;

        CommandReceipt consumeCommand(Work&& work) override
        {
            if (!this->eventsInstalled)
            {
                this->trading.setEventSink(&this->events);
                this->eventsInstalled = true;
            }

            if (work.close)
            {
                this->trading.setEventSink(nullptr);
                this->trading.close();
                return {this->trading.lastSequence(), this->trading.durableThrough(), false, {}};
            }

            return this->trading.execute(work.command);
        }

    public:
        TradingWorker()
            : trading([]
            {
                PersistenceConfig config;
                config.enabled = false;
                return config;
            }()), observer(this->trading, []
            {
                ObservationConfig config;
                config.indexUserOpenOrders = true;
                return config;
            }()), events(this->observer)
        {
            this->bindObserver(this->observer);
        }

        ~TradingWorker() override
        {
            this->stopBeforeDerivedDestruction();
        }

        CommandReceipt submit(Work work)
        {
            consuming::ResponseSlot<ObservedResponse<CommandReceipt>> slot;
            return std::get<0>(this->post(std::move(work), slot).get());
        }

        std::shared_ptr<const ObservationResult> query(ObservationQuery query, std::shared_ptr<ObservationSlot> handle = nullptr)
        {
            consuming::ResponseSlot<ObservedResponse<CommandReceipt>> slot;
            return std::get<1>(this->post(ObservationRequest{query, std::move(handle), {}}, slot).get());
        }
    };
}

int main()
{
    try
    {
        TradingWorker worker;
        worker.start(1024);
        const auto base = worker.submit({commands::CreateAsset{"Bitcoin", "BTC", 0}}).result.entityId;
        const auto quote = worker.submit({commands::CreateAsset{"Dollar", "USD", 0}}).result.entityId;
        const auto userId = worker.submit({commands::CreateUser{}}).result.entityId;
        const auto symbolId = static_cast<SymbolId>(worker.submit({commands::CreateSymbol{"BTC-USD", SymbolType::Spot, static_cast<AssetId>(base), static_cast<AssetId>(quote)}}).result.entityId);
        worker.submit({commands::AddSpotBook{symbolId}});

        ObservationQuery userQuery;
        userQuery.kind = ReadKind::User;
        userQuery.userId = userId;
        const auto accountId = worker.query(userQuery)->rows<UserView>().front().spotAccountId;
        worker.submit({commands::UpdateBalance{accountId, static_cast<AssetId>(quote), 10'000}});
        const auto placed = worker.submit({commands::PlaceOrder{userId, symbolId, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 99, 10, 1}});

        ObservationCache cache;
        ObservationQuery depth;
        depth.kind = ReadKind::Depth;
        depth.symbolId = symbolId;
        depth.side = OrderSide::Bid;
        depth.limit = 20;
        auto handle = cache.create(depth);
        worker.query(depth, handle);

        // Reader/network threads may retain these values without touching any
        // live engine object. Refresh is an explicit, queued owner-thread read.
        const auto previous = handle->load();
        worker.submit({commands::ModifyQuantity{userId, placed.result.entityId, 5, 2}});
        worker.query(depth, handle);
        const auto latest = handle->load();
        std::printf("Bid price=%llu oldQuantity=%llu newQuantity=%llu sequence=%llu\n",
            static_cast<unsigned long long>(latest->rows<DepthView>().front().price),
            static_cast<unsigned long long>(previous->rows<DepthView>().front().quantity),
            static_cast<unsigned long long>(latest->rows<DepthView>().front().quantity),
            static_cast<unsigned long long>(latest->page.stamp.sequence));

        ObservationQuery openOrders;
        openOrders.kind = ReadKind::UserOpenOrders;
        openOrders.userId = userId;
        std::printf("User open orders=%zu\n", worker.query(openOrders)->rows<OrderView>().size());
        worker.submit({commands::CreateUser{}, true});
        worker.stop();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Observation example failed: %s\n", error.what());
        return 1;
    }
}
