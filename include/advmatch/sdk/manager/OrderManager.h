#ifndef ADVMATCH_SDK_ORDER_MANAGER_H
#define ADVMATCH_SDK_ORDER_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class OrderManager final : public EntityManager<Order, ReadKind::Orders>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<ReadResponse<TradeView>> trades(ObservationQuery query = {}) const
        {
            return this->read<TradeView>(ReadKind::Trades, std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_ORDER_MANAGER_H
