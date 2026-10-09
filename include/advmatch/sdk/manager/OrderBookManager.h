#ifndef ADVMATCH_SDK_ORDER_BOOK_MANAGER_H
#define ADVMATCH_SDK_ORDER_BOOK_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class OrderBookManager final : public EntityManager<OrderBook, ReadKind::Books>
    {
        template <typename Operation>
        CompletableFuture<OrderBook> add(const Symbol& symbol, Operation operation, std::uint64_t requestId) const
        {
            return detail::Results::attempt<OrderBook>([&]
            {
                symbol.requireOwner(this->api);
                return detail::Results::checked(this->api, std::move(operation), requestId).thenApply([api = this->api, id = symbol.id()](const CommandReceipt&)
                {
                    return OrderBook(api, id);
                });
            });
        }

    public:
        using EntityManager::EntityManager;

        CompletableFuture<OrderBook> addSpot(const Symbol& symbol, OrderBookImplementation implementation = OrderBookImplementation::Radix, std::uint64_t requestId = 0) const
        {
            return this->add(symbol, commands::AddSpotBook{symbol.id(), implementation}, requestId);
        }

        CompletableFuture<OrderBook> addContract(const Symbol& symbol, ContractSpec spec, Price markPrice, Timestamp timestamp, OrderBookImplementation implementation = OrderBookImplementation::Radix, std::uint64_t requestId = 0) const
        {
            return this->add(symbol, commands::AddContractBook{symbol.id(), spec, markPrice, timestamp, implementation}, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_ORDER_BOOK_MANAGER_H
