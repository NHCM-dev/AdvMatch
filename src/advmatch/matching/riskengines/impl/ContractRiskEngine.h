#ifndef ADVMATCH_CONTRACT_RISK_ENGINE_H
#define ADVMATCH_CONTRACT_RISK_ENGINE_H

#include "../RiskEngine.h"
#include "advmatch/matching/contracts/ContractMarket.h"

namespace advmatch
{
    class ContractRiskEngine final : public RiskEngine
    {
        static bool hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept;

    public:
        static PlaceOrderState placeState(ContractState state) noexcept;
        static std::optional<Balance> reservation(const OrderNode& node, Price price, Quantity quantity) noexcept;
        static bool executable(const OrderNode& node, Price price) noexcept;
        PlaceOrderRiskDecision checkPlaceOrder(const OrderNode& node, Balance estimatedMarketBuyBalance) const override;
        ModifyOrderRiskDecision checkModifyPrice(const OrderNode& node, Price price) const override;
        ModifyOrderRiskDecision checkModifyQuantity(const OrderNode& node, QuantityChange change) const override;
    };
}

#endif // ADVMATCH_CONTRACT_RISK_ENGINE_H
