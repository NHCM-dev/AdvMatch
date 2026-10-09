#ifndef ADVMATCH_SDK_ORDERCOMMANDS_H
#define ADVMATCH_SDK_ORDERCOMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using AddSpotBook = TradingOperation<advmatch::commands::AddSpotBook>;
    using PlaceOrder = TradingOperation<advmatch::commands::PlaceOrder>;
    using ModifyPrice = TradingOperation<advmatch::commands::ModifyPrice>;
    using ModifyQuantity = TradingOperation<advmatch::commands::ModifyQuantity>;
    using CancelOrder = TradingOperation<advmatch::commands::CancelOrder>;
}

#endif // ADVMATCH_SDK_ORDERCOMMANDS_H
