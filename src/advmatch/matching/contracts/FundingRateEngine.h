#ifndef ADVMATCH_FUNDING_RATE_ENGINE_H
#define ADVMATCH_FUNDING_RATE_ENGINE_H

#include "ContractMarket.h"
#include "ContractSettlementBatch.h"

namespace advmatch
{
    class FundingRateEngine
    {
        mutable ContractSettlementBatch workspace;

    public:
        // Caller supplies an authoritative, bounded rate in millionths. This
        // settles one exact period; replay and skipped-period commands fail.
        ContractResult settle(ContractMarket& market, std::int32_t rate, Timestamp timestamp) const;
    };
}

#endif // ADVMATCH_FUNDING_RATE_ENGINE_H
