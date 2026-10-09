#ifndef ADVMATCH_MATCHING_H
#define ADVMATCH_MATCHING_H

#include "../trading/Order.h"
#include "advmatch/trading/rules/TradingRules.h"

#include <cstdint>

namespace advmatch
{
    namespace audit
    {
        class AuditEngine;
    }

    struct ContractParticipant;
    struct ContractMarket;
    class Account;
    class User;
    struct Symbol;
    struct PriceLevel;
    struct TradingRiskState;

    // MatchingEngine owns this node through BigAllocs. UserManager and
    // SymbolManager retain deactivated objects, so these borrowed addresses
    // remain valid for the complete engine lifetime.
    struct OrderNode
    {
        OrderNode(const BasicOrder& value, User& user, Account& account, const Symbol& symbol, AssetId baseAssetId,
                  AssetId quoteAssetId)
            : basicOrder(value), user(&user), account(&account), symbol(&symbol), baseAssetId(baseAssetId),
              quoteAssetId(quoteAssetId),
              reservationAssetId(basicOrder.orderSide == OrderSide::Bid ? quoteAssetId : baseAssetId)
        {
        }

        ~OrderNode();
        OrderNode(const OrderNode&) = delete;
        OrderNode& operator=(const OrderNode&) = delete;

        BasicOrder basicOrder;
        // Book links are accessed together during every fill/remove. Product
        // flags share the asset-ID block to avoid padding between pointers.
        PriceLevel* priceLevel = nullptr;
        OrderNode* prev = nullptr;
        OrderNode* next = nullptr;
        User* const user;
        Account* const account;
        const Symbol* const symbol;
        const AssetId baseAssetId;
        const AssetId quoteAssetId;
        AssetId reservationAssetId;
        LeverageMultiplier leverage = 1;
        bool reduceOnly = false;
        bool liquidationOrder = false;
        ContractMarket* contractMarket = nullptr;
        ContractParticipant* contractParticipant = nullptr;
        OrderNode* contractPrev = nullptr;
        OrderNode* contractNext = nullptr;
        Price executionPriceLimit = 0;
        Balance reservedBalance = 0;
        TradingRiskState* riskState = nullptr;
        Balance riskPendingNotional = 0;
        bool riskAcquired = false;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::Allow;
        MMRRate makerFeeRate = 0;
        MMRRate takerFeeRate = 0;
        Balance paidFees = 0;
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
