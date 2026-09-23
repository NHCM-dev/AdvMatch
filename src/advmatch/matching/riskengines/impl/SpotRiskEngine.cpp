#include "SpotRiskEngine.h"

#include "advmatch/account/User.h"
#include "advmatch/trading/Symbol.h"

#include <limits>

namespace advmatch
{
    std::optional<Balance> SpotRiskEngine::requiredReservationBalance(const BasicOrder& order, Price price,
                                                                      Quantity remaining,
                                                                      Balance estimatedMarketBuyBalance) noexcept
    {
        if (order.orderSide == OrderSide::Ask)
        {
            if (remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()))
            {
                return std::nullopt;
            }

            return static_cast<Balance>(remaining);
        }

        if (order.orderType == OrderType::Market)
        {
            return estimatedMarketBuyBalance < 0
                       ? std::nullopt
                       : std::optional<Balance>(estimatedMarketBuyBalance);
        }

        if (price != 0 && remaining > static_cast<Quantity>(std::numeric_limits<Balance>::max()) / price)
        {
            return std::nullopt;
        }

        return static_cast<Balance>(price * remaining);
    }

    bool SpotRiskEngine::hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept
    {
        if (requiredBalance < 0 || node.reservedBalance < 0)
        {
            return false;
        }

        if (requiredBalance <= node.reservedBalance)
        {
            return true;
        }

        const BalanceChange additionalChange = requiredBalance - node.reservedBalance;
        const PositiveBalanceChange additionalAmount = static_cast<PositiveBalanceChange>(additionalChange);
        const Balance availableBalance = node.user->queryActiveBalance(node.reservationAssetId).value_or(0);
        return availableBalance >= 0 && static_cast<PositiveBalanceChange>(availableBalance) >= additionalAmount;
    }

    PlaceOrderRiskDecision SpotRiskEngine::checkPlaceOrder(const OrderNode& node,
                                                           Balance estimatedMarketBuyBalance) const
    {
        const BasicOrder& order = node.order;
        const User& user = *node.user;
        const Symbol& symbol = *node.symbol;

        if (symbol.symbolType != SymbolType::Spot || symbol.symbolId != order.symbolId)
        {
            return PlaceOrderRiskDecision(PlaceOrderRejectReason::InvalidSymbol);
        }

        if (user.getUserId() != order.userId)
        {
            return PlaceOrderRiskDecision(PlaceOrderRejectReason::UserNotFound);
        }

        const auto requiredBalance = requiredReservationBalance(order, order.getPrice(), order.getUnfilledQuantity(),
                                                                estimatedMarketBuyBalance);
        if (!requiredBalance)
        {
            return PlaceOrderRiskDecision(PlaceOrderRejectReason::Overflow);
        }

        if (!hasAvailableReservation(node, *requiredBalance))
        {
            return PlaceOrderRiskDecision(PlaceOrderRejectReason::InsufficientBalance);
        }

        return PlaceOrderRiskDecision(*requiredBalance);
    }

    ModifyOrderRiskDecision SpotRiskEngine::checkModifyPrice(const OrderNode& node, Price price) const
    {
        const auto requiredBalance = requiredReservationBalance(node.order, price, node.order.getUnfilledQuantity());
        if (!requiredBalance)
        {
            return ModifyOrderRiskDecision(ModifyOrderState::Overflow);
        }

        if (!hasAvailableReservation(node, *requiredBalance))
        {
            return ModifyOrderRiskDecision(ModifyOrderState::InsufficientBalance);
        }

        return ModifyOrderRiskDecision(*requiredBalance);
    }

    ModifyOrderRiskDecision SpotRiskEngine::checkModifyQuantity(const OrderNode& node,
                                                                QuantityChange quantityChange) const
    {
        const Quantity currentQuantity = node.order.getUnfilledQuantity();
        Quantity newQuantity = currentQuantity;

        if (quantityChange > 0)
        {
            const Quantity increase = static_cast<Quantity>(quantityChange);
            if (increase > std::numeric_limits<Quantity>::max() - currentQuantity)
            {
                return ModifyOrderRiskDecision(ModifyOrderState::Overflow);
            }

            newQuantity += increase;
        }
        else if (quantityChange < 0)
        {
            const Quantity decrease = static_cast<Quantity>(-(quantityChange + 1)) + 1;
            if (decrease > currentQuantity)
            {
                return ModifyOrderRiskDecision(ModifyOrderState::InsufficientQuantity);
            }

            newQuantity -= decrease;
        }

        const auto requiredBalance = requiredReservationBalance(node.order, node.order.getPrice(), newQuantity);
        if (!requiredBalance)
        {
            return ModifyOrderRiskDecision(ModifyOrderState::Overflow);
        }

        if (!hasAvailableReservation(node, *requiredBalance))
        {
            return ModifyOrderRiskDecision(ModifyOrderState::InsufficientBalance);
        }

        return ModifyOrderRiskDecision(*requiredBalance);
    }
} // advmatch
