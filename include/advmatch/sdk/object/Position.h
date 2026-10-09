#ifndef ADVMATCH_SDK_POSITION_H
#define ADVMATCH_SDK_POSITION_H

#include "Entity.h"

namespace advmatch::sdk
{
    class Position final : public Entity<PositionView, PositionId, ReadKind::Position>
    {
        template <typename Operation>
        CompletableFuture<CommandReceipt> update(Operation operation, std::uint64_t requestId) const
        {
            return this->snapshot().thenCompose([api = this->api, operation, requestId](const PositionView& value)
            {
                return detail::Results::checked(api, operation(value), requestId);
            });
        }

    public:
        using Entity::Entity;

        CompletableFuture<CommandReceipt> setLeverage(LeverageMultiplier leverage, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update([leverage, timestamp](const PositionView& value)
            {
                return commands::SetLeverage{value.positionId, value.userId, value.symbolId, leverage, timestamp};
            }, requestId);
        }

        CompletableFuture<CommandReceipt> addMargin(BalanceChange amount, std::uint64_t requestId = 0) const
        {
            return this->update([amount](const PositionView& value)
            {
                return commands::AddMargin{value.positionId, value.userId, value.symbolId, amount};
            }, requestId);
        }

        CompletableFuture<CommandReceipt> liquidate(Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update([timestamp](const PositionView& value)
            {
                return commands::Liquidate{value.symbolId, value.positionId, timestamp};
            }, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_POSITION_H
