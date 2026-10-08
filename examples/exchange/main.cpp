#include "advmatch/Exchange.h"
#include "advmatch/api/command/Commands.h"
#include "advmatch/observe/cache/ObservationCache.h"

#include <cstdio>

int main()
{
    using namespace advmatch;
    namespace command = api::command;

    try
    {
        ExchangeConfig config;
        config.persistence.enabled = false;
        config.observation.indexUserOpenOrders = true;
        Exchange exchange(config);
        api::ExchangeApi& api = exchange;

        const auto base = api.submit(command::CreateAsset({"Bitcoin", "BTC", 0})).get().result.entityId;
        const auto quote = api.submit(command::CreateAsset({"Dollar", "USD", 0})).get().result.entityId;
        const auto userId = api.submit(command::CreateUser({})).get().result.entityId;
        const auto symbolId = api.submit(command::CreateSymbol({"BTCUSD", SymbolType::Spot, static_cast<AssetId>(base), static_cast<AssetId>(quote)})).get().result.entityId;
        api.submit(command::AddSpotBook({static_cast<SymbolId>(symbolId)})).get();

        observe::ObservationQuery userQuery;
        userQuery.userId = userId;
        const auto user = api.submit(command::ReadUser(userQuery)).get();
        const auto accountId = user.values.front().spotAccountId;
        api.submit(command::UpdateBalance({accountId, static_cast<AssetId>(quote), 10000})).get();

        // Compose without blocking callback workers. A business rejection is
        // contained in receipt.result; exceptional completion is infrastructure.
        const auto order = api.submit(command::PlaceOrder({userId, static_cast<SymbolId>(symbolId), OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, 100, 10, 1}));
        const auto state = order.thenCompose([&api](const CommandReceipt& receipt)
        {
            observe::ObservationQuery query;
            query.orderId = receipt.result.entityId;
            return api.submit(command::ReadOrder(query));
        });
        const auto image = state.getShared();
        std::printf("Order=%llu unfilled=%llu\n", static_cast<unsigned long long>(image->values.front().orderId), static_cast<unsigned long long>(image->values.front().filling.unfilled));

        observe::ObservationQuery depth;
        depth.kind = observe::ReadKind::Depth;
        depth.symbolId = static_cast<SymbolId>(symbolId);
        depth.side = OrderSide::Bid;
        const auto slot = api.submit(command::CreateObservationSlot(depth)).get();
        api.submit(command::RefreshObservation(slot)).get();
        const auto cached = slot->load();
        std::printf("Bid=%llu quantity=%llu\n", static_cast<unsigned long long>(cached->rows<observe::DepthView>().front().price), static_cast<unsigned long long>(cached->rows<observe::DepthView>().front().quantity));
        api.close().get();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Exchange example failed: %s\n", error.what());
        return 1;
    }
}
