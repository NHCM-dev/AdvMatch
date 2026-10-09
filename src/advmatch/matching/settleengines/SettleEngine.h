#ifndef ADVMATCH_SETTLE_ENGINE_H
#define ADVMATCH_SETTLE_ENGINE_H

#include "../TradeBuffer.h"

namespace advmatch
{
    class SettleEngine
    {
    protected:
        static bool settledSynchronously(const TradeBuffer& trades) noexcept
        {
            return trades.settledSynchronously;
        }

        [[nodiscard]]
        static OrderNode& maker(const TradeBuffer& trades, std::size_t index) noexcept
        {
            return trades.maker(index);
        }

        [[nodiscard]]
        static OrderNode& taker(const TradeBuffer& trades, std::size_t index) noexcept
        {
            return trades.taker(index);
        }

        static void clearNodeReferences(TradeBuffer& trades) noexcept
        {
            trades.clearNodeReferences();
        }

        virtual void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker) = 0;

    public:
        virtual ~SettleEngine() = default;

        virtual void settleTrades(TradeBuffer& trades) = 0;
        virtual void reconcileReservation(OrderNode& order) = 0;
        virtual void releaseReservation(OrderNode& order) = 0;
    };
} // advmatch

#endif // ADVMATCH_SETTLE_ENGINE_H
