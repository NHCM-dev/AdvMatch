#ifndef ADVMATCH_SDK_CONDITIONAL_ORDER_COMMANDS_H
#define ADVMATCH_SDK_CONDITIONAL_ORDER_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using PlaceConditionalOrder = TradingOperation<advmatch::commands::PlaceConditionalOrder>;
    using ModifyConditionalOrder = TradingOperation<advmatch::commands::ModifyConditionalOrder>;
    using CancelConditionalOrder = TradingOperation<advmatch::commands::CancelConditionalOrder>;
}

#endif // ADVMATCH_SDK_CONDITIONAL_ORDER_COMMANDS_H
