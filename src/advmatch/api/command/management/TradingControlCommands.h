#ifndef ADVMATCH_TRADING_CONTROL_COMMANDS_H
#define ADVMATCH_TRADING_CONTROL_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using SetTradingHalt = TradingOperation<advmatch::commands::SetTradingHalt>;
}

#endif // ADVMATCH_TRADING_CONTROL_COMMANDS_H
