#ifndef ADVMATCH_SDK_SYMBOL_H
#define ADVMATCH_SDK_SYMBOL_H

#include "OrderBook.h"

namespace advmatch::sdk
{
    class Symbol final : public Entity<SymbolView, SymbolId, ReadKind::Symbol>
    {
    public:
        using Entity::Entity;

        OrderBook book() const
        {
            return OrderBook(this->api, this->id());
        }

        CompletableFuture<CommandReceipt> remove(std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::RemoveSymbol{this->id()}, requestId);
        }

        CompletableFuture<MarketView> market() const
        {
            ObservationQuery query;
            query.kind = ReadKind::Market;
            query.symbolId = this->id();
            return this->api->readRows<MarketView>(query).thenApply(detail::Results::one<MarketView>);
        }
    };
}

#endif // ADVMATCH_SDK_SYMBOL_H
