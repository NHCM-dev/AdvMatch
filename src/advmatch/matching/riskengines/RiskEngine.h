#ifndef ADVMATCH_RISK_ENGINE_H
#define ADVMATCH_RISK_ENGINE_H

#include "../Matching.h"
#include "../orderbooks/OrderBook.h"

#include <optional>

namespace advmatch
{
    template <typename RejectReason>
    struct RiskDecision
    {
        explicit RiskDecision(Balance requiredReservationBalance)
            : requiredReservationBalance(requiredReservationBalance)
        {
        }

        explicit RiskDecision(RejectReason rejectReason)
            : rejectReason(rejectReason)
        {
        }

        Balance requiredReservationBalance = 0;
        std::optional<RejectReason> rejectReason;

        explicit operator bool() const noexcept
        {
            return !this->rejectReason.has_value();
        }
    };

    using PlaceOrderRiskDecision = RiskDecision<PlaceOrderRejectReason>;
    using ModifyOrderRiskDecision = RiskDecision<ModifyOrderState>;

    // Risk engines are read-only policy components. They validate an operation
    // and calculate the total reservation it requires, but never mutate an
    // account, an order, or an order book.
    class RiskEngine
    {
    public:
        virtual ~RiskEngine() = default;

        [[nodiscard]]
        virtual PlaceOrderRiskDecision checkPlaceOrder(const OrderNode& order,
                                                       Balance estimatedMarketBuyBalance) const = 0;

        [[nodiscard]]
        virtual ModifyOrderRiskDecision checkModifyPrice(const OrderNode& order, Price price) const = 0;

        [[nodiscard]]
        virtual ModifyOrderRiskDecision checkModifyQuantity(const OrderNode& order,
                                                            QuantityChange quantityChange) const = 0;
    };
} // advmatch

#endif // ADVMATCH_RISK_ENGINE_H
