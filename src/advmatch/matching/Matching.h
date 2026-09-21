#ifndef ADVMATCH_MATCHING_H
#define ADVMATCH_MATCHING_H

#include "../trading/Order.h"

#include <cstdint>
#include <utility>

namespace advmatch
{
    class User;
    struct Symbol;
    struct PriceLevel;

    // MatchingEngine owns this node through BigAllocs. UserManager and
    // SymbolManager retain deactivated objects, so these borrowed addresses
    // remain valid for the complete engine lifetime.
    struct OrderNode
    {
        OrderNode(BasicOrder value, User& user, const Symbol& symbol, AssetId baseAssetId, AssetId quoteAssetId)
            : order(std::move(value)), user(&user), symbol(&symbol), baseAssetId(baseAssetId), quoteAssetId(quoteAssetId),
              holdAssetId(order.orderSide == OrderSide::Bid ? quoteAssetId : baseAssetId)
        {}

        BasicOrder order;
        User* const user;
        const Symbol* const symbol;
        const AssetId baseAssetId;
        const AssetId quoteAssetId;
        const AssetId holdAssetId;
        Balance heldAmount = 0;
        PriceLevel* level = nullptr;
        OrderNode* prev = nullptr;
        OrderNode* next = nullptr;
    };

    struct PriceLevel
    {
        explicit PriceLevel(Price value)
        : price(value)
        {}

        Price price;
        OrderNode* head = nullptr;
        OrderNode* tail = nullptr;
        Quantity totalQuantity = 0;
        std::uint32_t orderCount = 0;
    };
} // advmatch

#endif // ADVMATCH_MATCHING_H
