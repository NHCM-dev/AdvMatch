#ifndef ADVMATCH_TRADE_H
#define ADVMATCH_TRADE_H

#include "advmatch/util/ValueTypes.h"

namespace advmatch
{
    struct Trade
    {
        OrderId makerOrderId;
        OrderId takerOrderId;
        Price price;
        Quantity quantity;
    };

}

#endif // ADVMATCH_TRADE_H
