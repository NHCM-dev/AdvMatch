#ifndef ADVMATCH_SPOT_RISK_ENGINE_H
#define ADVMATCH_SPOT_RISK_ENGINE_H

#include "../RiskEngine.h"

namespace advmatch
{
    class SpotRiskEngine final : public RiskEngine
    {
        [[nodiscard]]
        static std::optional<Balance> requiredReservationBalance(const BasicOrder& order, Price price, Quantity remaining,
                                                                 Balance estimatedMarketBuyBalance = 0) noexcept;

        [[nodiscard]]
        static bool hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept;

    public:
        [[nodiscard]]
        PlaceOrderRiskDecision checkPlaceOrder(const OrderNode& node,
                                               Balance estimatedMarketBuyBalance) const override;

        [[nodiscard]]
        ModifyOrderRiskDecision checkModifyPrice(const OrderNode& node, Price price) const override;

        [[nodiscard]]
        ModifyOrderRiskDecision checkModifyQuantity(const OrderNode& node,
                                                    QuantityChange quantityChange) const override;
    };
} // advmatch

#endif // ADVMATCH_SPOT_RISK_ENGINE_H
