#include "TradingRuleEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/util/ContractMath.h"

namespace advmatch
{
    void TradingRuleEngine::addSymbol(SymbolId symbolId)
    {
        this->userStates.resize(std::max(this->userStates.size(), static_cast<std::size_t>(symbolId)));
    }

    bool TradingRuleEngine::valid(const TradingRules& rules) noexcept
    {
        return rules.priceTick != 0 && rules.quantityStep != 0 && rules.minQuantity != 0 && rules.minQuantity % rules.quantityStep == 0 && rules.minOrderNotional >= 0 && rules.fees.makerRate <= RATE_SCALE && rules.fees.takerRate <= RATE_SCALE && rules.risk.maxOrderNotional >= 0 && rules.risk.maxPositionNotional >= 0 && rules.risk.maxAccountNotional >= 0 && (rules.risk.maxOrderQuantity == 0 || rules.risk.maxOrderQuantity >= rules.minQuantity) && rules.selfTradePrevention <= SelfTradePrevention::CancelBoth;
    }

    TradingRuleResult TradingRuleEngine::configure(SymbolId symbolId, const TradingRules& rules)
    {
        Symbol* symbol = this->matching.symbolManager.getSymbol(symbolId);
        OrderBook* book = this->matching.findBook(symbolId);
        if (!symbol || !book)
        {
            return {TradingRuleState::InvalidSymbol};
        }

        if (!valid(rules) || (symbol->symbolType == SymbolType::Spot && (rules.risk.maxPositionNotional != 0 || rules.risk.maxLeverage != 0)))
        {
            return {TradingRuleState::InvalidConfiguration};
        }

        const auto& previous = symbol->tradingRules;
        const bool onlyBandChange = rules.priceTick == previous.priceTick && rules.quantityStep == previous.quantityStep &&
            rules.minQuantity == previous.minQuantity && rules.minOrderNotional == previous.minOrderNotional &&
            rules.fees.makerRate == previous.fees.makerRate && rules.fees.takerRate == previous.fees.takerRate &&
            rules.risk.maxOrderQuantity == previous.risk.maxOrderQuantity && rules.risk.maxOrderNotional == previous.risk.maxOrderNotional &&
            rules.risk.maxPositionNotional == previous.risk.maxPositionNotional && rules.risk.maxAccountNotional == previous.risk.maxAccountNotional &&
            rules.risk.maxActiveOrders == previous.risk.maxActiveOrders && rules.risk.maxLeverage == previous.risk.maxLeverage &&
            rules.selfTradePrevention == previous.selfTradePrevention && rules.tickAwarePriceBand != previous.tickAwarePriceBand;
        auto* market = this->matching.getContractEngine().findMarket(symbolId);
        if (onlyBandChange && market && market->status == ContractStatus::Trading)
        {
            symbol->tradingRules.tickAwarePriceBand = rules.tickAwarePriceBand;
            ++market->liquidityRevision;
            return {};
        }

        if (book->bestBid() || book->bestAsk())
        {
            return {TradingRuleState::OpenExposure};
        }

        if (market && (market->status != ContractStatus::Trading || !market->activePositions.empty() || !market->orderParticipants.empty()))
        {
            return {TradingRuleState::OpenExposure};
        }

        symbol->tradingRules = rules;
        if (market)
        {
            market->spec.makerFeeRate = rules.fees.makerRate;
            market->spec.takerFeeRate = rules.fees.takerRate;
        }
        else
        {
            book->setMatchPolicy(&this->matching.settleEngine);
        }

        return {};
    }

    PlaceOrderState TradingRuleEngine::check(const Symbol& symbol, OrderSide side, OrderType type, Price price, Quantity quantity, Price priceLimit) const noexcept
    {
        const auto& rules = symbol.tradingRules;
        if ((type == OrderType::Limit && price % rules.priceTick != 0) || (priceLimit != 0 && priceLimit % rules.priceTick != 0))
        {
            return PlaceOrderState::InvalidPriceTick;
        }

        if (quantity < rules.minQuantity || quantity % rules.quantityStep != 0)
        {
            return PlaceOrderState::InvalidQuantityStep;
        }

        if (rules.risk.maxOrderQuantity != 0 && quantity > rules.risk.maxOrderQuantity)
        {
            return PlaceOrderState::RiskLimitExceeded;
        }

        if (rules.minOrderNotional == 0 && rules.risk.maxOrderNotional == 0)
        {
            return PlaceOrderState::Success;
        }

        WideBalance amount = 0;
        if (symbol.symbolType != SymbolType::Spot)
        {
            const auto* market = this->matching.getContractEngine().findMarket(symbol.symbolId);
            const Price effective = type == OrderType::Market ? priceLimit : price;
            amount = static_cast<WideBalance>(effective) * quantity * market->spec.contractMultiplier;
        }
        else if (type == OrderType::Market && side == OrderSide::Bid)
        {
            const auto cost = this->matching.findBook(symbol.symbolId)->estimateMarketBuyCost(quantity);
            if (!cost)
            {
                return PlaceOrderState::Overflow;
            }

            amount = *cost;
        }
        else
        {
            const auto bid = type == OrderType::Market ? this->matching.bestBid(symbol.symbolId) : std::nullopt;
            amount = static_cast<WideBalance>(type == OrderType::Market ? (bid ? bid->price : 0) : price) * quantity;
        }

        if (amount < rules.minOrderNotional)
        {
            return PlaceOrderState::MinimumNotional;
        }

        return rules.risk.maxOrderNotional != 0 && amount > rules.risk.maxOrderNotional ? PlaceOrderState::RiskLimitExceeded : PlaceOrderState::Success;
    }

    std::optional<Balance> TradingRuleEngine::notional(const OrderNode& node, Price price, Quantity quantity) noexcept
    {
        if (node.reduceOnly)
        {
            return 0;
        }

        if (node.contractMarket)
        {
            return ContractMath::notional(std::max(price, node.contractMarket->reservationPriceBand().second), quantity, node.contractMarket->spec.contractMultiplier);
        }

        return ContractMath::balance(static_cast<WideBalance>(price) * quantity);
    }

    PlaceOrderState TradingRuleEngine::prepare(OrderNode& node)
    {
        const auto& rules = node.symbol->tradingRules;
        node.selfTradePrevention = rules.selfTradePrevention;
        node.makerFeeRate = rules.fees.makerRate;
        node.takerFeeRate = rules.fees.takerRate;
        if (node.liquidationOrder)
        {
            node.takerFeeRate = node.contractMarket->spec.liquidationFeeRate;
        }
        if (rules.risk.maxAccountNotional != 0 || rules.risk.maxActiveOrders != 0)
        {
            auto& states = this->userStates[node.basicOrder.symbolId - 1];
            if (!states)
            {
                states = std::make_unique<UserStates>();
            }

            node.riskState = &(*states)[node.basicOrder.userId];
            if (node.contractParticipant)
            {
                node.contractParticipant->riskState = node.riskState;
            }
        }

        return this->check(node, node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity());
    }

    PlaceOrderState TradingRuleEngine::check(const OrderNode& node, Price price, Quantity quantity) const noexcept
    {
        if (node.liquidationOrder)
        {
            return PlaceOrderState::Success;
        }

        const auto shape = this->check(*node.symbol, node.basicOrder.orderSide, node.basicOrder.orderType, price, quantity, node.executionPriceLimit);
        if (shape != PlaceOrderState::Success)
        {
            return shape;
        }

        const auto& limits = node.symbol->tradingRules.risk;
        if (node.contractParticipant && limits.maxLeverage != 0 && node.leverage > limits.maxLeverage)
        {
            return PlaceOrderState::RiskLimitExceeded;
        }

        if (!node.riskState && limits.maxPositionNotional == 0)
        {
            return PlaceOrderState::Success;
        }

        const Price effectivePrice = node.basicOrder.orderType == OrderType::Market && !node.contractMarket ? (node.basicOrder.orderSide == OrderSide::Bid ? this->matching.bestAsk(node.basicOrder.symbolId) : this->matching.bestBid(node.basicOrder.symbolId)).value_or(Quote{}).price : price;
        auto pending = notional(node, effectivePrice, quantity);
        if (node.basicOrder.orderType == OrderType::Market && !node.contractMarket && node.basicOrder.orderSide == OrderSide::Bid)
        {
            pending = this->matching.findBook(node.basicOrder.symbolId)->estimateMarketBuyCost(quantity);
        }

        if (!pending)
        {
            return PlaceOrderState::Overflow;
        }

        if (limits.maxPositionNotional != 0 && !node.reduceOnly && static_cast<WideBalance>(node.contractParticipant->position->entryNotional) + *pending > limits.maxPositionNotional)
        {
            return PlaceOrderState::RiskLimitExceeded;
        }

        if (node.riskState)
        {
            const auto& state = *node.riskState;
            if (limits.maxActiveOrders != 0 && !node.riskAcquired && state.activeOrders >= limits.maxActiveOrders)
            {
                return PlaceOrderState::RiskLimitExceeded;
            }

            const WideBalance total = static_cast<WideBalance>(state.pendingNotional) - node.riskPendingNotional + state.positionNotional + *pending;
            if (total > std::numeric_limits<Balance>::max() || (limits.maxAccountNotional != 0 && total > limits.maxAccountNotional))
            {
                return PlaceOrderState::RiskLimitExceeded;
            }
        }

        return PlaceOrderState::Success;
    }

    void TradingRuleEngine::acquire(OrderNode& node)
    {
        if (!node.riskState)
        {
            return;
        }

        node.riskPendingNotional = node.contractMarket ? *notional(node, node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity()) : node.reservedBalance;
        // A spot sell reserves base units; convert its pending order to quote.
        if (!node.contractMarket && node.basicOrder.orderSide == OrderSide::Ask)
        {
            node.riskPendingNotional = *notional(node, node.basicOrder.orderType == OrderType::Market ? 0 : node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity());
        }

        node.riskAcquired = true;
        ++node.riskState->activeOrders;
        node.riskState->pendingNotional += node.riskPendingNotional;
    }

    void TradingRuleEngine::release(OrderNode& node) noexcept
    {
        if (node.riskAcquired)
        {
            --node.riskState->activeOrders;
            node.riskState->pendingNotional -= node.riskPendingNotional;
            node.riskPendingNotional = 0;
            node.riskAcquired = false;
        }
    }

    void TradingRuleEngine::reconcile(OrderNode& node)
    {
        if (!node.riskAcquired)
        {
            return;
        }

        if (isOrderStatusTerminal(node.basicOrder.getStatus()) || !node.priceLevel)
        {
            release(node);
            return;
        }

        const auto amount = notional(node, node.basicOrder.getPrice(), node.basicOrder.getUnfilledQuantity());
        if (!amount)
        {
            throw std::logic_error("accepted risk reservation overflowed");
        }

        node.riskState->pendingNotional += *amount - node.riskPendingNotional;
        node.riskPendingNotional = *amount;
    }

    ModifyOrderState TradingRuleEngine::modifyState(PlaceOrderState state) noexcept
    {
        switch (state)
        {
            case PlaceOrderState::Success: return ModifyOrderState::Success;
            case PlaceOrderState::InvalidPriceTick: return ModifyOrderState::InvalidPriceTick;
            case PlaceOrderState::InvalidQuantityStep: return ModifyOrderState::InvalidQuantityStep;
            case PlaceOrderState::MinimumNotional: return ModifyOrderState::MinimumNotional;
            case PlaceOrderState::RiskLimitExceeded: return ModifyOrderState::RiskLimitExceeded;
            default: return ModifyOrderState::Overflow;
        }
    }

    std::optional<UserRiskView> TradingRuleEngine::query(SymbolId symbolId, UserId userId) const
    {
        if (symbolId == 0 || symbolId > this->userStates.size())
        {
            return std::nullopt;
        }

        const auto& states = this->userStates[symbolId - 1];
        const auto* state = states && states->contains(userId) ? &states->at(userId) : nullptr;
        return UserRiskView{symbolId, userId, state ? state->activeOrders : 0, state ? state->pendingNotional : 0, state ? state->positionNotional : 0};
    }
}
