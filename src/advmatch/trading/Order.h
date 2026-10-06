//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_ORDER_H
#define ADVMATCH_ORDER_H

#include <string>
#include <utility>

#include "../util/Types.h"
#include "OrderTypes.h"
#include "../matching/orderbooks/OrderBook.h"
#include "../util/Utils.h"

namespace advmatch
{
    enum class OrderFillState : uint8_t
    {
        Success,
        InvalidQuantity,
        ExceedsRemainingQuantity,
        OrderAlreadyFilled,
        OrderCancelled,
        OrderRejected,
        Overflow,
        Unknown
    };

    struct OrderFillResult : Result<OrderFillState>
    {
        OrderFillResult(OrderFillState result)
            : Result(result)
        {
        }

        OrderFillResult(OrderFillState result, std::string errorMessage)
            : Result(result, std::move(errorMessage))
        {
        }

        explicit operator bool() const noexcept
        {
            return this->state == OrderFillState::Success;
        }
    };

    struct BasicOrder
    {
    private:
        Price price;
        Quantity requestedQuantity;
        OrderFillingStatus fillingStatus;

        const Timestamp createdTimestamp;
        Timestamp updatedTimestamp;

        Timestamp firstFilledTimestamp;
        Timestamp lastFilledTimestamp;
        Timestamp completedTimestamp;

        BasicOrder(OrderId orderId, UserId userId, SymbolId symbolId,
                   OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
                   Price price, Quantity quantity, Timestamp timestamp);

        friend class BasicOrderFactory;
    public:
        const OrderId orderId;
        const UserId userId;
        const SymbolId symbolId;

        const OrderSide orderSide;
        const OrderType orderType;
        const TimeInForce timeInForce;

        [[nodiscard]]
        ModifyOrderResult changePrice(Price newPrice, Timestamp timestamp);

        [[nodiscard]]
        ModifyOrderResult changeQuantity(QuantityChange quantityChange, Timestamp timestamp);

        [[nodiscard]]
        CancelOrderResult cancel(Timestamp timestamp);

        [[nodiscard]]
        OrderFillResult fill(Quantity fillQuantity, Timestamp timestamp);

        [[nodiscard]]
        Price getPrice() const noexcept;
        [[nodiscard]]
        Quantity getQuantity() const noexcept;

        [[nodiscard]]
        Quantity getFilledQuantity() const noexcept;
        [[nodiscard]]
        Quantity getUnfilledQuantity() const noexcept;

        [[nodiscard]]
        OrderStatus getStatus() const noexcept;
        [[nodiscard]]
        const OrderFillingStatus& getFillingStatus() const noexcept;

        [[nodiscard]]
        Timestamp getCreatedTimestamp() const noexcept;
        [[nodiscard]]
        Timestamp getUpdatedTimestamp() const noexcept;
        [[nodiscard]]
        Timestamp getFirstFilledTimestamp() const noexcept;
        [[nodiscard]]
        Timestamp getLastFilledTimestamp() const noexcept;
        [[nodiscard]]
        Timestamp getCompletedTimestamp() const noexcept;
    };
} // advmatch

#endif //ADVMATCH_ORDER_H
