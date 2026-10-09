#ifndef ADVMATCH_ORDERBOOK_H
#define ADVMATCH_ORDERBOOK_H

#include "../TradeBuffer.h"
#include "OrderBookTypes.h"
#include "../../trading/OrderTypes.h"
#include "../../util/Types.h"
#include "../../util/Utils.h"
#include "advmatch/observe/OrderBookListener.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace advmatch
{
    struct OrderNode;
    struct BasicOrder;
    struct PriceLevel;

    // Optional execution policy. The book still owns only price/time matching;
    // contract risk/settlement is supplied by the owning engine.
    class OrderBook;

    class MatchPolicy
    {
    public:
        virtual ~MatchPolicy() = default;
        virtual bool canFillAll(OrderNode& taker, const OrderBook& book) = 0;
        virtual MatchAction prepare(OrderNode& maker, OrderNode& taker, Price price, Quantity& quantity) = 0;
        virtual void commit(const Trade& trade, OrderNode& maker, OrderNode& taker, Timestamp timestamp) = 0;
        virtual void cancelled(OrderNode& node) = 0;
    };

    // The engine owns orders. One matching thread calls these methods.
    class OrderBook
    {
        friend class observe::TradingObserver;
        friend class MatchingEngine;
        mutable observe::OrderBookListener* observationListener = nullptr;

        virtual const PriceLevel* observedBestLevel(OrderSide side) const noexcept = 0;
        virtual const PriceLevel* observedNextLevel(OrderSide side, Price price) const noexcept = 0;
        virtual std::size_t observedLevelCount(OrderSide side) const noexcept = 0;

    protected:
        void notifyRested(const OrderNode& node) noexcept
        {
            if (this->observationListener != nullptr)
            {
                this->observationListener->onRested(node);
            }
        }

        void notifyRemoved(const OrderNode& node) noexcept
        {
            if (this->observationListener != nullptr)
            {
                this->observationListener->onRemoved(node);
            }
        }

        static void appendTrade(TradeBuffer& trades, Trade trade, OrderNode& maker, OrderNode& taker)
        {
            trades.append(trade, maker, taker);
        }

    public:
        virtual ~OrderBook() = default;

        // Reserve the expected live price levels before trading. Algorithms
        // have bounded index work; allocator and OS latency remain unbounded.
        virtual void reservePriceLevels(std::size_t, std::size_t)
        {
        }

        // Read on the matching thread; callbacks must not mutate the book.
        // Prices are best first, and orders at each price are FIFO.
        virtual void forEachLevel(OrderSide side, const std::function<void(const Quote&)>& visitor) const = 0;
        virtual void forEachOrderWhile(OrderSide side, const std::function<bool(const BasicOrder&)>& visitor) const = 0;
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
