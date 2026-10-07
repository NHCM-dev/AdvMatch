#ifndef ADVMATCH_LIQUIDATE_ENGINE_H
#define ADVMATCH_LIQUIDATE_ENGINE_H

#include "ContractMarket.h"

namespace advmatch
{
    class MatchingEngine;
    class ContractEngine;

    class LiquidateEngine
    {
    public:
        static void enqueue(ContractMarket& market, ContractParticipant& participant);
        static bool required(const ContractMarket& market, const ContractParticipant& participant) noexcept;
        ContractResult execute(ContractEngine& contracts, MatchingEngine& matching,
            ContractMarket& market, ContractParticipant& participant, Timestamp timestamp, TradeBuffer& trades) const;
    };
}

#endif // ADVMATCH_LIQUIDATE_ENGINE_H
