#ifndef ADVMATCH_ORDER_BOOK_TYPES_H
#define ADVMATCH_ORDER_BOOK_TYPES_H

#include "advmatch/trading/OrderTypes.h"
#include "advmatch/util/Result.h"

namespace advmatch
{
    enum class PlaceOrderState : std::uint8_t
    {
        Success,
        InvalidOrderSide,
        InvalidQuantity,
        InvalidPrice,
        InsufficientBalance,
        InsufficientLiquidity,
        InvalidSymbol,
        InvalidOrderType,
        InvalidTimeInForce,
        InvalidTimestamp,
        InvalidOrderBook,
        UserNotFound,
        TradingDisabled,
        DuplicateOrderId,
        Overflow,
        OrderIdOverflow,
        InvalidLeverage,
        InvalidMarginMode,
        ReduceOnlyViolation,
        PositionNotFound,
        ContractExpired,
        FundingRequired,
        Liquidating,
        Unknown,
        InvalidPriceTick,
        InvalidQuantityStep,
        MinimumNotional,
        RiskLimitExceeded,
        PostOnlyWouldTrade
    };

    struct PlaceOrderResult : Result<OrderFillingStatus>
    {
        PlaceOrderResult(OrderId orderId, OrderFillingStatus status)
            : Result(status), orderId(orderId), rejectReason(std::nullopt)
        {
        }

        PlaceOrderResult(OrderId orderId, OrderFillingStatus status, PlaceOrderState reason)
            : Result(status), orderId(orderId), rejectReason(reason)
        {
        }

        const OrderId orderId;
        PositionId positionId = 0;
        const std::optional<PlaceOrderState> rejectReason;

        explicit operator bool() const noexcept
        {
            return state.status != OrderStatus::Rejected && !rejectReason;
        }
    };

    enum class ModifyOrderOP : std::uint8_t
    {
        Price,
        Quantity
    };

    enum class ModifyOrderState : std::uint8_t
    {
        Success,
        OrderNotFound,
        WrongUser,
        InvalidPrice,
        InvalidQuantity,
        InvalidOrderType,
        InvalidSymbol,
        InsufficientQuantity,
        InsufficientBalance,
        Overflow,
        OrderAlreadyFilled,
        OrderCancelled,
        OrderRejected,
        TradingDisabled,
        ReduceOnlyViolation,
        UserNotFound,
        Unknown,
        InvalidPriceTick,
        InvalidQuantityStep,
        MinimumNotional,
        RiskLimitExceeded,
        PostOnlyWouldTrade
    };

    struct ModifyOrderResult : Result<ModifyOrderState>
    {
        ModifyOrderResult(ModifyOrderState state)
            : Result(state)
        {
        }

        ModifyOrderResult(ModifyOrderState state, std::string message)
            : Result(state, std::move(message))
        {
        }

        explicit operator bool() const noexcept
        {
            return state == ModifyOrderState::Success;
        }
    };

    enum class CancelOrderState : std::uint8_t
    {
        Success,
        OrderNotFound,
        WrongUser,
        OrderAlreadyFilled,
        OrderCancelled,
        OrderRejected,
        Unknown
    };

    struct CancelOrderResult : Result<CancelOrderState>
    {
        CancelOrderResult(CancelOrderState state)
            : Result(state)
        {
        }

        CancelOrderResult(CancelOrderState state, std::string message)
            : Result(state, std::move(message))
        {
        }

        explicit operator bool() const noexcept
        {
            return state == CancelOrderState::Success;
        }
    };

    struct Quote
    {
        Price price;
        Quantity quantity;
        std::uint32_t orderCount;
    };

    enum class MatchAction : std::uint8_t
    {
        Execute,
        CancelMaker,
        CancelTaker,
        CancelBoth
    };

    enum class OrderBookImplementation : std::uint8_t
    {
        Radix,
        IndexedTree
    };

}

#endif // ADVMATCH_ORDER_BOOK_TYPES_H
