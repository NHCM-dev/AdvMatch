#ifndef ADVMATCH_API_HISTORY_COMMANDS_H
#define ADVMATCH_API_HISTORY_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using ArchiveHistory = TradingOperation<advmatch::commands::ArchiveHistory>;
}

#endif // ADVMATCH_API_HISTORY_COMMANDS_H
