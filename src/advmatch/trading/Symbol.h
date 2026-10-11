//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SYMBOL_H
#define ADVMATCH_SYMBOL_H

#include "../util/ValueTypes.h"
#include "rules/TradingRules.h"
#include "TradingControl.h"
#include <string>
#include <utility>

namespace advmatch
{
    class SymbolManager;
    enum class SymbolType : uint8_t
    {
        Spot,
        Future,
        Perpetual,
        Option
    };

    struct Symbol
    {
        friend class ::advmatch::StateSnapshotCodec;
        Symbol(std::string name, SymbolId symbolId, SymbolType symbolType, AssetId baseAssetId, AssetId quoteAssetId)
            : name(std::move(name)), symbolId(symbolId), symbolType(symbolType), baseAssetId(baseAssetId),
              quoteAssetId(quoteAssetId)
        {
        }

        std::string name;
        SymbolId symbolId;

        SymbolType symbolType;

        AssetId baseAssetId; // Example: BTC
        AssetId quoteAssetId; // Example: USDT
        TradingRules tradingRules;
        TradingHalt tradingHalt;

        [[nodiscard]]
        bool isActive() const noexcept
        {
            return this->active;
        }

    private:
        bool active = true;

        friend class SymbolManager;
    };
} // advmatch

#endif //ADVMATCH_SYMBOL_H
