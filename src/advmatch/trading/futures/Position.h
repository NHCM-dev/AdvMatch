//
// Created by NHCM on 2026/9/23.
//

#ifndef ADVMATCH_POSITION_H
#define ADVMATCH_POSITION_H
#include "../../util/ValueTypes.h"
#include <optional>

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
        Position(PositionId positionId, UserId userId, SymbolId symbolId, PositionSide side,
            MarginMode mode, LeverageMultiplier leverage, Quantity multiplier, Timestamp timestamp)
            : positionId(positionId), userId(userId), symbolId(symbolId), positionSide(side), marginMode(mode),
              averageEntryPrice(0), predictLiquidatePrice(0), predictBankruptPrice(0), positionSize(0),
              realizedPnL(0), updatedUnrealizedPnl(0), initialMargin(0), mmr(0), maintenanceMargin(0),
              closed(true), contractMultiplier(multiplier), leverage(leverage), createdTimestamp(timestamp),
              updatedTimestamp(timestamp)
        {
        }

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

        // Exact entry value avoids losing PnL when averageEntryPrice rounds.
        Balance entryNotional = 0;
        Balance estimatedCloseFee = 0;
        Quantity contractMultiplier = 1;
        LeverageMultiplier leverage = 1;
        BalanceChange fundingPnL = 0;
        Balance paidFees = 0;
        bool liquidating = false;
        bool managed = false;

        const Timestamp createdTimestamp;
        Timestamp updatedTimestamp;

        [[nodiscard]]
        std::optional<BalanceChange> getUnrealizedPnL(Price settlePrice) const noexcept;
    };

    struct PositionEntry
    {
        PositionId positionId;
        Position position;
    };
} // advmatch

#endif //ADVMATCH_POSITION_H
