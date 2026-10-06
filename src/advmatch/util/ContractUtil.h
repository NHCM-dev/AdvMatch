//
// Created by NHCM on 2026/9/23.
//

#ifndef ADVMATCH_CONTRACTUTIL_H
#define ADVMATCH_CONTRACTUTIL_H
#include "advmatch/trading/Asset.h"
#include "advmatch/trading/OrderTypes.h"
#include "advmatch/trading/futures/Position.h"

namespace advmatch::util
{
    [[nodiscard]]
    static Balance getMaintenanceMargin(MMRRate mmr, Balance notionalPosition)
    {
        if (notionalPosition < 0)
        {
            throw std::invalid_argument(
                "Notional position cannot be negative"
            );
        }

        if (mmr > RATE_SCALE)
        {
            throw std::range_error(
                "MMR cannot exceed 100%"
            );
        }

        const __int128 numerator =
            static_cast<__int128>(notionalPosition) *
            static_cast<__int128>(mmr);

        // Risk requirement should round upward instead of downward.
        const __int128 result =
            (numerator + RATE_SCALE - 1) / RATE_SCALE;

        if (result > std::numeric_limits<Balance>::max())
        {
            throw std::overflow_error(
                "Maintenance margin overflow"
            );
        }

        return static_cast<Balance>(result);
    }

    [[nodiscard]]
    static Balance getMaintenanceMargin(MMRRate mmr, Price settlePrice, Quantity positionSize)
    {
        if (willMultiplyOverflow(settlePrice, positionSize))
        {
            throw std::invalid_argument("notional position value will overflow");
        }
        return getMaintenanceMargin(mmr, static_cast<Balance>(settlePrice * positionSize));
    }
} // advmatch

#endif //ADVMATCH_CONTRACTUTIL_H
