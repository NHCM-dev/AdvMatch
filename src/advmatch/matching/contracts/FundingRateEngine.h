#ifndef ADVMATCH_FUNDING_RATE_ENGINE_H
#define ADVMATCH_FUNDING_RATE_ENGINE_H

#include "ContractMarket.h"

namespace advmatch
{
    class FundingRateEngine
    {
    public:
        // Caller supplies an authoritative, bounded rate in millionths. This
        // settles one exact period; replay and skipped-period commands fail.
        ContractResult settle(ContractMarket& market, std::int32_t rate, Timestamp timestamp) const;
    };
}

#endif // ADVMATCH_FUNDING_RATE_ENGINE_H
