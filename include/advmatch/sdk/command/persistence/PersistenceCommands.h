#ifndef ADVMATCH_SDK_PERSISTENCE_COMMANDS_H
#define ADVMATCH_SDK_PERSISTENCE_COMMANDS_H

#include "../Command.h"

namespace advmatch::sdk::command
{
    template <PersistenceAction Action>
    class PersistenceOperation final : public Command<PersistenceStatus>
    {
    public:
        CompletableFuture<PersistenceStatus> execute(ExchangeApi& api) const override
        {
            return api.persistence(Action);
        }
    };

    using ReadPersistenceStatus = PersistenceOperation<PersistenceAction::Status>;
    using Flush = PersistenceOperation<PersistenceAction::Flush>;
    using Tick = PersistenceOperation<PersistenceAction::Tick>;
    using Checkpoint = PersistenceOperation<PersistenceAction::Checkpoint>;

    class ReadCommitted final : public Command<CommittedPage>
    {
        CommittedQuery query;

    public:
        explicit ReadCommitted(CommittedQuery query = {})
            : query(query)
        {
        }

        CompletableFuture<CommittedPage> execute(ExchangeApi& api) const override
        {
            return api.readCommitted(this->query);
        }
    };
}

#endif // ADVMATCH_SDK_PERSISTENCE_COMMANDS_H
