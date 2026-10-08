#ifndef ADVMATCH_API_ASSET_COMMANDS_H
#define ADVMATCH_API_ASSET_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using CreateAsset = TradingOperation<advmatch::commands::CreateAsset>;
    using RemoveAsset = TradingOperation<advmatch::commands::RemoveAsset>;
}

#endif // ADVMATCH_API_ASSET_COMMANDS_H
