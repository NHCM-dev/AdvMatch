#ifndef ADVMATCH_SPOT_SETTLE_ENGINE_H
#define ADVMATCH_SPOT_SETTLE_ENGINE_H

#include "../SettleEngine.h"
#include "advmatch/trading/Asset.h"

namespace advmatch
{
    class SpotSettleEngine final : public SettleEngine
    {
        [[nodiscard]]
        static Balance requiredReservationBalance(const OrderNode& node);

        void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker) override;

    public:
        void settleTrades(TradeBuffer& trades) override;
        void reconcileReservation(OrderNode& order) override;
        void releaseReservation(OrderNode& order) override;
    };
} // advmatch

#endif // ADVMATCH_SPOT_SETTLE_ENGINE_H
