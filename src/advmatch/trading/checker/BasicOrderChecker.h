#ifndef ADVMATCH_BASICORDERCHECKER_H
#define ADVMATCH_BASICORDERCHECKER_H

#include "advmatch/matching/orderbooks/OrderBook.h"
#include "advmatch/trading/OrderTypes.h"

namespace advmatch
{
    using MKTOverflowBidResult = Pair<PlaceOrderState, Balance>;

    class BasicOrderChecker
    {
    public:
        [[nodiscard]]
        PlaceOrderState basicCheck(OrderSide orderSide, OrderType orderType, TimeInForce timeInForce, Price price, Quantity quantity,
                                   Timestamp timestamp) const;

        [[nodiscard]]
        MKTOverflowBidResult checkMKTOverflowBid(OrderSide side, OrderType type, Quantity quantity, const OrderBook* orderBook) const;
    };
} // namespace advmatch

#endif // ADVMATCH_BASICORDERCHECKER_H
