#ifndef ADVMATCH_CONTRACT_UTIL_H
#define ADVMATCH_CONTRACT_UTIL_H

#include "ContractMath.h"

namespace advmatch::util
{
    [[nodiscard]]
    inline std::optional<Balance> getMaintenanceMargin(MMRRate mmr, Balance notionalPosition) noexcept
    {
        if (notionalPosition < 0 || mmr > RATE_SCALE)
        {
            return std::nullopt;
        }

        return ContractMath::rate(notionalPosition, mmr, true);
    }

    [[nodiscard]]
    inline std::optional<Balance> getMaintenanceMargin(MMRRate mmr, Price settlePrice, Quantity positionSize) noexcept
    {
        const auto notional = ContractMath::notional(settlePrice, positionSize);
        return notional ? getMaintenanceMargin(mmr, *notional) : std::nullopt;
    }
}

#endif // ADVMATCH_CONTRACT_UTIL_H
