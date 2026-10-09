#ifndef ADVMATCH_SDK_TRADING_OPERATION_H
#define ADVMATCH_SDK_TRADING_OPERATION_H

#include "../Command.h"

namespace advmatch::sdk::command
{
    template <typename Operation>
    inline constexpr bool isCreation = std::is_same_v<Operation, advmatch::commands::CreateAsset> || std::is_same_v<Operation, advmatch::commands::CreateUser> || std::is_same_v<Operation, advmatch::commands::CreateSymbol>;

    template <typename Operation>
    class TradingOperation final : public Command<std::conditional_t<isCreation<Operation>, CreationResult, CommandReceipt>>
    {
        Operation operation;
        std::uint64_t requestId;

    public:
        using ResponseType = std::conditional_t<isCreation<Operation>, CreationResult, CommandReceipt>;

        explicit TradingOperation(Operation operation = {}, std::uint64_t requestId = 0)
            : operation(std::move(operation)), requestId(requestId)
        {
        }

        CompletableFuture<ResponseType> execute(ExchangeApi& api) const override
        {
            auto result = api.execute(this->operation, this->requestId);
            if constexpr (isCreation<Operation>)
            {
                return result.thenApply([](const CommandReceipt& receipt)
                {
                    return CreationResult{receipt};
                });
            }
            else
            {
                return result;
            }
        }
    };

    using ExecuteTrading = TradingOperation<TradingCommand>;
}

#endif // ADVMATCH_SDK_TRADING_OPERATION_H
