//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SYMBOLMANAGER_H
#define ADVMATCH_SYMBOLMANAGER_H

#include "advmatch/account/User.h"
#include "advmatch/trading/Symbol.h"
#include <unordered_map>

namespace advmatch
{
    class SymbolManager
    {
        std::unordered_map<SymbolId, Symbol> symbols;
        std::unordered_map<std::string, SymbolId> symbolsByName;
        SymbolId nextSymbolId = 1;

    public:
        SymbolManager() = default;

        [[nodiscard]]
        Symbol* createSymbol(std::string name, SymbolType symbolType, AssetId base, AssetId quote);

        [[nodiscard]]
        bool removeSymbol(SymbolId symbolId);

        [[nodiscard]]
        bool containsSymbol(SymbolId symbolId) const noexcept;

        [[nodiscard]]
        Symbol* getSymbol(SymbolId symbolId) noexcept;

        [[nodiscard]]
        const Symbol* getSymbol(SymbolId symbolId) const noexcept;

        [[nodiscard]]
        std::size_t getSymbolCount() const noexcept;

        auto begin()
        {
            return symbols.begin();
        }

        auto end()
        {
            return symbols.end();
        }

        auto begin() const
        {
            return symbols.begin();
        }

        auto end() const
        {
            return symbols.end();
        }
    };
} // advmatch

#endif //ADVMATCH_SYMBOLMANAGER_H