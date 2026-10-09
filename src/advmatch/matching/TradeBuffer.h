#ifndef ADVMATCH_TRADE_BUFFER_H
#define ADVMATCH_TRADE_BUFFER_H

#include "../trading/OrderTypes.h"
#include "Trade.h"
#include "../util/Containers.h"

#include <cstddef>

namespace advmatch
{
    struct OrderNode;
    class OrderBook;
    class SettleEngine;

    class TradeBuffer
    {
        static constexpr std::size_t InlineTradeCapacity = 4;

        struct MatchedOrders
        {
            OrderNode* maker;
            OrderNode* taker;
        };

        SmallVector<Trade, InlineTradeCapacity> trades;

        // These addresses are borrowed only while SettleEngine settles the
        // current command. clearNodeReferences() removes them before the
        // buffer is returned to its caller; public access exposes Trade values.
        SmallVector<MatchedOrders, InlineTradeCapacity> matchedOrders;
        bool settledSynchronously = false;

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

        friend class OrderBook;
        friend class SettleEngine;

    public:
        using const_iterator = SmallVector<Trade, InlineTradeCapacity>::const_iterator;

        // Merge only fully settled value records; no borrowed node links survive.
        void appendSettled(const TradeBuffer& buffer)
        {
            this->trades.insert(this->trades.end(), buffer.begin(), buffer.end());
        }

        void clear() noexcept
        {
            this->trades.clear();
            this->matchedOrders.clear();
            this->settledSynchronously = false;
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
} // advmatch

#endif // ADVMATCH_TRADE_BUFFER_H
