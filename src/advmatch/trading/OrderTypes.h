#ifndef ADVMATCH_ORDER_TYPES_H
#define ADVMATCH_ORDER_TYPES_H

#include "../util/ValueTypes.h"

namespace advmatch
{
    enum class OrderSide : bool
    {
        Bid = true,
        Ask = false
    };

    enum class OrderType : bool
    {
        Limit = true,
        Market = false
    };

    enum class TimeInForce : std::uint8_t
    {
        GTC,
        IOC,
        FOK
    };

    enum class OrderStatus : std::uint8_t
    {
        Rejected,
        NotFilled,
        PartiallyFilled,
        Filled,
        Cancelled
    };

    [[nodiscard]]
    constexpr bool isOrderStatusSuccess(OrderStatus status) noexcept
    {
        return status == OrderStatus::NotFilled ||
            status == OrderStatus::PartiallyFilled ||
            status == OrderStatus::Filled;
    }

    [[nodiscard]]
    constexpr bool isOrderStatusTerminal(OrderStatus status) noexcept
    {
        return status == OrderStatus::Rejected ||
            status == OrderStatus::Filled ||
            status == OrderStatus::Cancelled;
    }

    struct OrderFillingStatus
    {
        OrderStatus status;
        Quantity filled;
        Quantity unfilled;
    };
}

#endif // ADVMATCH_ORDER_TYPES_H
