#ifndef ADVMATCH_TRADING_OPERATION_H
#define ADVMATCH_TRADING_OPERATION_H

#include "../Command.h"

namespace advmatch::api::command
{
    template <typename Operation>
    class TradingOperation final : public Command<CommandReceipt>
    {
        TradingCommand operation;
        const std::uint64_t requestId;

    public:
        explicit TradingOperation(Operation operation, std::uint64_t requestId = 0)
            : operation(std::move(operation)), requestId(requestId)
        {
        }

        CommandReceipt execute(CommandContext& context) const override
        {
            return context.trading().execute(this->operation, this->requestId);
        }
    };

    using ExecuteTrading = TradingOperation<TradingCommand>;
}

#endif // ADVMATCH_TRADING_OPERATION_H
