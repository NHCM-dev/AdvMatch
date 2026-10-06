//
// Created by NHCM on 2026/10/6.
//

#ifndef ADVMATCH_ORDERFACTORY_H
#define ADVMATCH_ORDERFACTORY_H
#include <optional>

#include "advmatch/trading/Order.h"
#include "advmatch/trading/Symbol.h"

namespace advmatch
{
    class CreateOrderResult : public ResultContent<PlaceOrderState, BasicOrder>
    {
    public:
        explicit CreateOrderResult(PlaceOrderState state, BasicOrder order)
        : ResultContent(state, order)
        {}

        explicit CreateOrderResult(PlaceOrderState state)
        : ResultContent(state)
        {}

        explicit operator bool() const {
            return this->state == PlaceOrderState::Success;
        }
    };

    using MKTOverflowBidResult = Pair<PlaceOrderState, Balance>;

    class OrderFactory
    {
    public:
        const SymbolType symbolType;

        OrderFactory(SymbolType symbolType)
        : symbolType(symbolType)
        {}

        virtual ~OrderFactory() = default;

        [[nodiscard]]
        virtual CreateOrderResult createOrder(OrderId orderId, UserId userId, SymbolId symbolId,
                   OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
                   Price price, Quantity quantity, Timestamp timestamp) = 0;

        [[nodiscard]]
        virtual PlaceOrderState basicCheck(OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
            Price price, Quantity quantity, Timestamp timestamp) const = 0;

        [[nodiscard]]
        virtual MKTOverflowBidResult checkMKTOverflowBid(BasicOrder& order, OrderBook* orderBook) const = 0;
    };
} // advmatch

#endif //ADVMATCH_ORDERFACTORY_H
