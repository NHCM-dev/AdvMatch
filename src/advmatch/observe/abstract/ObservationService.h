#ifndef ADVMATCH_OBSERVATION_SERVICE_H
#define ADVMATCH_OBSERVATION_SERVICE_H

#include "../model/ObservationTypes.h"

#include <memory>

namespace advmatch::observe
{
    class ObservationSlot;

    class ObservationService
    {
    public:
        virtual ~ObservationService() = default;
        virtual ObservationResult read(const ObservationQuery& query) const = 0;
        virtual ObservationBatch readBatch(std::span<const ObservationQuery> queries) const = 0;
        virtual std::shared_ptr<const ObservationResult> refresh(const std::shared_ptr<ObservationSlot>& slot) const = 0;
        virtual std::shared_ptr<ObservationSlot> createSlot(ObservationQuery query) = 0;
        virtual void rebuildOpenOrderIndex() = 0;
    };
}

#endif // ADVMATCH_OBSERVATION_SERVICE_H
