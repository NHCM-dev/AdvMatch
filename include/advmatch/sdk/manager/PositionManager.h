#ifndef ADVMATCH_SDK_POSITION_MANAGER_H
#define ADVMATCH_SDK_POSITION_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class PositionManager final : public EntityManager<Position, ReadKind::Positions>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<ReadResponse<PositionView>> managed(ObservationQuery query = {}) const
        {
            return this->read<PositionView>(ReadKind::ManagedPositions, std::move(query));
        }

        CompletableFuture<ReadResponse<PositionView>> pendingLiquidations(ObservationQuery query = {}) const
        {
            return this->read<PositionView>(ReadKind::PendingLiquidations, std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_POSITION_MANAGER_H
