//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_TYPES_H
#define ADVMATCH_TYPES_H

#include <cstdint>
#include <string>
#include <utility>
#include "../market/Asset.h"

namespace advmatch
{
    using SymbolId = uint32_t;
    class SymbolManager;

    enum class SymbolType : uint8_t
    {
        Spot,
        Futures,
        Perpetual,
        Option
    };

    struct Symbol
    {
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

#endif //ADVMATCH_TYPES_H
