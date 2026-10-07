#ifndef ADVMATCH_ORDERBOOK_H
#define ADVMATCH_ORDERBOOK_H

#include "../TradeBuffer.h"
#include "../../trading/OrderTypes.h"
#include "../../util/Types.h"
#include "../../util/Utils.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace advmatch
{
    struct OrderNode;
    struct BasicOrder;

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
        Unknown
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
        UserNotFound,
        Unknown
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

    // The engine owns orders. One matching thread calls these methods.
    class OrderBook
    {
    protected:
        static void appendTrade(TradeBuffer& trades, Trade trade, OrderNode& maker, OrderNode& taker)
        {
            trades.append(trade, maker, taker);
        }

    public:
        virtual ~OrderBook() = default;

        // Read on the matching thread; callbacks must not mutate the book.
        // Prices are best first, and orders at each price are FIFO.
        virtual void forEachLevel(OrderSide side, const std::function<void(const Quote&)>& visitor) const = 0;
        virtual void forEachOrder(OrderSide side, const std::function<void(const BasicOrder&)>& visitor) const = 0;

        [[nodiscard]]
        virtual SymbolId symbolId() const noexcept = 0;

        virtual PlaceOrderResult place(OrderNode& order, Timestamp timestamp, TradeBuffer& trades) = 0;
        virtual ModifyOrderResult modifyPrice(OrderNode& order, Price price, Timestamp timestamp,
                                              TradeBuffer& trades) = 0;
        virtual ModifyOrderResult modifyQuantity(OrderNode& order, QuantityChange change, Timestamp timestamp) = 0;
        virtual CancelOrderResult cancel(OrderNode& order, Timestamp timestamp) = 0;

        [[nodiscard]]
        virtual std::optional<Quote> bestBid() const noexcept = 0;
        [[nodiscard]]
        virtual std::optional<Quote> bestAsk() const noexcept = 0;

        [[nodiscard]]
        virtual std::optional<Quote> bidAt(Price price) const noexcept = 0;
        [[nodiscard]]
        virtual std::optional<Quote> askAt(Price price) const noexcept = 0;

        [[nodiscard]]
        virtual std::optional<Balance> estimateMarketBuyCost(Quantity quantity) const noexcept = 0;
    };
} // advmatch

#endif // ADVMATCH_ORDERBOOK_H
