//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SYMBOLMANAGER_H
#define ADVMATCH_SYMBOLMANAGER_H

#include "advmatch/account/User.h"
#include "advmatch/trading/Symbol.h"
#include <unordered_map>
#include <unordered_set>

namespace advmatch
{
    class SymbolManager
    {
        struct SymbolKey
        {
            SymbolType type;
            AssetId base;
            AssetId quote;

            bool operator==(const SymbolKey&) const = default;
        };

        struct SymbolKeyHash
        {
            std::size_t operator()(const SymbolKey& key) const noexcept
            {
                const std::uint64_t assets = (static_cast<std::uint64_t>(key.base) << 32) | key.quote;
                return std::hash<std::uint64_t>{}(assets) ^ (static_cast<std::size_t>(key.type) << 1);
            }
        };

        std::unordered_map<SymbolId, Symbol> symbols;
        std::unordered_map<std::string, SymbolId> symbolsByName;
        std::unordered_set<SymbolKey, SymbolKeyHash> symbolPairs;
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
