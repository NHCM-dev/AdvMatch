#include "advmatch/sdk/ExchangeApi.h"
#include "advmatch/sdk/NativeAdapter.h"
#include "advmatch/Exchange.h"
#include "advmatch/api/command/Commands.h"
#include "advmatch/observe/cache/ObservationCache.h"

namespace advmatch::sdk
{
    namespace native = advmatch::api::command;

    namespace
    {
        class NativeObservationSlot final : public ObservationSlot
        {
            std::shared_ptr<ExchangeApi> owner;
            std::shared_ptr<advmatch::api::ExchangeApi> exchange;
            std::shared_ptr<observe::ObservationSlot> slot;

        public:
            NativeObservationSlot(std::shared_ptr<ExchangeApi> owner, std::shared_ptr<advmatch::api::ExchangeApi> exchange, std::shared_ptr<observe::ObservationSlot> slot)
                : owner(std::move(owner)), exchange(std::move(exchange)), slot(std::move(slot))
            {
            }

            const ObservationQuery& query() const noexcept override
            {
                return this->slot->query();
            }

            bool belongsTo(const ExchangeApi& api) const noexcept override
            {
                return this->owner.get() == &api;
            }

            std::shared_ptr<const ObservationResult> load() const noexcept override
            {
                return this->slot->load();
            }

            CompletableFuture<std::shared_ptr<const ObservationResult>> refresh() override
            {
                return this->exchange->submit(native::RefreshObservation(this->slot));
            }
        };

        class NativeExchangeApi final : public ExchangeApi
        {
            std::shared_ptr<advmatch::api::ExchangeApi> exchange;

        public:
            explicit NativeExchangeApi(ExchangeConfig config)
                : exchange(std::make_shared<advmatch::Exchange>(std::move(config)))
            {
            }

            explicit NativeExchangeApi(std::shared_ptr<advmatch::api::ExchangeApi> exchange)
                : exchange(std::move(exchange))
            {
                if (this->exchange == nullptr)
                {
                    throw std::invalid_argument("native exchange API is null");
                }
            }

            CompletableFuture<CommandReceipt> execute(TradingCommand operation, std::uint64_t requestId) override
            {
                return this->exchange->submit(native::ExecuteTrading(std::move(operation), requestId));
            }

            CompletableFuture<ObservationResult> read(ObservationQuery query) override
            {
                return this->exchange->submit(native::Read(std::move(query)));
            }

            CompletableFuture<ObservationBatch> readBatch(std::vector<ObservationQuery> queries) override
            {
                return this->exchange->submit(native::ReadBatch(std::move(queries)));
            }

            CompletableFuture<std::shared_ptr<ObservationSlot>> createObservationSlot(ObservationQuery query) override
            {
                return this->exchange->submit(native::CreateObservationSlot(std::move(query))).thenApply([owner = this->shared_from_this(), exchange = this->exchange](std::shared_ptr<observe::ObservationSlot> slot) -> std::shared_ptr<ObservationSlot>
                {
                    return std::make_shared<NativeObservationSlot>(owner, exchange, std::move(slot));
                });
            }

            CompletableFuture<void> rebuildOrderIndex() override
            {
                return this->exchange->submit(native::RebuildOrderIndex{});
            }

            CompletableFuture<PersistenceStatus> persistence(PersistenceAction action) override
            {
                switch (action)
                {
                    case PersistenceAction::Status:
                        return this->exchange->submit(native::ReadPersistenceStatus{});
                    case PersistenceAction::Flush:
                        return this->exchange->submit(native::Flush{});
                    case PersistenceAction::Tick:
                        return this->exchange->submit(native::Tick{});
                    case PersistenceAction::Checkpoint:
                        return this->exchange->submit(native::Checkpoint{});
                }

                return CompletableFuture<PersistenceStatus>::failedFuture(std::make_exception_ptr(std::invalid_argument("invalid persistence action")));
            }

            CompletableFuture<CommittedPage> readCommitted(CommittedQuery query) override
            {
                return this->exchange->submit(native::ReadCommitted(query));
            }

            CompletableFuture<void> close() override
            {
                return this->exchange->close();
            }
        };
    }

    std::shared_ptr<ExchangeApi> makeExchangeApi(ExchangeConfig config)
    {
        return std::make_shared<NativeExchangeApi>(std::move(config));
    }

    std::shared_ptr<ExchangeApi> wrapExchange(std::shared_ptr<advmatch::api::ExchangeApi> exchange)
    {
        return std::make_shared<NativeExchangeApi>(std::move(exchange));
    }
}
