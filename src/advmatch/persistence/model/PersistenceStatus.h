#ifndef ADVMATCH_PERSISTENCE_STATUS_H
#define ADVMATCH_PERSISTENCE_STATUS_H

#include "CommittedCommand.h"

namespace advmatch::persistence
{
    struct PersistenceStatus
    {
        bool enabled;
        bool faulted;
        CommandSequence sequence;
        CommandSequence durableThrough;
    };

    struct CommittedQuery
    {
        CommandSequence afterSequence = 0;
        std::size_t limit = 64;
    };

    struct CommittedPage
    {
        PersistenceStatus status;
        CommandSequence nextSequence;
        bool hasMore;
        std::vector<CommittedCommand> records;
    };
}

#endif // ADVMATCH_PERSISTENCE_STATUS_H
