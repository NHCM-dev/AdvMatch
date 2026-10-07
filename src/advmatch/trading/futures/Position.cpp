#include "Position.h"
#include "advmatch/util/ContractMath.h"

namespace advmatch
{
    std::optional<BalanceChange> Position::getUnrealizedPnL(Price settlePrice) const noexcept
    {
        const auto value = ContractMath::notional(settlePrice, this->positionSize, this->contractMultiplier);
        if (!value)
        {
            return std::nullopt;
        }

        const Balance entry = this->entryNotional;
        return this->positionSide == PositionSide::Long ? *value - entry : entry - *value;
    }
}
