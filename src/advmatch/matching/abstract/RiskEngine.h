//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_RISKENGINE_H
#define ADVMATCH_RISKENGINE_H
#include "advmatch/management/SymbolManager.h"
#include "advmatch/management/UserManager.h"
#include "advmatch/trading/Order.h"

namespace advmatch
{
    class RiskEngine
    {
    protected:
        UserManager& userManager;
        SymbolManager& symbolManager;
    public:
        virtual ~RiskEngine() = default;

        RiskEngine(UserManager& userManager, SymbolManager& symbolManager)
        : userManager(userManager), symbolManager(symbolManager)
        {}

        virtual std::optional<PlaceOrderRejectReason> checkPlaceOrder(BasicOrder& order) = 0;

        virtual std::optional<ModifyOrderState> checkModifyPrice(BasicOrder& order, Price price) = 0;
        virtual std::optional<ModifyOrderState> checkModifyQuantity(BasicOrder& order, QuantityChange quantityChange) = 0;
    };
} // advmatch

#endif //ADVMATCH_RISKENGINE_H
