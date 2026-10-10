#ifndef ADVMATCH_SDK_ACCOUNTCOMMANDS_H
#define ADVMATCH_SDK_ACCOUNTCOMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using UpdateBalance = TradingOperation<advmatch::commands::UpdateBalance>;
    using Transfer = TradingOperation<advmatch::commands::Transfer>;
    using BalanceTransaction = TradingOperation<advmatch::commands::BalanceTransaction>;
}

#endif // ADVMATCH_SDK_ACCOUNTCOMMANDS_H
