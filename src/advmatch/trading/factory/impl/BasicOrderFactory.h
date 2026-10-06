//
// Created by NHCM on 2026/10/6.
//

#ifndef ADVMATCH_SPOTORDERFACTORY_H
#define ADVMATCH_SPOTORDERFACTORY_H
#include "advmatch/trading/factory/OrderFactory.h"

namespace advmatch
{
    class BasicOrderFactory : public OrderFactory
    {
    public:
        BasicOrderFactory()
        : OrderFactory(SymbolType::Spot)
        {}

        [[nodiscard]]
        CreateOrderResult createOrder(OrderId orderId, UserId userId, SymbolId symbolId,
                   OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
                   Price price, Quantity quantity, Timestamp timestamp) override;

        [[nodiscard]]
        PlaceOrderState basicCheck(OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
            Price price, Quantity quantity, Timestamp timestamp) const override;

        [[nodiscard]]
        MKTOverflowBidResult checkMKTOverflowBid(BasicOrder& order, OrderBook* orderBook) const override;
    };
} // advmatch

#endif //ADVMATCH_SPOTORDERFACTORY_H
