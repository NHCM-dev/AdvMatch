//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_TYPES_H
#define ADVMATCH_TYPES_H

#include <cstdint>
#include <string>
#include "../market/Asset.h"

namespace advmatch
{
    using SymbolId = uint32_t;

    enum class SymbolType : uint8_t
    {
        Spot,
        Futures,
        Perpetual,
        Option
    };

    struct Symbol
    {
        std::string name;
        SymbolId symbolId;

        SymbolType symbolType;

        AssetId baseAssetId;   // Example: BTC
        AssetId quoteAssetId;  // Example: USDT
    };

} // advmatch

#endif //ADVMATCH_TYPES_H
