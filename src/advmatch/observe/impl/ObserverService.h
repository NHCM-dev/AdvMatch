#ifndef ADVMATCH_OBSERVER_SERVICE_H
#define ADVMATCH_OBSERVER_SERVICE_H

#include "../abstract/ObservationService.h"
#include "../cache/ObservationCache.h"

namespace advmatch::observe
{
    class ObserverService final : public ObservationService
    {
        TradingObserver& observer;
        ObservationCache cache;

    public:
        explicit ObserverService(TradingObserver& observer, std::size_t cacheSlots = 1024)
            : observer(observer), cache(cacheSlots)
        {
        }

        ObservationResult read(const ObservationQuery& query) const override
        {
            return this->observer.read(query);
        }

        ObservationBatch readBatch(std::span<const ObservationQuery> queries) const override
        {
            return this->observer.readBatch(queries);
        }

        ObservationBatch readChanged(std::span<const ObservationQuery> queries, std::span<const ReadStamp> known) const override
        {
            return this->observer.readChanged(queries, known);
        }

        std::shared_ptr<const ObservationResult> refresh(const std::shared_ptr<ObservationSlot>& slot) const override
        {
            if (slot == nullptr)
            {
                throw std::invalid_argument("observation slot is null");
            }

            return slot->refresh(this->observer);
        }

        void rebuildOpenOrderIndex() override
        {
            this->observer.rebuildOpenOrderIndex();
        }

        std::shared_ptr<ObservationSlot> createSlot(ObservationQuery query) override
        {
            return this->cache.create(std::move(query));
        }
    };
}

#endif // ADVMATCH_OBSERVER_SERVICE_H
