#ifndef ADVMATCH_CONDITIONAL_ORDER_H
#define ADVMATCH_CONDITIONAL_ORDER_H

#include "advmatch/trading/OrderTypes.h"
#include "advmatch/trading/futures/Contract.h"
#include "advmatch/matching/orderbooks/OrderBookTypes.h"

#include <optional>

namespace advmatch
{
    enum class ConditionalOrderKind : std::uint8_t
    {
        StopLoss,
        TakeProfit
    };

    enum class TriggerPriceSource : std::uint8_t
    {
        LastTrade,
        Mark,
        Index
    };

    enum class ConditionalOrderStatus : std::uint8_t
    {
        Waiting,
        Activated,
        Rejected,
        Cancelled
    };

    enum class ConditionalOrderState : std::uint8_t
    {
        Success,
        InvalidUser,
        InvalidSymbol,
        InvalidOrder,
        InvalidTrigger,
        InvalidTimestamp,
        NotFound,
        WrongUser,
        NotWaiting,
        InvalidOco,
        Overflow,
        TradingDisabled
    };

    struct ConditionalOrderRequest
    {
        UserId userId = 0;
        SymbolId symbolId = 0;
        OrderSide side = OrderSide::Ask;
        OrderType type = OrderType::Market;
        TimeInForce timeInForce = TimeInForce::IOC;
        Price price = 0;
        Quantity quantity = 0;
        Timestamp timestamp = 0;
        ConditionalOrderKind kind = ConditionalOrderKind::StopLoss;
        TriggerPriceSource source = TriggerPriceSource::LastTrade;
        Price triggerPrice = 0;
        std::optional<ContractOrderOptions> contractOptions = std::nullopt;
        // Link to one waiting order of the same user/symbol. Acceptance of
        // either child order cancels its sibling; a risk rejection does not.
        ConditionalOrderId ocoOrderId = 0;
    };

    struct ConditionalOrderView
    {
        ConditionalOrderId conditionalOrderId = 0;
        ConditionalOrderRequest request;
        ConditionalOrderStatus status = ConditionalOrderStatus::Waiting;
        OrderId orderId = 0;
        PositionId positionId = 0;
        Timestamp updatedTimestamp = 0;
        Price triggeredPrice = 0;
        std::optional<PlaceOrderState> rejectReason = std::nullopt;
    };

    struct ConditionalOrderResult
    {
        ConditionalOrderState state = ConditionalOrderState::Success;
        ConditionalOrderId conditionalOrderId = 0;

        explicit operator bool() const noexcept
        {
            return this->state == ConditionalOrderState::Success;
        }
    };
}

#endif // ADVMATCH_CONDITIONAL_ORDER_H
