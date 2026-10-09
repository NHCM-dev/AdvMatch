#ifndef ADVMATCH_SDK_ORDER_BOOK_H
#define ADVMATCH_SDK_ORDER_BOOK_H

#include "Order.h"
#include "User.h"

namespace advmatch::sdk
{
    class OrderBook final : public Entity<BookView, SymbolId, ReadKind::Book>
    {
    public:
        using Entity::Entity;

        CompletableFuture<Order> place(const User& user, OrderSide side, OrderType type, TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, std::optional<ContractOrderOptions> options = std::nullopt, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<Order>([&]
            {
                user.requireOwner(this->api);
                return detail::Results::created<Order>(this->api, commands::PlaceOrder{user.id(), this->id(), side, type, timeInForce, price, quantity, timestamp, options}, requestId);
            });
        }

        CompletableFuture<ReadResponse<DepthView>> depth(OrderSide side, std::size_t limit = 20, ObservationQuery query = {}) const
        {
            query.kind = ReadKind::Depth;
            query.symbolId = this->id();
            query.side = side;
            query.limit = limit;
            return this->api->readRows<DepthView>(std::move(query));
        }

        CompletableFuture<ReadResponse<DepthView>> level(OrderSide side, Price price) const
        {
            ObservationQuery query;
            query.kind = ReadKind::Level;
            query.symbolId = this->id();
            query.side = side;
            query.price = price;
            return this->api->readRows<DepthView>(query);
        }

        CompletableFuture<ReadResponse<OrderView>> orders(OrderSide side, ObservationQuery query = {}) const
        {
            query.kind = ReadKind::BookOrders;
            query.symbolId = this->id();
            query.side = side;
            return this->api->readRows<OrderView>(std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_ORDER_BOOK_H
