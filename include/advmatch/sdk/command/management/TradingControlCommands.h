#ifndef ADVMATCH_SDK_TRADING_CONTROL_COMMANDS_H
#define ADVMATCH_SDK_TRADING_CONTROL_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using SetTradingHalt = TradingOperation<advmatch::commands::SetTradingHalt>;
}

#endif // ADVMATCH_SDK_TRADING_CONTROL_COMMANDS_H
