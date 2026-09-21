//
// Created by NHCM on 2026/9/20.
//

#include "SymbolManager.h"

#include <utility>

namespace advmatch
{
    Symbol* SymbolManager::createSymbol(std::string name, SymbolType symbolType, AssetId base, AssetId quote)
    {
        if (name.empty() || base == quote || base == 0 || quote == 0 || this->nextSymbolId == 0)
        {
            return nullptr;
        }

        if (this->symbolsByName.contains(name))
        {
            return nullptr;
        }

        const SymbolKey key{symbolType, base, quote};
        if (this->symbolPairs.contains(key))
        {
            return nullptr;
        }

        const SymbolId symbolId = this->nextSymbolId;

        auto result = this->symbols.emplace(
            symbolId,
            Symbol(std::move(name), symbolId, symbolType, base, quote)
        );

        if (!result.second)
        {
            return nullptr;
        }

        try
        {
            this->symbolsByName.emplace(result.first->second.name, symbolId);
            this->symbolPairs.emplace(key);
        }
        catch (...)
        {
            this->symbolsByName.erase(result.first->second.name);
            this->symbols.erase(result.first);
            throw;
        }

        ++this->nextSymbolId;
        ++this->activeSymbolCount;

        return &result.first->second;
    }

    bool SymbolManager::containsSymbol(SymbolId symbolId) const noexcept
    {
        const auto iterator = this->symbols.find(symbolId);
        return iterator != this->symbols.end() && iterator->second.active;
    }

    const Symbol* SymbolManager::getSymbol(SymbolId symbolId) const noexcept
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end() || !iterator->second.active)
        {
            return nullptr;
        }

        return &iterator->second;
    }

    Symbol* SymbolManager::getSymbol(SymbolId symbolId) noexcept
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end() || !iterator->second.active)
        {
            return nullptr;
        }

        return &iterator->second;
    }

    bool SymbolManager::removeSymbol(SymbolId symbolId)
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end() || !iterator->second.active)
        {
            return false;
        }

        this->symbolsByName.erase(iterator->second.name);
        this->symbolPairs.erase(SymbolKey{iterator->second.symbolType, iterator->second.baseAssetId, iterator->second.quoteAssetId});
        iterator->second.active = false;
        --this->activeSymbolCount;

        return true;
    }

    std::size_t SymbolManager::getSymbolCount() const noexcept
    {
        return this->activeSymbolCount;
    }
} // advmatch
