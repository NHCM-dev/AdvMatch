#ifndef ADVMATCH_ORDER_ACCESS_H
#define ADVMATCH_ORDER_ACCESS_H

// Hot accessors and checked fill operations remain visible to optimizers even
// when clients link the static library without link-time optimization.
namespace advmatch
{
    inline OrderFillResult BasicOrder::fill(Quantity fillQuantity, Timestamp timestamp)
    {
        if (fillQuantity == 0)
        {
            return {
                OrderFillState::InvalidQuantity,
                "Fill quantity cannot be zero"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Filled)
        {
            return {
                OrderFillState::OrderAlreadyFilled,
                "Order is already filled"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Cancelled)
        {
            return {
                OrderFillState::OrderCancelled,
                "Cancelled order cannot be filled"
            };
        }

        if (this->fillingStatus.status == OrderStatus::Rejected)
        {
            return {
                OrderFillState::OrderRejected,
                "Rejected order cannot be filled"
            };
        }

        if (fillQuantity > this->fillingStatus.unfilled)
        {
            return {
                OrderFillState::ExceedsRemainingQuantity,
                "Fill quantity exceeds unfilled quantity"
            };
        }

        if (util::willAddOverflow(this->fillingStatus.filled, fillQuantity))
        {
            return {
                OrderFillState::Overflow,
                "Filled quantity will overflow"
            };
        }

        if (this->fillingStatus.filled == 0)
        {
            this->firstFilledTimestamp = timestamp;
        }

        this->fillingStatus.filled += fillQuantity;
        this->fillingStatus.unfilled -= fillQuantity;

        this->lastFilledTimestamp = timestamp;
        this->updatedTimestamp = timestamp;

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


    inline Price BasicOrder::getPrice() const noexcept
    {
        return this->price;
    }

    inline Quantity BasicOrder::getQuantity() const noexcept
    {
        return this->requestedQuantity;
    }


    inline Quantity BasicOrder::getFilledQuantity() const noexcept
    {
        return this->fillingStatus.filled;
    }

    inline Quantity BasicOrder::getUnfilledQuantity() const noexcept
    {
        return this->fillingStatus.unfilled;
    }


    inline OrderStatus BasicOrder::getStatus() const noexcept
    {
        return this->fillingStatus.status;
    }


    inline const OrderFillingStatus& BasicOrder::getFillingStatus() const noexcept
    {
        return this->fillingStatus;
    }


    inline Timestamp BasicOrder::getCreatedTimestamp() const noexcept
    {
        return this->createdTimestamp;
    }

    inline Timestamp BasicOrder::getUpdatedTimestamp() const noexcept
    {
        return this->updatedTimestamp;
    }

    inline Timestamp BasicOrder::getFirstFilledTimestamp() const noexcept
    {
        return this->firstFilledTimestamp;
    }

    inline Timestamp BasicOrder::getLastFilledTimestamp() const noexcept
    {
        return this->lastFilledTimestamp;
    }

    inline Timestamp BasicOrder::getCompletedTimestamp() const noexcept
    {
        return this->completedTimestamp;
    }
}

#endif // ADVMATCH_ORDER_ACCESS_H
