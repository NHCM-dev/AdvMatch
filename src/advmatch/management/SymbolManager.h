//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SYMBOLMANAGER_H
#define ADVMATCH_SYMBOLMANAGER_H

#include "advmatch/account/User.h"
#include "advmatch/trading/Symbol.h"
#include "advmatch/util/Containers.h"

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

        NodeHashMap<SymbolId, Symbol> symbols;
        DenseMap<std::string, SymbolId> symbolsByName;
        DenseSet<SymbolKey, SymbolKeyHash> symbolPairs;
        SymbolId nextSymbolId = 1;
        std::size_t activeSymbolCount = 0;

    public:
        SymbolManager() = default;

        void reserve(std::size_t symbolCount)
        {
            this->symbols.reserve(symbolCount);
            this->symbolsByName.reserve(symbolCount);
            this->symbolPairs.reserve(symbolCount);
        }

        [[nodiscard]]
        Symbol* createSymbol(std::string name, SymbolType symbolType, AssetId base, AssetId quote);

        [[nodiscard]]
        bool removeSymbol(SymbolId symbolId);

        [[nodiscard]]
        bool containsSymbol(SymbolId symbolId) const noexcept;

        // NodeHashMap keeps Symbol addresses valid across insertions and
        // rehashes. Removal deactivates the Symbol without destroying it, so
        // accepted orders can safely retain its address. New lookups do not
        // return deactivated symbols.
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

        [[nodiscard]]
        auto begin() const
        {
            return symbols.begin();
        }

        [[nodiscard]]
        auto end() const
        {
            return symbols.end();
        }
    };
} // advmatch

#endif //ADVMATCH_SYMBOLMANAGER_H
