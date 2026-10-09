#ifndef ADVMATCH_SDK_OBSERVATION_SLOT_H
#define ADVMATCH_SDK_OBSERVATION_SLOT_H

#include "../model/Models.h"

namespace advmatch::sdk
{
    class ExchangeApi;

    // Explicit refresh publishes an owning snapshot. load never visits live
    // engine state. The final shared owner releases the slot's capacity.
    class ObservationSlot
    {
    public:
        virtual ~ObservationSlot() = default;
        virtual const ObservationQuery& query() const noexcept = 0;
        virtual bool belongsTo(const ExchangeApi& api) const noexcept = 0;
        virtual std::shared_ptr<const ObservationResult> load() const noexcept = 0;
        virtual CompletableFuture<std::shared_ptr<const ObservationResult>> refresh() = 0;
    };
}

#endif // ADVMATCH_SDK_OBSERVATION_SLOT_H
