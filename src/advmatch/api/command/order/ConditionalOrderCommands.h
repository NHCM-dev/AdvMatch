#ifndef ADVMATCH_CONDITIONAL_ORDER_COMMANDS_H
#define ADVMATCH_CONDITIONAL_ORDER_COMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::api::command
{
    using PlaceConditionalOrder = TradingOperation<advmatch::commands::PlaceConditionalOrder>;
    using ModifyConditionalOrder = TradingOperation<advmatch::commands::ModifyConditionalOrder>;
    using CancelConditionalOrder = TradingOperation<advmatch::commands::CancelConditionalOrder>;
}

#endif // ADVMATCH_CONDITIONAL_ORDER_COMMANDS_H
