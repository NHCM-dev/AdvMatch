#include "ContractRiskEngine.h"
#include "advmatch/util/ContractMath.h"
#include "advmatch/matching/contracts/LiquidateEngine.h"

#include <algorithm>

namespace advmatch
{
    PlaceOrderState ContractRiskEngine::placeState(ContractState state) noexcept
    {
        switch (state)
        {
            case ContractState::RiskLimitExceeded:
                return PlaceOrderState::RiskLimitExceeded;
            case ContractState::InvalidLeverage:
                return PlaceOrderState::InvalidLeverage;
            case ContractState::InvalidMarginMode:
                return PlaceOrderState::InvalidMarginMode;
            case ContractState::InvalidPrice:
                return PlaceOrderState::InvalidPrice;
            case ContractState::InvalidTimestamp:
                return PlaceOrderState::InvalidTimestamp;
            case ContractState::Expired:
                return PlaceOrderState::ContractExpired;
            case ContractState::FundingRequired:
                return PlaceOrderState::FundingRequired;
            case ContractState::PositionNotFound:
                return PlaceOrderState::PositionNotFound;
            case ContractState::ReduceOnlyViolation:
                return PlaceOrderState::ReduceOnlyViolation;
            case ContractState::Overflow:
                return PlaceOrderState::Overflow;
            default:
                return PlaceOrderState::InvalidSymbol;
        }
    }

    std::optional<Balance> ContractRiskEngine::reservation(const OrderNode& node, Price price, Quantity quantity) noexcept
    {
        if (!node.contractMarket || !node.contractParticipant || node.leverage == 0)
        {
            return std::nullopt;
        }

        const ContractMarket& market = *node.contractMarket;
        const Price upper = market.reservationPriceBand().second;
        if (upper > static_cast<Price>(std::numeric_limits<Balance>::max()))
        {
            return std::nullopt;
        }

        const auto value = ContractMath::notional(std::max(price, upper), quantity, market.spec.contractMultiplier);
        if (!value || (!node.reduceOnly && *value > market.spec.maxPositionNotional))
        {
            return std::nullopt;
        }

        const Balance fee = ContractMath::rate(*value, std::max(market.spec.makerFeeRate, market.spec.takerFeeRate), true);
        return ContractMath::balance(static_cast<WideBalance>(fee) + (node.reduceOnly ? 0 : ContractMath::margin(*value, node.leverage)));
    }

    bool ContractRiskEngine::executable(const OrderNode& node, Price price) noexcept
    {
        const ContractMarket& market = *node.contractMarket;
        const auto [lower, upper] = market.priceBand();
        if (price < lower || price > upper)
        {
            return false;
        }

        if (node.basicOrder.orderType == OrderType::Market)
        {
            return node.basicOrder.orderSide == OrderSide::Bid ? price <= node.executionPriceLimit : price >= node.executionPriceLimit;
        }

        return true;
    }

    bool ContractRiskEngine::hasAvailableReservation(const OrderNode& node, Balance requiredBalance) noexcept
    {
        if (requiredBalance <= node.reservedBalance)
        {
            return true;
        }

        const ContractParticipant& participant = *node.contractParticipant;
        const Balance additional = requiredBalance - node.reservedBalance;
        const Balance active = node.account->queryActiveBalance(node.reservationAssetId).value_or(0);
        const CrossMarginState& cross = *participant.crossMargin;
        return active >= additional && !cross.liquidating &&
            static_cast<WideBalance>(active) + cross.unrealizedPnL - additional >= cross.initialMargin;
    }

    PlaceOrderRiskDecision ContractRiskEngine::checkPlaceOrder(const OrderNode& node, Balance) const
    {
        if (!node.contractMarket || !node.contractParticipant || node.account->getAccountType() != AccountType::Contract)
        {
            return PlaceOrderRiskDecision(PlaceOrderState::InvalidSymbol);
        }

        const ContractMarket& market = *node.contractMarket;
        const Position& position = *node.contractParticipant->position;
        if (market.status != ContractStatus::Trading || (market.spec.expiryTimestamp != 0 && market.commandTimestamp >= market.spec.expiryTimestamp))
        {
            return PlaceOrderRiskDecision(PlaceOrderState::ContractExpired);
        }

        if (node.reduceOnly && (position.positionSize == 0 || node.basicOrder.getUnfilledQuantity() > position.positionSize))
        {
            return PlaceOrderRiskDecision(PlaceOrderState::ReduceOnlyViolation);
        }

        if (!node.liquidationOrder && (position.liquidating || node.contractParticipant->crossMargin->liquidating ||
            (position.positionSize != 0 && LiquidateEngine::required(market, *node.contractParticipant))))
        {
            return PlaceOrderRiskDecision(PlaceOrderState::Liquidating);
        }

        if (node.liquidationOrder)
        {
            return PlaceOrderRiskDecision(Balance(0));
        }

        const auto required = reservation(node, node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity());
        if (!required)
        {
            return PlaceOrderRiskDecision(PlaceOrderState::Overflow);
        }

        return hasAvailableReservation(node, *required) ? PlaceOrderRiskDecision(*required)
            : PlaceOrderRiskDecision(PlaceOrderState::InsufficientBalance);
    }

    ModifyOrderRiskDecision ContractRiskEngine::checkModifyPrice(const OrderNode& node, Price price) const
    {
        const ContractMarket& market = *node.contractMarket;
        if (market.status != ContractStatus::Trading || node.contractParticipant->position->liquidating || node.contractParticipant->crossMargin->liquidating ||
            (market.spec.expiryTimestamp != 0 && market.commandTimestamp >= market.spec.expiryTimestamp))
        {
            return ModifyOrderRiskDecision(ModifyOrderState::TradingDisabled);
        }

        const auto required = reservation(node, price, node.basicOrder.getUnfilledQuantity());
        if (!required)
        {
            return ModifyOrderRiskDecision(ModifyOrderState::Overflow);
        }

        return hasAvailableReservation(node, *required) ? ModifyOrderRiskDecision(*required)
            : ModifyOrderRiskDecision(ModifyOrderState::InsufficientBalance);
    }

    ModifyOrderRiskDecision ContractRiskEngine::checkModifyQuantity(const OrderNode& node, QuantityChange change) const
    {
        const WideBalance quantity = static_cast<WideBalance>(node.basicOrder.getUnfilledQuantity()) + change;
        if (quantity < 0 || quantity > std::numeric_limits<Quantity>::max())
        {
            return ModifyOrderRiskDecision(ModifyOrderState::InvalidQuantity);
        }

        if (node.reduceOnly && quantity > node.contractParticipant->position->positionSize)
        {
            return ModifyOrderRiskDecision(ModifyOrderState::ReduceOnlyViolation);
        }

        const auto required = reservation(node, node.basicOrder.getPrice(), static_cast<Quantity>(quantity));
        if (!required)
        {
            return ModifyOrderRiskDecision(ModifyOrderState::Overflow);
        }

        const auto permission = this->checkModifyPrice(node, node.basicOrder.getPrice());
        if (!permission)
        {
            return permission;
        }

        return hasAvailableReservation(node, *required) ? ModifyOrderRiskDecision(*required)
            : ModifyOrderRiskDecision(ModifyOrderState::InsufficientBalance);
    }
}
