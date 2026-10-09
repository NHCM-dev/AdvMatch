#pragma once

#include "advmatch/config/ExchangeConfig.h"
#include "advmatch/persistence/model/PersistenceStatus.h"
#include "advmatch/observe/cache/ObservationCache.h"

namespace advmatch::binding
{
    // Kept distinct from the C++ config: executor objects cannot cross an ABI.
    struct BindingConfig
    {
        PersistenceConfig persistence;
        observe::ObservationConfig observation;
        int ringBufferSize = 1024;
        std::size_t progressBatchSize = 0;
        std::size_t observationCacheSlots = 1024;
        std::size_t maxHistoryRows = 1024;
        std::size_t maxPendingRequests = 4096;
        std::size_t maxRequestBytes = 4194304;

        ExchangeConfig exchange() const
        {
            ExchangeConfig config;
            config.persistence = this->persistence;
            config.observation = this->observation;
            config.ringBufferSize = this->ringBufferSize;
            config.progressBatchSize = this->progressBatchSize;
            config.observationCacheSlots = this->observationCacheSlots;
            config.maxHistoryRows = this->maxHistoryRows;
            return config;
        }
    };

    using Response = std::variant<std::monostate, CommandReceipt, observe::ObservationResult,
        observe::ObservationBatch, persistence::PersistenceStatus, persistence::CommittedPage, std::uint64_t>;
}
