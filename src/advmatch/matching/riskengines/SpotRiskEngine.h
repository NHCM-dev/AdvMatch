//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SPOTRISKENGINE_H
#define ADVMATCH_SPOTRISKENGINE_H
#include "../abstract/RiskEngine.h"
#include <unordered_map>

namespace advmatch
{
    class SpotRiskEngine : public RiskEngine
    {
        struct Hold
        {
            AssetId assetId;
            Balance amount;
        };

        std::unordered_map<OrderId, Hold> holds;

        [[nodiscard]]
        static std::optional<Balance> requiredHold(const BasicOrder& order, Price price, Quantity remaining) noexcept;

        [[nodiscard]]
        bool increaseHold(const BasicOrder& order, Balance amount);

        void reduceHold(const BasicOrder& order, Balance amount);

        void settleOrder(const BasicOrder& order, Price tradePrice, Quantity quantity);

    public:
        SpotRiskEngine(UserManager& userManager, SymbolManager& symbolManager)
            : RiskEngine(userManager, symbolManager)
        {}

        std::optional<PlaceOrderRejectReason> checkPlaceOrder(BasicOrder& order) override;

        std::optional<ModifyOrderState> checkModifyPrice(BasicOrder& order, Price price) override;
        std::optional<ModifyOrderState> checkModifyQuantity(BasicOrder& order, QuantityChange quantityChange) override;

        std::optional<PlaceOrderRejectReason> reservePlaceOrder(const BasicOrder& order, Balance marketBuyCost);
        std::optional<ModifyOrderState> reserveModifyOrder(const BasicOrder& order, Price price, Quantity remaining);

        void settleTrade(const Trade& trade, const BasicOrder& maker, const BasicOrder& taker);
        void reconcileOrder(const BasicOrder& order);
        void releaseOrder(const BasicOrder& order);
    };
} // advmatch

#endif //ADVMATCH_SPOTRISKENGINE_H
