#ifndef ADVMATCH_ENGINE_PERSISTENCE_SERVICE_H
#define ADVMATCH_ENGINE_PERSISTENCE_SERVICE_H

#include "../abstract/PersistenceService.h"
#include "PersistentTradingEngine.h"

namespace advmatch::persistence
{
    class EnginePersistenceService final : public PersistenceService
    {
        PersistentTradingEngine& engine;
        bool enabled;
        std::size_t maxHistoryRows;

    public:
        EnginePersistenceService(PersistentTradingEngine& engine, bool enabled, std::size_t maxHistoryRows)
            : engine(engine), enabled(enabled), maxHistoryRows(maxHistoryRows)
        {
            if (maxHistoryRows == 0)
            {
                throw std::invalid_argument("history row limit must be positive");
            }
        }

        PersistenceStatus status() const noexcept override
        {
            return {this->enabled, this->engine.isFaulted(), this->engine.lastSequence(), this->engine.durableThrough()};
        }

        PersistenceStatus flush() override
        {
            this->engine.flush();
            return this->status();
        }

        PersistenceStatus tick() override
        {
            this->engine.tick();
            return this->status();
        }

        PersistenceStatus checkpoint() override
        {
            this->engine.checkpoint();
            return this->status();
        }

        CommittedPage readCommitted(const CommittedQuery& query) override
        {
            if (query.limit == 0 || query.limit > this->maxHistoryRows)
            {
                throw std::invalid_argument("committed history exceeds row limit");
            }

            CommittedPage page{this->status(), query.afterSequence, false, {}};
            page.records.reserve(query.limit);
            // Existing durable outbox visitor scans the history. Bound result
            // memory here; this administrative read is not an O(1) hot query.
            this->engine.visitCommitted(query.afterSequence, [&](const CommittedCommand& record)
            {
                if (page.records.size() < query.limit)
                {
                    page.records.push_back(record);
                    page.nextSequence = record.sequence;
                }
                else
                {
                    page.hasMore = true;
                }
            });
            page.status = this->status();
            return page;
        }
    };
}

#endif // ADVMATCH_ENGINE_PERSISTENCE_SERVICE_H
