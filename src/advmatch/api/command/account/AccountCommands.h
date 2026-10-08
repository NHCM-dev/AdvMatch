#ifndef ADVMATCH_API_ACCOUNT_COMMANDS_H
#define ADVMATCH_API_ACCOUNT_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using UpdateBalance = TradingOperation<advmatch::commands::UpdateBalance>;
    using Transfer = TradingOperation<advmatch::commands::Transfer>;
}

#endif // ADVMATCH_API_ACCOUNT_COMMANDS_H
