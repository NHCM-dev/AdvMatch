#ifndef ADVMATCH_API_USER_COMMANDS_H
#define ADVMATCH_API_USER_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using CreateUser = TradingOperation<advmatch::commands::CreateUser>;
    using RemoveUser = TradingOperation<advmatch::commands::RemoveUser>;
}

#endif // ADVMATCH_API_USER_COMMANDS_H
