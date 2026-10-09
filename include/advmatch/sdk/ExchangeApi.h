#ifndef ADVMATCH_SDK_EXCHANGE_API_H
#define ADVMATCH_SDK_EXCHANGE_API_H

#include "Export.h"
#include "model/Errors.h"
#include "observe/ObservationSlot.h"

namespace advmatch::sdk
{
    enum class PersistenceAction
    {
        Status,
        Flush,
        Tick,
        Checkpoint
    };

    // Public capabilities contain values and asynchronous submissions only.
    // Implementations must preserve the native owner thread and WAL boundary.
    class ExchangeApi : public std::enable_shared_from_this<ExchangeApi>
    {
    public:
        virtual ~ExchangeApi() = default;
        virtual CompletableFuture<CommandReceipt> execute(TradingCommand operation, std::uint64_t requestId = 0) = 0;
        virtual CompletableFuture<ObservationResult> read(ObservationQuery query) = 0;
        virtual CompletableFuture<ObservationBatch> readBatch(std::vector<ObservationQuery> queries) = 0;
        virtual CompletableFuture<std::shared_ptr<ObservationSlot>> createObservationSlot(ObservationQuery query) = 0;
        virtual CompletableFuture<void> rebuildOrderIndex() = 0;
        virtual CompletableFuture<PersistenceStatus> persistence(PersistenceAction action) = 0;
        virtual CompletableFuture<CommittedPage> readCommitted(CommittedQuery query) = 0;
        virtual CompletableFuture<void> close() = 0;

        template <typename Concrete>
        CompletableFuture<typename Concrete::ResponseType> submit(const Concrete& command)
        {
            try
            {
                auto result = command.execute(*this);
                if (auto owner = this->weak_from_this().lock())
                {
                    // A composed custom command may outlive its submitting
                    // facade. Retain the API until that command is complete.
                    result.whenComplete([owner](const auto&)
                    {
                    });
                }

                return result;
            }
            catch (...)
            {
                return CompletableFuture<typename Concrete::ResponseType>::failedFuture(std::current_exception());
            }
        }

        template <typename Concrete>
        CompletableFuture<typename Concrete::ResponseType> submit(const std::shared_ptr<Concrete>& command)
        {
            if (command == nullptr)
            {
                return CompletableFuture<typename Concrete::ResponseType>::failedFuture(std::make_exception_ptr(std::invalid_argument("command is null")));
            }

            return this->submit(*command);
        }

        template <typename Row>
        CompletableFuture<ReadResponse<Row>> readRows(ObservationQuery query)
        {
            return this->read(std::move(query)).thenApply([](const ObservationResult& result)
            {
                ReadResponse<Row> response{result.query, result.page, {}};
                if (const auto* values = std::get_if<std::vector<Row>>(&result.data))
                {
                    response.values = *values;
                }

                return response;
            });
        }
    };

    ADVMATCH_SDK_API std::shared_ptr<ExchangeApi> makeExchangeApi(ExchangeConfig config = {});
}

#endif // ADVMATCH_SDK_EXCHANGE_API_H
