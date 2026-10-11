//
// Created by NHCM on 2026/10/6.
//

#include "BasicOrderChecker.h"

#include "advmatch/util/Utils.h"

namespace advmatch
{
    [[nodiscard]]
    PlaceOrderState BasicOrderChecker::basicCheck(OrderSide orderSide, OrderType orderType, TimeInForce timeInForce, Price price,
                                                  Quantity quantity, Timestamp timestamp) const
    {
        if (orderSide != OrderSide::Bid && orderSide != OrderSide::Ask)
        {
            return PlaceOrderState::InvalidOrderSide;
        }
        if (orderType != OrderType::Limit && orderType != OrderType::Market)
        {
            return PlaceOrderState::InvalidOrderType;
        }
        if (timeInForce != TimeInForce::GTC && timeInForce != TimeInForce::IOC && timeInForce != TimeInForce::FOK &&
            timeInForce != TimeInForce::PostOnly)
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
    MKTOverflowBidResult BasicOrderChecker::checkMKTOverflowBid(OrderSide side, OrderType type, Quantity quantity,
                                                                const OrderBook* orderBook) const
    {
        if (type != OrderType::Market || side != OrderSide::Bid)
        {
            return {PlaceOrderState::Success, -1};
        }
        if (orderBook == nullptr)
        {
            return {PlaceOrderState::InvalidOrderBook, -1};
        }
        Balance estimatedMarketBuyBalance = -1;
        if (type == OrderType::Market && side == OrderSide::Bid)
        {
            auto cost = orderBook->estimateMarketBuyCost(quantity);
            if (!cost)
            {
                return {PlaceOrderState::Overflow, -1};
            }

            estimatedMarketBuyBalance = *cost;
        }

        return {PlaceOrderState::Success, estimatedMarketBuyBalance};
    }
} // namespace advmatch
