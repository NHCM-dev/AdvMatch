//
// Created by NHCM on 2026/10/6.
//

#include "BasicOrderFactory.h"

#include "advmatch/trading/Asset.h"

namespace advmatch
{
    CreateOrderResult BasicOrderFactory::createOrder(OrderId orderId, UserId userId, SymbolId symbolId,
        OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
        Price price, Quantity quantity, Timestamp timestamp)
    {
        PlaceOrderState state = basicCheck(orderSide, orderType, timeInForce, price, quantity, timestamp);
        if (state != PlaceOrderState::Success)
        {
            return CreateOrderResult(state);
        }
        BasicOrder order = BasicOrder(orderId, userId, symbolId, orderSide, orderType, timeInForce, price, quantity, timestamp);
        return CreateOrderResult(state, order);
    }

    [[nodiscard]]
    PlaceOrderState BasicOrderFactory::basicCheck(OrderSide orderSide, OrderType orderType, TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp) const
    {
        if (orderSide != OrderSide::Bid && orderSide != OrderSide::Ask)
        {
            return PlaceOrderState::InvalidOrderSide;
        }
        if (orderType != OrderType::Limit && orderType != OrderType::Market)
        {
            return PlaceOrderState::InvalidOrderType;
        }
        if (timeInForce != TimeInForce::GTC && timeInForce != TimeInForce::IOC && timeInForce != TimeInForce::FOK && timeInForce != TimeInForce::PostOnly)
        {
            return PlaceOrderState::InvalidTimeInForce;
        }
        if (orderType == OrderType::Market && (timeInForce == TimeInForce::GTC || timeInForce == TimeInForce::PostOnly))
        {
            return PlaceOrderState::InvalidTimeInForce;
        }
        if (orderType == OrderType::Limit && price == 0)
        {
            return PlaceOrderState::InvalidPrice;
        }
        if (quantity == 0)
        {
            return PlaceOrderState::InvalidQuantity;
        }
        if (util::willMultiplyOverflow(price, quantity))
        {
            return PlaceOrderState::Overflow;
        }
        if (util::getCurrentTime() < timestamp)
        {
            return PlaceOrderState::InvalidTimestamp;
        }
        return PlaceOrderState::Success;
    }

    [[nodiscard]]
    MKTOverflowBidResult BasicOrderFactory::checkMKTOverflowBid(BasicOrder& order, OrderBook* orderBook) const
    {
        if (order.orderType != OrderType::Market || order.orderSide != OrderSide::Bid)
        {
            return {PlaceOrderState::Success, -1};
        }
        if (orderBook == nullptr)
        {
            return {PlaceOrderState::InvalidOrderBook, -1};
        }
        Balance estimatedMarketBuyBalance = -1;
        if (order.orderType == OrderType::Market && order.orderSide == OrderSide::Bid)
        {
            auto cost = orderBook->estimateMarketBuyCost(order.getQuantity());
            if (!cost)
            {
                return {PlaceOrderState::Overflow, -1};
            }

            estimatedMarketBuyBalance = *cost;
        }

        return {PlaceOrderState::Success, estimatedMarketBuyBalance};
    }
} // advmatch
