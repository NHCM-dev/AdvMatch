#ifndef ADVMATCH_SDK_MATCHING_ENGINE_H
#define ADVMATCH_SDK_MATCHING_ENGINE_H

#include "Manager.h"

namespace advmatch::sdk
{
    class MatchingEngine final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<Order> place(const User& user, const Symbol& symbol, OrderSide side, OrderType type, TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, std::optional<ContractOrderOptions> options = std::nullopt, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<Order>([&]
            {
                symbol.requireOwner(this->api);
                return symbol.book().place(user, side, type, timeInForce, price, quantity, timestamp, options, requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_MATCHING_ENGINE_H
