#ifndef ADVMATCH_SELF_TRADE_POLICY_H
#define ADVMATCH_SELF_TRADE_POLICY_H

#include "advmatch/matching/Matching.h"

namespace advmatch
{
    struct SelfTradePolicy
    {
        static MatchAction prepare(const OrderNode& maker, const OrderNode& taker) noexcept
        {
            if (maker.basicOrder.userId != taker.basicOrder.userId)
            {
                return MatchAction::Execute;
            }

            switch (taker.selfTradePrevention)
            {
                case SelfTradePrevention::CancelTaker:
                    return MatchAction::CancelTaker;
                case SelfTradePrevention::CancelMaker:
                    return MatchAction::CancelMaker;
                case SelfTradePrevention::CancelBoth:
                    return MatchAction::CancelBoth;
                default:
                    return MatchAction::Execute;
            }
        }
    };
}

#endif // ADVMATCH_SELF_TRADE_POLICY_H
