//
// Created by NHCM on 2026/9/23.
//

#ifndef ADVMATCH_CONTRACTRISKENGINE_H
#define ADVMATCH_CONTRACTRISKENGINE_H
#include "advmatch/matching/riskengines/RiskEngine.h"

namespace advmatch
{
    class ContractRiskEngine final : public RiskEngine
    {
        [[nodiscard]]
        static std::optional<Balance> requiredReservationBalance(const BasicOrder& order, Price price,
                                                                 Quantity remaining,
                                                                 Balance estimatedMarketBuyBalance = 0) noexcept;

        [[nodiscard]]
        static bool hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept;

    public:
        [[nodiscard]]
        PlaceOrderRiskDecision checkPlaceOrder(const OrderNode& node, Balance estimatedMarketBuyBalance) const override;

        [[nodiscard]]
        ModifyOrderRiskDecision checkModifyPrice(const OrderNode& order, Price price) const override;

        [[nodiscard]]
        ModifyOrderRiskDecision checkModifyQuantity(const OrderNode& order, QuantityChange quantityChange) const override;
    };
} // advmatch

#endif //ADVMATCH_CONTRACTRISKENGINE_H
