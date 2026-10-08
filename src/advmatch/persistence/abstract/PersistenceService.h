#ifndef ADVMATCH_PERSISTENCE_SERVICE_H
#define ADVMATCH_PERSISTENCE_SERVICE_H

#include "../model/PersistenceStatus.h"

namespace advmatch::persistence
{
    class PersistenceService
    {
    public:
        virtual ~PersistenceService() = default;
        virtual PersistenceStatus status() const noexcept = 0;
        virtual PersistenceStatus flush() = 0;
        virtual PersistenceStatus tick() = 0;
        virtual PersistenceStatus checkpoint() = 0;
        virtual CommittedPage readCommitted(const CommittedQuery& query) = 0;
    };
}

#endif // ADVMATCH_PERSISTENCE_SERVICE_H
