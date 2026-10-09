#ifndef ADVMATCH_SDK_OBSERVATION_MANAGER_H
#define ADVMATCH_SDK_OBSERVATION_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class ObservationManager final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<ObservationResult> read(ObservationQuery query) const
        {
            return this->api->read(std::move(query));
        }

        CompletableFuture<ObservationBatch> readBatch(std::vector<ObservationQuery> queries) const
        {
            return this->api->readBatch(std::move(queries));
        }

        CompletableFuture<OverviewResponse> overview() const
        {
            return this->api->read({}).thenApply([](const ObservationResult& result)
            {
                OverviewResponse response{result.query, result.page, std::nullopt};
                if (const auto* value = std::get_if<OverviewView>(&result.data))
                {
                    response.value = *value;
                }

                return response;
            });
        }

        CompletableFuture<std::shared_ptr<ObservationSlot>> createSlot(ObservationQuery query) const
        {
            return this->api->createObservationSlot(std::move(query));
        }

        CompletableFuture<void> rebuildOrderIndex() const
        {
            return this->api->rebuildOrderIndex();
        }
    };
}

#endif // ADVMATCH_SDK_OBSERVATION_MANAGER_H
