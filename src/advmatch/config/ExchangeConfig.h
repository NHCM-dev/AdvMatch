#ifndef ADVMATCH_EXCHANGE_CONFIG_H
#define ADVMATCH_EXCHANGE_CONFIG_H

#include "advmatch/api/async/Executor.h"
#include "advmatch/observe/model/ObservationTypes.h"
#include "advmatch/persistence/config/PersistenceConfig.h"

namespace advmatch
{
    struct ExchangeConfig
    {
        PersistenceConfig persistence;
        observe::ObservationConfig observation;
        int ringBufferSize = 1024;
        std::size_t progressBatchSize = 0;
        std::size_t observationCacheSlots = 1024;
        std::size_t maxHistoryRows = 1024;
        std::shared_ptr<api::async::Executor> callbackExecutor;
    };
}

#endif // ADVMATCH_EXCHANGE_CONFIG_H
