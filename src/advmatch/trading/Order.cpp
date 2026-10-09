//
// Created by NHCM on 2026/9/20.
//

#include "Order.h"
#include <stdexcept>

namespace advmatch
{
    BasicOrder::BasicOrder(OrderId orderId, UserId userId, SymbolId symbolId,
                           OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
                           Price price, Quantity quantity, Timestamp timestamp)
        : price(orderType == OrderType::Market ? 0 : price), requestedQuantity(quantity),
          fillingStatus{OrderStatus::NotFilled, 0, quantity}, orderId(orderId), userId(userId), symbolId(symbolId),
          orderSide(orderSide), orderType(orderType), timeInForce(timeInForce), createdTimestamp(timestamp),
          updatedTimestamp(timestamp), firstFilledTimestamp(0), lastFilledTimestamp(0), completedTimestamp(0)
    {
    }

    ModifyOrderResult BasicOrder::changePrice(Price newPrice, Timestamp timestamp)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                ModifyOrderState::OrderAlreadyFilled,
                "Filled order cannot be modified"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                ModifyOrderState::OrderCancelled,
                "Cancelled order cannot be modified"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                ModifyOrderState::OrderRejected,
                "Rejected order cannot be modified"
            };
        }

        if (this->orderType != OrderType::Limit)
        {
            return {
                ModifyOrderState::InvalidOrderType,
                "Market order price cannot be modified"
            };
        }

        if (newPrice == 0)
        {
            return {
                ModifyOrderState::InvalidPrice,
                "Limit order price cannot be zero"
            };
        }

        this->price = newPrice;

        this->updatedTimestamp = timestamp;

        return {ModifyOrderState::Success};
    }

    ModifyOrderResult BasicOrder::changeQuantity(QuantityChange quantityChange, Timestamp timestamp)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                ModifyOrderState::OrderAlreadyFilled,
                "Filled order cannot be modified"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                ModifyOrderState::OrderCancelled,
                "Cancelled order cannot be modified"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                ModifyOrderState::OrderRejected,
                "Rejected order cannot be modified"
            };
        }

        if (quantityChange == 0)
        {
            return {ModifyOrderState::Success};
        }

        if (quantityChange < 0)
        {
            const Quantity subtractQuantity =
                static_cast<Quantity>(-(quantityChange + 1)) + 1;

            if (subtractQuantity > this->fillingStatus.unfilled)
            {
                return {
                    ModifyOrderState::InsufficientQuantity,
                    "Quantity change exceeds unfilled quantity"
                };
            }

            this->requestedQuantity -= subtractQuantity;
            this->fillingStatus.unfilled -= subtractQuantity;

            this->updatedTimestamp = timestamp;

            if (this->fillingStatus.unfilled == 0)
            {
                this->fillingStatus.status = OrderStatus::Cancelled;
                this->completedTimestamp = timestamp;
            }

            return {ModifyOrderState::Success};
        }

        const auto addQuantity =
            static_cast<Quantity>(quantityChange);

        if (util::willAddOverflow(this->requestedQuantity, addQuantity))
        {
            return {
                ModifyOrderState::Overflow,
                "Requested quantity will overflow"
            };
        }

        if (util::willAddOverflow(this->fillingStatus.unfilled, addQuantity))
        {
            return {
                ModifyOrderState::Overflow,
                "Unfilled quantity will overflow"
            };
        }

        this->requestedQuantity += addQuantity;
        this->fillingStatus.unfilled += addQuantity;

        this->updatedTimestamp = timestamp;

        return {ModifyOrderState::Success};
    }

    CancelOrderResult BasicOrder::cancel(Timestamp timestamp)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                CancelOrderState::OrderAlreadyFilled,
                "Filled order cannot be cancelled"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                CancelOrderState::OrderCancelled,
                "Order is already cancelled"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                CancelOrderState::OrderRejected,
                "Rejected order cannot be cancelled"
            };
        }

        this->fillingStatus.status = OrderStatus::Cancelled;

        this->updatedTimestamp = timestamp;
        this->completedTimestamp = timestamp;

        return {CancelOrderState::Success};
    }

} // advmatch
