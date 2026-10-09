#ifndef ADVMATCH_SDK_PERSISTENCE_MANAGER_H
#define ADVMATCH_SDK_PERSISTENCE_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class PersistenceManager final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<PersistenceStatus> status() const
        {
            return this->api->persistence(PersistenceAction::Status);
        }

        CompletableFuture<PersistenceStatus> flush() const
        {
            return this->api->persistence(PersistenceAction::Flush);
        }

        CompletableFuture<PersistenceStatus> tick() const
        {
            return this->api->persistence(PersistenceAction::Tick);
        }

        CompletableFuture<PersistenceStatus> checkpoint() const
        {
            return this->api->persistence(PersistenceAction::Checkpoint);
        }

        CompletableFuture<CommittedPage> readCommitted(CommittedQuery query = {}) const
        {
            return this->api->readCommitted(query);
        }

        CompletableFuture<CommandReceipt> archiveHistory(std::uint64_t scanBudget, std::vector<OrderId> orders, std::vector<PositionId> positions, std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::ArchiveHistory{scanBudget, std::move(orders), std::move(positions)}, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_PERSISTENCE_MANAGER_H
