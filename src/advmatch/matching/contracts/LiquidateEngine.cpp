#include "LiquidateEngine.h"
#include "ContractEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/util/ContractMath.h"

namespace advmatch
{
    void LiquidateEngine::enqueue(ContractMarket& market, ContractParticipant& participant)
    {
        if (participant.position->positionSize != 0 && (participant.position->liquidating || required(market, participant)))
        {
            market.pendingLiquidations[participant.position->positionId] = &participant;
        }
        else
        {
            market.pendingLiquidations.erase(participant.position->positionId);
        }
    }

    bool LiquidateEngine::required(const ContractMarket& market, const ContractParticipant& participant) noexcept
    {
        const Position& position = *participant.position;
        if (position.positionSize == 0)
        {
            return false;
        }

        if (position.marginMode == MarginMode::Cross)
        {
            const CrossMarginState& state = *participant.crossMargin;
            const WideBalance equity = static_cast<WideBalance>(participant.account->queryActiveBalance(market.symbol->quoteAssetId).value_or(0)) + state.unrealizedPnL + state.orderReservation;
            return equity <= static_cast<WideBalance>(state.maintenanceMargin) + state.liquidationFeeReserve;
        }

        const auto notional = ContractMath::notional(market.markPrice, position.positionSize, market.spec.contractMultiplier);
        if (!notional)
        {
            return true;
        }

        const Balance fee = ContractMath::rate(*notional, market.spec.liquidationFeeRate, true);
        return static_cast<WideBalance>(position.initialMargin) + position.updatedUnrealizedPnl <=
            static_cast<WideBalance>(position.maintenanceMargin) + fee;
    }

    ContractResult LiquidateEngine::execute(ContractEngine& contracts, MatchingEngine& matching,
        ContractMarket& market, ContractParticipant& participant, Timestamp timestamp, TradeBuffer& trades) const
    {
        trades.clear();
        Position& position = *participant.position;
        if (timestamp < market.commandTimestamp)
        {
            return {ContractState::InvalidTimestamp};
        }

        if (market.status != ContractStatus::Trading || (market.spec.expiryTimestamp != 0 && timestamp >= market.spec.expiryTimestamp))
        {
            return {ContractState::Expired};
        }

        if (position.positionSize == 0 || (!position.liquidating && !required(market, participant)))
        {
            return {ContractState::LiquidationNotRequired};
        }

        SmallVector<ContractParticipant*, 4> targets;
        if (position.marginMode == MarginMode::Cross)
        {
            participant.crossMargin->liquidating = true;
            participant.account->forEachTradingPosition([&](PositionId id, const Position& candidate)
            {
                ContractParticipant* target = contracts.findParticipant(id);
                if (target && candidate.marginMode == MarginMode::Cross &&
                    target->market->symbol->quoteAssetId == market.symbol->quoteAssetId)
                {
                    targets.push_back(target);
                }
            });
        }
        else
        {
            targets.push_back(&participant);
        }

        // All orders are removed first so their reservations become available
        // before a cross liquidation consumes collateral. Intrusive lists make
        // each cancellation O(1) except removal of an empty tree price level.
        for (ContractParticipant* target : targets)
        {
            target->position->liquidating = target->position->positionSize != 0;
            contracts.cancelOrders(*target->market, *target, timestamp);
            enqueue(*target->market, *target);
        }

        ContractResult result;
        for (ContractParticipant* target : targets)
        {
            Position& candidate = *target->position;
            if (candidate.positionSize == 0)
            {
                continue;
            }

            ContractMarket& targetMarket = *target->market;
            const bool buy = candidate.positionSide == PositionSide::Short;
            const WideBalance boundedPrice = static_cast<WideBalance>(targetMarket.markPrice) *
                (buy ? RATE_SCALE + targetMarket.spec.priceBandRate : RATE_SCALE - targetMarket.spec.priceBandRate) / RATE_SCALE;
            const auto limit = ContractMath::balance(boundedPrice);
            if (!limit || *limit <= 0)
            {
                result.state = ContractState::PendingLiquidation;
                continue;
            }

            ContractOrderOptions options;
            options.positionId = candidate.positionId;
            options.marginMode = candidate.marginMode;
            options.reduceOnly = true;
            options.priceLimit = static_cast<Price>(*limit);
            const auto order = matching.submitOrder(candidate.userId, candidate.symbolId, buy ? OrderSide::Bid : OrderSide::Ask,
                OrderType::Market, TimeInForce::IOC, 0, candidate.positionSize, timestamp, trades, &options, true);
            static_cast<void>(order);
            ++result.affectedPositions;
            enqueue(targetMarket, *target);
            if (candidate.positionSize != 0)
            {
                result.state = ContractState::PendingLiquidation;
            }
        }

        if (position.marginMode == MarginMode::Cross && participant.crossMargin->maintenanceMargin == 0)
        {
            participant.crossMargin->liquidating = false;
        }

        return result;
    }
}
