#ifndef ADVMATCH_SDK_CONDITIONAL_ORDER_H
#define ADVMATCH_SDK_CONDITIONAL_ORDER_H

#include "Order.h"

namespace advmatch::sdk
{
    class ConditionalOrder final : public Entity<ConditionalOrderView, ConditionalOrderId, ReadKind::ConditionalOrder>
    {
    public:
        using Entity::Entity;

        CompletableFuture<std::optional<Order>> order() const
        {
            return this->snapshot().thenApply([api = this->api](const ConditionalOrderView& value) -> std::optional<Order>
            {
                return value.orderId == 0 ? std::nullopt : std::optional(Order(api, value.orderId));
            });
        }

        CompletableFuture<CommandReceipt> cancel(Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->snapshot().thenCompose([api = this->api, timestamp, requestId](const ConditionalOrderView& value)
            {
                return detail::Results::checked(api, commands::CancelConditionalOrder{value.request.userId, value.conditionalOrderId, timestamp}, requestId);
            });
        }

        CompletableFuture<CommandReceipt> modify(Price triggerPrice, Price price, Quantity quantity, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->snapshot().thenCompose([api = this->api, triggerPrice, price, quantity, timestamp, requestId](const ConditionalOrderView& value)
            {
                return detail::Results::checked(api, commands::ModifyConditionalOrder{value.request.userId, value.conditionalOrderId, triggerPrice, price, quantity, timestamp}, requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_CONDITIONAL_ORDER_H
