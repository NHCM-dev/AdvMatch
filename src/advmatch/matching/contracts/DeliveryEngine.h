#ifndef ADVMATCH_DELIVERY_ENGINE_H
#define ADVMATCH_DELIVERY_ENGINE_H

#include "ContractMarket.h"
#include "ContractSettlementBatch.h"

namespace advmatch
{
    class DeliveryEngine
    {
        mutable ContractSettlementBatch workspace;

    public:
        ContractResult settle(ContractMarket& market, Price settlementPrice, Timestamp timestamp) const;
    };
}

#endif // ADVMATCH_DELIVERY_ENGINE_H
