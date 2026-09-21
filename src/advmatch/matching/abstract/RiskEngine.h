//
// Created by NHCM on 2026/9/20.
//

#ifndef ADVMATCH_RISKENGINE_H
#define ADVMATCH_RISKENGINE_H
#include "advmatch/matching/Matching.h"
#include "advmatch/matching/abstract/OrderBook.h"

namespace advmatch
{
    class RiskEngine
    {
    public:
        virtual ~RiskEngine() = default;

        virtual std::optional<PlaceOrderRejectReason> checkPlaceOrder(OrderNode& order) = 0;

        virtual std::optional<ModifyOrderState> checkModifyPrice(OrderNode& order, Price price) = 0;
        virtual std::optional<ModifyOrderState> checkModifyQuantity(OrderNode& order, QuantityChange quantityChange) =
        0;
    };
} // advmatch

#endif //ADVMATCH_RISKENGINE_H
