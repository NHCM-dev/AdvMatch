#ifndef ADVMATCH_SDK_ORDER_H
#define ADVMATCH_SDK_ORDER_H

#include "Position.h"

namespace advmatch::sdk
{
    class Order final : public Entity<OrderView, OrderId, ReadKind::Order>
    {
        template <typename Operation>
        CompletableFuture<CommandReceipt> update(Operation operation, std::uint64_t requestId) const
        {
            return this->snapshot().thenCompose([api = this->api, operation, requestId](const OrderView& value)
            {
                return detail::Results::checked(api, operation(value), requestId);
            });
        }

    public:
        using Entity::Entity;

        CompletableFuture<std::optional<Position>> position() const
        {
            return this->snapshot().thenApply([api = this->api](const OrderView& value) -> std::optional<Position>
            {
                if (value.positionId == 0)
                {
                    return std::nullopt;
                }

                return Position(api, value.positionId);
            });
        }

        CompletableFuture<CommandReceipt> modifyPrice(Price price, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update([price, timestamp](const OrderView& value)
            {
                return commands::ModifyPrice{value.userId, value.orderId, price, timestamp};
            }, requestId);
        }

        CompletableFuture<CommandReceipt> modifyQuantity(QuantityChange change, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update([change, timestamp](const OrderView& value)
            {
                return commands::ModifyQuantity{value.userId, value.orderId, change, timestamp};
            }, requestId);
        }

        CompletableFuture<CommandReceipt> cancel(Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update([timestamp](const OrderView& value)
            {
                return commands::CancelOrder{value.userId, value.orderId, timestamp};
            }, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_ORDER_H
