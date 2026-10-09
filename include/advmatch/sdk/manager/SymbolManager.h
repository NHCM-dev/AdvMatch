#ifndef ADVMATCH_SDK_SYMBOL_MANAGER_H
#define ADVMATCH_SDK_SYMBOL_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class SymbolManager final : public EntityManager<Symbol, ReadKind::Symbols>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<Symbol> create(std::string name, SymbolType type, const Asset& base, const Asset& quote, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<Symbol>([&]
            {
                base.requireOwner(this->api);
                quote.requireOwner(this->api);
                return detail::Results::created<Symbol>(this->api, commands::CreateSymbol{std::move(name), type, base.id(), quote.id()}, requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_SYMBOL_MANAGER_H
