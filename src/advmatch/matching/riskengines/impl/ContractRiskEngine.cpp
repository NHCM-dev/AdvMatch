#include "ContractRiskEngine.h"

#include "advmatch/ledging/account/Account.h"
#include "advmatch/ledging/User.h"
#include "../../../trading/Symbol.h"

#include <limits>

namespace advmatch
{
    std::optional<Balance> ContractRiskEngine::requiredReservationBalance(const BasicOrder& order, Price price,
                                                                          Quantity remaining,
                                                                          Balance estimatedMarketBuyBalance) noexcept
    {
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

        // Until leverage and risk tiers are attached to Symbol, contract
        // orders reserve their full notional in the quote asset (1x leverage).
        return static_cast<Balance>(price * remaining);
    }

    bool ContractRiskEngine::hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept
    {
        if (!node.account || node.account->getAccountType() != AccountType::Contract ||
            requiredBalance < 0 || node.reservedBalance < 0)
        {
            return false;
        }

        if (requiredBalance <= node.reservedBalance)
        {
            return true;
        }

        const PositiveBalanceChange additionalAmount = static_cast<PositiveBalanceChange>(
            requiredBalance - node.reservedBalance);
        const Balance availableBalance = node.account->queryActiveBalance(node.reservationAssetId).value_or(0);
        return availableBalance >= 0 && static_cast<PositiveBalanceChange>(availableBalance) >= additionalAmount;
    }

    PlaceOrderRiskDecision ContractRiskEngine::checkPlaceOrder(const OrderNode& node,
                                                               Balance estimatedMarketBuyBalance) const
    {
        const BasicOrder& order = node.basicOrder;
        const User& user = *node.user;
        const Symbol& symbol = *node.symbol;

        if (symbol.symbolId != order.symbolId || symbol.symbolType == SymbolType::Spot)
        {
            return PlaceOrderRiskDecision(PlaceOrderState::InvalidSymbol);
        }

        if (user.getUserId() != order.userId || node.account != &user.getContractAccount())
        {
            return PlaceOrderRiskDecision(PlaceOrderState::UserNotFound);
        }

        const auto requiredBalance = requiredReservationBalance(order, order.getPrice(), order.getUnfilledQuantity(),
                                                                estimatedMarketBuyBalance);
        if (!requiredBalance)
        {
            return PlaceOrderRiskDecision(PlaceOrderState::Overflow);
        }

        if (!hasAvailableReservation(node, *requiredBalance))
        {
            return PlaceOrderRiskDecision(PlaceOrderState::InsufficientBalance);
        }

        return PlaceOrderRiskDecision(*requiredBalance);
    }

    ModifyOrderRiskDecision ContractRiskEngine::checkModifyPrice(const OrderNode& node, Price price) const
    {
        const auto requiredBalance = requiredReservationBalance(node.basicOrder, price,
                                                                node.basicOrder.getUnfilledQuantity());
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

    ModifyOrderRiskDecision ContractRiskEngine::checkModifyQuantity(const OrderNode& node,
                                                                    QuantityChange quantityChange) const
    {
        const Quantity currentQuantity = node.basicOrder.getUnfilledQuantity();
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

        const auto requiredBalance = requiredReservationBalance(node.basicOrder, node.basicOrder.getPrice(),
                                                                newQuantity);
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
}
