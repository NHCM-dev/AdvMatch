#ifndef ADVMATCH_PERSISTENCE_COMMANDS_H
#define ADVMATCH_PERSISTENCE_COMMANDS_H

#include "../Command.h"

namespace advmatch::api::command
{
    class ReadPersistenceStatus final : public Command<persistence::PersistenceStatus>
    {
    public:
        persistence::PersistenceStatus execute(CommandContext& context) const override
        {
            return context.persistence().status();
        }
    };

    enum class PersistenceAction
    {
        Flush,
        Tick,
        Checkpoint
    };

    template <PersistenceAction Action>
    class PersistenceOperation final : public Command<persistence::PersistenceStatus>
    {
    public:
        persistence::PersistenceStatus execute(CommandContext& context) const override
        {
            if constexpr (Action == PersistenceAction::Flush)
            {
                return context.persistence().flush();
            }
            else if constexpr (Action == PersistenceAction::Tick)
            {
                return context.persistence().tick();
            }
            else
            {
                return context.persistence().checkpoint();
            }
        }
    };

    using Flush = PersistenceOperation<PersistenceAction::Flush>;
    using Tick = PersistenceOperation<PersistenceAction::Tick>;
    using Checkpoint = PersistenceOperation<PersistenceAction::Checkpoint>;

    class ReadCommitted final : public Command<persistence::CommittedPage>
    {
        persistence::CommittedQuery query;

    public:
        explicit ReadCommitted(persistence::CommittedQuery query = {})
            : query(query)
        {
        }

        persistence::CommittedPage execute(CommandContext& context) const override
        {
            return context.persistence().readCommitted(this->query);
        }
    };
}

#endif // ADVMATCH_PERSISTENCE_COMMANDS_H
