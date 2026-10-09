#ifndef ADVMATCH_SDK_MANAGEMENTCOMMANDS_H
#define ADVMATCH_SDK_MANAGEMENTCOMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using CreateAsset = TradingOperation<advmatch::commands::CreateAsset>;
    using RemoveAsset = TradingOperation<advmatch::commands::RemoveAsset>;
    using CreateUser = TradingOperation<advmatch::commands::CreateUser>;
    using RemoveUser = TradingOperation<advmatch::commands::RemoveUser>;
    using CreateSymbol = TradingOperation<advmatch::commands::CreateSymbol>;
    using RemoveSymbol = TradingOperation<advmatch::commands::RemoveSymbol>;
}

#endif // ADVMATCH_SDK_MANAGEMENTCOMMANDS_H
