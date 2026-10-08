#ifndef ADVMATCH_API_SYMBOL_COMMANDS_H
#define ADVMATCH_API_SYMBOL_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using CreateSymbol = TradingOperation<advmatch::commands::CreateSymbol>;
    using RemoveSymbol = TradingOperation<advmatch::commands::RemoveSymbol>;
}

#endif // ADVMATCH_API_SYMBOL_COMMANDS_H
