#ifndef ADVMATCH_REFERENCE_PRICE_ENGINE_H
#define ADVMATCH_REFERENCE_PRICE_ENGINE_H

#include "ContractMarket.h"
#include "advmatch/trading/futures/ReferencePriceAlgorithm.h"

namespace advmatch
{
    class MatchingEngine;
    class SymbolManager;

    class ReferencePriceEngine
    {
        DefaultReferencePriceAlgorithm algorithm;

    public:
        ContractResult configure(ContractMarket& market, const ReferencePriceConfig& config, const SymbolManager& symbols) const;
        ContractResult publish(ContractMarket& market, const ExternalPrice& price) const;
        std::optional<ReferencePrices> calculate(ContractMarket& market, const MatchingEngine& matching, const SymbolManager& symbols, Timestamp timestamp) const;
    };
}

#endif // ADVMATCH_REFERENCE_PRICE_ENGINE_H
