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
        this->symbolIndex.resize(static_cast<std::size_t>(symbolId), nullptr);

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
            this->symbolPairs.emplace(key, symbolId);
        }
        catch (...)
        {
            this->symbolsByName.erase(result.first->second.name);
            this->symbols.erase(result.first);
            throw;
        }

        ++this->nextSymbolId;
        ++this->activeSymbolCount;
        this->symbolIndex[symbolId - 1] = &result.first->second;

        return &result.first->second;
    }

    bool SymbolManager::containsSymbol(SymbolId symbolId) const noexcept
    {
        return this->getSymbol(symbolId) != nullptr;
    }

    const Symbol* SymbolManager::getSymbol(SymbolId symbolId) const noexcept
    {
        const Symbol* symbol = symbolId != 0 && symbolId <= this->symbolIndex.size() ? this->symbolIndex[symbolId - 1] : nullptr;
        return symbol && symbol->isActive() ? symbol : nullptr;
    }

    Symbol* SymbolManager::getSymbol(SymbolId symbolId) noexcept
    {
        Symbol* symbol = symbolId != 0 && symbolId <= this->symbolIndex.size() ? this->symbolIndex[symbolId - 1] : nullptr;
        return symbol && symbol->isActive() ? symbol : nullptr;
    }

    const Symbol* SymbolManager::getSymbol(SymbolType type, AssetId base, AssetId quote) const noexcept
    {
        const auto iterator = this->symbolPairs.find(SymbolKey{type, base, quote});
        return iterator == this->symbolPairs.end() ? nullptr : this->getSymbol(iterator->second);
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
