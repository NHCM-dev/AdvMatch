#ifndef ADVMATCH_API_ORDER_COMMANDS_H
#define ADVMATCH_API_ORDER_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using AddSpotBook = TradingOperation<advmatch::commands::AddSpotBook>;
    using PlaceOrder = TradingOperation<advmatch::commands::PlaceOrder>;
    using ModifyPrice = TradingOperation<advmatch::commands::ModifyPrice>;
    using ModifyQuantity = TradingOperation<advmatch::commands::ModifyQuantity>;
    using CancelOrder = TradingOperation<advmatch::commands::CancelOrder>;
}

#endif // ADVMATCH_API_ORDER_COMMANDS_H
