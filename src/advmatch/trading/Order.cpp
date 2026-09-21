//
// Created by NHCM on 2026/9/20.
//

#include "Order.h"
#include <stdexcept>

namespace advmatch
{
    BasicOrder::BasicOrder(OrderId orderId, UserId userId, SymbolId symbolId,
                           OrderSide orderSide, OrderType orderType, TimeInForce timeInForce,
                           Price price, Quantity quantity, Timestamp timestamp, Sequence sequence)
        : price(orderType == OrderType::Market ? 0 : price), requestedQuantity(quantity),
          fillingStatus{OrderStatus::NotFilled, 0, quantity}, createdTimestamp(timestamp), updatedTimestamp(timestamp),
          firstFilledTimestamp(0), lastFilledTimestamp(0), completedTimestamp(0), createdSequence(sequence),
          updatedSequence(sequence), orderId(orderId), userId(userId), symbolId(symbolId), orderSide(orderSide),
          orderType(orderType), timeInForce(timeInForce)
    {
        if (util::willMultiplyOverflow(price, quantity))
        {
            throw std::invalid_argument("price * quantity will overflow");
        }
    }

    ModifyOrderResult BasicOrder::changePrice(Price newPrice,
                                              Timestamp timestamp,
                                              Sequence sequence)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                ModifyOrderState::OrderAlreadyFilled,
                "Filled order cannot be modified"};
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                ModifyOrderState::OrderCancelled,
                "Cancelled order cannot be modified"};
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                ModifyOrderState::OrderRejected,
                "Rejected order cannot be modified"};
        }

        if (this->orderType != OrderType::Limit)
        {
            return {
                ModifyOrderState::InvalidOrderType,
                "Market order price cannot be modified"};
        }

        if (newPrice == 0)
        {
            return {
                ModifyOrderState::InvalidPrice,
                "Limit order price cannot be zero"};
        }

        this->price = newPrice;

        this->updatedTimestamp = timestamp;
        this->updatedSequence = sequence;

        return {ModifyOrderState::Success};
    }

    ModifyOrderResult BasicOrder::changeQuantity(QuantityChange quantityChange,
                                                 Timestamp timestamp,
                                                 Sequence sequence)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                ModifyOrderState::OrderAlreadyFilled,
                "Filled order cannot be modified"};
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                ModifyOrderState::OrderCancelled,
                "Cancelled order cannot be modified"};
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                ModifyOrderState::OrderRejected,
                "Rejected order cannot be modified"};
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
                    "Quantity change exceeds unfilled quantity"};
            }

            this->requestedQuantity -= subtractQuantity;
            this->fillingStatus.unfilled -= subtractQuantity;

            this->updatedTimestamp = timestamp;
            this->updatedSequence = sequence;

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
                "Requested quantity will overflow"};
        }

        if (util::willAddOverflow(this->fillingStatus.unfilled, addQuantity))
        {
            return {
                ModifyOrderState::Overflow,
                "Unfilled quantity will overflow"};
        }

        this->requestedQuantity += addQuantity;
        this->fillingStatus.unfilled += addQuantity;

        this->updatedTimestamp = timestamp;
        this->updatedSequence = sequence;

        return {ModifyOrderState::Success};
    }

    CancelOrderResult BasicOrder::cancel(Timestamp timestamp,
                                         Sequence sequence)
    {
        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                CancelOrderState::OrderAlreadyFilled,
                "Filled order cannot be cancelled"};
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                CancelOrderState::OrderCancelled,
                "Order is already cancelled"};
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                CancelOrderState::OrderRejected,
                "Rejected order cannot be cancelled"};
        }

        this->fillingStatus.status = OrderStatus::Cancelled;

        this->updatedTimestamp = timestamp;
        this->completedTimestamp = timestamp;
        this->updatedSequence = sequence;

        return {CancelOrderState::Success};
    }

    OrderFillResult BasicOrder::fill(Quantity fillQuantity,
                                     Timestamp timestamp,
                                     Sequence sequence)
    {
        if (fillQuantity == 0)
        {
            return {
                OrderFillState::InvalidQuantity,
                "Fill quantity cannot be zero"};
        }

        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                OrderFillState::OrderAlreadyFilled,
                "Order is already filled"};
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                OrderFillState::OrderCancelled,
                "Cancelled order cannot be filled"};
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                OrderFillState::OrderRejected,
                "Rejected order cannot be filled"};
        }

        if (fillQuantity > this->fillingStatus.unfilled)
        {
            return {
                OrderFillState::ExceedsRemainingQuantity,
                "Fill quantity exceeds unfilled quantity"};
        }

        if (util::willAddOverflow(this->fillingStatus.filled, fillQuantity))
        {
            return {
                OrderFillState::Overflow,
                "Filled quantity will overflow"};
        }

        if (this->fillingStatus.filled == 0)
        {
            this->firstFilledTimestamp = timestamp;
        }

        this->fillingStatus.filled += fillQuantity;
        this->fillingStatus.unfilled -= fillQuantity;

        this->lastFilledTimestamp = timestamp;
        this->updatedTimestamp = timestamp;
        this->updatedSequence = sequence;

        if (this->fillingStatus.unfilled == 0)
        {
            this->fillingStatus.status = OrderStatus::Filled;
            this->completedTimestamp = timestamp;
        }
        else
        {
            this->fillingStatus.status = OrderStatus::PartiallyFilled;
        }

        return {OrderFillState::Success};
    }



    Price BasicOrder::getPrice() const noexcept
    {
        return this->price;
    }
    Quantity BasicOrder::getQuantity() const noexcept
    {
        return this->requestedQuantity;
    }



    Quantity BasicOrder::getFilledQuantity() const noexcept
    {
        return this->fillingStatus.filled;
    }
    Quantity BasicOrder::getUnfilledQuantity() const noexcept
    {
        return this->fillingStatus.unfilled;
    }



    OrderStatus BasicOrder::getStatus() const noexcept
    {
        return this->fillingStatus.status;
    }



    const OrderFillingStatus& BasicOrder::getFillingStatus() const noexcept
    {
        return this->fillingStatus;
    }



    Timestamp BasicOrder::getCreatedTimestamp() const noexcept
    {
        return this->createdTimestamp;
    }
    Timestamp BasicOrder::getUpdatedTimestamp() const noexcept
    {
        return this->updatedTimestamp;
    }
    Timestamp BasicOrder::getFirstFilledTimestamp() const noexcept
    {
        return this->firstFilledTimestamp;
    }
    Timestamp BasicOrder::getLastFilledTimestamp() const noexcept
    {
        return this->lastFilledTimestamp;
    }
    Timestamp BasicOrder::getCompletedTimestamp() const noexcept
    {
        return this->completedTimestamp;
    }



    Sequence BasicOrder::getCreatedSequence() const noexcept
    {
        return this->createdSequence;
    }
    Sequence BasicOrder::getUpdatedSequence() const noexcept
    {
        return this->updatedSequence;
    }
} // advmatch
