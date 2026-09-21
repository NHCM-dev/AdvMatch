//
// Created by NHCM on 2026/9/20.
//

#include "SymbolManager.h"

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
            Symbol(name, symbolId, symbolType, base, quote)
        );

        if (!result.second)
        {
            return nullptr;
        }

        try
        {
            this->symbolsByName.emplace(name, symbolId);
            this->symbolPairs.emplace(key);
        }
        catch (...)
        {
            this->symbolsByName.erase(name);
            this->symbols.erase(result.first);
            throw;
        }

        ++this->nextSymbolId;

        return &result.first->second;
    }

    bool SymbolManager::containsSymbol(SymbolId symbolId) const noexcept
    {
        return this->symbols.contains(symbolId);
    }

    const Symbol* SymbolManager::getSymbol(SymbolId symbolId) const noexcept
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end())
        {
            return nullptr;
        }

        return &iterator->second;
    }

    Symbol* SymbolManager::getSymbol(SymbolId symbolId) noexcept
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end())
        {
            return nullptr;
        }

        return &iterator->second;
    }

    bool SymbolManager::removeSymbol(SymbolId symbolId)
    {
        const auto iterator = this->symbols.find(symbolId);

        if (iterator == this->symbols.end())
        {
            return false;
        }

        this->symbolsByName.erase(iterator->second.name);
        this->symbolPairs.erase(SymbolKey{iterator->second.symbolType, iterator->second.baseAssetId, iterator->second.quoteAssetId});
        this->symbols.erase(iterator);

        return true;
    }

    std::size_t SymbolManager::getSymbolCount() const noexcept
    {
        return this->symbols.size();
    }
} // advmatch
