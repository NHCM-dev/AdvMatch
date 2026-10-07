#ifndef ADVMATCH_DELIVERY_ENGINE_H
#define ADVMATCH_DELIVERY_ENGINE_H

#include "ContractMarket.h"

namespace advmatch
{
    class DeliveryEngine
    {
    public:
        ContractResult settle(ContractMarket& market, Price settlementPrice, Timestamp timestamp) const;
    };
}

#endif // ADVMATCH_DELIVERY_ENGINE_H
