#ifndef ADVMATCH_ORDERBOOK_H
#define ADVMATCH_ORDERBOOK_H

#include "../../trading/OrderTypes.h"
#include "../../trading/Symbol.h"
#include "../../util/Containers.h"
#include "../../util/Utils.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace advmatch
{
    struct OrderNode;

    enum class PlaceOrderRejectReason : std::uint8_t
    {
        InvalidQuantity,
        InvalidPrice,
        InsufficientBalance,
        InsufficientLiquidity,
        InvalidSymbol,
        InvalidOrderType,
        InvalidTimeInForce,
        UserNotFound,
        TradingDisabled,
        DuplicateOrderId,
        Overflow,
        Unknown
    };

    struct PlaceOrderResult : Result<OrderFillingStatus>
    {
        PlaceOrderResult(OrderId orderId, OrderFillingStatus status)
        : Result(status), orderId(orderId), rejectReason(std::nullopt)
        {}

        PlaceOrderResult(OrderId orderId, OrderFillingStatus status, PlaceOrderRejectReason reason)
        : Result(status), orderId(orderId), rejectReason(reason)
        {}

        const OrderId orderId;
        const std::optional<PlaceOrderRejectReason> rejectReason;

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
        {}
        ModifyOrderResult(ModifyOrderState state, std::string message)
        : Result(state, std::move(message))
        {}

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
        {}
        CancelOrderResult(CancelOrderState state, std::string message)
        : Result(state, std::move(message))
        {}

        explicit operator bool() const noexcept
        {
            return state == CancelOrderState::Success;
        }
    };

    struct Trade
    {
        OrderId makerOrderId;
        OrderId takerOrderId;
        Price price;
        Quantity quantity;
        Sequence sequence;
    };

    class TradeBuffer
    {
        static constexpr std::size_t InlineTradeCapacity = 4;

        struct MatchedOrders
        {
            OrderNode* maker;
            OrderNode* taker;
        };

        SmallVector<Trade, InlineTradeCapacity> trades;
        // These addresses are borrowed only while MatchingEngine settles the
        // current command. clearNodeReferences() removes them before the
        // buffer is returned to its caller; public access exposes Trade values.
        SmallVector<MatchedOrders, InlineTradeCapacity> matchedOrders;

        void append(Trade trade, OrderNode& maker, OrderNode& taker)
        {
            this->trades.push_back(trade);
            try
            {
                this->matchedOrders.push_back({&maker, &taker});
            }
            catch (...)
            {
                this->trades.pop_back();
                throw;
            }
        }

        [[nodiscard]]
        OrderNode& maker(std::size_t index) const noexcept
        {
            return *this->matchedOrders[index].maker;
        }

        [[nodiscard]]
        OrderNode& taker(std::size_t index) const noexcept
        {
            return *this->matchedOrders[index].taker;
        }

        void clearNodeReferences() noexcept
        {
            this->matchedOrders.clear();
        }

        friend class MatchingEngine;
        friend class SpotOrderBook;

    public:
        using const_iterator = typename SmallVector<Trade, InlineTradeCapacity>::const_iterator;

        void clear() noexcept
        {
            this->trades.clear();
            this->matchedOrders.clear();
        }

        [[nodiscard]]
        bool empty() const noexcept
        {
            return this->trades.empty();
        }

        [[nodiscard]]
        std::size_t size() const noexcept
        {
            return this->trades.size();
        }

        [[nodiscard]]
        std::size_t capacity() const noexcept
        {
            return this->trades.capacity();
        }

        [[nodiscard]]
        const Trade& operator[](std::size_t index) const noexcept
        {
            return this->trades[index];
        }

        [[nodiscard]]
        const Trade* data() const noexcept
        {
            return this->trades.data();
        }

        [[nodiscard]]
        const_iterator begin() const noexcept
        {
            return this->trades.begin();
        }

        [[nodiscard]]
        const_iterator end() const noexcept
        {
            return this->trades.end();
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
    public:
        virtual ~OrderBook() = default;

        [[nodiscard]]
        virtual SymbolId symbolId() const noexcept = 0;

        virtual PlaceOrderResult place(OrderNode& order, Timestamp timestamp,
            Sequence sequence, TradeBuffer& trades) = 0;
        virtual ModifyOrderResult modifyPrice(OrderNode& order, Price price, Timestamp timestamp,
            Sequence sequence, TradeBuffer& trades) = 0;
        virtual ModifyOrderResult modifyQuantity(OrderNode& order, QuantityChange change, Timestamp timestamp,
            Sequence sequence) = 0;
        virtual CancelOrderResult cancel(OrderNode& order, Timestamp timestamp, Sequence sequence) = 0;

        [[nodiscard]]
        virtual std::optional<Quote> bestBid() const noexcept = 0;
        [[nodiscard]]
        virtual std::optional<Quote> bestAsk() const noexcept = 0;

        [[nodiscard]]
        virtual std::optional<Quote> bidAt(Price price) const noexcept = 0;
        [[nodiscard]]
        virtual std::optional<Quote> askAt(Price price) const noexcept = 0;
    };
} // advmatch

#endif // ADVMATCH_ORDERBOOK_H
