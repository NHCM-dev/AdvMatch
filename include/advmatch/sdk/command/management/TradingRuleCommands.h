#ifndef ADVMATCH_SDK_TRADING_RULE_COMMANDS_H
#define ADVMATCH_SDK_TRADING_RULE_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using ConfigureTradingRules = TradingOperation<advmatch::commands::ConfigureTradingRules>;
    using WithdrawSpotFees = TradingOperation<advmatch::commands::WithdrawSpotFees>;
}

#endif // ADVMATCH_SDK_TRADING_RULE_COMMANDS_H
