#ifndef ADVMATCH_MATCHING_H
#define ADVMATCH_MATCHING_H

#include "../trading/Order.h"

#include <cstdint>

namespace advmatch
{
    struct ContractParticipant;
    struct ContractMarket;
    class Account;
    class User;
    struct Symbol;
    struct PriceLevel;

    // MatchingEngine owns this node through BigAllocs. UserManager and
    // SymbolManager retain deactivated objects, so these borrowed addresses
    // remain valid for the complete engine lifetime.
    struct OrderNode
    {
        OrderNode(BasicOrder value, User& user, Account& account, const Symbol& symbol, AssetId baseAssetId,
                  AssetId quoteAssetId)
            : basicOrder(value), user(&user), account(&account), symbol(&symbol), baseAssetId(baseAssetId),
              quoteAssetId(quoteAssetId),
              reservationAssetId(basicOrder.orderSide == OrderSide::Bid ? quoteAssetId : baseAssetId)
        {
        }

        BasicOrder basicOrder;
        User* const user;
        Account* const account;
        const Symbol* const symbol;
        const AssetId baseAssetId;
        const AssetId quoteAssetId;
        AssetId reservationAssetId;
        ContractMarket* contractMarket = nullptr;
        ContractParticipant* contractParticipant = nullptr;
        OrderNode* contractPrev = nullptr;
        OrderNode* contractNext = nullptr;
        LeverageMultiplier leverage = 1;
        bool reduceOnly = false;
        bool liquidationOrder = false;
        Price executionPriceLimit = 0;
        Balance reservedBalance = 0;
        PriceLevel* priceLevel = nullptr;
        OrderNode* prev = nullptr;
        OrderNode* next = nullptr;
    };

    struct PriceLevel
    {
        explicit PriceLevel(Price value)
            : price(value)
        {
        }

        Price price;
        OrderNode* head = nullptr;
        OrderNode* tail = nullptr;
        Quantity totalQuantity = 0;
        std::uint32_t orderCount = 0;
    };
} // advmatch

#endif // ADVMATCH_MATCHING_H
