#ifndef ADVMATCH_FILE_PERSISTENCE_STORE_H
#define ADVMATCH_FILE_PERSISTENCE_STORE_H

#include "../config/PersistenceConfig.h"
#include "../abstract/PersistenceStore.h"

#include <memory>

namespace advmatch
{
    class FilePersistenceStore final : public PersistenceStore
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

    public:
        explicit FilePersistenceStore(const PersistenceConfig& config);
        ~FilePersistenceStore() override;
        std::optional<StoredSnapshot> loadSnapshot() override;
        void replay(const std::function<void(const JournalRecord&)>& visitor) override;
        void append(const JournalRecord& record) override;
        void sync() override;
        void publishSnapshot(CommandSequence sequence, std::span<const std::byte> payload) override;
        void resetJournalThrough(CommandSequence sequence) override;
    };
}

#endif // ADVMATCH_FILE_PERSISTENCE_STORE_H
