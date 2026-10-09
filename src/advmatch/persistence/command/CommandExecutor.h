#ifndef ADVMATCH_COMMAND_EXECUTOR_H
#define ADVMATCH_COMMAND_EXECUTOR_H

#include "advmatch/matching/MatchingEngine.h"

#include "advmatch/api/command/model/TradingCommand.h"
#include "advmatch/management/AssetManager.h"

namespace advmatch
{
    // Ownership order keeps cached account/user/symbol pointers alive until
    // matching and its pooled orders/participants have been destroyed.
    struct TradingState
    {
        AssetManager assets;
        UserManager users;
        SymbolManager symbols;
        MatchingEngine matching{this->users, this->symbols};
    };

    CommandResult executeTradingCommand(TradingState& state, const TradingCommand& command, TradeBuffer& trades);
}

#endif // ADVMATCH_COMMAND_EXECUTOR_H
