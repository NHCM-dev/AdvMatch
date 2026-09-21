//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_SPOTRISKENGINE_H
#define ADVMATCH_SPOTRISKENGINE_H
#include "../abstract/RiskEngine.h"

namespace advmatch
{
    class SpotRiskEngine : public RiskEngine
    {
        [[nodiscard]]
        static std::optional<Balance> requiredHold(const BasicOrder& order, Price price, Quantity remaining) noexcept;

        [[nodiscard]]
        static bool increaseHold(OrderNode& node, Balance amount);

        static void reduceHold(OrderNode& node, Balance amount);

        static void settleOrder(OrderNode& node, Price tradePrice, Quantity quantity);

    public:
        std::optional<PlaceOrderRejectReason> checkPlaceOrder(OrderNode& node) override;

        std::optional<ModifyOrderState> checkModifyPrice(OrderNode& node, Price price) override;
        std::optional<ModifyOrderState> checkModifyQuantity(OrderNode& node, QuantityChange quantityChange) override;

        std::optional<PlaceOrderRejectReason> reservePlaceOrder(OrderNode& node, Balance marketBuyCost);
        std::optional<ModifyOrderState> reserveModifyOrder(OrderNode& node, Price price, Quantity remaining);

        static void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker);
        static void reconcileOrder(OrderNode& node);
        static void releaseOrder(OrderNode& node);
    };
} // advmatch

#endif //ADVMATCH_SPOTRISKENGINE_H
