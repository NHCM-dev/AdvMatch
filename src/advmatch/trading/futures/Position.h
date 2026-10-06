//
// Created by NHCM on 2026/9/23.
//

#ifndef ADVMATCH_POSITION_H
#define ADVMATCH_POSITION_H
#include "../../util/Types.h"

namespace advmatch
{
    // Fixed-point decimal rate.
    //
    // RATE_SCALE = 1'000'000
    //
    // 100%  = 1'000'000
    // 10%   =   100'000
    // 1%    =    10'000
    // 0.5%  =     5'000
    // 0.01% =       100
    //
    // Decimal value = rate / RATE_SCALE
    constexpr MMRRate RATE_SCALE = 1'000'000;

    enum class PositionSide : bool
    {
        Long = true,
        Short = false
    };

    enum class MarginMode : bool
    {
        Cross = true,
        Isolated = false,
    };

    struct Position
    {
        const PositionId positionId;
        const UserId userId;
        const SymbolId symbolId;

        PositionSide positionSide;
        const MarginMode marginMode;

        // This is just for reference only
        Price averageEntryPrice;

        Price predictLiquidatePrice;
        Price predictBankruptPrice;

        Quantity positionSize;

        BalanceChange realizedPnL;
        BalanceChange updatedUnrealizedPnl;

        Balance initialMargin;
        MMRRate mmr;
        Balance maintenanceMargin;

        bool closed;

        const Timestamp createdTimestamp;
        Timestamp updatedTimestamp;

        [[nodiscard]]
        BalanceChange getUnrealizedPnL(Price settlePrice) const;
    };

    struct PositionEntry
    {
        PositionId positionId;
        Position position;
    };
} // advmatch

#endif //ADVMATCH_POSITION_H
