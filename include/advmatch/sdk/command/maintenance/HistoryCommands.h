#ifndef ADVMATCH_SDK_HISTORYCOMMANDS_H
#define ADVMATCH_SDK_HISTORYCOMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using ArchiveHistory = TradingOperation<advmatch::commands::ArchiveHistory>;
}

#endif // ADVMATCH_SDK_HISTORYCOMMANDS_H
