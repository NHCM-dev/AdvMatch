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
        static bool increaseHold(OrderNode& order, Balance amount);

        static void reduceHold(OrderNode& order, Balance amount);

        static void settleOrder(OrderNode& order, Price tradePrice, Quantity quantity);

    public:
        std::optional<PlaceOrderRejectReason> checkPlaceOrder(OrderNode& order) override;

        std::optional<ModifyOrderState> checkModifyPrice(OrderNode& order, Price price) override;
        std::optional<ModifyOrderState> checkModifyQuantity(OrderNode& order, QuantityChange quantityChange) override;

        std::optional<PlaceOrderRejectReason> reservePlaceOrder(OrderNode& order, Balance marketBuyCost);
        std::optional<ModifyOrderState> reserveModifyOrder(OrderNode& order, Price price, Quantity remaining);

        static void settleTrade(const Trade& trade, OrderNode& maker, OrderNode& taker);
        static void reconcileOrder(OrderNode& order);
        static void releaseOrder(OrderNode& order);
    };
} // advmatch

#endif //ADVMATCH_SPOTRISKENGINE_H
