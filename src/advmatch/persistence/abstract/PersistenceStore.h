#ifndef ADVMATCH_PERSISTENCE_STORE_H
#define ADVMATCH_PERSISTENCE_STORE_H

#include "../model/PersistenceTypes.h"

namespace advmatch
{
    // Single writer. Custom backends must preserve append order, validate
    // record integrity, and atomically publish a durable snapshot before any
    // covered WAL is discarded. sync() must reach durable storage, not merely
    // a language/runtime buffer. Exceptions stop the owning trading session.
    class PersistenceStore
    {
    public:
        virtual ~PersistenceStore() = default;
        virtual std::optional<StoredSnapshot> loadSnapshot() = 0;
        virtual void replay(const std::function<void(const JournalRecord&)>& visitor) = 0;
        virtual void append(const JournalRecord& record) = 0;
        virtual void sync() = 0;
        virtual void publishSnapshot(CommandSequence sequence, std::span<const std::byte> payload) = 0;
        // Backends must durably archive covered records before publishing a state snapshot.
        // Older custom backends fail closed until they implement this contract.
        virtual void archiveRecords(const std::vector<JournalRecord>& records)
        {
            if (!records.empty()) throw PersistenceError(PersistenceFailure::InvalidConfiguration, "persistence backend does not support durable history archives");
        }
        virtual void replayArchive(CommandSequence afterSequence, const std::function<void(const JournalRecord&)>& visitor) {}
        virtual void resetJournalThrough(CommandSequence sequence) = 0;
    };
}

#endif // ADVMATCH_PERSISTENCE_STORE_H
