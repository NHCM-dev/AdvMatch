#ifndef ADVMATCH_SDK_CONDITIONAL_ORDER_MANAGER_H
#define ADVMATCH_SDK_CONDITIONAL_ORDER_MANAGER_H

#include "Manager.h"
#include "../object/ConditionalOrder.h"

namespace advmatch::sdk
{
    class ConditionalOrderManager final : public EntityManager<ConditionalOrder, ReadKind::ConditionalOrders>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<ConditionalOrder> place(ConditionalOrderRequest request, std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::PlaceConditionalOrder{std::move(request)}, requestId).thenApply([api = this->api](const CommandReceipt& receipt)
            {
                return ConditionalOrder(api, receipt.result.entityId);
            });
        }

        CompletableFuture<ConditionalOrder> place(const User& user, const Symbol& symbol, ConditionalOrderRequest request, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<ConditionalOrder>([&]
            {
                user.requireOwner(this->api);
                symbol.requireOwner(this->api);
                request.userId = user.id();
                request.symbolId = symbol.id();
                return this->place(std::move(request), requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_CONDITIONAL_ORDER_MANAGER_H
