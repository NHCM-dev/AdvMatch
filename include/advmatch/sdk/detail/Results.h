#ifndef ADVMATCH_SDK_RESULTS_H
#define ADVMATCH_SDK_RESULTS_H

#include "../ExchangeApi.h"

namespace advmatch::sdk::detail
{
    struct Results
    {
        static CommandReceipt accepted(const CommandReceipt& receipt)
        {
            if (!receipt.result.accepted)
            {
                throw BusinessError(receipt);
            }

            return receipt;
        }

        template <typename Row>
        static Row one(const ReadResponse<Row>& response)
        {
            if (response.page.state != ReadState::Success || response.values.size() != 1)
            {
                throw ReadError(response.page);
            }

            return response.values.front();
        }

        template <typename T, typename Function>
        static CompletableFuture<T> attempt(Function action)
        {
            try
            {
                return action();
            }
            catch (...)
            {
                return CompletableFuture<T>::failedFuture(std::current_exception());
            }
        }

        template <typename Handle>
        static CompletableFuture<Handle> created(std::shared_ptr<ExchangeApi> api, TradingCommand operation, std::uint64_t requestId)
        {
            return api->execute(std::move(operation), requestId).thenApply([api](const CommandReceipt& receipt)
            {
                accepted(receipt);
                return Handle(api, static_cast<typename Handle::IdType>(receipt.result.entityId));
            });
        }

        static CompletableFuture<CommandReceipt> checked(std::shared_ptr<ExchangeApi> api, TradingCommand operation, std::uint64_t requestId = 0)
        {
            return api->execute(std::move(operation), requestId).thenApply(accepted);
        }
    };
}

#endif // ADVMATCH_SDK_RESULTS_H
